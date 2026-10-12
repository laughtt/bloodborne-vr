// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_dlss.dll: the only part of the project that uses the NVIDIA DLSS (NGX) SDK. Built with
// MSVC (the SDK's libraries are MSVC only); the port loads it at run time.
// Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR (dlss_bridge).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "bbport_dlss_bridge.h" // Vulkan first: the NGX headers expect its types

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

namespace {

constexpr char ProjectId[] = "42359704-c9f3-4806-9fb5-d469000fdb8a";
constexpr char EngineVersion[] = "bbport-windows";

struct State {
    BbDlssLogFn log{};
    std::wstring dll_directory;
    std::wstring data_directory;
    const wchar_t* dll_path{};
    NVSDK_NGX_FeatureCommonInfo common{};
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    NVSDK_NGX_Parameter* capabilities{};
    // One feature per slot (0 the flat game's, 1/2 the VR eyes): each has its own history.
    struct Feature {
        NVSDK_NGX_Parameter* parameters{};
        NVSDK_NGX_Handle* handle{};
        BbDlssFeature desc{};
    };
    std::array<Feature, BBPORT_DLSS_BRIDGE_SLOTS> features{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    PFN_vkGetInstanceProcAddr get_instance_proc{};
    PFN_vkGetDeviceProcAddr get_device_proc{};
    bool configured{};
    bool initialized{};
} state;

template <typename... Args>
void Log(int warning, const char* format, Args... args) {
    if (!state.log)
        return;
    std::array<char, 1024> text{};
    std::snprintf(text.data(), text.size(), format, args...);
    state.log(warning, text.data());
}

bool Check(const char* operation, NVSDK_NGX_Result result) {
    if (NVSDK_NGX_FAILED(result)) {
        Log(1, "%s failed: 0x%08x", operation, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    Log(0, "NGX: %s", message ? message : "");
}

int32_t Configure(const wchar_t* dll_directory, const wchar_t* data_directory, BbDlssLogFn log) {
    state.log = log;
    state.dll_directory = dll_directory ? dll_directory : L".";
    state.data_directory = data_directory ? data_directory : L".";
    state.dll_path = state.dll_directory.c_str();
    state.common.PathListInfo = {&state.dll_path, 1};
    state.common.LoggingInfo = {NgxLog, NVSDK_NGX_LOGGING_LEVEL_OFF, false};
    state.discovery.SDKVersion = NVSDK_NGX_Version_API;
    state.discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    state.discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    state.discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                EngineVersion};
    state.discovery.ApplicationDataPath = state.data_directory.c_str();
    state.discovery.FeatureInfo = &state.common;
    state.configured = true;
    return 1;
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    VkExtensionProperties* required{};
    if (!state.configured ||
        !Check("Instance extension query",
               NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&state.discovery, count,
                                                                       &required)))
        return 0;
    *extensions = required;
    return 1;
}

int32_t DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    if (!state.configured)
        return 0;
    NVSDK_NGX_FeatureRequirement support{};
    if (!Check("Feature requirement query",
               NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &state.discovery,
                                                       &support)))
        return 0;
    if (support.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        Log(1, "DLSS is not supported on this GPU or driver (code 0x%x)",
            static_cast<unsigned>(support.FeatureSupported));
        return 0;
    }
    VkExtensionProperties* required{};
    if (!Check("Device extension query",
               NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                   instance, physical, &state.discovery, count, &required)))
        return 0;
    *extensions = required;
    return 1;
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   PFN_vkGetInstanceProcAddr get_instance_proc,
                   PFN_vkGetDeviceProcAddr get_device_proc) {
    if (!state.configured || state.initialized)
        return 0;
    if (!Check("NGX initialization",
               NVSDK_NGX_VULKAN_Init_with_ProjectID(
                   ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion,
                   state.data_directory.c_str(), instance, physical, device, get_instance_proc,
                   get_device_proc, &state.common)))
        return 0;
    state.initialized = true;
    state.instance = instance;
    state.physical = physical;
    state.device = device;
    state.get_instance_proc = get_instance_proc;
    state.get_device_proc = get_device_proc;
    if (!Check("Capability query", NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.capabilities)))
        return 0;
    int available{}, needs_driver{};
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_SuperSampling_Available,
                             &available);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (needs_driver) {
        Log(1, "DLSS needs a newer NVIDIA driver");
        return 0;
    }
    if (!available) {
        Log(1, "DLSS Super Resolution is not available on this system");
        return 0;
    }
    return 1;
}

