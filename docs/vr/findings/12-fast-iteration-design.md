# 12: Fast iteration loop for VR development on Windows (Pimax)

Track: make every kind of VR change testable in seconds where possible and unattended where not.
Four parts: (1) hot reload of `bbport_xr.dll` inside the running game, (2) a Windows-native automation
layer that replaces `tools/restart.sh` / `press.sh` / `soak.sh`, (3) sandbox upgrades, (4) an
iteration-recipes table.

Date: 2026-10-09. Branch `vr`, working tree of that day. The bridge, `vk_xr.*`, `bbgpu.*` and the new
`src/runtime_hooks.c` were being edited in parallel, so moving files are cited by function name, not
line. Design only: no code was changed, nothing was built or launched.

Legend (as in 05/07): **[V]** verified here (source read). **[R]** reported by an external source (URL
given). **[S]** speculation, inference, or an estimate still to be measured.

---

## 0. Summary

- **Split the bridge in two (recommended).**
  - A stable **host**, `bbport_xr.dll`: the XrInstance, session, spaces, actions, swapchains, frame
    thread, `AcquireFlat`/`ReleaseFlat` and the game ABI.
  - A reloadable **logic module**, `bbport_xr_logic.dll`: screen placement and recenter, the layer list
    (quad or cylinder, poses, sizes, curve), pad mapping, stick calibration, gestures, the laser and
    menu pointer, and later the Phase 2 head-camera math.
  - The logic has a pure C ABI (`FrameIn → FrameOut`). It owns no OpenXR or Vulkan handles and no
    threads, and its state lives in a block of host memory.
  - A reload is a pointer swap between `xrEndFrame` and the next `xrWaitFrame`. Nothing restarts, the
    presenter and the queue lock never notice, and the state is kept.
  - **The split needs no change in `vk_xr.cpp` or `vk_presenter.cpp`**, which others are editing right
    now.
- **Tiered loop** (all times are estimates **[S]**, see §8 for what to measure):

| Tier | What changes | Mechanism | Turnaround |
|---|---|---|---|
| 0 | numbers: screen distance, size and curve; deadzones; gesture speeds | `vr_tune.ini`, watched by the host and parsed by the logic | < 0.5 s |
| 1 | logic code | `bbctl xr build logic`, then auto reload in the game and the sandbox | 3–6 s |
| 2 | host code | restart the sandbox now; an in-game host reload later (the session is recreated, the game stays up and plays flat meanwhile) | 8–15 s |
| 3 | game side (vk_xr, presenter, runtime_pad, probe hooks, renderer) | rebuild, then `bbctl boot --vr` (golden save, scripted title, VR log assertions, auto capture) | 1.5–3 min, unattended |
| offline | input mapping and gesture regressions | record VR input once, replay it through the logic DLL (an exe, or Python ctypes) | 2–5 s, no headset |

- **Automation.** One stdlib-only Python CLI, `tools/bbctl.py`, with cmd and PowerShell shims. It:
  - wraps `run.py` and controls the game process the Windows way;
  - writes `BB_PAD_FILE` correctly for Windows, whose change detection is a whole-second mtime plus the
    file size;
  - waits on log lines and asserts VR lines;
  - captures frames;
  - boots to Hunter's Dream from a golden save in a separate user dir;
  - writes a self-contained HTML report.
- **Headset-free smoke tests.** Point the OpenXR loader at another runtime for one process
  (`XR_RUNTIME_JSON`, which is ignored if the game runs elevated). Details in §3.8.
  - **SteamVR's null driver, today.** It has a static head pose and no controllers. VISIBLE is enough for
    the cinema screen; FOCUSED is unverified.
  - **The Meta XR Simulator, optional.** It works with Vulkan and simulates controllers.
  - **A small scripted fake runtime of our own, for deterministic CI.** No open-source runtime can script
    poses and buttons on Windows.
  - **On the real Pimax:** set the sleep timeout to its maximum and tape the proximity sensor.
  - Monado on Windows is not ready.

---

## 1. The loop today, and where the time goes

What happens now **[V]**:
- **Loading.** `Xr::Get()` loads `bbport_xr.dll` once from next to `bb-probe.exe` and requires `abi ==
  BBPORT_XR_BRIDGE_ABI` (3 in the working tree). It keeps a `const BbXrApi*` into the DLL's static table
  forever. It also registers an `atexit` `Shutdown`, which the real exit paths skip (§3.2). Windows locks
  a loaded DLL, so even copying a new bridge into `out/` fails while the game runs.
- **Threads that call the bridge:**
  - the presenter: `AcquireFlat`, then `CopyToFlat` is recorded, then `scheduler.Flush`, then
    `ReleaseFlat` (`Presenter::Present`);
  - the game thread: `pad_read_state` → `bbgpu_vr_pad` → `GetInput`, inside the pad mutex;
  - `scePadSetVibration` → `Haptic`;
  - any thread: `bbgpu_vr_views` → `GetViews` (new in ABI 3);
  - the bridge's own `FrameLoop` thread.
- **Tuning is env vars read once.**
  - `BB_XR_HEADLOCK` and `BB_XR_CURVE` are read in `Init`.
  - `BB_XR_SCREEN_M`, `BB_XR_SCREEN_DIST`, `BB_XR_STAGE`, `BB_XR_HANDS`, `BB_XR_BG` and `BB_XR_MODE`
    are read in `StartSession`.
  - `BB_XR_GESTURES` is read in `XrInput::Create`.
  - The gesture thresholds are `constexpr` in `xr_input.cpp`.
  - Every change means a game restart.
- **Placement state is lost on restart.** The screen pose, `placed` flag and recenter timers are
  `FrameLoop()` locals, so a restart waits again for steady tracking before the screen is placed.
  `xrscreen::Placer` (`xr_screen_math.h`) is already pure and trivially copyable, which is a good sign
  for the split.
- **`run.py` re-prepares the image on every launch.**
  - It reruns `mods.py`, `prepare.py`, `link_libc.py`, `link_modules.py`, `content_profile.py` and
    `patches.py` (`run.py:156-205`).
  - It rewrites `out/eboot.elf`, `boot.bin`, `boot-libc.bin` and `boot-linked.bin`, 94–101 MB each. All
    four carry the same timestamp from today's launch.
- **Automation is bash only** (`pkill`, `setsid`, `stdbuf`, `taskset`, `grep`). Nothing on Windows
  navigates the title or asserts VR log lines.
- **The log can be tailed live.** `bb-probe`'s stdout is unbuffered (`setvbuf(stdout,NULL,_IONBF,0)` at
  the top of `main` in probe.c), so a redirected log file updates as the game runs. Every bridge message
  is a `VR: …` line through `BridgeLog`.

Problems found while reading that also slow iteration (fix them when the code is touched):
1. **Stale image views.** `MakeFlatOpaque` caches image views in a `static std::unordered_map<VkImage,
   vk::ImageView>` (`vk_presenter.cpp:595,653`).
   - Each `CreateFlat` destroys the old flat swapchain: on a window resize today, and on every session
     restart or host reload later.
   - The driver may hand out the same `VkImage` value again, and the cached view would then point at a
     destroyed image.
   - Fix: key the cache by a swapchain generation (§2.7) and destroy old views through
     `DeferOperation`.
2. **`CreateFlat` destroys the old chain while the GPU may still be running the previous frame's copy
   into it.** Idle the queue under the queue lock first. This is cheap, because it only happens on a
   resize.
3. **Losing the session kills VR until the game restarts.** On `EXITING`, `LOSS_PENDING` or instance
   loss the frame loop ends for good ("the headset stops showing the game until it restarts"). So a
   Pimax Play restart or a USB hiccup costs a full game restart. The recreate path in §2.9 fixes this.
4. **No exit path destroys the OpenXR session.** Closing the window ends in `std::_Exit(0)` and the
   in-game restart ends in `TerminateProcess`, so the next launch can race the runtime's cleanup of the
   dead client. `QuickExit` fixes this (§3.2).
5. **`xrBeginFrame` and the queue lock (already fixed in the working tree, keep it that way).**
   `xrBeginFrame` now takes the queue lock. The spec lists `xrBeginFrame`, `xrEndFrame`,
   `xrAcquireSwapchainImage` and `xrReleaseSwapchainImage` as the functions in which the runtime may
   use the VkQueue **[R]**
   (<https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable-concurrency.html>).

---

## 2. Hot reload of the VR bridge

### 2.1 Three ways to do it

| | A. Full teardown and reload of one DLL | B. One DLL, handles handed over | **C. Stable host + logic DLL** |
|---|---|---|---|
| A reload does | `Shutdown` (join the thread; destroy chains, actions, spaces, session, instance), load the new copy, then `Init` + `StartSession` | stop the thread, serialize every handle and all state, load the new copy, adopt them | swap a function table at a frame boundary; the state stays in host memory |
| The headset shows | the session restarts (IDLE → READY → … → FOCUSED), a gap of 1–3 s **[S]**, maybe the runtime's loading or home scene | 1–3 missed frames **[S]** | nothing (at most one late frame) |
| Runtime risk | re-creating the instance in one process: the loader allows only one XrInstance at a time and unloads the runtime DLL in `xrDestroyInstance` **[R]**, and runtimes vary (an Oculus-runtime crash on a second session is reported) **[R]** (§3.8, §8) | none new | none |
| What can change | everything | everything except actions and bindings (attached once per session) and the instance extension set | logic only, which is where most iteration happens |
| ABI that gets frozen | game↔bridge only | every internal handle and struct | the host↔logic C ABI (small, plain data) |
| Game-side work | a lock and reload in `vk_xr.cpp`; the presenter view-cache fix | same as A | none |

**Decision.**
- **C** for daily work (Tier 1).
- For host changes, restart the sandbox. Later, use **A** for an in-game host reload (Tier 2). It reuses
  the teardown and recreate code that the bridge needs anyway to survive `LOSS_PENDING` and `EXITING`.
- **B is rejected.** It would freeze every field of `Bridge` into an ABI for little gain over A plus C.

### 2.2 The split

```
 bb-probe.exe (game)                  bbport_xr.dll = host (stable)                  bbport_xr_logic.<gen>.dll (reloadable)
 ───────────────────                  ─────────────────────────────                  ──────────────────────────────────────
 presenter ─ AcquireFlat/ReleaseFlat ► flat Chain, queue lock
 pad thread ─ GetInput ──────────────► published pad      ◄────────────┐
 any thread ─ GetViews ──────────────► published views/screen ◄────────┤
 camera hook ─ CameraHook (Phase 2) ─► try-shared logic_lock ──────────┼────────► ComposeCamera (pure)
                                       frame thread:                    │
                                         swap? tune? command?           │
                                         xrWaitFrame / xrBeginFrame     │
                                         xrSyncActions, locate ──► FrameIn ─────► Frame(state, in, out)
                                         sanitize ◄──────────────────── FrameOut ◄──┘
                                         build layers, xrEndFrame
                                       state block, 64 KiB (the logic's State lives here)
```

