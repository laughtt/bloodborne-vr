// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
// SPDX-License-Identifier: MIT
//
// C interface between bbport and bbport_dlss.dll. The port contains no NVIDIA code: it loads the
// bridge at run time when it is present (next to bb-probe.exe, with nvngx_dlss.dll) and keeps
// its other upscalers when it is absent. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI 2 (bbport VR DLSS, docs/vr/findings/37-dlss-vr.md): independent features in slots (0 is the
// flat game's, the one the ABI 1 functions use; 1 and 2 the VR eyes), each with its own history.
// The ABI 1 members keep their place, so an ABI 1 caller works with an ABI 2 bridge.
// ABI 3: BbDlssEvaluate gets a bias_mask image at the end (NGX "bias current colour mask": 1 = trust the
// current frame, e.g. first-person arms that move with the hands while the motion vectors are
// camera-only). A null view leaves it out, as in ABI 2 and for the flat game.
// DLSS-NR is a separate export (BbDlssGetNrApi, below) and leaves this ABI as it is.
#define BBPORT_DLSS_BRIDGE_ABI 3
#define BBPORT_DLSS_BRIDGE_SLOTS 4

typedef void (*BbDlssLogFn)(int warning, const char* message);

typedef struct BbDlssImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} BbDlssImage;

typedef struct BbDlssFeature {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    int32_t quality; // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int32_t depth_inverted;
    uint32_t preset; // NVSDK_NGX_DLSS_Hint_Render_Preset value, 0 = driver default
    int32_t hdr;     // 1: linear HDR colour with automatic exposure, 0: tonemapped colour
} BbDlssFeature;

typedef struct BbDlssEvaluate {
    BbDlssImage color;  // sampled
    BbDlssImage depth;  // sampled, hardware depth
    BbDlssImage motion; // sampled, render-resolution pixels, current to previous
    BbDlssImage output; // storage, general layout
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
    float sharpness; // 0..1; 0 leaves the output unsharpened
    // ABI 3, optional (view == VK_NULL_HANDLE: none): R8_UNORM sampled, render resolution.
    BbDlssImage bias_mask;
} BbDlssEvaluate;

typedef struct BbDlssApi {
    uint32_t abi;
    // UTF-16 paths: the directory with nvngx_dlss.dll, and a writable data directory.
    int32_t (*Configure)(const wchar_t* dll_directory, const wchar_t* data_directory,
                         BbDlssLogFn log);
    // Vulkan extensions NGX needs. The arrays stay valid until Shutdown.
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(VkCommandBuffer command, const BbDlssFeature* feature);
    int32_t (*Evaluate)(VkCommandBuffer command, const BbDlssEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature.
    void (*ReleaseFeature)(void);
    void (*Shutdown)(void);
    // ABI 2: the same per feature slot (0 .. BBPORT_DLSS_BRIDGE_SLOTS - 1).
    int32_t (*CreateFeatureSlot)(uint32_t slot, VkCommandBuffer command,
                                 const BbDlssFeature* feature);
    int32_t (*EvaluateSlot)(uint32_t slot, VkCommandBuffer command,
                            const BbDlssEvaluate* evaluate);
    void (*ReleaseFeatureSlot)(uint32_t slot);
} BbDlssApi;

typedef const BbDlssApi* (*BbDlssGetApiFn)(void);

// DLSS-NR (neural rendering, docs/vr/findings/42-dlss-nr.md): NVIDIA's experimental
// nvngx_dlssnr.dll, which the player puts next to bb-probe.exe (never shipped). Its own export
// (BbDlssGetNrApi) rather than an ABI 4: exes that know ABI 1-3 keep loading this bridge, and an
// exe without DLSS-NR support simply never asks for it. Calls after BbDlssApi::Initialize.
#define BBPORT_DLSS_NR_ABI 1
#define BBPORT_DLSS_NR_SLOTS 6

typedef struct BbDlssRect {
    uint32_t x, y, width, height;
} BbDlssRect;

typedef struct BbDlssNrEvaluate {
    BbDlssImage color;  // sampled, display-referred (tonemapped) colour
    BbDlssImage output; // storage, general layout
    BbDlssRect color_rect;
    BbDlssRect output_rect;
    int32_t reset;
    int32_t style; // 0..2
    float intensity;
    float local_tone;
    float local_structure;
    float skin_structure;
} BbDlssNrEvaluate;

typedef struct BbDlssNrApi {
    uint32_t abi;
    // Loads nvngx_dlssnr.dll from the Configure directory and initialises it on the device given to
    // Initialize. 1 = ready. Tried once; later calls return the first result.
    int32_t (*Initialize)(void);
    // A feature per slot (0 .. BBPORT_DLSS_NR_SLOTS - 1), each with its own history, for regions
    // up to width x height.
    int32_t (*CreateFeature)(uint32_t slot, VkCommandBuffer command, uint32_t width,
                             uint32_t height);
    int32_t (*Evaluate)(uint32_t slot, VkCommandBuffer command, const BbDlssNrEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature.
    void (*ReleaseFeature)(uint32_t slot);
} BbDlssNrApi;

typedef const BbDlssNrApi* (*BbDlssGetNrApiFn)(void);

#ifdef __cplusplus
}
#endif