void ReleaseFeatureSlot(uint32_t slot) {
    if (slot >= state.features.size())
        return;
    auto& f = state.features[slot];
    if (f.handle) {
        NVSDK_NGX_VULKAN_ReleaseFeature(f.handle);
        f.handle = nullptr;
    }
    if (f.parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(f.parameters);
        f.parameters = nullptr;
    }
}

void ReleaseFeature() {
    ReleaseFeatureSlot(0);
}

int32_t CreateFeatureSlot(uint32_t slot, VkCommandBuffer command, const BbDlssFeature* desc) {
    if (!state.initialized || !state.capabilities || !desc || slot >= state.features.size())
        return 0;
    ReleaseFeatureSlot(slot);
    auto& f = state.features[slot];
    if (!Check("Parameter allocation", NVSDK_NGX_VULKAN_AllocateParameters(&f.parameters)))
        return 0;
    for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(f.parameters, key, desc->preset);
    static constexpr std::array qualities{
        NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    NVSDK_NGX_DLSS_Create_Params settings{};
    settings.Feature.InWidth = desc->input_width;
    settings.Feature.InHeight = desc->input_height;
    settings.Feature.InTargetWidth = desc->output_width;
    settings.Feature.InTargetHeight = desc->output_height;
    settings.Feature.InPerfQualityValue = qualities[std::clamp(desc->quality, 0, 4)];
    settings.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        (desc->depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
        (desc->hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                   : 0);
    // Node masks 1: single GPU. Each slot is its own feature, so each keeps its own history.
    const auto result = NGX_VULKAN_CREATE_DLSS_EXT1(state.device, command, 1, 1, &f.handle,
                                                   f.parameters, &settings);
    if (!Check("DLSS feature creation", result) || !f.handle) {
        f.handle = nullptr;
        return 0;
    }
    f.desc = *desc;
    Log(0, "DLSS slot %u: %ux%u -> %ux%u, quality %d, preset %u%s", slot, desc->input_width,
        desc->input_height, desc->output_width, desc->output_height, desc->quality, desc->preset,
        desc->hdr ? ", HDR input" : "");
    return 1;
}

int32_t CreateFeature(VkCommandBuffer command, const BbDlssFeature* desc) {
    return CreateFeatureSlot(0, command, desc);
}

NVSDK_NGX_Resource_VK Wrap(const BbDlssImage& image, bool writable) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
                                                  image.format, image.width, image.height,
                                                  writable);
}

