# 30. VR image quality: rough edges, distant shimmer (alt-eye stereo)

User report: "the graphics look rough around the edges, especially when far away". Setup: `out/vr_all.sh`,
`BB_XR_MODE=alt`, `BB_UPSCALER=off`, patches `Disable Motion Blur;Disable AA;Disable DoF;Disable CA`,
Pimax Dream Air (3832x3076 per eye recommended), `BB_XR_OVERSCAN` 0.15/0.10.

All measurements are headset-free: `tools/vr_iq_capture.py` runs the game flat with the VR hooks, a
fake head that renders the headset's eye frustum (97.1 deg vertical, tangent aspect 1.178), dumps the
presented frame (the image an eye swapchain gets 1:1) and keeps `BB_GPU_PROFILE=1` per variant.
Captures are in `out/vr_iq/` (not in git).

## 1. Causes, in order of weight

1. **Pixel density: the eye image is 1920x1080 and is stretched about 2.2x horizontally and 3.3x
   vertically.** The game frame covers the rendered frustum (115 x 97 deg, tangent spans 2.67 x 2.26).
   That gives 720 px per tangent unit horizontally and 477 vertically (12.6 / 8.3 px per degree at the
   center). The panel has 1555 / 1538 px per tangent unit (27 px/deg). The pixels are also not square:
   a 16:9 frame is mapped onto a 1.18:1 frustum, so each game pixel is 1.5x taller than wide.
   Near-horizontal edges (floorboards, steps, railings) get the coarsest stairs. The overscan costs
   another 1/1.1 x 1/1.15. Every game pixel becomes a 2x3 block of headset pixels, which makes the
   jaggies big and the image soft (the compositor's bilinear stretch).
2. **No anti-aliasing at all.** `Disable AA` (`0x026C254A=00`) removes the game's AA.
3. **Distant detail.** With the image this coarse, distant geometry (railings, bookshelves, foliage)
   is below one pixel and shimmers as the head moves. The `VR LOD Fix (SetCamera)` hook restores the
   flat game's LOD distances (findings/15 §4). That is correct for pop-in, but at 2.3x fewer pixels
   per degree than flat play it keeps meshes finer than the pixel grid, which adds shimmer. Leave it
   on; a LOD bias knob would trade shimmer for pop-in (not done).
4. Ruled out:
   - **Mip bias:** none. `TemporalUpscaler::SceneMipBias()` returns 0 with `BB_UPSCALER=off` (no
     `Upscaler: scene texture LOD bias` line in any log).
   - **Anisotropy:** the game's per-sampler anisotropy is kept (`texture_cache/sampler.cpp`).
   - **sRGB:** a raw copy of sRGB-encoded UNORM into an sRGB swapchain (format 50 from 44) is
     correct, and the compositor filters in linear.
   - **Shadow cascades:** not measured. Both eyes share them (findings/22).
   - **The shader:** no host shader degrades the image. The softness is the stretch.

## 2. The game's AA is spatial, but don't re-enable it yet

Frame analyzer (`BB_CAPTURE_TRIGGER`), `out/vr_iq/diag_aa_on` vs `diag_aa_off`. With AA on, two passes
are added after tonemap and before the UI:
- `#269 ps f7229091`: copies the frame to a scratch target.
- `#270 ps da1faca9`: writes the frame and samples **only** that copy (no depth, no history).

So the game's AA is a single-input post AA (FXAA-like). It does not read the other eye's frame, so it
is safe in alternate-eye mode. It costs less than 0.1 ms (below the profile's top 30).

**However:** at the VR frustum (97 deg) with AA on, every frame shows a large dithered orange quad
around the hunter's legs (`out/vr_iq/crop_box_aaon.png`, all 4 dumps, both eyes). It does not appear
with AA on at the game's FOV (`diag_aa_on_gamefov`), nor flat without hooks (`diag_aa_on_flat`), nor
with AA off. The cause is unknown (a near-camera/dither effect gated by the AA flag byte?). Until it is
understood, **keep `Disable AA`** and use the port's own spatial AA below.

## 3. What was added (all spatial, no history, alt-safe)

`gpu/shadps4/video_core/renderer_vulkan/vk_xr_eye_iq.{h,cpp}`, `host_passes/xr_fxaa_pass.{h,cpp}`,
`host_shaders/xr_fxaa.comp`, and additive hooks in `vk_presenter.{h,cpp}` (`PrepareFrame` before the
FSR1/post passes, `Present` for the frame size).

| Variable | Effect |
|---|---|
| `BB_XR_AA=off\|fxaa` | edge AA of the game frame (an independent implementation of the published FXAA 3.11 "Quality" algorithm, not NVIDIA's code). Applies to frames with a scene camera only, so menus and title stay crisp |
| `BB_XR_UPSCALE=off\|fsr1` | the frame, and so the eye swapchain (the bridge sizes it from the frame), at `BB_XR_EYE_RES` instead of the window's size, upscaled by shadPS4's existing AMD FSR 1 EASU (+RCAS) pass. `host_shaders/fsr/ffx_*.h` and `fsr.comp` carry AMD's MIT licence headers (Copyright (c) 2021 Advanced Micro Devices) |
| `BB_XR_SHARPEN=<0..1>` | RCAS strength (0 = EASU only; attenuation = 2·(1−s) stops; default 0.5) |
| `BB_XR_EYE_RES=<scale>\|WxH` | default 2 = twice the display buffer (3840x2160); keep it 16:9 (the desktop mirror and the cinema-screen path fit the frame into the window) |
| `BB_XR_IQ_CONTROL=<file>` | the same keys live: `aa=fxaa upscale=fsr1 sharpen=0.5 res=2` |
| `BB_XR_UPSCALE=dlss` | NVIDIA DLSS per eye (temporal, one history per eye): findings/37 |

No bridge change was needed. `AcquireEye` already creates the eye chain at the frame's size, and the
cinema/menu path still uses the window image.

Not done: SMAA. It needs area/search lookup textures, and FXAA already removes the stairs here.

## 4. Measurements (`out/vr_iq/variants`, one process, switched live, 72 FPS cap)

GPU profile, ms/frame (the busy time is noisy, ±0.5 ms):

| Variant | IQ passes | busy near / far | Result |
|---|---|---|---|
| off (today) | – | 9.65 / 9.02 | big stairs on every edge (`grid_near.png` top left) |
| fxaa | < 0.1 | 10.02 / 9.41 | stairs gone, slight texture softening |
| fsr1 (2x, sharpen 0.5) | 0.36 / 0.33 | 10.69 / 9.92 | sharper, but the stairs are kept (EASU preserves edges) |
| fsr1 sharpen 0 | 0.20 / 0.15 | 10.56 / 10.27 | |
| **fxaa + fsr1 (2x)** | 0.41 / 0.33 | 10.87 / 10.45 | **best: smooth edges, crisper than the bilinear stretch** |
| fxaa + fsr1 1.5x | 0.24 / 0.21 | 10.92 / 10.26 | |

The whole fxaa+fsr1 chain costs about 1 ms of GPU time, including the 4K post pass. In the headset,
add a 4K copy per eye image on the present queue and the compositor reading a 4K layer. FPS stayed at
the 72 cap throughout.

Captures (per view `near`, `far`): `out/vr_iq/variants/png/<view>_<variant>_0.png`. Magnified
comparisons: `out/vr_iq/grid_near.png`, `grid_far.png` (the far view in this save is a dark hall; its
detail is hard to judge).

## 5. Higher internal resolution (task 2d): feasible, already supported

`BB_RENDER_RES=2560x1440 BB_OUTPUT_RES=2560x1440 BB_DMEM_MB=9152` (the startup resolution patch;
without the `BB_DMEM_MB` raise the game faults at boot) renders scene and UI at 1440p
(`UI: native composition 2560x1440`). Measured at 72 FPS: busy 9.96 / 11.77 ms (near/far) vs 9.65 /
9.02 ms at 1080p, so +0.3 to +2.7 ms.

Without the new code the frame is still sized to the desktop window (1920x1080), so 1440p only acts
as 1.33x supersampling. That alone visibly smooths edges (`out/vr_iq/grid_near_1440.png`). With
`BB_XR_UPSCALE=fsr1 BB_XR_EYE_RES=1.5` the 1440p frame is upscaled to 3840x2160 per eye. 4K internal
(4x the pixels) is too heavy for 72 FPS alternate-eye. A frame shaped like the frustum (about
1920x1630) would fix the 1.5:1 pixel shape, but the resolution patches are 16:9 only. Not attempted.

## 6. Recommendation for `out/vr_all.sh`

Add `BB_XR_AA=fxaa BB_XR_UPSCALE=fsr1 BB_XR_SHARPEN=0.5` (needs a bb-probe built from this tree; test
binary `out/gpu/bb-probe-iq.exe`). On a GPU with headroom, also add
`BB_RENDER_RES=2560x1440 BB_OUTPUT_RES=2560x1440 BB_DMEM_MB=9152 BB_XR_EYE_RES=1.5`. Keep
`Disable AA` (section 2).

## 7. Will true stereo (Phase 4, findings/23) solve it?

Partly.
- **What it changes:** each eye is a full frame from its own camera every frame, so temporal passes
  get per-eye history (P4.5). The port's TAA, FSR 3 or DLSS can then run per eye. Temporal AA is the
  real cure for distant shimmer and thin geometry, which spatial AA cannot fix (single frames have no
  sub-pixel information). FSR 3/DLSS can also reconstruct above 1080p at a lower cost than native
  1440p. Each eye's image also refreshes at the full rate (72 Hz instead of 36), so there is less
  reprojection smear.
- **What it doesn't fix:** the resolution budget. Each eye is still a 1080p-class frame stretched over
  about 115 x 97 deg, with non-square pixels, unless the render size goes up. True stereo also doubles
  the render cost, which leaves less room for a higher resolution. The LOD and shadow behaviour at
  distance stay as they are.