**The host owns** (never reloaded in Tier 1):
- **The instance and system.** Enable every optional extension the logic might want, when the runtime
  offers it: cylinder today, depth and others later. A new extension needs a new instance.
- **The session and its state machine.**
- **All three spaces (LOCAL, VIEW, STAGE), created always.** Head lock and stage anchoring then become
  per-frame choices instead of startup settings.
- **One action set of raw actions** for every input of every suggested profile.
  `xrAttachSessionActionSets` is allowed once per session, so the mapping must not live in the
  bindings.
- **The swapchains:** flat, markers, background, laser.
- **Everything else stable:** the frame thread, `AcquireFlat`/`ReleaseFlat`,
  `GetInput`/`GetViews`/`Haptic`, the logic loader, the recorder and replayer, and the command file.

**The logic owns:**
- placement and recenter (`Placer`), and the follow mode;
- the layer list: which layers, their space, pose, size, quad or cylinder, and curve radius;
- the background color;
- the raw → PS4 pad mapping, stick peak rescaling, the d-pad chord and gestures;
- haptic feedback requests;
- the laser and menu mode (07 §5-6);
- tune parsing;
- the Phase 2 camera math.

### 2.3 The logic ABI (`gpu/xr_bridge/bbport_xr_logic.h`, MIT, C only)

```c
#define BBXR_LOGIC_ABI 1u
#define BBXR_STATE_BYTES (64u * 1024u)

enum { BBXR_SPACE_LOCAL, BBXR_SPACE_STAGE, BBXR_SPACE_VIEW };
enum { BBXR_LAYER_QUAD, BBXR_LAYER_CYLINDER, BBXR_LAYER_BACKGROUND, BBXR_LAYER_MONO };
enum { BBXR_IMAGE_FLAT, BBXR_IMAGE_MARKER_IDLE, BBXR_IMAGE_MARKER_HELD, BBXR_IMAGE_MARKER_FIRED,
       BBXR_IMAGE_BEAM_MISS, BBXR_IMAGE_BEAM_HIT, BBXR_IMAGE_DOT };
/* BbXrFrameIn.flags */
#define BBXR_IN_FOCUSED 0x01u        /* session FOCUSED */
#define BBXR_IN_SHOULD_RENDER 0x02u
#define BBXR_IN_SPACE_CHANGED 0x04u  /* a reference-space-change event arrived this frame */
#define BBXR_IN_REFOCUSED 0x08u      /* FOCUSED again: the headset was put back on */
#define BBXR_IN_FLAT_SHOWN 0x10u     /* the flat chain has a released image */
#define BBXR_IN_HAS_CYLINDER 0x20u
#define BBXR_IN_HAS_STAGE 0x40u

typedef struct BbXrRawHand {
    float trigger, squeeze;       /* 0..1 */
    float stick[2];               /* -1..1, y up: the vector2 binding, else the x/y axes (Pimax) */
    uint32_t buttons;             /* BBXR_RAW_A/B/X/Y/MENU/SYSTEM/STICK_CLICK/... */
    BbXrPose grip, aim;           /* LOCAL */
    float linear_velocity[3];     /* grip, m/s, LOCAL */
    uint32_t velocity_valid, profile; /* BBXR_PROFILE_TOUCH/INDEX/SIMPLE/OTHER/NONE */
} BbXrRawHand;

typedef struct BbXrFrameIn {
    uint32_t size, flags;
    uint64_t frame;                       /* host frame counter */
    int64_t display_time;                 /* XrTime, ns */
    double now, dt;                       /* host steady clock, s */
    BbXrPose head_local, head_stage;      /* the VIEW origin in LOCAL / STAGE */
    BbXrEye eye[2];                       /* as in BbXrViews */
    BbXrRawHand hand[2];                  /* 0 left, 1 right */
    uint32_t flat_width, flat_height;
    uint32_t tune_serial, tune_length;    /* serial changes when vr_tune.ini changed */
    const char* tune_text;                /* the whole file; valid during the call only */
    const char* command;                  /* one command for the logic, or NULL */
} BbXrFrameIn;

typedef struct BbXrLayer {
    uint32_t kind, image, space, flags;
    BbXrPose pose;
    float width, height;                  /* quad, m */
    float radius, central_angle, aspect;  /* cylinder */
} BbXrLayer;

typedef struct BbXrFrameOut {
    uint32_t layer_count;
    BbXrLayer layer[8];                   /* back to front */
    BbXrInput pad;                        /* what GetInput returns until the next frame */
    BbXrScreen screen;                    /* published through GetViews */
    BbXrPointer pointer;
    float haptic[2][2];                   /* per hand: amplitude, seconds; 0 = none */
    float background_rgb[3];              /* the host refills its two small chains on change */
    BbXrCameraShared camera;              /* Phase 2, §2.10 */
} BbXrFrameOut;

typedef struct BbXrLogicHost {
    uint32_t size;
    void (*log)(int level, const char* text);
    int (*write_tune)(const char* key, const char* value); /* keeps an in-headset adjustment */
} BbXrLogicHost;

typedef struct BbXrLogicApi {
    uint32_t abi, size;
    uint32_t frame_in_size, frame_out_size, state_size, state_layout;
    const char* build_id;                 /* __DATE__ " " __TIME__ of the unity TU (+ git hash) */
    /* The new code adopts the block: first load (magic 0), the same layout, or migrate the
     * persistent prefix and reset the rest. */
    void (*Attach)(void* state, uint32_t bytes, const BbXrLogicHost* host);
    /* Frame thread only, once per frame after xrBeginFrame. */
    void (*Frame)(void* state, const BbXrFrameIn* in, BbXrFrameOut* out);
    /* Phase 2, on the camera-hook thread: reads `shared`, owns `camera_state`. */
    void (*ComposeCamera)(const BbXrCameraShared* shared, void* camera_state, BbXrGameCamera* io);
} BbXrLogicApi;

__declspec(dllexport) const BbXrLogicApi* BbXrLogicGetApi(void);
```
`BbXrPose`, `BbXrFov`, `BbXrEye`, `BbXrInput`, `BbXrScreen`, `BbXrPointer` and the `BBXR_BTN_*` bits
are reused from ABI 3. Move them out of `bbport_xr_bridge.h` into a new `bbport_xr_types.h` that has no
`<vulkan/vulkan.h>`, and include it from both headers. That keeps the logic free of Vulkan headers and
leaves the game ABI unchanged. `BbXrCameraShared` and `BbXrGameCamera` are defined in §2.10.

**The state block.** The logic defines `struct State { u32 magic, layout, persist_bytes, reserved;
Persist persist; FrameState frame; CameraState camera; }`.
- `Persist` is append-only, so fields are never reordered and never change meaning.
- `FrameState` belongs to the frame thread. `CameraState` belongs to the camera-hook thread (§2.10).
- `static_assert(std::is_trivially_copyable_v<State> && sizeof(State) <= BBXR_STATE_BYTES)`.

**Rules for code in the logic module** (enforced by `static_assert`s and review):
- No OpenXR or Vulkan calls, no threads, no `thread_local`, and no mutable globals (constants are fine).
- No file IO. The tune text arrives in `FrameIn`, and persistence goes through `host->write_tune`.
- No heap allocation that outlives a call. No exceptions (`-fno-exceptions -fno-rtti`), and no
  `assert`/`abort`: the UCRT `abort` ends the process. Use `BBXR_CHECK(x)`, which calls
  `__builtin_trap()`, so the fault guard (§2.4) catches it.
- Link with `-static-libstdc++ -static-libgcc` like the host, and export only `BbXrLogicGetApi`.
- Build it as one unity TU (`logic/logic_unity.cpp`), with everything in the `bbxr_logic` namespace
  (anonymous namespaces collide in unity builds). It recompiles in about 1–2 s **[S]**.
- The same sources are compiled into the host as the **builtin logic** (`BBXR_LOGIC_BUILTIN`,
  `BbXrLogicGetBuiltinApi`, not exported). Release builds use only that, and there is no drift between
  dev and release.

### 2.4 Reload mechanics (inside the host)

**Turning it on.** Set `BB_XR_LOGIC=<path to bbport_xr_logic.dll>` (dev only). When it is unset the
host uses the builtin logic and starts no watcher, so release behavior is unchanged.

**The watcher thread** (low priority, started in `StartSession` when `BB_XR_LOGIC` is set):
1. **Poll every 250 ms** with `GetFileAttributesExW`, which gives a 100 ns mtime plus the size. The CRT
   `stat` has whole-second mtimes on Windows, which is why `runtime_pad.c` compares sizes too.
   - It also consumes a trigger file, `BB_XR_RELOAD_TRIGGER` (default `<dll>.reload`).
     `bbctl xr build` creates it after a successful link. This is the race-free path, and it works the
     same way as `BB_FRAME_DUMP_TRIGGER`: the file exists, so it is removed and acted on.
2. **Debounce.** Proceed when the file has been unchanged for 300 ms and `CreateFileW(dll,
   GENERIC_READ, FILE_SHARE_READ, …)` succeeds, which means the linker has let go of it.
3. **Shadow copy.** `CopyFileW` it to `out/xr_shadow/<pid>/bbport_xr_logic.<gen>.dll`.
   - Crash reports then name the generation: `describe_address` in probe.c prints module+offset.
   - Debug info: MSYS2 clang64 puts DWARF inside the DLL by default, so no PDB gets locked. If someone
     switches to `-gcodeview`, give every link a unique PDB name and copy the PDB with the DLL.
4. **Load and check.** `LoadLibraryExW(copy, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`, then `GetProcAddress("BbXrLogicGetApi")`, then check `abi`,
   `frame_in_size`, `frame_out_size` and `state_size`. On a mismatch it logs
   `VR: warning: logic reload rejected: …`, frees the copy and keeps the current logic.
5. **Dry run (on by default).** Copy the state block, then call the new `Attach` and `Frame` on the copy
   with the last `FrameIn`, under the fault guard, and sanitize the output. A build that crashes or emits
   NaN poses never goes live.
6. **Hand over.** Give the loaded module to the frame thread through an atomic `pending` pointer. All
   disk and loader work happens on the watcher, so Windows Defender scanning a freshly written DLL (tens
   to hundreds of ms **[S]**) never stalls a headset frame.

**The swap** runs on the frame thread, at the top of the loop before `HandleEvents`. At that point no
old logic code is on any stack:
```cpp
AcquireSRWLockExclusive(&logic_lock);          // only the Phase 2 camera hook takes it (shared, try)
std::memcpy(rollback_block, state, BBXR_STATE_BYTES);
pending->api->Attach(state, BBXR_STATE_BYTES, &host_services);
previous = active; active = pending; pending = nullptr; ++generation;
ReleaseSRWLockExclusive(&logic_lock);
Log(0, "logic gen %u loaded: %s (load %.1f ms, swap %.0f us)", generation, active->api->build_id, ...);
```
- **Old modules stay loaded** (the dev default, about 1–2 MB each **[S]**). Rollback is instant, and a
  stale function pointer runs old code instead of jumping into freed memory. `BB_XR_RELOAD_UNLOAD=1`
  frees every generation older than the previous one.

