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
//                      dlss_mask=1 dlss_mask_m=0.9 dlss_jitter=0|1 (0 = no jitter, A/B test)
//
// VR frame rate counter (docs/vr/findings/31-true-stereo.md section 10.9, xr_fps_logic.h), default off:
//   BB_XR_FPS=1                a small counter in the HUD layer of the headset (the HUD layer is
//                              on by default in alt, mono and stereo): frames per second / headset
//                              Hz, and the frame time with the bottleneck word. Green at
//                              >= Hz - 1, yellow at >= 80 %, red below. Not in the eye images.
//   BB_XR_FPS_POS=tl|tr|bl|br  its corner of the HUD (default tr)
//   control file keys: fps=0|1 fps_pos=tr
//
// The HUD quad in the middle of the view (xr_hud_center.h, findings/26 "HP and stamina on the wrist"), default auto:
//   BB_XR_HUD_CENTER=menus|subtitles|auto|on|off   default menus: hidden in plain gameplay
//                              (HP and stamina are on the wrist), shown while a game menu is open;
//                              subtitles: + the message band; auto: + any message content; on = always
//   control file key: hud_center=menus|subtitles|auto|on|off
// NVIDIA DLSS-NR (docs/vr/findings/42-dlss-nr.md, vk_xr_dlssnr.h), default off, needs the
// player's nvngx_dlssnr.dll next to the exe: neural rendering on the finished eye image.
//   BB_XR_DLSSNR=0|1           on/off (control key dlssnr)
//   BB_XR_DLSSNR_PASSES=1..3   run it again on its result (dlssnr_passes)
//   BB_XR_DLSSNR_STYLE=0..2, _INTENSITY=0..2 (1), _TONE (0.2), _STRUCTURE (0.7), _SKIN (0.5)
//   BB_XR_DLSSNR_FOVEA=0.1..1  the centred share of the eye the first pass covers (0.66)
//   BB_XR_DLSSNR_INNER=0.1..1  the share passes 2 and 3 cover (0.4)
//   BB_XR_DLSSNR_PLACE=post|pre  post (default): on the finished eye image; pre: on the
//                              render-size image before DLSS SR / FSR 1 (findings/42 section 7)
//   control keys: dlssnr dlssnr_passes dlssnr_style dlssnr_intensity dlssnr_tone
//                 dlssnr_structure dlssnr_skin dlssnr_fovea dlssnr_inner dlssnr_place
//
// The passes apply to frames with a scene camera (3D frames: what the eyes get); title, menus
// and loading screens without one stay as they are.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/xr_dlssnr_logic.h"
#include "video_core/renderer_vulkan/xr_hud_center.h"

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
    bool dlss_jitter = true;       ///< false: zero jitter (projection and DLSS), an A/B diagnostic
    // bbport VR fps counter (xr_fps_logic.h): in the HUD layer of the headset.
    bool fps = false;              ///< fps=1 / BB_XR_FPS=1
    int fps_corner = 1;            ///< fps_pos / BB_XR_FPS_POS: 0 tl, 1 tr (default), 2 bl, 3 br
    XrHudCenter::Mode hud_center = XrHudCenter::kDefaultMode; ///< hud_center / BB_XR_HUD_CENTER
    XrDlssNrLogic::Settings nr;    ///< bbport VR DLSS-NR (vk_xr_dlssnr.h)
};

/// The current settings: the environment, then the control file (re-read when it changes, at
/// most every 250 ms). Thread-safe.
Settings Current();

/// The frame size for a display buffer of src_w x src_h under `s` (fsr1, or the DLSS output size
/// with dlss), else {0, 0}.
void TargetSize(const Settings& s, u32 src_w, u32 src_h, u32& out_w, u32& out_h);

} // namespace Vulkan::XrEyeIq
