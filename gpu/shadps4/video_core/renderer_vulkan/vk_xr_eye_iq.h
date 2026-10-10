// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR image quality (docs/vr/findings/30-vr-image-quality.md): spatial passes on the game
// frame before it goes to an eye, all without history (alternate-eye stereo safe).
//
//   BB_XR_AA=off|fxaa          edge anti-aliasing of the display image (host_passes/xr_fxaa_pass)
//   BB_XR_UPSCALE=off|fsr1     the frame (and so the eye swapchain) at BB_XR_EYE_RES instead of the
//                              window's size, upscaled by AMD FSR 1 EASU (+ RCAS)
//   BB_XR_SHARPEN=<0..1>       RCAS sharpening with fsr1 (0 = none; default 0.5)
//   BB_XR_EYE_RES=WxH|<scale>  the upscaled size (default 2: twice the display buffer, 3840x2160)
//   BB_XR_IQ_CONTROL=<file>    the same keys live: "aa=fxaa upscale=fsr1 sharpen=0.5 res=2"
//
// NVIDIA DLSS (docs/vr/findings/37-dlss-vr.md, vk_xr_dlss.h), default off:
//   BB_XR_UPSCALE=dlss         DLSS Super Resolution per eye (one temporal history per eye),
//                              falling back to fsr1 + fxaa when it cannot run (the log says why)
//   BB_XR_DLSS_QUALITY=dlaa|quality|balanced|performance|ultra
//                              the eye image = display buffer x 1 / 1.5 / 1.72 / 2 / 3; unset:
//                              BB_XR_EYE_RES decides the size and the mode follows the ratio
//   BB_XR_DLSS_PRESET=default|J|K|L|M   NGX render preset (driver default when unset)
//   BB_XR_DLSS_SHARPEN=<0..1>  RCAS after DLSS (default 0: DLSS 4 models ignore NGX sharpness)
//   BB_XR_DLSS_MASK=0|1        arm/weapon "bias current colour" mask (default 1): DLSS trusts the
//                              current frame for surfaces nearer than the distance below, because
//                              the hands move the arms and the motion vectors are camera-only
//   BB_XR_DLSS_MASK_M=<metres> that distance from the camera (default 0.9; 0 = no mask)
//   control file keys: upscale=dlss dlss_quality=quality dlss_preset=K dlss_sharpen=0.2
//                      dlss_mask=1 dlss_mask_m=0.9
//
// The passes apply to frames with a scene camera (3D frames: what the eyes get); title, menus
// and loading screens without one stay as they are.

#pragma once

#include "common/types.h"

namespace Vulkan::XrEyeIq {

struct Settings {
    bool fxaa = false;
    bool fsr1 = false;
    float sharpen = 0.5f;
    float scale = 2.0f;        ///< res as a factor of the display buffer (when width == 0)
    u32 width = 0, height = 0; ///< res as an explicit size
    // bbport VR DLSS (vk_xr_dlss.h).
    bool dlss = false;             ///< upscale=dlss (fsr1 is then false)
    bool dlss_quality_set = false; ///< dlss_quality given: it decides the output size
    int dlss_quality = 3;          ///< XrDlssLogic::Quality (0 DLAA .. 4 Ultra Performance)
    u32 dlss_preset = 0;           ///< NVSDK_NGX_DLSS_Hint_Render_Preset (0 default)
    float dlss_sharpen = 0.0f;     ///< RCAS after DLSS, 0..1
    bool dlss_mask = true;         ///< bias-current-colour mask of the near surfaces (arms)
    float dlss_mask_m = 0.9f;      ///< its view distance in metres (0 = off)
};

/// The current settings: the environment, then the control file (re-read when it changes, at
/// most every 250 ms). Thread-safe.
Settings Current();

/// The frame size for a display buffer of src_w x src_h under `s` (fsr1, or the DLSS output size
/// with dlss), else {0, 0}.
void TargetSize(const Settings& s, u32 src_w, u32 src_h, u32& out_w, u32& out_h);

} // namespace Vulkan::XrEyeIq