**The fault guard.**
- **Handler order.**
  - The host registers `AddVectoredExceptionHandler(0 /* last */, LogicFault)`.
  - probe.c's `main` registers its own handler first: `AddVectoredExceptionHandler(1, vectored_fault)`.
    GPU page-tracking faults are resolved there and never reach ours, so there is no hot-path cost.
  - A fault whose RIP is in a module other than `bb-probe.exe` and the guest image falls through to
    ours, because `vectored_fault`'s `ours` test is false for it.
- **Guarded calls.** Every logic call runs inside `GuardedCall`: a thread-local `Guard*` (trivially
  destructible) plus a jump buffer.
- **Recovery.** `LogicFault` acts on an access violation, an illegal instruction (including
  `__builtin_trap`), a divide by zero or a misalignment while a guard is active.
  - It records `module+offset`.
  - It resumes the thread in a longjmp stub outside the exception dispatcher. This is the technique
    probe.c already uses for speculative reads: Rcx = buffer, Rdx = 1, Rsp aligned, Rip = longjmp, then
    return `EXCEPTION_CONTINUE_EXECUTION` (the `runtime_fault_recover` branch of `vectored_fault`;
    `bb_setjmp`/`bb_longjmp` in `compat_win.c`). The bridge carries its own copy of the two asm
    routines.
- **Limits.** A stack overflow is not recovered. Unwinding is skipped, which is safe only because the
  logic holds no locks and owns no resources (the rules in §2.3).
- **A fault in `Frame`** rolls back: `active = previous`, and the log reads
  `VR: error: logic gen 7 faulted at bbport_xr_logic.7.dll+0x1a2b (access violation), rolled back to
  gen 6`.
  - **Which state comes back.** The host copies the block into `frame_snapshot` before every guarded
    `Frame` (64 KiB, a few µs per frame), because a fault can leave the block half-written.
  - If both generations share a `state_layout`, the host restores `frame_snapshot`, so nothing placed
    or learned since the swap is lost.
  - Otherwise it restores `rollback_block`, the copy taken at the swap in the old layout.
  - Three rollbacks within 60 s switch to the builtin logic and pause the watcher until the next
    trigger.
- **A fault in `ComposeCamera`** happens on the game thread. It only marks the fault; the camera passes
  through unmodified, and the frame thread does the rollback.

**The output sanitizer** runs on the host every frame, for any logic including the builtin one.
- **Poses.** Quaternions must be finite with |q|² in [0.98, 1.02], and are then normalized. Positions
  must be finite and under 100 m.
- **Layers.** Quad sides must be 0.01–50 m, cylinder radius 0.1–100 m, and the angle in (0, 2π). Image
  ids must be valid and already released. At most 8 layers.
- **Input.** Sticks and triggers must be in range; haptics at most 1.0 amplitude and 1 s.
- **What happens to failures.**
  - Invalid layers are dropped and replaced by the last good screen layer.
  - 30 bad frames in a row count as a fault.
  - Without the sanitizer, the runtime rejects the whole frame (`XR_ERROR_POSE_INVALID`), and after 3
    rejections today's `DropOptionalLayer` switches the cylinder or background off for good. A logic bug
    must not be able to do that.

**A stats line, for assertions and the report.** Every 10 s the host logs
`VR: stats 90.0 Hz, 0 late, flat 59.8 fps, logic 0.04 ms (max 0.21), gen 3`:
- Hz comes from the `xrWaitFrame` return intervals, measured against `predictedDisplayPeriod`.
- "late" counts intervals longer than 1.5 periods.
- flat fps counts `ReleaseFlat` calls.
- logic time is the duration of `Frame()`.

### 2.5 Threads, locks, the presenter and the queue

- **Lock order.** `[Tier 2 only: the vk_xr api_lock, an SRW lock]` → the bridge's `state_mutex` →
  `Scheduler::submit_mutex` (the queue-lock callback).
  - `g_views_mutex`, `logic_lock` and the published-pad mutex are leaves.
  - `submit_mutex` is a plain `std::mutex` (`vk_scheduler.cpp:27`), and the scheduler holds it for the
    whole `SubmitExecution`. Never call into the bridge while holding it: `QueueLock` would deadlock on
    itself.
- **A Tier 1 swap does not touch the presenter's path at all.**
  - `AcquireFlat`/`ReleaseFlat`, the flat chain, `MakeFlatOpaque`'s view cache and the queue lock are
    host-only, and a swap changes none of them.
  - The swap point sits between `xrEndFrame` and `xrWaitFrame`. Each submitted frame's layers therefore
    come from exactly one generation, and the flat chain's last released image keeps showing.
- **The logic never touches the GPU,** so it has nothing to do with `submit_mutex`.
- **Game threads never run logic code**, except the Phase 2 `ComposeCamera`.
  - `GetInput`, `GetViews` and `Haptic` read or write host caches.
  - `ComposeCamera` takes `logic_lock` shared with `TryAcquireSRWLockShared`. If a swap is in progress,
    which lasts microseconds, that one call uses the host's fixed composition instead of blocking the
    game.

### 2.6 State that must survive

| State | Owner | Logic reload | Host reload (Tier 2) | Game restart |
|---|---|---|---|---|
| Screen pose and space, `placed`, the `Placer` | logic `Persist` | kept | kept (the block lives in vk_xr.cpp memory, §2.9) | `vr_state.ini`, STAGE coordinates only |
| Recenter yaw and height offsets, follow mode | logic `Persist` | kept | kept | `vr_state.ini` |
| Stick peaks per hand and axis (Dream Air y ≈ 0.35), stick source (vector2 or x/y) | logic `Persist` | kept | kept | `vr_state.ini`, so the peaks are not learned again |
| Head calibration (Phase 2: seated origin, yaw offset, height, world scale) | logic `Persist` | kept | kept | `vr_state.ini` |
| Gesture arming, pulse timers, d-pad chord, repeaters, laser hover | logic `FrameState` | kept if the layout is the same, else reset (the pad goes neutral for one frame) | reset | reset |
| Camera filters (Phase 2) | logic `CameraState` | kept | kept | reset |
| Tunables | `vr_tune.ini` | read again | read again | the file |
| The saved camera for inject/restore, the pose↔frame tags (08 §7.4) | host or runtime, **never the logic** | not affected | host samples again | not affected |
| Session, spaces, actions, chains, interaction profile | host | not touched | recreated | recreated |

**Pimax caveat** **[V]** (from comments in the bridge): Pimax's LOCAL forward is not the user's forward,
and its first poses can be junk. A screen pose saved in LOCAL can be wrong in a new session, so save it
in STAGE when the runtime has STAGE, and otherwise place the screen again.

### 2.7 ABI versioning

- **Game↔host** keeps `BBPORT_XR_BRIDGE_ABI` with an exact-match check. Make the next bump (ABI 4)
  extensible:
  - Add `uint32_t size` right after `abi`. From then on only append; the game checks `size` before
    calling a newer entry.
  - New entries:
    - `AcquireFlat2(…, uint32_t* generation)`, for the presenter's view cache;
    - `Command(const char*)`;
    - `GetStatus(BbXrStatus*)`, which reports logic generation and build id, session state, Hz, late
      frames and recording state;
    - `CameraHook(uint32_t hook, BbXrGameCamera*)`;
    - `QuickExit(void)`, a bounded session teardown called before `_Exit` and `runtime_restart` (§3.2).
  - `BbXrSessionInfo` gains `size`, `persist` and `persist_bytes`.
- **Host↔logic:**
  - `BBXR_LOGIC_ABI` must match exactly, and so must the struct sizes and `state_layout`. A mismatch is
    rejected (the current logic stays); it never crashes.
  - `state_layout` is bumped by hand when `State` changes incompatibly. `Persist` is append-only, so even
    a layout bump keeps the persistent prefix, and `persist_bytes` says how much of it is valid.
  - `build_id` is logged at every load and reported by `GetStatus`, so automation can assert which build
    is live.

### 2.8 When something fails, keep playing

| Failure | Detected by | Action | The player sees |
|---|---|---|---|
| Logic DLL missing, wrong ABI, wrong sizes | loader checks | keep the current logic (builtin at start) | nothing |
| A new build crashes in the dry run | fault guard | reject it | nothing (one log line) |
| The live logic faults | fault guard | roll back to the previous generation and the state snapshot; 3 times → builtin | one repeated frame |
| The logic emits bad poses or sizes | sanitizer | drop the layer, show the last good screen; 30 frames → rollback | nothing, or the last good screen |
| A host reload fails (Tier 2) | vk_xr | `api = null`, flat game, retry on the next trigger | the runtime's idle scene in the headset; the game keeps running flat |
| Session lost or exiting, instance lost | events and XrResults | tear down, retry every 2 s (`xrGetSystem` fails while the headset is off) | flat until the headset is back |
| Headset asleep (not VISIBLE) | session state | `AcquireFlat` returns 0, as today | flat |

### 2.9 Tier 2: reloading the host inside the running game (later, needs game-side changes)

**Game side** (`vk_xr.cpp`; coordinate with the engineers editing it now):
- **Locking.** Keep `std::atomic<const BbXrApi*> api` plus an `SRWLOCK api_lock`.
  - Calls from game threads use `TryAcquireSRWLockShared` and never block on a reload: `GetInput`
    returns "no VR input" (the pad falls back to gamepad or keyboard), `Haptic` is skipped, and the
    camera passes through.
  - The presenter takes the lock shared in `AcquireFlat` and keeps it until `ReleaseFlat`, after
    `scheduler.Flush`. If `AcquireFlat` returns 0 it releases at once.
- **The reload thread** waits on the exclusive lock, so it waits for at most one present.
- **Where the host comes from.** Load it from a shadow copy of `BB_XR_HOST` (default:
  `bbport_xr.dll` next to the exe). Point that at `out\xr-bridge\bbport_xr.dll`, so a fresh build is
  never blocked by the copy the game has loaded. A reload trigger, `<host>.reload`, works as in §2.4.
- **Pinning.** Pin `libopenxr_loader.dll` with `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN)`, so
  freeing the old host never unloads the loader.
- **Persistent memory.** The 64 KiB logic state block is allocated here and passed in
  `BbXrSessionInfo.persist`, so it survives host reloads.

**Old host, `Shutdown(RELOAD)`:**
1. **Stop the frame thread.** It calls `xrRequestExitSession`, keeps pumping events and ending frames
   until `STOPPING` (at most 500 ms), calls `xrEndSession`, and returns. The frame thread does this
   itself because it is the one already pumping events.
2. **Join it** with a 2 s timeout. If it does not exit, abort the reload and keep the old host loaded:
   code that a thread is still running must never be freed.
