# 42. DLSS-NR (neural rendering) on the eye images

Status: implemented, off by default (`BB_XR_DLSSNR=1` / DLSS tab "DLSS-NR neural rendering").
Needs an NVIDIA RTX GPU, the DLSS bridge (`bbport_dlss.dll` of this build) and NVIDIA's
experimental `nvngx_dlssnr.dll`, which the player puts next to `bb-probe.exe`. We never ship it.

## 1. What it is

`nvngx_dlssnr.dll` is an NGX feature DLL ("snippet") for NGX feature 18 (`Reserved18`), a
post-process that changes local tone, structure (detail) and skin on a finished, display-referred
image. It became known through mbucchia's experimental VirtualDesktop-OpenXR build
(github.com/mbucchia/VirtualDesktop-OpenXR, branch `dlssnr`, file `virtualdesktop-openxr/dlssnr.cpp`,
MIT), which runs it on the OpenXR projection layer, through D3D12. That code is the reference for
the parameter names and the loading trick below; nothing of it is copied beyond those names.

The DLL as distributed is modified after NVIDIA signed it (Windows: signature "HashMismatch"), so the
NGX loader refuses it. That is why it is loaded by hand and why it is not shipped.

## 2. Loading it (gpu/dlss_bridge, `BbDlssGetNrApi`)

- A separate export of the bridge, not a DLSS bridge ABI 4: exes that check `abi <= 3` keep loading
  this bridge; an exe without DLSS-NR never asks for it.
- The bridge `LoadLibrary`s `nvngx_dlssnr.dll` from the exe's directory, points the DLL's import of
  `KERNEL32!GetModuleFileNameW` at a function that answers `nvngx.dll` (the DLL checks it is called
  by NGX; VirtualDesktop-OpenXR answers the same through a Detours hook), and calls its exports:
  `NVSDK_NGX_VULKAN_Init_Ext2` (app id 123456, as VirtualDesktop-OpenXR), `CreateFeature1`,
  `EvaluateFeature`, `ReleaseFeature`, `Shutdown1`. The DLL exports the full Vulkan NGX interface,
  so no D3D12 interop is needed.
- Parameter objects come from the bridge's own NGX instance (`NVSDK_NGX_VULKAN_AllocateParameters`),
  resources are `NVSDK_NGX_Resource_VK` pointers, as for DLSS.
- `Shutdown1` is called before the device goes. Without it the process crashed at exit (smoke test
  exit code 139); VirtualDesktop-OpenXR never shuts it down.

Parameters set per evaluation: `DLSSNR.Color`/`Output` and their sub-rectangles, `Reset`,
`UseAutoMask=1`, `Style` (0..2), `Intensity`, `LocalToneStrength`, `LocalStructureStrength`,
`SkinStructureStrength`, `UICorrection=0`. At creation: `Width`/`Height` (the region), render preset
0, node masks 1, `DepthInverted=0`, `Enabled=1`. Depth and motion vectors are not given (section 6).

## 3. Measured (smoke test, RTX 4090)

`dlss-bridge-smoke.exe <dir>` with `nvngx_dlssnr.dll` in `<dir>`:

| region of a 3840x2160 eye | GPU time per evaluation |
|---|---|
| centre 66 % (2536x1428), VirtualDesktop-OpenXR's default | 5.4 ms |
| all (3840x2160) | 10.3-10.7 ms |

Roughly proportional to the area. A second pass on the centre 40 % adds about 2 ms more (area
0.16 against 0.44).

In the game (RTX 4090, Pimax, alternate eyes, DLSS Quality to 2880x1620 per eye, 72 Hz cap; eye
images per second from the `VR DLSS-NR: 10 s` log line):

| setting | eye images / s |
|---|---|
| 1 pass on 66 %, any strength | 70-72 (the cap) |
| 2 passes, 80 % then 50 %, all strengths at their maximum | about 57 |
| 3 passes, 80 % then 50 % twice, all at their maximum | about 46 |

The player's verdict: the DLL's GUI strengths (intensity 1, tone 0.2, structure 0.7, skin 0.5)
changed too little to notice; intensity 2, structure 1, skin 1 looked "really good", 3 passes most.
The defaults are those strengths with 1 pass (tone 0.6). In alternate eyes the image flickers when
the head turns (section 6).

## 4. Where it runs (vk_xr_dlssnr.h)

`Presenter::PrepareFrame`, after the post-process pass wrote the frame image (the eye image as the
headset gets it: upscaled by DLSS or FSR 1, tonemapped, no HUD), in place:

