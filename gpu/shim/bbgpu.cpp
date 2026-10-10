#include "bbport_write_log.h"
// bbport: glue between the C loader and the vendored shadPS4 video core.
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "bbport_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../bbgpu.h"
#include "video_core/renderer_vulkan/vk_xr.h"
#include "video_core/renderer_vulkan/vk_xr_stereo.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
/* Frame heartbeat for the hang watchdog (src/runtime_crash.c); bumped by the video-out flip. */
extern "C" volatile unsigned long long bb_heartbeat_ticks = 0;
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void* process, unsigned code);
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess(void);
#endif
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // bbport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !BbCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    BbCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    BbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, BbWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    BbStats::Timer timer{BbStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    BbStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 BbDisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

#ifdef BB_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (BB_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    BbSettings::Load();
#ifdef BB_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("BB_GPU_USER_DIR")) _putenv_s("BB_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("BB_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "Bloodborne";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        std::fflush(stdout);
        Vulkan::Xr::ShutdownForExit(2000); // OpenXR session and swapchains first, _Exit skips atexit
        std::fflush(stderr);
#ifdef _WIN32
        // _Exit still runs the DLLs' process-detach: with the XR runtime's threads alive it
        // hung after a clean teardown. Nothing is left to flush, so end the process outright.
        TerminateProcess(GetCurrentProcess(), 0);
#endif
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void bbgpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned bbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int bbgpu_overlay_captures_input(void) {
    return BbOverlay::CapturesInput() ? 1 : 0;
}

extern "C" int bbgpu_vr_pad(BbVrPad* out) {
    Vulkan::Xr* xr = Vulkan::Xr::Get();
    const auto pad = xr ? xr->Input() : std::nullopt;
    if (!pad) {
        return 0;
    }
    *out = {pad->buttons, pad->lx, pad->ly, pad->rx, pad->ry, pad->l2, pad->r2};
    return 1;
}

extern "C" void bbgpu_vr_rumble(uint8_t large, uint8_t small) {
    if (Vulkan::Xr* xr = Vulkan::Xr::Get()) {
        xr->Rumble(large, small);
    }
}

extern "C" int bbgpu_vr_hands(BbVrHands* out) {
    Vulkan::Xr* xr = Vulkan::Xr::Get();
    const auto hands = xr ? xr->LatestHands() : std::nullopt;
    *out = {};
    if (!hands) {
        return 0;
    }
    const auto pose = [](const float* in, BbVrPose& to) {
        std::memcpy(to.position, in, sizeof(to.position));
        std::memcpy(to.orientation, in + 3, sizeof(to.orientation));
    };
    pose(hands->head, out->head);
    pose(hands->grip[0], out->grip[0]);
    pose(hands->grip[1], out->grip[1]);
    pose(hands->aim[0], out->aim[0]);
    pose(hands->aim[1], out->aim[1]);
    out->valid = hands->valid;
    out->time_ns = hands->time_ns;
    return 1;
}

extern "C" int bbgpu_vr_views(BbVrViews* out) {
    Vulkan::Xr* xr = Vulkan::Xr::Get();
    const auto views = xr ? xr->LatestViews() : std::nullopt;
    *out = {};
    if (!views) {
        return 0;
    }
    const auto pose = [](const Vulkan::Xr::Pose& in, BbVrPose& to) {
        std::memcpy(to.position, in.position, sizeof(to.position));
        std::memcpy(to.orientation, in.orientation, sizeof(to.orientation));
    };
    out->display_time_ns = views->display_time;
    out->flags = views->flags;
    pose(views->head, out->head);
    for (int i = 0; i < 2; ++i) {
        const auto& eye = views->eyes[i];
        pose(eye.pose, out->eye[i].pose);
        out->eye[i].fov_left = eye.fov_left;
        out->eye[i].fov_right = eye.fov_right;
        out->eye[i].fov_up = eye.fov_up;
        out->eye[i].fov_down = eye.fov_down;
        out->eye[i].width = eye.width;
        out->eye[i].height = eye.height;
    }
    pose(views->screen, out->screen);
    out->screen_flags = views->screen_flags;
    return 1;
}

extern "C" void bbgpu_vr_set_render_view(const BbVrRenderView* view) {
    Vulkan::Xr* xr = Vulkan::Xr::Get();
    if (!xr || !view) {
        return;
    }
    Vulkan::Xr::RenderView out{view->seq, view->display_time, {}, view->fov_left,
                               view->fov_right, view->fov_up, view->fov_down, {}};
    std::memcpy(out.head.position, view->head.position, sizeof(out.head.position));
    std::memcpy(out.head.orientation, view->head.orientation, sizeof(out.head.orientation));
    std::memcpy(out.rows, view->rows, sizeof(out.rows));
    xr->SetRenderView(out);
}

extern "C" void bbgpu_vr_set_eye_view(const BbVrEyeView* view) {
    Vulkan::XrStereo& stereo = Vulkan::XrStereo::Get();
    if (!view || !stereo.Enabled()) {
        return;
    }
    xrstereo::EyeView out;
    out.seq = view->seq;
    out.eye = view->eye ? 1 : 0;
    out.has_pose = view->has_pose != 0;
    out.display_time = view->display_time;
    std::memcpy(out.pose_position, view->pose.position, sizeof(out.pose_position));
    std::memcpy(out.pose_orientation, view->pose.orientation, sizeof(out.pose_orientation));
    out.fov[0] = view->fov_left;
    out.fov[1] = view->fov_right;
    out.fov[2] = view->fov_up;
    out.fov[3] = view->fov_down;
    std::memcpy(out.rows, view->rows, sizeof(out.rows));
    std::memcpy(out.position, view->position, sizeof(out.position));
    std::memcpy(out.center, view->center, sizeof(out.center));
    stereo.Add(out);
}

extern "C" int bbgpu_vr_stereo_status(BbVrStereoStatus* out) {
    Vulkan::XrStereo& stereo = Vulkan::XrStereo::Get();
    if (!out || !stereo.Enabled()) {
        return 0;
    }
    const auto status = stereo.GetStatus();
    out->assigned[0] = status.assigned[0];
    out->assigned[1] = status.assigned[1];
    out->last_eye = status.last_eye;
    out->run = status.run;
    return 1;
}

extern "C" void bbgpu_vr_ui_state(const void* state, uint32_t size) {
    if (Vulkan::Xr* xr = Vulkan::Xr::Get(); xr && state) {
        xr->SetGameState(state, size);
    }
}

extern "C" void bbgpu_vr_ui_event(uint32_t kind, int32_t a, int32_t b, int32_t c) {
    if (Vulkan::Xr* xr = Vulkan::Xr::Get()) {
        xr->GameEvent(kind, a, b, c);
    }
}

extern "C" uint32_t bbgpu_vr_ui_actions(void* out, uint32_t capacity) {
    Vulkan::Xr* xr = Vulkan::Xr::Get();
    return xr && out ? xr->TakeUiActions(out, capacity) : 0;
}

extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