3. Take the queue lock, then `vkQueueWaitIdle` (images idle), then release the lock.
4. Destroy the chains, spaces and action set, then `xrDestroySession`.
5. Destroy any `XR_EXT_debug_utils` messenger: its callback lives in the host DLL.
6. **The instance:** keep it if the new host asks for the same instance-extension set (hand over
   `XrInstance` and `XrSystemId` as two opaque `u64` values), otherwise `xrDestroyInstance`. Adopting
   avoids the riskiest step, re-creating an instance in the same process. The Khronos loader refuses a
   second simultaneous XrInstance (`XR_ERROR_LIMIT_REACHED`) and unloads the runtime library in
   `xrDestroyInstance`, so a re-create also reloads the runtime DLL **[R]**
   (<https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/loader/loader_core.cpp>,
   `runtime_interface.cpp`). Enabling every useful extension up front (§2.2) makes adoption the normal
   case.

**New host:**
1. Shadow-copy and load it, then `GetApi` and check the ABI.
2. `Init` (adopt or create the instance).
3. Check that `PhysicalDevice` is the game's device and that the required Vulkan instance and device
   extensions are a subset of what the game enabled at startup. If not, refuse with "VR: the new host
   needs … — restart the game".
4. `StartSession` (with `persist`), then release the exclusive lock.
5. The flat chain is recreated on the next `AcquireFlat2`, and its new `generation` tells the presenter
   to flush its view cache.

**Recovery from session loss** uses the same code inside the host: `LOSS_PENDING`, `EXITING`,
`XR_ERROR_SESSION_LOST` or `XR_ERROR_INSTANCE_LOST` trigger a teardown, then a retry every 2 s.
- **Session loss** recreates the session on the same instance.
- **Instance loss** (`XrEventDataInstanceLossPending`) means destroying the instance and retrying
  `xrCreateInstance` in a loop. The runtime answers `XR_ERROR_RUNTIME_UNAVAILABLE` while it is
  restarting **[R]**
  (<https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEventDataInstanceLossPending.html>).
  Pimax Play restarts land here.
- The game plays flat throughout. The Vulkan device is the game's own and is not affected.

### 2.10 Phase 2: forwarding camera hooks into the reloadable logic

What exists now **[V]** (work in progress in the tree):
- `src/runtime_hooks.c` installs trampolines into a fixed registry of host functions with the signature
  `ABI void fn(BbHookContext*)` (all GPRs, XMM0-15, flags). The names are still changing:
  `bb_hook_log_once`, `bb_hook_camera_inject` (H1), `bb_hook_camera_inject_fmtb` (H2),
  `bb_hook_camera_restore`. The camera ones are no-ops so far.
- **The patches.** `patches/Bloodborne.xml` holds `Type="hook"` lines in "VR Camera Hook H1 (draw
  PersCam)" and "VR Camera Hook H2 (draw main context)". They are disabled by default and compiled only
  when named in patches.py `--hooks`.
  - `run.py` does not pass `--hooks` yet. Add `BB_HOOKS` → `--hooks`, and include it in the
    `BB_REUSE_PREPARED` stamp.
- **Loading.** probe.c's `apply_patches` reads the hook records from `BBPATCH3`: offset, length, flags
  (replay-before) and the expected original bytes. Each install logs
  `Hook: <name> at image offset 0x… (N bytes, replay-before|after)`.
- 08 §7 places H1 at 0x22de113 (rbx = PersCam\*, after the private copy) and H2 at 0x22de386.
- **Hook sites are fixed for a run.** They are installed once at load from `patches.bin`, so moving a
  site means a game restart (Tier 3). Only what the hook *does* can be made live.
- The registry is fixed by design ("third-party XML cannot introduce new host code"), so the reloadable
  part has to sit behind it:

```
guest H1 site ─► trampoline ─► hook_camera_inject(ctx)                 bb-probe.exe, fixed (runtime_hooks.c)
                                 copy PersCam +0x10..+0x5F into BbVrGameCamera io  (guest memory stays exe-side)
                                 bbgpu_vr_camera(BBVR_HOOK_H1, &io)                 gpu/bbgpu.h, C
                                   └► Xr::CameraHook ─► api->CameraHook(hook, &io)  vk_xr.cpp → host
                                         host: pick the views for this frame, tag them (08 §7.4)
                                         TryAcquireSRWLockShared(logic_lock)
                                           ok         → GuardedCall(ComposeCamera(&shared, camera_state, &io))
                                           busy/fault → fixed M' = M_game · T(head) · R(head), FOV from tune
                                 write io back: +0x10..+0x4F matrix, +0x50 FOV, +0x54 aspect
```

- **`BbVrGameCamera`** is plain data: `{ float matrix[16]; float fov_y, aspect, near_z, far_z; uint32_t
  hook; uint64_t frame_tag; }`. The logic never dereferences a guest pointer.
- **`BbXrCameraShared`** is published by `Frame` through a two-slot buffer with an atomic index. It holds:
  - the head pose relative to the calibrated origin;
  - the eye poses and FOVs;
  - the tunables: `head.position_scale`, `head.yaw = full|decoupled`, `camera.fov = hmd|game|<deg>`,
    `camera.height_offset_m`, `head.smoothing_hz`, `world.scale`.
- **`camera_state`** is the region of the state block owned by the hook thread (its filters). It is
  separate from the frame thread's region, so the swap lock is the only lock needed.
- **Live tuning.** While standing in Hunter's Dream:
  - edit `vr_tune.ini` (Tier 0) or the math itself (Tier 1);
  - `BB_XR_CAMERA_LOG=1` prints one line per second (game matrix, head pose, result) for assertions;
  - the command `camera identity` turns composition off at once, for when the math makes you sick.
- **Stereo inputs for Phase 3/4** (IPD scale, convergence) take the same route: the logic writes them
  into `FrameOut`, and the host publishes them next to `GetViews`. The renderer never calls logic code.

### 2.11 Files and build targets

| File | New or changed | Contents |
|---|---|---|
| `gpu/xr_bridge/bbport_xr_logic.h` | new | the host↔logic ABI (§2.3) |
| `gpu/xr_bridge/bbport_xr_types.h` | new | the plain-data types shared by both ABIs, moved out of `bbport_xr_bridge.h` (no Vulkan include) |
| `gpu/xr_bridge/logic/logic_unity.cpp`, `placement.cpp`, `layers.cpp`, `pad_map.cpp`, `gestures.cpp`, `pointer.cpp`, `tune.cpp`, `camera.cpp` | new | the logic, moved out of `FrameLoop()` (placement, layer building) and out of `xr_input.cpp` (mapping, gestures) |
| `gpu/xr_bridge/xr_logic_host.cpp/.h` | new | loader, watcher, shadow copy, swap, rollback, fault guard, sanitizer, state block |
| `gpu/xr_bridge/xr_guard_win.S` | new | the setjmp/longjmp pair for the guard (as `bb_setjmp`/`bb_longjmp`, MIT copy) |
| `gpu/xr_bridge/xr_input.cpp/.h` | changed | raw actions only (`XrInput::Sync(time) → BbXrRawHand[2]`) and haptics |
| `gpu/xr_bridge/bbport_xr_bridge.cpp` | changed | `FrameLoop`: FrameIn → logic → sanitizer → layers; polling of the tune and command files; recorder; all three spaces always created |
| `gpu/xr_bridge/xr_record.cpp/.h` | new | `.bbxrrec` writer and reader (§4.4) |
| `gpu/xr_bridge/xr_replay.cpp` | new | `bbport_xr_replay.exe`, the offline logic runner |
| `gpu/xr_bridge/xr_sandbox.cpp` | changed | §4 |
| `gpu/xr_bridge/fake_runtime/` | new (M5) | §3.8 |
| `tools/bbctl.py`, `tools/bbctl/*.py`, `tools/bbctl.cmd`, `tools/bbctl.ps1` | new | §3 |
| `tests/test_xr_logic.py` | new | ctypes tests of the logic DLL (`skipUnless` it is built, like `test_content.py`) |
| `run.py` | small change | `BB_REUSE_PREPARED` (§3.7); `BB_HOOKS` → patches.py `--hooks` (§2.10) |
| `vk_xr.cpp/.h`, `bbgpu.h`, `gpu/shim/bbgpu.cpp` | changed (M2 `QuickExit`; Tier 2 and Phase 2) | `QuickExit` before the window thread's `_Exit`; api lock; `CameraHook`, `bbgpu_vr_camera` |
| `vk_presenter.cpp` | changed (M6) | `AcquireFlat2` generation keys `MakeFlatOpaque`'s view cache |
| `src/probe.c` | changed (M2) | `runtime_restart` calls the VR `QuickExit` before `CreateProcess` |
| `src/runtime_hooks.c` | changed (Phase 2) | `hook_camera_inject` → `bbgpu_vr_camera` |

```cmake
set(BBXR_LOGIC logic/logic_unity.cpp)                 # #includes logic/*.cpp
add_library(bbport_xr_logic SHARED ${BBXR_LOGIC})
set_target_properties(bbport_xr_logic PROPERTIES PREFIX "")
target_compile_options(bbport_xr_logic PRIVATE -fno-exceptions -fno-rtti)
target_link_options(bbport_xr_logic PRIVATE -static-libstdc++ -static-libgcc)
target_sources(bbport_xr PRIVATE ${BBXR_LOGIC} xr_logic_host.cpp xr_guard_win.S xr_record.cpp)
target_compile_definitions(bbport_xr PRIVATE BBXR_LOGIC_BUILTIN=1)   # builtin fallback, same sources
add_executable(bbport_xr_replay xr_replay.cpp xr_record.cpp ${BBXR_LOGIC})
```

The bridge builds with native tools: `C:/msys64/clang64/bin/ninja.exe` and `clang++` (from
`out/xr-bridge/CMakeCache.txt`) **[V]**. So `bbctl` only puts `C:\msys64\clang64\bin` first on PATH and
runs `ninja -C out/xr-bridge bbport_xr_logic`. No MSYS2 bash is needed for bridge work.

---

## 3. Windows automation layer

### 3.1 Shape

One stdlib-only Python CLI: `tools/bbctl.py` plus a `tools/bbctl/` package (`proc.py`, `log.py`,
`pad.py`, `capture.py`, `vr.py`, `runtimes.py`, `report.py`).
- It uses `subprocess`, `ctypes`, `re`, `json`, `zlib` and `struct` only, and runs from PowerShell, cmd
  and Git Bash.
- Shim: `tools\bbctl.cmd` = `@py -3 "%~dp0bbctl.py" %*`, plus a `bbctl.ps1` that does the same.
- Exit code 0 means pass, 1 means fail, so scripts can chain it.
- The Linux scripts stay as they are.