1. The DLL reads the centre region of the frame image (an UNORM view of it: the encoded values, as
   VirtualDesktop-OpenXR gives it the swapchain's bytes) and writes the same region of a scratch
   image of the eye image's size. A region-sized output written at its origin is refused
   (`0xbad00005`, InvalidParameter; `BB_NR_SMOKE=1920x1080,scratch` reproduces it in the smoke
   test).
2. The region is copied back over the frame image.
3. Passes 2 and 3 (`dlssnr_passes`) repeat this on the result, on the smaller centre region
   `dlssnr_inner`, so each pass reads only what the one before wrote.

Histories: one feature per stream and pass (6 slots). Alternate eyes have a stream per eye, each fed
only its own eye's frames, and only the eye rendered this frame is processed (half the cost of
processing both every frame, as VirtualDesktop-OpenXR must). True stereo processes eye L and its own
eye R image. A stream's history starts over after a gap of more than 250 ms (menus, the option off).
Alternate eyes' mono frames (the scene camera without eye views: the headset's flat screen) use
stream 0, as flat play does. Title, menus, loading screens (no scene camera), held frames and HDR
frames are left alone.

The options are live (`BB_XR_IQ_CONTROL` keys); a change of region size recreates that feature
(one wait for the GPU).

## 5. Options