int32_t EvaluateSlot(uint32_t slot, VkCommandBuffer command, const BbDlssEvaluate* evaluate) {
    if (slot >= state.features.size() || !evaluate)
        return 0;
    auto& f = state.features[slot];
    if (!f.handle || !f.parameters)
        return 0;
    auto color = Wrap(evaluate->color, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
    auto output = Wrap(evaluate->output, true);
    NVSDK_NGX_Resource_VK bias{};
    NVSDK_NGX_VK_DLSS_Eval_Params parameters{};
    parameters.Feature.pInColor = &color;
    parameters.Feature.pInOutput = &output;
    parameters.Feature.InSharpness = evaluate->sharpness;
    parameters.pInDepth = &depth;
    parameters.pInMotionVectors = &motion;
    if (evaluate->bias_mask.view != VK_NULL_HANDLE) {
        bias = Wrap(evaluate->bias_mask, false);
        parameters.pInBiasCurrentColorMask = &bias;
    }
    parameters.InRenderSubrectDimensions = {f.desc.input_width, f.desc.input_height};
    parameters.InJitterOffsetX = evaluate->jitter_x;
    parameters.InJitterOffsetY = evaluate->jitter_y;
    parameters.InReset = evaluate->reset;
    parameters.InMVScaleX = parameters.InMVScaleY = 1.0f;
    parameters.InPreExposure = parameters.InExposureScale = 1.0f;
    parameters.InFrameTimeDeltaInMsec = evaluate->frame_ms;
    return Check("DLSS evaluation",
                 NGX_VULKAN_EVALUATE_DLSS_EXT(command, f.handle, f.parameters, &parameters))
               ? 1
               : 0;
}

int32_t Evaluate(VkCommandBuffer command, const BbDlssEvaluate* evaluate) {
    return EvaluateSlot(0, command, evaluate);
}

// ---- DLSS-NR (BbDlssGetNrApi) ----
// nvngx_dlssnr.dll is an NGX feature DLL ("snippet") for feature 18 that the NGX loader rejects
// (it is not signed as NVIDIA ships it). As VirtualDesktop-OpenXR (dlssnr.cpp, MIT, the reference
// user of this DLL) does, the bridge loads it itself, answers its "loaded by NGX?" check and calls
// its exported NGX functions directly, with parameter objects from the NGX instance above.
namespace nr {

constexpr wchar_t DllName[] = L"nvngx_dlssnr.dll";
constexpr unsigned long long ApplicationId = 123456; // the id VirtualDesktop-OpenXR uses
constexpr NVSDK_NGX_Feature FeatureId = NVSDK_NGX_Feature_Reserved18;

// The DLL's parameter names (VirtualDesktop-OpenXR dlssnr.cpp).
constexpr char Width[] = "DLSSNR.Width";
constexpr char Height[] = "DLSSNR.Height";
constexpr char RenderPreset[] = "DLSSNR.Hint.Render.Preset";
constexpr char Enabled[] = "DLSSNR.Enabled";
constexpr char DepthInverted[] = "DLSSNR.DepthInverted";
constexpr char Color[] = "DLSSNR.Color";
constexpr char Output[] = "DLSSNR.Output";
constexpr char Reset[] = "DLSSNR.Reset";
constexpr char UICorrection[] = "DLSSNR.UICorrection";
constexpr char Style[] = "DLSSNR.Style";
constexpr char Intensity[] = "DLSSNR.Intensity";
constexpr char LocalToneStrength[] = "DLSSNR.LocalToneStrength";
constexpr char LocalStructureStrength[] = "DLSSNR.LocalStructureStrength";
constexpr char SkinStructureStrength[] = "DLSSNR.SkinStructureStrength";
constexpr char UseAutoMask[] = "DLSSNR.UseAutoMask";
constexpr char ColorX[] = "DLSSNR.ColorSubrectBaseX";
constexpr char ColorY[] = "DLSSNR.ColorSubrectBaseY";
constexpr char ColorW[] = "DLSSNR.ColorSubrectWidth";
constexpr char ColorH[] = "DLSSNR.ColorSubrectHeight";
constexpr char OutputX[] = "DLSSNR.OutputSubrectBaseX";
constexpr char OutputY[] = "DLSSNR.OutputSubrectBaseY";
constexpr char OutputW[] = "DLSSNR.OutputSubrectWidth";
constexpr char OutputH[] = "DLSSNR.OutputSubrectHeight";

// The DLL's exports (NGX snippet signatures, nvsdk_ngx_vk.h with NGX_SNIPPET_BUILD).
using InitExt2Fn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, VkInstance,
                                                 VkPhysicalDevice, VkDevice,
                                                 PFN_vkGetInstanceProcAddr,
                                                 PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version,
                                                 const NVSDK_NGX_Parameter*);
using CreateFeature1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature,
                                                       NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*,
                                                        const NVSDK_NGX_Parameter*,
                                                        PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using Shutdown1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice);

struct Feature {
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* handle{};
    uint32_t width{}, height{};
};

struct NrState {
    HMODULE module{};
    InitExt2Fn init{};
    CreateFeature1Fn create{};
    EvaluateFeatureFn evaluate{};
    ReleaseFeatureFn release{};
    Shutdown1Fn shutdown{};
    std::array<Feature, BBPORT_DLSS_NR_SLOTS> features{};
    bool tried{}, ready{};
} nr_state;