| Command | What it does | Linux counterpart |
|---|---|---|
| `bbctl up [--vr] [--save NAME] [--runtime R] [--patches "A;B"] [--hooks "A;B"] [--cpus LIST] [-- run.py args]` | stops any running game, resets `out/pad` and `out/toggles`, starts `run.py` detached with the session env (§3.2), logs to `out/session.log`, writes `out/session.json` (pid, start time, env) | first half of restart.sh |
| `bbctl down` | graceful close, then kill | `pkill` |
| `bbctl press TOKENS [--hold 0.3]`, `bbctl seq FILE` | scripted pad (§3.3) | press.sh |
| `bbctl wait REGEX [--timeout 60] [--since MARK]` | waits for a log line; fails on forbidden lines or if the process dies | `grep` loops |
| `bbctl boot [--vr] [--to dream]` | `up`, title navigation, waits until in the level, capture (§3.5) | restart.sh |
| `bbctl capture [LABEL] [--present] [--analyze]` | frame dump → BMP/PNG, copied to `out/vr_frames/` (§3.6) | trigger files by hand |
| `bbctl soak SECONDS [--vr]` | boot, turn the camera now and then, check the process is alive, run VR checks, capture periodically | soak.sh |
| `bbctl vr check [--profile boot\|reload\|stable\|input]` | runs the VR assertion profiles against the session log (§3.4) | none |
| `bbctl vr smoke [--runtime active\|steamvr-null\|fake\|metasim]` | headset-free run, HTML report (§3.8) | none |
| `bbctl xr build [logic\|host\|all]` | `ninja -C out/xr-bridge <target>` with clang64 on PATH; creates the reload trigger on success | part of `scripts/vr_sandbox.sh` |
| `bbctl xr tune KEY=VALUE…` | edits `vr_tune.ini` in place, keeping comments | none |
| `bbctl xr replay REC [--logic DLL] [--golden]` | runs `bbport_xr_replay.exe` and diffs the result against `REC.golden.jsonl` (§4.4); `--bless` writes a new golden | none |
| `bbctl xr cmd recenter\|reload\|rollback\|status\|record start F\|record stop\|camera identity` | writes the command file (§3.4) | none |
| `bbctl sandbox [--folder DIR] [--watch] [--record F] [--replay F]` | builds if stale, copies the exe, the DLLs and the loader into `out\sandbox\<n>\` (§4.1), runs the sandbox there | `scripts/vr_sandbox.sh` |
| `bbctl report` | HTML report of the last session | none |
| `bbctl calibrate` | records draws/frame at the title and in the dream into `tools/bbctl/areas.json` | none |

**The command file**, so the running game can take commands without any IPC (the repo's
trigger-file style):
- The host polls `BB_XR_COMMAND_FILE` every 250 ms.
- `bbctl` writes `xr.cmd.tmp` and renames it to `xr.cmd`.
- The host renames that to `xr.cmd.taken` (an atomic consume), runs each line, and logs
  `VR: command <line>: ok|<error>`.
- Lines the logic understands are passed on in `FrameIn.command`.

### 3.2 Process control on Windows

**Stopping.**
- `taskkill /IM bb-probe.exe` without `/F` posts `WM_CLOSE`. SDL turns it into
  `SDL_EVENT_WINDOW_CLOSE_REQUESTED`/`QUIT`, which sets `is_open = false` (`gpu/shim/window.cpp:137-139`).
  The window thread then logs "Window closed by user", flushes stdout and calls **`std::_Exit(0)`**
  (`bbgpu_init`'s window thread in `gpu/shim/bbgpu.cpp`) **[V]**.
  - `_Exit` skips `atexit`, so vk_xr's `Shutdown` never runs.
  - The in-game "Apply and restart" (`runtime_restart`) ends with `TerminateProcess` too.
  - So today **no exit path destroys the OpenXR session**; the runtime has to notice the dead client.
- **Fix (small, game side, M2):** add `QuickExit` to the bridge ABI and call it before `_Exit` and before
  `runtime_restart`'s `CreateProcess`. It does `xrRequestExitSession`, waits at most 300 ms for
  `STOPPING`, then `xrEndSession` + `xrDestroySession` + `xrDestroyInstance`. Every wait is bounded, and
  the presenter is kept out by `state_mutex`.
- **`bbctl down`:** `WM_CLOSE`, wait up to 8 s, then `taskkill /F /T /IM bb-probe.exe`. Before
  relaunching in VR, wait about 3 s **[S]** (until `QuickExit` exists, always) so the runtime has
  released the old session.

**Starting, detached so it outlives the shell:**
```python
log = open(OUT / 'session.log', 'wb')
proc = subprocess.Popen([sys.executable, str(ROOT / 'run.py'), *args], cwd=ROOT, env=env,
                        stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS)
```
- `run.py` sets only stdin when it starts `bb-probe.exe` (`run.py:224`). CPython then passes its own
  stdout handle to the child, so the log file receives both.
- Without a console, `run.py`'s `no_console()` adds `CREATE_NO_WINDOW` (`run.py:85-87`). That hides only
  a console; the SDL window is unaffected.

**Session env** (restart.sh parity, Windows paths):
- Always: `BB_PAD_FILE=out\pad`, `BB_TOGGLE_FILE=out\toggles`, `BB_FRAME_STATS=1`, `BB_FPS_LIMIT=0`.
- Frame dumps and captures:
  - `BB_FRAME_DUMP_TRIGGER=out\triggers\frame` with `BB_DUMP_DIR=out\dump\<session>`;
  - `BB_PRESENT_DUMP_TRIGGER=out\triggers\present`;
  - `BB_CAPTURE_TRIGGER=out\triggers\capture` with `BB_CAPTURE_DIR=out\captures\<session>`.
- On request: `BB_PAD_RECORD` and `BB_PAD_REPLAY`.
- With `--vr`: `BB_VR=1`, `BB_XR_LOGIC=out\xr-bridge\bbport_xr_logic.dll`, `BB_XR_TUNE=out\vr_tune.ini`,
  `BB_XR_COMMAND_FILE=out\xr.cmd`, `XR_LOADER_DEBUG=warn` (loader messages go to stderr, so into the
  log), and `XR_RUNTIME_JSON` for `--runtime`.

**CPU pinning** (taskset parity, `--cpus 0-7`): once `bb-probe.exe` appears, call
`SetProcessAffinityMask` through ctypes.

**Do not run elevated.** The loader reads `XR_RUNTIME_JSON` and `XR_API_LAYER_PATH` through
`PlatformUtilsGetSecureEnv`, which returns nothing above medium integrity. An elevated game would
silently get the registry runtime (Pimax) instead of the null or fake one. `XR_LOADER_DEBUG` and
`XR_ENABLE_API_LAYERS` still work **[R]**
(<https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/common/platform_utils.hpp>). `bbctl`
refuses to start when it is itself elevated.

### 3.3 The scripted pad on Windows

- **Equal-size writes can be missed.** `read_inject` (runtime_pad.c) looks at the file at most every
  20 ms. On Windows it compares `st_mtime`, which has whole-second resolution, and the size. Two
  different token sets of the same length written within one second look like one write.
  - `bbctl` pads each write with 1–3 spaces so that its size always differs from the previous write.
    Tokens are whitespace-separated, so the padding is harmless.
- **Hold at least 100 ms:** 2 game frames at 30 FPS plus the 20 ms poll. The default is 300 ms, like
  press.sh. A release is an empty file, size 0, which always differs.
- **Write directly.** A plain `open(…, 'w')` is fine, because the CRT's `fopen` shares read and write.
  Avoid `os.replace`: it fails while the game has the file open.
- **`BB_TOGGLE_FILE` works differently.** `toggle_watcher` (runtime_memory.c) re-reads the content every
  250 ms with no mtime check, so equal sizes are no problem. But a read in the middle of a truncating
  write sees mask 0 for one poll. Write the toggles with a temp file + `os.replace`, and retry for up to
  100 ms if the game has the file open.
- **`bbctl seq FILE`** lines look like `press cross 0.3`, `wait 6`, `stick rx=60 2`,
  `wait-log "^Frame stats" 30`, `capture title`.

### 3.4 Waiting on logs and asserting VR lines

```python
class LogTail:                       # tools/bbctl/log.py
    def __init__(self, path, pid=None): ...
    def mark(self) -> int: ...       # current end offset
    def wait(self, pattern, timeout=60, since=None, forbid=DEFAULT_FORBID) -> re.Match: ...
    def lines(self, since=None) -> list[str]: ...