| key (control file) | env | default |
|---|---|---|
| dlssnr | BB_XR_DLSSNR | 0 |
| dlssnr_passes | BB_XR_DLSSNR_PASSES | 1 (1..3) |
| dlssnr_style | BB_XR_DLSSNR_STYLE | 0 (0..2) |
| dlssnr_intensity | BB_XR_DLSSNR_INTENSITY | 2 (0..2; the DLL's own GUI stops at 1, default 1) |
| dlssnr_tone | BB_XR_DLSSNR_TONE | 0.6 (the DLL's GUI: 0.2) |
| dlssnr_structure | BB_XR_DLSSNR_STRUCTURE | 1 (the DLL's GUI: 0.7) |
| dlssnr_skin | BB_XR_DLSSNR_SKIN | 1 (the DLL's GUI: 0.5) |
| dlssnr_fovea | BB_XR_DLSSNR_FOVEA | 0.66 |
| dlssnr_inner | BB_XR_DLSSNR_INNER | 0.4 |
| dlssnr_place | BB_XR_DLSSNR_PLACE | post (pre: section 7) |

The launcher (`bbvr_settings.py`) shows them on the DLSS tab, greys them out unless `detect_dlssnr`
finds an RTX GPU, a DLSS-capable exe that knows `BB_XR_DLSSNR`, a bridge with `BbDlssGetNrApi` and
`nvngx_dlssnr.dll`, and writes `dlssnr=0` when it is off. Release packaging deletes any
`nvngx_dlssnr.dll` from the stage.

The **Super High** preset sets `dlssnr=1`, `dlssnr_passes=1`, `dlssnr_place=pre` on top of High (about 50-54 fps on an RTX 4090
in full stereo, measured in the headset). The model file is not shipped and not linked: the launcher's **Find file...** copies the
player's own copy next to the game exe (`bbvr_settings.install_nr_dll`, `bbvr_cli.py nr-dll`, web menu `/api/nr-dll`), and
**What is this?** opens `docs/vr/dlss-nr.md` (`nr-help`).

## 6. Open

- Flicker when the head turns (headset, alternate eyes): each eye's history sees every second
  frame and no motion vectors, so the content jumps twice as far between its evaluations and the
  invented detail changes. True/hybrid stereo (both eyes every frame) halves the jump; motion
  vectors (below) are the real fix.
- Depth: the DLL takes `DLSSNR.Depth` (same size as the colour region). Our depth is at render
  size; it would need upscaling per eye.
- Motion vectors: `DLSSNR.MVec` exists; VirtualDesktop-OpenXR never sets it. We have camera motion
  vectors at render size (vk_camera_motion).
- The region's edge is a hard seam (as in VirtualDesktop-OpenXR); a feathered blend would hide it.
- Each eye invents its own detail: the eyes may disagree (shimmer in stereo), more so with several
  passes. Ideas: a lower structure strength, or one eye's change carried to the other by depth.
- GPU time of the pass in the frame profiler: done (section 7.4, `BB_GPU_PROFILE=1` labels
  `VR DLSS-NR: eye L/R post|pre-SR (N passes)`).

## 7. Pre-SR placement (branch `dlssnr-presr`, experimental)

Date 2026-10-12. Built, unit tested, measured flat [M][U]; **not yet in the headset**. Off by
default: `dlssnr_place=post` keeps everything above unchanged. Idea from OptiScaler's "run the model
before Super Resolution" (concept only, no code).

### 7.1 What it does

`dlssnr_place=pre` (`BB_XR_DLSSNR_PLACE=pre`, control key `dlssnr_place=pre|post`): DLSS-NR runs on
the render-size image the upscaler reads, before DLSS SR, instead of on the finished eye image.

1. `Presenter::PrepareFrame`, before the hybrid warp: the display buffer (1920x1080, the game's
   finished, tonemapped frame) is copied bitwise into a pre-SR image of its own (one per stream,
   mutable format: the DLL and DLSS read it as the UNORM twin of the presenter's view format, the
   presenter's passes in the view format, the warp as R8G8B8A8 UNORM). The same passes as in post
   (`dlssnr_passes`, the centred regions `dlssnr_fovea` / `dlssnr_inner`, now of the render image)
   run in place on that copy (`XrDlssNr::RunPre`).
2. The copy replaces the display buffer as DLSS's colour (`XrDlss::Input::color_override`; depth,
   motion vectors and jitter are unchanged), as the hybrid warp's source
   (`StereoWarpAtFlip(..., color_id, raw_color)`: both given = warp from `raw_color`) and, without
   DLSS, as the input of FSR 1 / FXAA.
3. The post call (`XrDlssNr::Run`) does nothing in pre. The flat upscaler's own display image (not
   used in VR) is never processed in pre.

**What pre means per stereo mode:**

| mode | eye L | eye R |
|---|---|---|
| hybrid (`full=0`) | display buffer, stream 0 | **no evaluation of its own**: warped from eye L's processed image, so it carries eye L's result (one evaluation per frame; both eyes show the same invented detail, moved by the disparity) |
| full stereo | display buffer, stream 0 | its own rendered image (`EyeRImage::rendered`, new), stream 1, before `RunEyeR` and eye R's chain |
| alternate eyes | the frame's eye, stream 0/1 as post | (same image) |

Pre processes **every** 3D frame of true stereo, held and flat-screen ones too
(`PreStreamFor`): eye L's DLSS evaluates those frames to keep its history in step, so its input must
not alternate between processed and unprocessed frames. Title, menus, loading, HDR: never.

### 7.2 Histories

- **Placement change** (`PlaceTracker`): every DLSS-NR feature's history starts over
  (`Slot::place_reset` -> `NeedsReset(..., place_changed)`), also of a feature whose region size
  happens to stay (DLAA).
- **DLSS SR** (`SrInputTracker`, `SrInputProcessed = on && pre`): when SR's input changes kind
  (pre switched on or off, post <-> pre while on) `XrDlss::Input::reset_history` resets SR's tracker
  (both eyes; eye R follows eye L's reset). Not on post toggles, nor on placement changes while off.
- Logs: `VR DLSS-NR: placement -> pre: DLSS-NR histories start over`, `VR DLSS-NR: DLSS SR's input
  is now ...`, `VR DLSS: histories start over (the input changed: pre-SR DLSS-NR)`; the 10 s line
  ends with the placement; `VR DLSS-NR: pre-SR image (eye L) 1920x1080 B8G8R8A8Unorm`.
- Unit tests (`xr-dlssnr-test`): parsing and env key, `PreStreamFor` vs `StreamFor` per mode,
  `PreEyeR`, `WithFlatPairs`, the trackers and the reset rule.

### 7.3 Measuring flat (`tools/vr_true_stereo_test.py nrplace`)

Flat, fake head yaw 0 (Cathedral Ward gate), `BB_XR_MODE=stereo`, `BB_UPSCALER=off`,
`BB_XR_HUD=force`, `BB_XR_UPSCALE=dlss dlss_quality=quality` (1920x1080 -> 2880x1620 per eye, as
in the headset), eye R's own DLSS, no fps cap, `BB_GPU_PROFILE=1`. New test switch
`BB_XR_DLSSNR_FLAT_PAIRS=1`: flat 3D frames count as pairs, so both eyes go through NR (post) and
eye R's chain every frame, as in the headset (without it flat post processes nothing: the pair kind
is never "Eye" without a headset view). Per mode DLSS only / NR post / NR pre-SR, A B C A B C, 12 s
each, then frame dumps. Default strengths (intensity 2, tone 0.6, structure 1, skin 1).

### 7.4 Results, 1 pass [M] (`out/stereo_ab/run25presr/nrplace1`, `bb-probe-presr1.exe`, RTX 4090)

busy = GPU ms per frame minus idle; NR = the `VR DLSS-NR` labels; fps uncapped, flat.

| mode | setting | busy ms | NR ms | fps |
|---|---|---|---|---|
| hybrid | DLSS only | 7.51 | - | 121 |
| hybrid | NR post | 16.49 | 8.94 (L 4.9, R 4.0) | 60 |
| hybrid | **NR pre-SR** | **10.89** | **3.35** (eye L only) | **90** |
| full | DLSS only | 10.21 | - | 95 |
| full | NR post | 19.03 | 8.94 (L 4.5, R 4.4) | 52 |
| full | **NR pre-SR** | **16.93** | **6.28** (L 3.1, R 3.2) | **58** |

Both rounds agree within 0.2 ms. From the busy differences against DLSS only, one evaluation costs
about 4.5 ms post (region 1900x1072 of 2880x1620) and about **3.3 ms pre (1268x712 of 1920x1080)**:
2.2x fewer pixels, only about 27 % cheaper. The DLL has a large cost per evaluation that does not
scale with the region (the smoke test's 5.4 ms at 2536x1428 fits the same picture). Hence:

- **Hybrid**: pre costs 3.4 ms instead of 9.0 ms (-63 %): one evaluation instead of two, and a
  smaller one. This is the big gain.
- **Full stereo**: 6.3 ms instead of 8.9 ms (-30 %): still two evaluations.
- A work-resolution knob (`dlssnr_scale`, review c4) would buy little on top: a smaller region saves
  only the per-pixel part (under 1 ms per evaluation at render size). Not built.

For the headset (hybrid, 1 pass: DLSS 68 fps, NR post 53 fps): pre removes about 5.6 of the 9 ms
NR costs flat. If the headset frame is GPU-bound the same way, hybrid + pre should land close to
DLSS only; 72 needs DLSS only itself to reach 72 first. Unverified until measured in the headset.

### 7.5 Images (`nrplace1/png`; `crop_hybrid_off_post_pre.png`: DLSS only | post | pre)

Post visibly adds local contrast and structure (darker cloth folds, crisper chains and carvings).
**Pre is much closer to DLSS only**: the structure the DLL adds at render size is largely smoothed
away by DLSS SR (temporal accumulation over jittered frames, and the upscale), and what is left is
softer. Mean colour unchanged (post raises it by about one level). So pre at the same strength is
weaker, not equivalent to post; section 7.6 has the 2-pass check.

Other quality concerns, to look for in the headset:
- NR runs on jittered render frames without motion vectors; SR then accumulates frames whose
  invented detail changes from frame to frame: possible extra shimmer or ghosting in motion (or SR
  averages it away, as the still images suggest).
- Hybrid: eye R gets eye L's detail moved by the disparity (consistent eyes), but the warp's hole
  fill (from eye R's own albedo) is unprocessed: a faint texture change at disocclusions beside
  near objects.
- The fovea region's edge is upscaled with the image: still a seam, softened by SR.

### 7.6 Two passes

`nrplace --env BB_XR_DLSSNR_PASSES=2` (`out/stereo_ab/run25presr/nrplace2pass`, same spot and
method; pass 2 on the inner 40 %) [M]:

| mode | setting | busy ms | NR ms | fps |
|---|---|---|---|---|
| hybrid | DLSS only | 7.35 | - | 126 |
| hybrid | NR post, 2 passes | 22.37 | 15.16 | 44 |
| hybrid | **NR pre-SR, 2 passes** | **13.64** | **6.17** | **72** |
| full | DLSS only | 10.08 | - | 97 |
| full | NR post, 2 passes | 25.09 | 15.14 | 40 |
| full | NR pre-SR, 2 passes | 21.94 | 11.96 | 45 |

The second pass costs about 2.8 ms pre (on 768x432) against 3.0 ms post: again mostly the fixed
cost per evaluation. Images (`nrplace2pass/png/crop_hybrid_off_post1_pre2_post2.png`: DLSS only |
post 1 pass | pre 2 passes | post 2 passes): pre with 2 passes brings the structure back to about
post-1-pass strength (cloth weave, stone), a little grainier, and in hybrid it still costs less
than post with 1 pass (6.2 against 8.9 ms). So the comparable hybrid settings are **post 1 pass
(8.9 ms) against pre 2 passes (6.2 ms)** or pre 1 pass (3.4 ms, weaker).

### 7.7 Headset A/B

Hybrid stereo, DLSS quality, NR on, 1 pass: `dlssnr_place=post` against `dlssnr_place=pre` (live
through the control file; each switch resets both histories). Look for: fps; whether pre's effect
is visible at all at intensity 2 / structure 1; then pre with 2 passes against post with 1; shimmer
on cloth and stone in motion; stereo agreement (pre in hybrid should agree better than post, whose
eyes invent their own detail); the fovea seam. Then full stereo the same way.