// The DLL asks for the file name of the module that called it and accepts only NGX's own
// (nvngx.dll). Its import of GetModuleFileNameW is pointed here, so only its own calls see this.
DWORD WINAPI LoaderModuleName(HMODULE, LPWSTR filename, DWORD size) {
    static constexpr wchar_t name[] = L"nvngx.dll";
    if (!filename || size == 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    _snwprintf_s(filename, size, _TRUNCATE, L"%s", name);
    if (size < std::size(name)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
    }
    return size; // as VirtualDesktop-OpenXR answers it
}

// Points `module`'s import of dll!function at `replacement`.
bool PatchImport(HMODULE module, const char* dll, const char* function, void* replacement) {
    auto* const base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) {
        return false;
    }
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
             base + directory.VirtualAddress);
         import->Name; ++import) {
        if (_stricmp(reinterpret_cast<const char*>(base + import->Name), dll) != 0 ||
            !import->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + import->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            const auto* by_name =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) {
                continue;
            }
            DWORD protection{};
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE,
                                &protection)) {
                return false;
            }
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), protection,
                           &protection);
            return true;
        }
    }
    return false;
}

int32_t Initialize() {
    if (nr_state.tried) {
        return nr_state.ready ? 1 : 0;
    }
    nr_state.tried = true;
    if (!state.initialized || !state.device) {
        Log(1, "DLSS-NR: NGX is not initialised");
        return 0;
    }
    const std::wstring path = state.dll_directory + L"\\" + DllName;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(0, "DLSS-NR: nvngx_dlssnr.dll is not next to the game");
        return 0;
    }
    nr_state.module = LoadLibraryW(path.c_str());
    if (!nr_state.module) {
        Log(1, "DLSS-NR: loading nvngx_dlssnr.dll failed (Windows error %lu)", GetLastError());
        return 0;
    }
    const auto find = [](const char* name) { return GetProcAddress(nr_state.module, name); };
    nr_state.init = reinterpret_cast<InitExt2Fn>(find("NVSDK_NGX_VULKAN_Init_Ext2"));
    nr_state.create = reinterpret_cast<CreateFeature1Fn>(find("NVSDK_NGX_VULKAN_CreateFeature1"));
    nr_state.evaluate =
        reinterpret_cast<EvaluateFeatureFn>(find("NVSDK_NGX_VULKAN_EvaluateFeature"));
    nr_state.release = reinterpret_cast<ReleaseFeatureFn>(find("NVSDK_NGX_VULKAN_ReleaseFeature"));
    nr_state.shutdown = reinterpret_cast<Shutdown1Fn>(find("NVSDK_NGX_VULKAN_Shutdown1"));
    if (!nr_state.init || !nr_state.create || !nr_state.evaluate || !nr_state.release) {
        Log(1, "DLSS-NR: nvngx_dlssnr.dll lacks the NGX Vulkan functions");
        return 0;
    }
    if (!PatchImport(nr_state.module, "KERNEL32.dll", "GetModuleFileNameW",
                     reinterpret_cast<void*>(&LoaderModuleName))) {
        Log(1, "DLSS-NR: could not answer the DLL's loader check; its initialisation may fail");
    }
    if (!Check("DLSS-NR initialization",
               nr_state.init(ApplicationId, state.data_directory.c_str(), state.instance,
                             state.physical, state.device, state.get_instance_proc,
                             state.get_device_proc, NVSDK_NGX_Version_API, nullptr)))
        return 0;
    nr_state.ready = true;
    Log(0, "DLSS-NR: ready (nvngx_dlssnr.dll)");
    return 1;
}

void ReleaseFeature(uint32_t slot) {
    if (slot >= nr_state.features.size())
        return;
    auto& f = nr_state.features[slot];
    if (f.handle && nr_state.release) {
        nr_state.release(f.handle);
    }
    f.handle = nullptr;
    if (f.parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(f.parameters);
        f.parameters = nullptr;
    }
}