DEFAULT_FORBID = [r'^Fault:', r'^STOP: first unsupported', r'^ERROR:', r'^VR: error:']
```
- It polls every 100 ms, reads new bytes, splits them into lines and keeps any partial line.
- The game prints no timestamps, so the tail records each line's arrival time in
  `out/session.timeline.jsonl` for the report.
- It fails fast if the process died: `OpenProcess`, then `GetExitCodeProcess`.

**VR assertion profiles**, using the exact strings in the working tree **[V]**:

| Profile | Expected, in order | Forbidden |
|---|---|---|
| `boot` | `^VR: runtime (.+) \d+\.\d+\.\d+` (the runtime name is recorded) · `^VR: headset (.+), max swapchain` · `^VR: session started \((world\|head-locked) screen, (cinema\|mono) mode\)` · `^VR: session state 5` (FOCUSED) within 30 s · `^VR: flat screen \d+x\d+ format` · `^VR: first game frame on the headset screen` · `^VR: screen placed [\d.]+ m ahead` within 5 s of FOCUSED (the `Placer` falls back after 3 s) · `^VR: (left\|right) controller: /interaction_profiles/` (skip with `--no-controllers`) | `^VR: error:` · `no usable headset` · `is from another version` · `rejects the (curved screen\|mono projection layer\|background layer)` · `session (exiting\|lost)` · `OpenXR instance lost` |
| `reload` | after `xr build`: `^VR: logic gen (\d+) loaded` within 10 s, with a fresh build id | `rolled back`, `logic reload rejected` |
| `stable` | a new host stats line every 10 s, `^VR: stats ([\d.]+) Hz, (\d+) late, flat ([\d.]+) fps`: Hz ≥ 0.98 × display rate, late ≤ 1 % (the Phase 1 exit is "90 Hz without dropped compositor frames") | same as `boot` |
| `input` | pad edges `^VR: pad \+(\w+)` in the scripted order (a new debug line, tune key `debug.log_pad=1`) | none |
| `hooks` (Phase 2, with `bbctl up --hooks "VR Camera Hook H1 (draw PersCam)"`) | `^Hook: (bb_hook_\w+) at image offset 0x[0-9a-f]+ \(\d+ bytes, replay-(before\|after)\)` once per selected hook · `^Patches: .*, (\d+) hooks` with the expected count · the first-fire line of `bb_hook_log_once` · later `^VR: camera` lines under `BB_XR_CAMERA_LOG=1` | `^ERROR:` (for example "hook original bytes do not match the eboot") |

**Runs with nobody wearing a headset** use a variant of `boot` (§3.8):
- **Pimax asleep:** states 1–3 are accepted, and everything from `first game frame` on is skipped,
  because the flat screen only runs while VISIBLE.
- **SteamVR null:** state 4 or 5 is required, plus the first frame and the placement. The controller
  lines are skipped.
- **Fake runtime:** everything, plus its scripted checks.

### 3.5 Boot to Hunter's Dream

**A golden save, so every boot starts from the same spot.**
- Keep a save that stands in Hunter's Dream under `tools/testdata/saves/<name>/`. It is local and
  gitignored, because it is the user's own data.
- `bbctl up --save dream` copies it into `out\test-user\savedata\…`, using the layout
  `savedata/1/CUSA00207/SPRJ0005/userdata0000…` plus `SPRJ0005.sce_sys/` **[V `user/`]**, and sets
  `BB_USER_DIR=out\test-user`.
- `BB_GPU_USER_DIR` keeps pointing at the real `user\`, so the shader and pipeline caches stay warm.
  `run.py` only `setdefault`s it (`run.py:213`).
- The real saves are never touched.

**Steps.** "Skip Intro" is enabled by default (`patches/Bloodborne.xml:13`), so the title comes up
quickly.
1. Wait for `pad opened for user 1` (`pad_open` in runtime_pad.c) and one `^Frame stats` line (they
   come every 5 s).
2. Wait `TITLE_WAIT` (6 s, as restart.sh does), `press cross`, wait 6 s, `press cross`. With a save
   present, Continue is the default item.
3. **In the level** when two `Frame stats` lines in a row show draws/frame above the area's threshold.
   - The `Frame stats` line format is `… %.0f draws/frame …` (`driver.cpp:439`).
   - restart.sh uses > 600 for its level. Hunter's Dream's value is unknown, so `bbctl calibrate`
     records it once.
4. Wait 3 s, `capture dream`, run the `boot` profile with `--vr`, and write the report.

**A stronger "where am I" signal** (optional, small change): `BB_FILE_TRACE=<prefix>` in
`runtime_file.c`'s open path prints `Files: open <guest path>` for matching paths such as `map/`. `bbctl`
then waits for the dream's map folder. Read the folder name from the trace once; `m21_00_00_00` is a
guess **[S]**.

**Useful test patches** through `BB_PATCHES` (patches.py `--extra`):
- "Player No Dead (Read note)" and "Player Stealth (Read note)", for soaks outside the dream;
- "Disable Camera Auto Rotation via Movement", for Phase 2 head-tracking tests.

### 3.6 Frame capture and the report

- **`bbctl capture LABEL`** creates `BB_FRAME_DUMP_TRIGGER`.
  1. The presenter removes the trigger in the next `PrepareFrame` and queues raw dumps
     (`vk_presenter.cpp:324-366,437-471`): `display_NNN_WxH.raw` when the upscaler is not used, and
     `output_NNN_WxH.raw` always. They are written once the GPU is done.
  2. `bbctl` waits until the trigger is gone. More than 5 s means the game is not presenting.
  3. Then it waits until the sizes of the new `.raw` files are stable.
  4. It converts them with `scripts/vr_frames.py`'s `to_bmp` (imported), plus a 20-line zlib PNG writer
     for the report, and copies the BMPs into `out/vr_frames/` for the sandbox (§4.2).
- **`--present`** uses `BB_PRESENT_DUMP_TRIGGER` (and `BB_PRESENT_DUMP_COUNT=N`): the upscaler's finished
  display buffer, native UI included (`vk_temporal_upscaler.cpp:1901`).
- **`--analyze`** uses `BB_CAPTURE_TRIGGER`, then waits for
  `^Frame capture: (\d+) passes written to (.+)` (`vk_frame_capture.cpp`).
- **Not capturable: the compositor's output in the headset.** OpenXR has no read-back.
  - Under the fake runtime, check layer poses numerically and dump its swapchain images (§3.8).
  - Under Pimax or SteamVR, grab their mirror window if needed (`PrintWindow` through ctypes).
- **The report** goes to `out/reports/<session>.html`. It is self-contained and works on a phone and in
  dark mode. It has:
  - the timeline;
  - the assertion table;
  - the captures as inline PNGs;
  - the last `Frame stats`/`Frame pacing` lines;
  - runtime and headset strings;
  - host and logic build ids.

### 3.7 Faster relaunch

- **Skip unchanged preparation.** Add `BB_REUSE_PREPARED=1` to `run.py` (`bbctl` sets it). It writes a
  stamp, `out/prepared.json`, with the inputs:
  - `eboot.bin` size and mtime, and the six scripts' mtimes;
  - the mods config hash and the mods dir listing;
  - `BB_PATCHES`, `BB_HOOKS`, `BB_FPS`, render and output resolution, and the settings file mtime.
  When the stamp matches, the first five steps (mods through content_profile) are skipped. `patches.py`
  still runs: it is cheap and depends on settings. Expected saving: several seconds per launch **[S]**.
- **Never rebuild `bb-probe` for bridge-only work.** Today a new host DLL can only be copied into `out/`
  while the game is stopped. Tier 2 shadow copies remove that limit, and the logic never had it.

### 3.8 Smoke tests without anyone wearing the headset

**The problem.** With nobody wearing the Pimax, the headset sleeps. The session never becomes VISIBLE,
`AcquireFlat` returns 0, and "first game frame on the headset screen" never appears. The game itself
keeps running flat, so a smoke test needs either a runtime that does not sleep or a way to keep the
headset awake.

**Switching the runtime for one process.** `bbctl` sets `XR_RUNTIME_JSON=<manifest>` in the game's
environment only.
- The loader then ignores `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`, so the system's runtime
  (Pimax Play, manifest reported at `C:\Program Files\Pimax\Runtime\PiOpenXR_64.json`) stays untouched
  **[R]** (loader spec §5.1.5 "Overriding the Default Runtime Usage",
  <https://registry.khronos.org/OpenXR/specs/1.1/loader.html>).
- A manifest is `{"file_format_version": "1.0.0", "runtime": {"library_path": …}}`. A relative
  `library_path` resolves from the manifest's folder **[R]**.
- `XR_LOADER_DEBUG=all` prints which runtime the loader chose.

| Option | Selected with | Works without anyone wearing it | Input | Cost | Use it for |
|---|---|---|---|---|---|
| **Real Pimax, kept awake** | nothing (Pimax Play is the system runtime) | partly: the headset must be connected and awake. Pimax documents a screen-off after 5 min without movement **[R]**; a "Screen Timeout"/"Headset Sleep" setting in Pimax Play is reported to go up to "Never" **[R, unverified]**; tape over the proximity sensor is a common trick **[R]** but may not stop the movement timer **[S]** | real controllers; `BB_PAD_FILE` for the game | minutes | desk checks on the real compositor: cylinder, Pimax's alpha blending, timing |
| **SteamVR null driver** | `XR_RUNTIME_JSON=C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json` **[R]**, plus `driver_null` in `steamvr.vrsettings` (below) | yes, with a static head pose and no tracking **[R]**. Whether its OpenXR session reaches FOCUSED is unverified: SteamVR ties focus to the dashboard and the proximity sensor **[R]**. VISIBLE is enough for the cinema screen, because `AcquireFlat` only needs `shouldRender` **[V]** | none: no controllers **[R]** (OpenVR-InputEmulator can script virtual controllers, but it is work in progress **[R]**) | half a day | a smoke test on a real compositor, nobody wearing anything |
| **Meta XR Simulator** | standalone Windows install (v83+; 207.0 as of Sep 2026). Its own toggle switches the system-wide runtime, which would take Pimax Play out **[R]**. Use `XR_RUNTIME_JSON` at its `meta_openxr_simulator.json` instead: the loader honors it **[R]**, but the install path is undocumented, so find it once | yes (a desktop window); a headless mode is announced for 207.0 **[R]** | keyboard, mouse or Xbox pad as Quest controllers; Session Capture record/replay (`.vrs`), unattended through `persistent_data.json` **[R]**, but 207.0 lists a recording bug **[R]** | half to one day; Meta SDK license, so the binaries stay out of the repo **[R]** | interactive controller tests without a headset |
| **Monado** | build from source (vcpkg, VS2022, Vulkan SDK). The Windows build is "not ready for widespread usage" **[R]**. `QWERTY_ENABLE=1` gives a keyboard/mouse HMD and controllers; `XR_MND_headless` has no compositor, so nothing renders **[R]** | yes | keyboard and mouse | days, fragile | not recommended on Windows today |
| **bbport fake runtime** (below) | `XR_RUNTIME_JSON=out\xr-bridge\bbxr_fake_runtime.json` | yes, no window | scripted timeline or `.bbxrrec` replay | 3–5 days | deterministic CI, recovery paths, layer math, queue-lock checks |
| `XR_EXT_conformance_automation`, or an API layer that injects input | the runtime must offer the extension (conformance only, sometimes "developer mode") **[R]**; or `XR_API_LAYER_PATH` + `XR_ENABLE_API_LAYERS` with an MIT template (mittorn/OpenXR-API-Layer-Template, mbucchia/OpenXR-Layer-Template) **[R]** | no: it runs on a real runtime | injected | 1–2 days | not needed: the host's own `BB_XR_REPLAY` (§4.4) does the same inside our code |

**SteamVR null driver setup** (`bbctl vr smoke --runtime steamvr-null`). The keys are
community-reported **[R]**
(<https://www.vrwiki.cs.brown.edu/hardware/vr-hardware/hardware-emulators/null-driver-tutorial>,
<https://github.com/username223/SteamVRNoHeadset>). `bbctl` backs the file up, merges the keys in,
and restores it afterwards: `forcedDriver` would otherwise also hijack the user's normal SteamVR use.
```jsonc
// <Steam>\config\steamvr.vrsettings, merged into the existing JSON (a syntax error makes SteamVR
// ignore the file; see vrserver.txt)
"steamvr":     { "forcedDriver": "null", "activateMultipleDrivers": true, "requireHmd": false },
"driver_null": { "enable": true, "id": "Null Driver", "serialNumber": "Null 4711", "modelNumber": "Null Model",
                 "windowX": 0, "windowY": 0, "windowWidth": 1344, "windowHeight": 1512,
                 "renderWidth": 1344, "renderHeight": 1512, "displayFrequency": 90 }
```
- **Starting SteamVR.** `start steam://rungameid/250820`, or `SteamVR\bin\win64\vrstartup.exe`
  (neither is documented **[R]**). Then poll for `vrmonitor.exe`/`vrserver.exe`.
- **Stopping.** There is no documented command, so kill `vrmonitor.exe` and `vrserver.exe` after the run.
- **The no-wearer `boot` variant** expects:
  - session state 4 (VISIBLE) or 5;
  - `first game frame on the headset screen`;
  - `screen placed …` (the static pose is valid).
  It skips the controller lines.

No ready-made open-source runtime scripts poses and buttons on Windows **[R]**:
- Khronos's `src/tests/test_runtimes/runtime_test.cpp` is a stub. Its actions are inactive and
  `xrCreateSwapchain` returns `XR_ERROR_FUNCTION_UNSUPPORTED`.
- Chromium's OpenXR test helper uses D3D11 and is not standalone.

So the fake runtime below is worth writing. Start from `runtime_test.cpp` and its `.def` exports for the
loader plumbing.

**Recommendation.**
1. Now: the real Pimax kept awake for desk work, plus `steamvr-null` for a real compositor with nobody
   wearing the headset.
