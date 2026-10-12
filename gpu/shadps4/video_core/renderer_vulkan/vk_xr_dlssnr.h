// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR: NVIDIA DLSS-NR (neural rendering) on the finished eye images
// (docs/vr/findings/42-dlss-nr.md). BB_XR_DLSSNR=1 (default off; options in vk_xr_eye_iq.h).
// The port contains no NVIDIA code: it goes through vk_dlss (bbport_dlss.dll's BbDlssGetNrApi),
// which loads nvngx_dlssnr.dll, an experimental NVIDIA DLL the player puts next to the exe.
//
// Where it runs: Presenter::PrepareFrame, after the post-process pass wrote the frame image (the
// eye image as the headset gets it: upscaled, tonemapped, without the HUD), in place: the DLL reads
// the centre region of the image and writes a scratch image, which is copied back over it. Each
// further pass does the same on the result, on a smaller centre region (dlssnr_inner).
//
// History: one DLSS-NR feature per stream and pass. Alternate eyes (BB_XR_MODE=alt) have a
// stream per eye, each fed only its own eye's frames; only the eye rendered this frame is
// processed. True stereo (BB_XR_MODE=stereo) runs eye L and eye R (its own image) each frame.
// Title, menus, loading screens (no scene camera), held and HDR frames are left as they are.
//
// dlssnr_place=pre (BB_XR_DLSSNR_PLACE=pre; findings/42 section 7): it runs instead on the
// render-size image the upscaler reads, before DLSS Super Resolution (RunPre): the display buffer
// is copied into an image of this class (the DLL's processed regions in place on it), which
// replaces the display buffer as the input of DLSS / FSR 1 / FXAA and of the hybrid warp. Full
// stereo's own eye R image gets the same (stream 1); the hybrid's eye R is warped from eye L's
// processed image. A change of placement starts the DLSS-NR and the DLSS SR histories over.

#pragma once

#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;
struct Frame;

class XrDlssNr {
public:
    XrDlssNr(const Instance& instance, Scheduler& scheduler);
    ~XrDlssNr();

    /// After the post-process pass wrote frame.image (eGeneral, in `format`): records DLSS-NR on it
    /// when it is on and the frame is a 3D eye image. eye_r: frame.eye_r_out instead (true
    /// stereo's eye R, after its own image-quality chain). Leaves the image in eGeneral.
    /// Post placement only (does nothing with dlssnr_place=pre).
    void Run(vk::CommandBuffer cmdbuf, const Frame& frame, vk::Format format, bool eye_r = false);

    /// Pre-SR placement: a render-size image with its processed copy.
    struct PreResult {
        vk::Image image;     ///< the processed copy (eGeneral; bytes as the source's), or null
        vk::ImageView view;  ///< `view_format` with alpha 1, for the presenter's passes
        u32 width = 0, height = 0;
    };
    /// With dlssnr_place=pre and DLSS-NR on: copies `source` (eGeneral, the bytes the presenter
    /// samples as `view_format`, width x height) into this stream's own image and runs DLSS-NR on
    /// it. eye_r: true stereo's eye R image (stream 1; the caller decides, PreEyeR); else the
    /// frame's display buffer (the stream from the frame, PreStreamFor). Null image: not run
    /// (off, post, not a 3D frame, not 8-bit, no DLL): the caller keeps `source`.
    PreResult RunPre(vk::CommandBuffer cmdbuf, const Frame& frame, vk::Image source,
                     vk::Format view_format, u32 width, u32 height, bool eye_r = false);

    /// Once per presented frame, before RunPre and DLSS: notes the placement (a change starts
    /// every DLSS-NR history over) and returns true when DLSS SR's input changed kind (pre-SR
    /// DLSS-NR switched on or off, or the placement changed): DLSS's histories start over too.
    bool SrInputChanged();

    /// BB_XR_DLSSNR_FLAT_PAIRS=1 (tests): flat true-stereo frames count as pairs
    /// (XrDlssNrLogic::WithFlatPairs); the presenter then runs eye R's chain every frame.
    static bool FlatPairs();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