int32_t CreateFeature(uint32_t slot, VkCommandBuffer command, uint32_t width, uint32_t height) {
    if (!nr_state.ready || slot >= nr_state.features.size() || width == 0 || height == 0)
        return 0;
    ReleaseFeature(slot);
    auto& f = nr_state.features[slot];
    if (!Check("DLSS-NR parameter allocation", NVSDK_NGX_VULKAN_AllocateParameters(&f.parameters)))
        return 0;
    NVSDK_NGX_Parameter_SetI(f.parameters, Width, static_cast<int>(width));
    NVSDK_NGX_Parameter_SetI(f.parameters, Height, static_cast<int>(height));
    NVSDK_NGX_Parameter_SetI(f.parameters, RenderPreset, 0);
    NVSDK_NGX_Parameter_SetUI(f.parameters, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(f.parameters, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    NVSDK_NGX_Parameter_SetI(f.parameters, DepthInverted, 0);
    NVSDK_NGX_Parameter_SetI(f.parameters, Enabled, 1);
    if (!Check("DLSS-NR feature creation",
               nr_state.create(state.device, command, FeatureId, f.parameters, &f.handle)) ||
        !f.handle) {
        f.handle = nullptr;
        return 0;
    }
    f.width = width;
    f.height = height;
    Log(0, "DLSS-NR slot %u: up to %ux%u", slot, width, height);
    return 1;
}

int32_t Evaluate(uint32_t slot, VkCommandBuffer command, const BbDlssNrEvaluate* e) {
    if (slot >= nr_state.features.size() || !e)
        return 0;
    auto& f = nr_state.features[slot];
    if (!f.handle || !f.parameters)
        return 0;
    NVSDK_NGX_Resource_VK color = Wrap(e->color, false);
    NVSDK_NGX_Resource_VK output = Wrap(e->output, true);
    NVSDK_NGX_Parameter* const p = f.parameters;
    const auto rect = [p](const char* x, const char* y, const char* w, const char* h,
                          const BbDlssRect& r) {
        NVSDK_NGX_Parameter_SetI(p, x, static_cast<int>(r.x));
        NVSDK_NGX_Parameter_SetI(p, y, static_cast<int>(r.y));
        NVSDK_NGX_Parameter_SetI(p, w, static_cast<int>(r.width));
        NVSDK_NGX_Parameter_SetI(p, h, static_cast<int>(r.height));
    };
    NVSDK_NGX_Parameter_SetVoidPointer(p, Color, &color);
    rect(ColorX, ColorY, ColorW, ColorH, e->color_rect);
    NVSDK_NGX_Parameter_SetVoidPointer(p, Output, &output);
    rect(OutputX, OutputY, OutputW, OutputH, e->output_rect);
    NVSDK_NGX_Parameter_SetI(p, Reset, e->reset ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(p, UseAutoMask, 1);
    NVSDK_NGX_Parameter_SetI(p, Style, std::clamp(e->style, 0, 2));
    NVSDK_NGX_Parameter_SetF(p, Intensity, e->intensity);
    NVSDK_NGX_Parameter_SetF(p, LocalToneStrength, e->local_tone);
    NVSDK_NGX_Parameter_SetF(p, LocalStructureStrength, e->local_structure);
    NVSDK_NGX_Parameter_SetF(p, SkinStructureStrength, e->skin_structure);
    NVSDK_NGX_Parameter_SetI(p, UICorrection, 0);
    return Check("DLSS-NR evaluation", nr_state.evaluate(command, f.handle, p, nullptr)) ? 1 : 0;
}

void Shutdown() {
    for (uint32_t slot = 0; slot < nr_state.features.size(); ++slot)
        ReleaseFeature(slot);
    // Before the device goes: a DLL still holding device objects crashes the process at exit.
    // It stays loaded (unloading it under NGX's parameter objects is not worth the risk).
    if (nr_state.ready && nr_state.shutdown) {
        Check("DLSS-NR shutdown", nr_state.shutdown(state.device));
    }
    nr_state.ready = false;
}

const BbDlssNrApi api{BBPORT_DLSS_NR_ABI, Initialize, CreateFeature, Evaluate, ReleaseFeature};

} // namespace nr

void Shutdown() {
    nr::Shutdown();
    for (uint32_t slot = 0; slot < state.features.size(); ++slot)
        ReleaseFeatureSlot(slot);
    if (state.capabilities) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.capabilities);
        state.capabilities = nullptr;
    }
    if (state.initialized) {
        NVSDK_NGX_VULKAN_Shutdown1(state.device);
        state.initialized = false;
        state.device = VK_NULL_HANDLE;
    }
}

const BbDlssApi api{BBPORT_DLSS_BRIDGE_ABI, Configure,         InstanceExtensions,
                    DeviceExtensions,       Initialize,        CreateFeature,
                    Evaluate,               ReleaseFeature,    Shutdown,
                    CreateFeatureSlot,      EvaluateSlot,      ReleaseFeatureSlot};

} // namespace

extern "C" __declspec(dllexport) const BbDlssApi* BbDlssGetApi() {
    return &api;
}

extern "C" __declspec(dllexport) const BbDlssNrApi* BbDlssGetNrApi() {
    return &nr::api;
}