2. M5: the fake runtime for CI and anything scripted.
3. Optional: the Meta XR Simulator, when simulated controllers are needed interactively.
4. Skip Monado on Windows.

**Pimax session states when the headset sleeps are undocumented** **[R: not found]**.
- The spec allows `STOPPING` when a user-engagement sensor reports disengagement, and `READY` may wait
  for engagement **[R]**. The bridge already handles both (`xrEndSession` and `xrBeginSession`).
- Measure it once (§8) and encode the observed states in the `boot` profile's no-wearer variant.

**The bbport fake runtime** (`gpu/xr_bridge/fake_runtime/`, MIT, milestone M5): the deterministic CI
option.
- **Files.** `bbxr_fake_runtime.dll` plus a manifest:
  `{"file_format_version": "1.0.0", "runtime": {"name": "bbport fake runtime", "library_path":
  "bbxr_fake_runtime.dll"}}`.
- **Entry point.** It exports `xrNegotiateLoaderRuntimeInterface`, which fills `XrNegotiateRuntimeRequest`
  with interface version 1, `XR_CURRENT_API_VERSION` and its own `xrGetInstanceProcAddr`.
- **Only the about 50 functions the bridge calls:**
  - instance and system, view configurations;
  - `XR_KHR_vulkan_enable`: empty extension lists, and the graphics device picked from the app's
    VkInstance;
  - session, the three reference spaces, `xrLocateSpace`/`xrLocateViews` with canted Pimax-like views;
  - `xrWaitFrame` at a steady 90 Hz, `xrBeginFrame`/`xrEndFrame`;
  - swapchains: real `VkImage`s on the app's device;
  - actions and paths, `oculus/touch_controller` as the current profile, haptics (logged);
  - `XR_KHR_composition_layer_cylinder`.
- **Scripted input** from `BBXR_FAKE_SCRIPT`, a timeline:
  - `t 0 head 0 1.6 0 yaw 0`
  - `t 2 press right a 0.2`
  - `t 3 stick left 0 1 0.5`
  - `t 4 trigger right 1 0.2`
  - `t 5 recenter` (emits a reference-space-change event)
  - `t 8 dump flat`
  - `t 20 state loss` (tests §2.9's recovery)
  It can also replay a `.bbxrrec` (`BBXR_FAKE_REPLAY`), so input recorded on the real Pimax drives a
  headset-free run.
- **Strict checks**, logged as `FAKEXR: violation: …` (forbidden in every profile):
  - layer quaternions not normalized;
  - a layer naming a swapchain that has never released an image;
  - release without a successful wait;
  - `xrEndFrame` without `xrBeginFrame`;
  - **two threads inside the four queue-using functions at once**: the app forgot its queue lock.
- **Its own Vulkan work** happens only inside those four functions, as the spec requires. The first
  layout transition of new images is done lazily in `xrAcquireSwapchainImage`, and image dumps in
  `xrEndFrame`.
- **Output.**
  - `BBXR_FAKE_LOG=out\fakexr.jsonl` gets one line per frame: layer types, spaces, poses, sizes,
    cylinder radius and angle, and swapchain image indices.
  - A `dump` writes the flat chain's last released image as raw + PNG. Checks: alpha = 255 everywhere
    (`MakeFlatOpaque` ran), the size equals the window, and the image is not black.
  - `bbctl vr smoke` then asserts numerically that the screen layer sits `distance_m` ahead along the
    scripted head yaw, that the cylinder radius and angle match the tune file, and that the markers sit
    at the scripted hand poses.
- **Cost.** About 1.5–2.5 k lines and 3–5 days **[S]**.
- **What it cannot catch:** runtime quirks such as Pimax blending layers by alpha whatever their flags
  say, real timing and reprojection, or the real controllers.

**`bbctl vr smoke` flow** (any runtime):
1. Select the runtime (env above), and start SteamVR first for `steamvr-null`.
2. `bbctl boot --vr --save dream`, plus `--hooks …` once Phase 2 hooks exist.
3. Assert the `boot` profile. For the fake runtime also assert the script's expected `VR:` lines and the
   layer checks.
4. Capture game frames and the fake runtime's flat dump.
5. Optionally run `soak 120`.
6. Write the HTML report and exit 0 or 1.

A plain run on the real Pimax with the sensor taped is the same command with `--runtime active`.

---

## 4. Sandbox upgrades

### 4.1 The same host and logic as the game

The sandbox already aims its laser at the screen the bridge reports through `GetViews`, using
`xr_screen_math.h` **[V]**.
- With `BB_XR_LOGIC`, the tune file, the command file and the recorder it becomes the default target of
  the Tier 1 loop.
- It needs no game and starts in about 1 s **[S]**.
- Both processes watch the same logic DLL. Each loads its own shadow copy, so both reload together.
- **`bbctl sandbox` runs from a shadow folder.** It copies the sandbox exe, `bbport_xr.dll` and
  `libopenxr_loader.dll` into `out\sandbox\<n>\` and runs them there. Rebuilding the host while a sandbox
  or the game is running then never hits a locked file. (`scripts/vr_sandbox.sh` copies into `out/`,
  which fails while the game has `out\bbport_xr.dll` loaded.)

### 4.2 Folder playback

- **`--folder DIR`** (default `out/vr_frames`, which already holds title, intro movie, Yharnam
  cutscene, three Hunter's Dream shots and the weapon menu **[V]**).
  - Loads `*.bmp` and raw dumps (`NAME_INDEX_WxH.raw`, BGRA unless `--rgba`, alpha forced to 255, the
    same rules as vr_frames.py), sorted by name.
  - Shows each image at its native size; `AcquireFlat` recreates the chain when the size changes.
- **Navigation.**
  - Left X/Y (square and triangle in the pad mapping) or ←/→ in the sandbox console (non-blocking
    `ReadConsoleInput`) step back and forward.
  - `--cycle SECONDS` advances on its own.
  - `--watch` jumps to new files as they arrive, so a `bbctl capture` from a flat game shows in the
    headset within about 1 s.
  - Running the game flat while the sandbox holds the headset works; two VR sessions at once usually
    don't **[S]**.
- **Caption** (B toggles it): file name, index, and the screen's distance, width and curve, burned into a
  corner so that screenshots document themselves.
- **Use:** judge menu legibility and pointer accuracy on real game screens (07) without booting the game
  in VR.

### 4.3 Live tunables: `vr_tune.ini`

The same file serves the game and the sandbox. The host polls it every 250 ms; the logic parses it.
Every applied change is logged (`VR: tune screen.distance_m 2.6 -> 2.2`), and unknown keys are warned
about once.
```ini
[screen]
distance_m = 2.6        ; BB_XR_SCREEN_DIST is the default
width_m = 3.2           ; BB_XR_SCREEN_M
curve = 1               ; 0 quad, 1 cylinder (needs XR_KHR_composition_layer_cylinder; else quad, logged once)
curve_radius_m = 0      ; 0 = distance (viewer on the axis); otherwise flatter or rounder
height_offset_m = 0
anchor = local          ; local | stage | head   (was BB_XR_STAGE / BB_XR_HEADLOCK; now live)
follow = fixed          ; fixed | lazy (re-place when the head turns more than lazy_deg)
lazy_deg = 35
background = 0.02       ; r,g,b or one grey (was BB_XR_BG); the host refills its two chains
mode = cinema           ; cinema | mono (was BB_XR_MODE)
[input]
stick_deadzone = 0.12
stick_rescale = 1       ; per-axis peak rescale (Pimax Dream Air)
dpad_chord = 1          ; L3 held + right stick flick = d-pad
[gestures]
enabled = 1             ; was BB_XR_GESTURES
swing_mps = 2.0
heavy_mps = 3.5
shoot_mps = 1.8
rearm_mps = 1.0
mouth_radius_m = 0.15
mouth_dwell_ms = 300
pulse_ms = 100
[hands]
markers = 1             ; was BB_XR_HANDS
marker_size_m = 0.05
laser = 0               ; was BB_XR_LASER
[menu]
repeat_first_ms = 350
repeat_ms = 120
[debug]
log_pad = 0
[sandbox]               ; read by the sandbox itself
stick_threshold = 70
cycle_s = 0
caption = 1
```
- **Old env vars keep working.** They are the defaults, and the file overrides them.
- **Live with the split, but not live today:**
  - the anchor (all three spaces exist, so the logic picks one per frame);
  - curve on/off (the extension is enabled whenever it is offered);
  - the background color (the host refills when `background_rgb` changes);
  - mono or cinema (`BB_XR_MODE`; `BBXR_LAYER_MONO` is just another layer the logic can choose).
- **Never live:**
  - a new OpenXR extension, which needs a new instance;
  - action bindings, which need a new session.
- **In-headset adjust mode.** Enter it with `bbctl xr cmd adjust`, or with a chord (default: both stick
  clicks held for 1 s; `[debug] adjust_chord`).
  - While it is active the game gets a neutral pad, so the chord leaks at most one lock-on toggle.
  - The right stick changes distance (up/down) and width (left/right).
  - A toggles the curve.
  - B saves through `host->write_tune`, which edits the file and keeps its comments.

### 4.4 Recording and replay

- **Format `.bbxrrec`**, written by the host in the game or the sandbox.
  - A header: magic `BBXRREC1`, `BBXR_LOGIC_ABI`, `sizeof(BbXrFrameIn)`, host build id, runtime and
    headset names.
  - Then records `{u32 type, u32 bytes, payload}`:
    - `FRAME`: a `BbXrFrameIn` with its pointers cleared;
    - `TUNE`: the full text whenever its serial changes;
    - `LOGIC`: generation and build id;
    - `COMMAND`;
    - `MARK`: a bookmark set by a chord.
  - About 90 Hz × 400 B ≈ 36 KB/s **[S]**.
- **Starting and stopping:**
  - `BB_XR_RECORD=<file>` records the whole session.
  - The command file (`record start|stop`) records part of it; this is the way to do it in the game.
  - In the sandbox a chord also works: both grips + left stick click for 1 s, confirmed by a haptic tick.
    In the game that chord would reach it as L1/R2/L3.
- **Offline:** `bbport_xr_replay.exe rec [--logic dll] [--tune ini] [--jsonl out]`.
  - It feeds the `FRAME` records to the builtin logic or to a DLL, and writes per-frame outputs: pad
    edges, gestures, layer poses and placement events. These are diffed against a golden JSONL.
  - `tests/test_xr_logic.py` runs the replays and synthetic cases through ctypes. It checks the ctypes
    struct sizes against `api.frame_in_size`, so the Python mirror cannot drift silently. Examples:
    - a head yaw of 30° gives a screen 2.6 m ahead at 30° once the `Placer` is satisfied;
    - a 2.5 m/s right-hand swing gives R1, and with the grip held it gives R2.
- **Live:** `BB_XR_REPLAY=<file>` makes the host feed the recorded hands and buttons to the logic.
  - The head stays live unless `BB_XR_REPLAY_HEAD=1`.
  - This repeats a gesture or menu sequence in the sandbox or the game. Under the fake runtime it drives
    the whole session.
- **The game side already does half of this.** F9 records the merged pad (`BB_PAD_RECORD`), VR included,
  because `sample()` in runtime_pad.c calls `sample_vr` before `record_sample`. `BB_PAD_REPLAY` plays it
  back. A VR play session therefore becomes a gameplay repro that needs no headset.
- **Video:** if a visual record of the headset view is wanted, record the runtime's mirror window with
  OBS. That is out of scope here.

---

## 5. Iteration recipes

A typical session once M1–M3 exist:
```bat
bbctl xr build all
bbctl boot --vr --save dream            :: unattended: ends standing in Hunter's Dream, report written
:: headset on, game running; from here nothing restarts:
bbctl xr tune screen.distance_m=2.2     :: Tier 0, "VR: tune screen.distance_m 2.6 -> 2.2"
:: edit gpu\xr_bridge\logic\placement.cpp, then
bbctl xr build logic                    :: Tier 1, "VR: logic gen 2 loaded: …"
bbctl xr cmd recenter
bbctl capture placement_check
:: before committing, headset off:
bbctl vr smoke --runtime fake && py -3 -m unittest tests.test_xr_logic
```

Times are estimates **[S]** until the measurements in §8 replace them.

| Change type | Fastest loop | Command(s) | Turnaround | Headset |
|---|---|---|---|---|
| Screen distance, width, curve, height, anchor, background, marker size, gesture speeds, deadzones, repeat timings | Tier 0, in the running game or sandbox | `bbctl xr tune screen.distance_m=2.2`, edit `out\vr_tune.ini`, or the in-headset adjust mode | < 0.5 s | yes, to judge it |
| Placement and recenter algorithm, follow modes, layer layout, laser and menu mode (07), pad mapping, stick calibration, gesture code | Tier 1 in the sandbox (synthetic menu or captured frames), then in the running game | `bbctl xr build logic` (auto reload); `bbctl vr check --profile reload` | 3–6 s | yes |
| Gesture and mapping regressions | offline replay of recorded sessions | `bbctl xr replay rec\swing.bbxrrec --golden`; `py -3 -m unittest tests.test_xr_logic` | 2–5 s | no |
| Menu legibility and pointer accuracy on real screens | sandbox folder playback + tune file; new screens from a flat game | `bbctl sandbox --folder out\vr_frames --watch`; `bbctl capture <label>` | seconds (+10 s for a new capture) | to view |
| Phase 2 head-camera math (composition, FOV, yaw decoupling, smoothing, scale) | Tier 1 + Tier 0 with the game standing in Hunter's Dream | `bbctl boot --vr` once, then `bbctl xr build logic` / `bbctl xr tune head.position_scale=…` | 3–6 s / < 0.5 s | yes |
| Host: swapchains, layer plumbing, session lifecycle, recovery, raw action set | sandbox restart now; Tier 2 in the game later | `bbctl xr build host && bbctl sandbox` | 8–15 s | yes |
| New OpenXR extension or binding change | host + new instance/session | sandbox restart; game restart (Tier 3) for the final check | 8–15 s; ~2 min | yes |
| `vk_xr.cpp`, presenter copy and opaque pass, `runtime_pad.c`, `bbgpu.*` | Tier 3, unattended | `bash build.sh` (MSYS2), then `bbctl boot --vr --to dream` | ~1.5–2 min | for the final look |
| Phase 2 hook plumbing (`runtime_hooks.c`, probe patch records, PersCam offsets) | Tier 3 + scripted head poses under the fake runtime + captures | `bbctl vr smoke --runtime fake --script head_yaw.txt` | ~2 min | no |
| Renderer stereo and shaders (Phase 3/4) | Tier 3 + frame dumps; live A/B with `BB_TOGGLE_FILE` bits (add VR bits) | `bbctl capture`; write `out\toggles` | 2–4 min per code change; < 1 s per toggle | for the final look |
| Before a commit | headset-free smoke | `bbctl vr smoke --runtime fake` (or `steamvr-null`) | 1–2 min | no |
| Stability | long soak with VR checks and periodic captures | `bbctl soak 1800 --vr` | unattended | no (null driver or fake) or taped sensor |

---

## 6. Implementation order

| Milestone | Contents | Effort [S] | Touches files others are editing? |
|---|---|---|---|
| M1 | host/logic split, builtin logic, tune file, Tier 1 reload, fault guard, sanitizer, command file, stats line | 2–3 days | bridge directory only |
| M2 | `bbctl` core: up/down/press/seq/wait/capture/boot/soak/vr check/report; golden save; `BB_REUSE_PREPARED`; `QuickExit` before `_Exit` and `runtime_restart` | 1–2 days | `run.py` (small); `QuickExit` touches `bbgpu.cpp`, `probe.c` and `vk_xr.cpp` (a few lines each, coordinate) |
| M3 | sandbox folder playback and adjust mode, recorder, `bbport_xr_replay.exe`, `tests/test_xr_logic.py` | 1–2 days | bridge directory only |
| M4 | `bbctl vr smoke --runtime steamvr-null` profile and docs | 0.5 day | none |
| M5 | fake runtime: scripted poses and buttons, strict validation, layer log, image dumps | 3–5 days | none (new dir) |
| M6 | Tier 2 host reload, session-loss recovery, `AcquireFlat2` generation + presenter view-cache fix | 2–3 days | **yes**: vk_xr, presenter, bbgpu |
| M7 | Phase 2 forwarding: `bbgpu_vr_camera`, `CameraHook`, `ComposeCamera`, fallback composition | 1–2 days, after H1/H2 land | **yes**: runtime_hooks.c, bbgpu |

M1–M3 give most of the speed-up and stay inside `gpu/xr_bridge/` and `tools/`.

---

## 7. Pitfalls checklist

1. **A loaded DLL is locked.** Always load a shadow copy, and never build into the file that is loaded.
2. **The linker can race the copy.** lld writes to a temp file and renames it over the output **[S]**.
   If our `CopyFileW` holds the old file at that moment, the link fails ("permission denied"); rebuild.
   The debounce and the trigger file make this rare.
3. **Defender scans each new DLL.** Load on the watcher thread, never on the frame thread.
4. **DWARF vs PDB.** The default DWARF has no lock issue. With `-gcodeview`, every link needs a unique
   PDB name.
5. **Reloadable code must not leave anything behind.** No threads, no `thread_local` with destructors,
   and no callbacks registered with the runtime or the loader. An `XR_EXT_debug_utils` messenger must be
   destroyed before its host DLL is freed.
6. **Static libc++ exists once per module.** Never free memory across modules; the ABIs are
   plain-data only.
7. **Unity builds merge anonymous namespaces.** Use a named namespace.
8. **`assert`/`abort` in the logic kill the process.** Use `BBXR_CHECK` → `__builtin_trap()`.
9. **Exception handler order.** Ours is registered last and probe.c's first, so ours only sees what
   probe.c passes on. Ours checks a thread-local guard and nothing else, so it costs nothing on the GPU
   page-fault path.
10. **Action sets attach once per session.** Raw actions belong in the host. Changing a binding means a
    new session.
11. **LOCAL can change between sessions** (and Pimax's first poses can be junk). Save poses in STAGE.
12. **The four queue functions** (`xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage`,
    `xrReleaseSwapchainImage`) and every `vkQueue*` call need the queue lock. Never call into the bridge
    while holding `submit_mutex`.
13. **The presenter's view cache is keyed by VkImage** (§1, problem 1). Add the swapchain generation
    before any host reload ships.
14. **Destroying the flat chain needs an idle queue** (§1, problem 2).
15. **The pad file is compared by whole-second mtime plus size on Windows.** Pad each write so its size
    differs from the previous one. The toggle file is re-read by content, so replace it atomically
    instead (§3.3).
16. **An elevated process ignores `XR_RUNTIME_JSON` and `XR_API_LAYER_PATH`** (§3.2) and silently gets
    the registry runtime. Never run the game as administrator for runtime overrides, and check the
    `VR: runtime …` line.
17. **No exit path destroys the OpenXR session today.** Closing the window ends in `std::_Exit(0)`, the
    in-game restart in `TerminateProcess`, and `taskkill /F` likewise. Add `QuickExit` (§3.2), and until
    then wait about 3 s before relaunching in VR.
18. **When Pimax sleeps, the session is not VISIBLE.** `AcquireFlat` then returns 0 and "first game frame
    on the headset screen" never appears. That is expected, not a failure, when nobody wears the
    headset.
19. **`DropOptionalLayer` is permanent.** Sanitize the logic's output before `xrEndFrame`, so a logic bug
    cannot switch the cylinder or background off for the rest of the session.
20. **Two OpenXR sessions at once are usually refused** **[S]**. Use a flat game together with the VR
    sandbox for captures.

---

## 8. To measure in the first week (and replace the [S] numbers)

1. **Build and reload timing:** logic DLL build time; shadow copy + `LoadLibrary` time with Defender on;
   swap time; first-frame-after-swap jitter.
2. **Launch timing:** `run.py` preparation time with and without `BB_REUSE_PREPARED`; boot to title;
   boot to the dream with the golden save.
3. **Pimax Play session timing:** session create → FOCUSED, and the `Placer`'s placement time.
4. **Tier 2 viability on Pimax Play and SteamVR:**
   - `xrDestroySession` + `xrCreateSession` with the same instance in one process;
   - `xrDestroyInstance` + `xrCreateInstance` in one process;
   - this decides between the instance-adopting and instance-recreating paths in §2.9.
5. **Null-driver session states:** which session states the SteamVR null driver reaches with our Vulkan
   session (VISIBLE is enough, FOCUSED is unverified), and whether `XR_KHR_composition_layer_cylinder`
   is offered there. A 2021 community list marks the cylinder unsupported on SteamVR **[R]**, so expect
   the quad fallback.
6. **Pimax sleep states:** which states Pimax Play reports when the screen times out or the proximity
   sensor trips, and whether taping the sensor plus a "Never" timeout keeps a session VISIBLE on the desk.
7. **Hunter's Dream thresholds:** draws/frame there, and the map folder name from `BB_FILE_TRACE`.
8. **The Pimax controller profile:** the `VR: left controller: …` line. Pimax does not document it
   **[R]**, and the bindings only cover touch, index and simple.

## Cross-links

- **07-menu-pointer.md §5-6:** the laser and menu mode that move into the logic module, and the
  captured menu screens the sandbox plays back.
- **09-stereo-renderer-design.md:** the Phase 3/4 renderer work. Its milestones verify through
  `BB_CAPTURE_TRIGGER` captures and live toggles, which `bbctl capture --analyze` and the Tier 3 row of
  §5 drive.
- **08-phase0-eboot-re.md §7:** the H1/H2 hook sites, the PersCam layout, and the pose bookkeeping that
  stays in the host.
- **PLAN.md Phase 1 exit** ("90 Hz without dropped compositor frames") is the `stable` assertion
  profile. **Phase 2** is what §2.10 makes tunable live.
