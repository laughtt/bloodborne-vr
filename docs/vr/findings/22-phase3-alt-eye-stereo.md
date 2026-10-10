# 22: Phase 3, alternate-eye stereo (BB_XR_MODE=alt)

Date: 2026-10-09. PLAN Phase 3: the first real stereo. Each game frame renders one eye, L, R, L, R,
from that eye's own camera; the headset shows each eye's latest image with the view it was rendered
with, and the runtime reprojects the older eye. Mono (`BB_XR_MODE=mono`) stays the default.

Code:
- **Eye choice and offset (game thread, H1/H2):** `src/runtime_vr_stereo.c`, interface `src/vr_stereo.h`,
  pure math `src/vr_stereo_math.h` (tests `tests/test_vr_stereo.c`, target `vr-stereo-test`). Hook points
  in `src/runtime_vr_head.c`: `vh_stereo()` as the last step of H1's camera (third and first person),
  `vr_stereo_h2()` after H2 composed the head; `BB_XR_MODE=alt` also selects the both-eyes frustum
  (`BB_VR_HEAD_FOV=hmd`) by default, as mono does.
- **Pairing (GPU thread) and delivery (present thread):** `gpu/shadps4/video_core/renderer_vulkan/
  vk_xr_stereo.{h,cpp}` (ring, log, trace), pure logic `gpu/xr_bridge/xr_stereo.h` (tests
  `gpu/xr_bridge/xr_stereo_test.cpp`, target `bbport_xr_stereo_test`); `Presenter::PrepareFrame`
  pairs, `Presenter::Present` copies the frame to its eye (`vk_presenter.{h,cpp}`); `Xr::AcquireEye/
  ReleaseEye/HasEyes` (`vk_xr.{h,cpp}`); `bbgpu_vr_set_eye_view`, `bbgpu_vr_stereo_status`
  (`gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`); `CameraMotion::CurrentProjection/CurrentShadow`
  (`vk_camera_motion.h`: the eye tag and the shadow-cascade check).
- **Bridge ABI 6:** `AcquireEye`/`ReleaseEye` + `BbXrEyeView` (`gpu/xr_bridge/bbport_xr_bridge.{h,cpp}`):
  one swapchain per eye, the alt projection layer, the pacing log.
- **Off-axis hooks (optional):** `bb_hook_vr_proj_setcamera` / `bb_hook_vr_proj_pass` in
  `src/runtime_hooks.c` (handlers registered with `bb_hook_set_stereo_impl`, `src/hooks.h`), the group
  "VR Stereo Off-Axis (projections)" in `patches/Bloodborne.xml` (disabled by default); test
  `tests/test_vr_stereo_hooks.c` (target `vr-stereo-hooks-test`).
- **Flat test:** `tools/vr_stereo_test.py` (parallax, negative control, long run, trace, pacing,
  off-axis, `--fp` first person).
  `BB_FRAME_DUMP_COUNT=<n>` (presenter) dumps n consecutive frames per `BB_FRAME_DUMP_TRIGGER`.

Legend: **[M]** measured (flat test, fake head; the frame dumps and the trace), **[U]** unit test,
**[C]** confirmed in code or the disassembly, **[S]** design, not exercised in the headset yet.

## 1. Results

| Question | Answer |
|---|---|
| Does each frame render one eye, alternating? | **Yes [M]**: 14822 eye frames in a 287 s flat run (walking, turning, a moving head), L 7411 / R 7411, eye = frame parity for every one, longest run of one eye 1. |
| Is the eye offset right (sign, size)? | **Yes [M]**: the hunter 4 m ahead shifts **+20-22 px** right in the left eye (expected f*IPD/Z = 21.6 px at the game's 43 deg), 4.2x at a 250 mm IPD (expected 3.97x), the distant garden and sky 0 px, the screen-fixed HUD 0 px, frames of the same eye 0 px. The inverted control gives -22 px. |
| Does every frame reach the eye it was set up for? | **Yes [M]**: paired by its camera at the flip (rows, position, and an eye tag in the projection's x scale); **0 frames on the wrong side** of the center camera; seq always increasing; nothing given twice; the negative control flags 271/272 frames. Lost, repeated, late frames and strafing at one IPD per frame never swap eyes [U]. |
| Frustum | The symmetric frustum containing both eyes (110.2 x 89.1 deg on the Dream Air), the same for both eyes; the layer gets exactly that frustum and the eye's own pose. Off-axis eye frusta (optional hooks, off by default): the geometry is exact (0 px residual), a distant-haze pass still differs (section 6). |
| Pacing (72 Hz display) | 60 FPS: each eye 30 Hz, image age 0/1/2 display frames 42/42/17%. **72 FPS (uncap + `BB_FPS_LIMIT=72`): each eye 36 Hz, age 0/1 = 50/50%, never both eyes stale** [M]. |
| Culling / LOD / shadows | About 1 draw in 900 differs between the eyes (mono 0.03); frustum differences lie outside the other eye's view; the shadow constants differ slightly per eye (section 5). |
| Temporal effects | Motion blur smears every alt frame (the "previous camera" is the other eye); FSR 3 softens slightly; off with `BB_PATCHES` + `BB_UPSCALER=off` [M]. |
| First person | **Yes [M]**: the offset is applied after the first-person eye and the head; a wall 0.8 m ahead shifts +106 px, the inverted control -112, 0 frames on the wrong side (section 3, run 11). This needed a fix in the scene-camera tracking (section 2.5). |
| Bridge | ABI 6: `AcquireEye`/`ReleaseEye`, one swapchain per eye, each shown with its own view; loading screens and menus as mono. Not yet run in the headset [S]. |

## 2. Design

### 2.1 Data flow

```
H1 (game thread, once per game frame)    the camera as for mono: head rotation (level), first person,
                                          head position, the frustum that contains both eyes
  vrs_schedule_next                       eye = L, R, L, R ... counted per rendered frame
  vrs_apply_offset                        camera += the eye's offset from the head (xrLocateViews,
                                          head frame) along the camera's own axes, AFTER everything else
  vrs_tag_aspect                          right eye: aspect x (1 + 4e-5)   (the eye tag, 0.04 px)
  bbgpu_vr_set_eye_view                   {seq, eye, rows, position, center, layer pose, frustum}
                                          -> XrStereo ring (16)
H2                                         the same world offset and aspect for the main context
GPU thread, PrepareFrame (flip order)     the frame's scene camera (constants: view 3x4, P00)
  Pairing::OnFrame                        oldest view newer than the last frame given to an eye that
                                          matches rotation (2e-3), position (2 mm) and tag (1e-5)
                                          -> Eye(e) | Hold (no camera, repeat, unmatched) |
                                             Mono (30 held frames in a row: menus, loading)
Present thread                            Eye: AcquireEye(e) <- the frame image (no letterbox, no
                                          overlay), alpha 1, ReleaseEye(e, pose, frustum)
                                          Hold: nothing (both eyes keep their image)
                                          Mono: AcquireFlat/ReleaseFlat(View) as BB_XR_MODE=mono
bridge frame loop (72 Hz)                 projection layer: eye e = its chain's latest image with ITS
                                          pose and frustum; mono layer while the flat image is newer
```

### 2.2 The eye (H1)

- **Chosen when the camera is set**, never later: the eye travels with the frame's camera. Strict
  alternation per rendered frame, so a lost or late frame delays its eye, it never swaps the order.
  Starvation guard (`VRS_STARVE_RUN`): if the presenter received 4 frames in a row for one eye, the next
  two frames go to the starved eye (a one-frame phase shift); two shifts within 64 frames switch to
  pairs (L L R R), which feed both eyes under a 2:1 decimation of either phase, and the schedule returns
  to L R L R when the presenter sees whole pairs again [U]. In the flat runs the guard never acted
  (eye = seq parity for every frame) [M].
- **Offset:** `conj(head) . (eye - head)` from one `GetViews` snapshot, in the head's frame, mapped to
  the camera's axes (`x right, y up, -z forward` -> game `x, y, +z`), so it follows the composed camera
  (head rotation and, in first person, the eye at the head bone) [U]. A runtime offset is used when it
  is 15-75 mm to the side and within 40 mm up/down and fore/aft; else the default IPD (63 mm,
  `BB_VR_STEREO_IPD`). The Pimax log gives IPD 62.5 mm, displays 0 deg apart. `BB_VR_STEREO_SCALE`
  scales the camera offset only (world scale); the layer pose keeps the real eye.
- **Frustum:** the symmetric frustum around the head axis that contains both eyes (the mono "hmd"
  frustum: Dream Air 110.2 x 89.1 deg, aspect 1.428), the same for both eyes, so the LOD scale, the LOD
  fix and the culling shape are identical; each eye uses 87% of the image width (it sees -55..46 deg of
  +-55) [U]. The layer gets exactly this frustum (and the eye's own pose), the same principle as mono's
  ReleaseFlatView, so the compositor places the image right whatever frustum it was rendered with. An
  off-axis per-eye projection is section 6.
- **Eye tag:** the right eye's aspect is raised by a relative 4e-5. The finished frame's projection x
  scale (scene constants [52]) then says which eye it is even when the two eyes' cameras coincide:
  strafing at exactly one IPD per frame without turning makes R(n+1) = L(n), and a lost frame n would
  otherwise send frame n+1 to the left eye [U]. Invisible: 0.04 px at the edge of a 2000 px image; the
  LOD fix keys on the FOV, which stays.

### 2.3 Pairing (presenter)

- At the flip (`PrepareFrame`, GPU thread, flip order) the frame's scene camera (the first 864-byte
  scene constant buffer of its G-buffer pass, `CameraMotion`) is matched against the ring: the camera
  position is `-R^T t` of the view [C, `vk_camera_motion.cpp` Multiply]. Oldest-first among views newer
  than the last frame given to an eye: in order, so a repeated flip is held ("repeat"), and a still
  scene (all L views identical) still pairs each frame with its own seq [U][M].
- **Side check**, independent of the label: the frame's camera must be on its eye's side of the
  center camera (the camera before the offset) along the right row. A real swap shows here; the
  negative control (`invert=1`) proves the check fires [M].
- Held frames go nowhere; 30 held frames in a row (no camera: loading, menus without a scene;
  unmatched: a camera H1 did not set) switch to the mono path until an eye matches again [U][M].
  Coming back, the bridge shows the eyes only when both have an image newer than the latest mono image,
  so a fresh eye is never paired with one from before a loading screen [U].
- Needs the camera tracking: `BB_UPSCALER=none` turns `CameraMotion` off (no scene camera: every frame
  held, then mono). Use `BB_UPSCALER=off` to drop temporal AA.

### 2.4 Bridge (ABI 6)

- `AcquireEye(eye, w, h, format)` / `ReleaseEye(eye, BbXrEyeView{display_time, pose, fov, seq})`: one
  swapchain per eye at the frame size (the presenter's frame image, 1920x1080 with a 1080p window),
  sRGB of the window format, alpha forced to 1 (`MakeFlatOpaque`).
- The frame loop: app stereo (ABI 4) > **alt layer** (both eyes newer than the latest flat image, the
  newest under 1 s old) > mono (flat image, also used by alt between eye frames) > background. Each eye
  of the projection layer is its chain (the compositor shows a chain's last released image) with its
  own pose and frustum; no cinema screen while the eyes or mono show.
- `AcquireEye` returns -1 when the bridge shows no eye images (not alt, or the runtime refused the
  layer: `DropOptionalLayer` then falls back to mono); the host then sends every frame as flat.
- Every 10 s: `VR: alt: N of M display frames showed the eyes; eye images L a R b; age in display
  frames L mean x max y, R ...; one eye new p%, both new q%, both old r%` (the headset's pacing).

### 2.5 First person

H1's first-person path (`vh_first_person`, findings/16) builds the camera at the hunter's eye with the
head's rotation (level) and position, writes the FOV, then `vh_stereo` adds the eye offset last; H2
copies H1's camera from before the offset (and gets the offset only with `BB_VR_STEREO_H2=eye`).

**Bug found and fixed on the way [M]:** the first-person near plane (0.2 m) changes the scene constants'
first value from exactly 3000 to 3001.49 (it is derived from near and far), and `CameraMotion::
OnConstants` required exactly 3000: in first person the port never found the scene camera. That broke
the alt pairing (every frame "no camera", run 5), and also the mono pairing (ABI 5) and the TAA/FSR
motion vectors in first person. `OnConstants` now accepts 3000 +- 100 (the 1/far and render-size checks
stay). The Phase 4 stereo code (`vk_stereo.cpp`, another track) has the same exact `== 3000.0f` test.

## 3. Evidence (flat, fake head)

`tools/vr_stereo_test.py` on a copy of `user/` (the Hunter's Dream save), fake head file, scripted pad,
`out/gpu/bb-probe-st.exe`, frame 1920x1080, `BB_PATCHES` = motion blur, game AA, DoF and CA off,
`BB_UPSCALER=off` (section 5). Each step dumps 4 consecutive frames; the presenter logs each dump's eye.
Disparity = x_L - x_R per 32x32 block (NCC, +-128 px); "near" = the center-bottom region (the hunter
in third person), "hud" = the item boxes (screen-fixed). Expected for the game's 43 deg FOV:
f = 1371 px, a point 4 m away 21.6 px at IPD 63 mm, 85.7 px at 250 mm, 1.7 px at 50 m.

**Run 2** (`--fps 60 --long 180`, 287 s, all 12 checks pass) [M]:

| Step | Pairs (eyes of the 4 dumps) | near | p5 / p50 / p95 | hud | same eye 2 apart |
|---|---|---|---|---|---|
| alt (third person, 43 deg) | L R L R | **+20** | +10 / +18 / +28 | 0 | 0 / 0 |
| alt_hmd (110 x 89 deg frustum, f = 670 px) | R L R L | +12 | +2 / +10 / +26 | 0 | 0 / 0 |
| alt_ipd250 | R L R L | **+84** (x4.2; expected x3.97, 85.7 px) | +38 / +76 / +110 | 0 | 0 / 0 |
| mono (alt=0) | mono x4 | 0 | 0 / 0 / 0 | 0 | consecutive 0 |
| invert (negative control) | L R L R | **-22** | -28 / -18 / -10 | 0 | 0 / 0 |
| notag (pairing by the camera only) | R L R L | +22 | +10 / +18 / +28 | 0 | 0 / 0 |
| alt_far (head turned 180, 12 up: fog, trees, sky) | L R L R | +4 | **0** / +4 / +82 | 0 | 0 / 0 |
| mono_far | mono x4 | 0 | 0 / 0 / 0 | 0 | consecutive 0 |
| alt_turned (head 25 left, 10 down) | R L R L | +20 | +4 / +20 / +28 | 0 | 0 / 0 |
| long run, 10 pairs after walking and turning | L/R | +8 .. +122 (all > 0) | p5 >= 0 | 0 | |

- The hunter 4 m ahead shifts 20-22 px (expected 21.6), a 250 mm IPD scales it 4.2x, the distant
  garden and sky do not shift (p5 0), the HUD never shifts, two frames of the same eye never shift,
  and the inverted control flips every sign: **the left eye's image is rendered from the left**, with
  the right geometry.
- Trace (one line per finished frame): 17299 frames, **14822 given to an eye (L 7411, R 7411)**,
  held 87, mono 2390 (title screen and loading 1934 "no camera", the two mono steps 543 "unmatched");
  **eye frames on the wrong side of the center camera 0**; seq always increasing; eye = seq parity for
  every frame (the guard never acted); **same eye twice in a row 0 times**, longest run 1; camera set
  up to flip: 1 game frame. Negative control: 271 of its 272 eye frames flagged by the side check (the
  first one is the switch frame).
- The 10 s summaries in the long run: `L 300 R 301, held 0, same eye twice 0, eye tag kept apart 0,
  side check failures 0`.

Run 1 (`--fps` unset: uncap with the desktop's 60 Hz limit, no long run) gave the same numbers (near
+20..22, x3.91 at 250 mm, 2753 eye frames L 1376 R 1377, wrong side 0, longest run 1).

**Run 7** (`--fps 72`, all steps, 105 s) [M]: the same picture at 72 FPS (near +20..22 px, x3.91 at
250 mm, far p5 0, HUD 0, same eye 0, mono 0, inverted -22); trace 4736 eye frames (L 2368, R 2368),
wrong side 0, longest run 1, lag 1; negative control 320 of 321 flagged. **H2 at the eye** (`h2eye`:
the main context, which the LOD reads, at the eye instead of the center): the same disparity, and
consecutive L/R frames differ by 0.99 draws, as with H2 at the center (0.99): no measurable LOD or
culling effect in this view. The center stays the default (the same LOD distances for both eyes by
construction, section 5).

**Run 11, first person** (`--fp --fps 72`, `BB_VR_FP=1` with the FP heading hook, after the
`CameraMotion` fix of section 2.5; 69 s, all 5 checks pass) [M]: the hunter faces a low stone wall
under 1 m away; L/R pairs near **+106 px** (f IPD / Z = 107 px at 0.81 m), looking 60 deg down at the
ground +72..74, p95 at the +-128 px search limit (closer than 0.7 m); frames of the same eye 0; mono 0;
the inverted control **-112**; HUD 0. Trace: 2548 eye frames (L 1275, R 1273), wrong side 0, seq always
increasing; the left eye got two frames in a row once, right after the loading screen (seq 107 and 109:
the right-eye frame 108 between them had no scene camera yet and was held, as designed); negative
control 327 of 328 flagged; 71.6 FPS, each eye 36 Hz, age 0/1 display frames 50/49%.

Unit tests: `vr-stereo-test` 74 checks, `vr-stereo-hooks-test` (6 builds through the real windows),
`bbport_xr_stereo_test` 39 checks, `vr-head-test` 2067, `hooks-test`, `bbport_xr_math_test` 2679: all
pass [U].

Pictures (side by side L | R, red-cyan anaglyphs, disparity maps: red near, grey zero, blue negative,
dark no match) are in the run folders `png/` (session scratch, not in the repository); the far view's
map shows the sky and the distant pillars grey, the stone lantern 3 m ahead bright red.

## 4. Pacing

Each eye refreshes at half the game's frame rate. The trace's flip times give each eye's update
intervals; a 72 Hz display sampling the newest image of each eye then gives its age in display frames
(`tools/vr_stereo_test.py` `pacing()`; the presenter's copy and the compositor's own latency are a
constant shift on top) [M]. The bridge logs the same ages in the headset (`VR: alt: ... age`) [S].

| Game rate (flat runs) | Frame interval median / p99 | Each eye | Eye interval median / p99 | Age at 72 Hz: 0 / 1 / 2 display frames | Mean age | Both eyes older than 1 display frame |
|---|---|---|---|---|---|---|
| 60 FPS (`BB_FPS_LIMIT=60`, run 2, 205 s) | 16.7 / 18.1 ms | 30.0 Hz | 33.3 / 34.8 ms | 42% / 42% / 17% | 0.75 | 17% |
| 72 FPS (uncap, `BB_FPS_LIMIT=72`, run 3) | 13.8 / 15.4 ms | 36.1 Hz | 27.8 / 29.4 ms | 50% / 48% / 2% | 0.52 | 4% |

- **72 FPS is the better cadence for the 72 Hz Dream Air**: every display frame gets exactly one new
  eye image (the eyes take turns), each eye is at most one display frame old. At 60 FPS 5 eye images
  arrive per 6 display frames: every sixth display frame refreshes neither eye and the ages wobble
  0-1-2 (beat at 12 Hz), which the reprojection hides for rotation but not for animation.
- The flat runs held 72.0 FPS (worst frame 15.7 ms) in the Hunter's Dream on this PC; the headset adds
  the copy into the eye swapchain (one 1920x1080 image per frame instead of the window image) and
  costs nothing else (one eye per frame is the same rendering as mono).
- "uncap" without `BB_FPS_LIMIT` limits the game to the desktop monitor's refresh (60 here,
  `EmulatorSettings::GetFrameLimit`): set the limit to the headset's rate. Above it (no limit) eye
  images arrive faster than the display shows them: some are never shown and the cadence becomes
  irregular. 144 FPS (both eyes at 72 Hz) is out of reach: GPU time, and the game's movement timing
  breaks above about 120 FPS (findings/08 open #5).
- Latency: flat, a frame reaches its flip 1 game frame after H1 set its camera (trace lag median 1);
  the head pose is the bridge's latest prediction at H1, so the image is about 1-3 game frames older
  than the pose's display time; the runtime reprojects that (rotation), and the older eye is one more
  game frame behind.
- Pairing never lost a frame to the pacing: every finished frame went to its eye (lag 1, no repeats,
  no holds outside loading screens and the mono steps).

## 5. Culling, LOD, shadows, temporal effects

Measured from the trace (draws per frame, the shadow block of the scene constants) over the 90
frames before each dump of a still scene, alternate eyes against the mono camera in the same view [M]:

| | Consecutive frames differ by (draws) | Shadow block (floats 112-175), consecutive frames | Same eye two frames apart |
|---|---|---|---|
| alt, third person (905 draws/frame) | **1.08** (max 2) | max abs diff **0.0032** | 0 / 0 (identical) |
| mono, same view | 0.03 (max 1) | 0 | 0 |
| alt_hmd (110 x 89 deg, 960 draws) | 3.62 (max 7) | 0.0014 | 0 |
| alt_far / mono_far (423 draws) | 0.21 / 0.09 | 0.0032 / 0 | 0 |
| long run, moving (600-900 draws) | 3-5.5 | 0.004 | |

- **Culling and LOD.** The eye offset changes about one draw in 900 (0.1%; 0.4% with the wide headset
  frustum); mono is stable to 0.03. Both eyes use the same FOV and aspect (to 4e-5), so the LOD scale
  (ctx+0x150) and the LOD fix are identical for them; what differs is the camera position (+-31 mm):
  - frustum culling: the eyes' frusta are the same shape translated sideways, so only their left and
    right planes differ (the top, bottom, near and far planes contain the sideways direction). Those
    planes lie outside the other eye's view with the both-eyes frustum (each eye's inner edge is 9 deg
    inside the image), so a prop culled in one eye only cannot show as flicker in the overlap;
  - distance tests (LOD selection, small-object fade/cull): an object whose distance is within about
    3 cm of a threshold can take a different LOD in each eye. Transient while moving; a prop exactly at
    a threshold while standing still would show a different mesh per eye. Not seen in the flat dumps
    (same-eye frames identical, L/R pairs only shifted).
  - **How LOD is chosen [C]:** `CSLod::Update` 0x2141d60, tail-called by `SprjDrawStep::STEP_Update`
    right after the camera copies (H1/H2, same thread), takes the **main context** (`[[0x5940298]+0x10]
    +0x50`, the one SetCamera builds at 0x22de3a2 from **H2's** camera) and, per registered node
    (0x2142ef0), the distance from ctx+0x80 (the camera position) to the node's box times ctx+0x150
    (the LOD scale), with **hysteresis** per node (0->1 needs d >= t0+h0, 1->0 needs d < t0-h0, ...).
    ctx+0x120/+0x130 are the view's depth row and go to the GPU, not to LOD.
  - **So H2 stays at the center (the default, `BB_VR_STEREO_H2=eye` to move it):** the LOD distances
    are then exactly those of the center camera, the same for both eyes, and no hysteresis band can
    flip-flop with the eye. H2's context draws nothing visible (findings/15: H2 alone changed nothing
    in the picture); its other consumer may be the HUD markers, which then sit at the center like the
    HUD. Measured: section 3 (run 7, `h2eye` step).
- **Shadows.** The shadow block (four 4x4 matrices) is identical for the same eye every other frame and
  differs between the eyes in two columns only (e.g. matrix 2 row 0: -4.94392 / 4.94418 for L, -4.94708 /
  4.94734 for R; the last row of each matrix is 0 0 -19.97 19.97).
  - **What those matrices are [C/L]:** clip-to-shadow matrices relative to the camera, M = P^-1 . C .
    S (the last row is the inverse projection's w row: 1/B, -A/B with B = -0.0500679), so they differ
    per eye even when the shadow map is the same. Columns 2/3 hold -19.97 q and fwd.S + 19.97 q, q =
    the camera position in shadow clip space; col2 + col3 is 0.00026 in both eyes (orientation only).
  - **The cascade fit follows the camera, texel-snapped [C]:** the uniform fit 0x12b1ef0 (4 cascades,
    via the shadow-scene handler 0x12acb30) builds a light-space box around the context's frustum
    corners (camera position = ctx+0x80), then, with AlignTexel (default on), a fixed-size square
    whose x/y origin is floored to whole texels (vroundsd at 0x12b252d/0x12b253b); z is effectively
    continuous. Our numbers fit that: for matrix 0 a light camera that stayed put would have moved the
    eye 0.0098 NDC (20 texels of 4096) in shadow x; the observed change is 3.5e-5 NDC (0.07 texel):
    the square moved with the eye by whole texels. Matrix 2: 2.7 texels expected, 0.32 observed (a
    3-texel snap).
  - **Consequence:** the two eyes' shadow maps sample the world on the **same texel grid**, shifted by
    whole texels, so a shadow edge in the overlap is rasterized identically for both eyes; only the
    cascade's outer edge (a few cm of coverage) and the cascade split distances (measured from each
    eye, +-31 mm) differ. No shadow shimmer between the eyes is expected. If it shows anyway, the fit's
    camera position can be replaced by the center (hook windows 0x12b1f50, 0x12b1087, 0x12c870f,
    16 bytes each, keyed by the eye's position bits: the fit may run a frame behind on another thread).
- **Temporal effects ghost under alternation [M].** Hunter crops of L frames vs mono frames, same views
  (horizontal / vertical gradient energy):

  | Settings | alt (L frame) | mono |
  |---|---|---|
  | game effects off (`BB_PATCHES`), `BB_UPSCALER=off` | 3.81 / 4.36, sharp | sharp |
  | game effects off, FSR 3 (the port's default upscaler) | **3.26** / 4.05: softer sideways | 3.85 / 4.54 |
  | game defaults (motion blur, AA, DoF, CA on) + FSR 3 | **2.37** / 3.54: smeared sideways | 2.72 / 3.87, sharp |

  The game's **motion blur** reads the previous draw camera (RendMan +0x2980, findings/08 3), which is
  the other eye's, 63 mm away: every frame looks like a sideways camera jump and the hunter smears by
  several pixels. The port's **FSR 3/TAA** reprojects its history from the other eye: disocclusions at
  every depth edge and view-dependent shading make it reject or blend, a mild sideways softening
  (-15% horizontal gradient energy). The **game AA** is not temporal (FXAA/DLAA types; the AA pass
  takes no previous camera [C]), so it is harmless here but adds blur. The previous-camera copy
  (0x22de1c0) also feeds GX passes that may reproject SSAO ("UseReProj", "PrevFrame Strength" [L]);
  if AO looks unstable in the headset, add `Disable SSAO`. **Turn off**: motion blur, game AA (and DoF, CA for VR anyway) with
  `BB_PATCHES="Disable Motion Blur (perf increase);Disable AA;Disable DoF;Disable Chromatic Aberration"`
  (or `effect_motion_blur=0`, `effect_game_aa=0`, `effect_dof=0`, `effect_chromatic_aberration=0` in
  bbport.ini / the launcher); the port's TAA with `BB_UPSCALER=off` (or the menu). Keep FSR 3 only if
  the performance is needed and the softening is acceptable. A proper fix is per-eye history
  (`EyeState x2`, findings/09 3.4).

## 6. Asymmetric (off-axis) per-eye frustum

**Why:** the Dream Air's eyes see -55..46 deg (left) and -46..55 (right). The symmetric frustum spends
2 tan 55 = 2.856 of tangent width per eye where the eye needs tan 55 + tan 46 = 2.464: 13.7% of the
image's columns lie outside the eye's view [U]. Off-axis rendering puts all of them in view (16% more
horizontal pixels per degree for the same image).

**How (implemented, optional, off by default):** the game's projections are symmetric (`0x25aa6c0`,
SetCamera's builder; `0x25aa290`, 13 passes' builder incl. the scene view 0x2690dd0 and DoF;
`0x25aaa90` wraps it) [C]. With `BB_VR_STEREO_OFFAXIS=1` H1 gives each eye's camera the width of its
own frustum (aspect = (r - l) / 2 ty; the vertical stays the both-eyes ty, so the FOV, the LOD scale
and the LOD fix do not change) and publishes, per eye, the camera's FOV and aspect, the x and y scales
its projection will have and the shift P20 = -(r + l) / (r - l). The hooks **"VR Stereo Off-Axis
(projections)"** sit at both builders' epilogues (0x25aa933, 14 bytes, out = r15; 0x25aa69c, 18 bytes,
out = r14, camera = rbx; verified clean, both end in the builder's `ret`, which the trampoline replays
after the handler):
- **SetCamera's builder** (the camera pointer is gone at its epilogue): a projection whose x and y
  scales are an eye camera's gets that eye's P20 (the eye tag tells the eyes apart; the shadow and
  other cameras do not match). SetCamera extracts the culling planes after its builder returns, so
  culling follows the off-axis frustum.
- **The passes' builder:** the camera itself is matched (its FOV and aspect bits, +0x50/+0x64, are
  those H1 left for an eye). **The main view goes through the builder's tile path with one tile**
  (+0x70 = 1: index 0, the whole 1920x1080 frame), and so do three depth slices of the same view
  (near/far 0.1-2, 2-8, 8-15 m) [M, run 9]. For a smaller tile the shift scales with the tile's x scale
  against the whole frame's. (The first version skipped the tile path and so shifted the culling but
  not the picture: run 7, residual shift up to +-48 px.)

The layer gets the eye's own frustum (`vrs_fov_shifted`); the pairing's tag check uses the half-width
tangent, which also holds off-axis. Without the hooks H1 stays symmetric (it waits until a builder hook
is seen to run). `tests/test_vr_stereo_hooks.c` runs both real windows through the trampoline (shift
by scales and by camera, the tile scaling, another camera untouched, the return value and the
callee-saved registers intact) [U].

**Evidence (run 10: `--fps 72 --only alt,axis_sym,axis_off`) [M].** The same view rendered with the
symmetric both-eyes frustum (`axis_sym`) and with the off-axis eye frusta (`axis_off`: left -54.6..46
deg, right -46..54.6); the symmetric image is resampled onto the off-axis frustum's columns and
compared with the off-axis render:

| | Left eye | Right eye |
|---|---|---|
| Residual shift of the geometry, p5 / p50 / p95 (NCC blocks) | **0 / 0 / 0 px** | **0 / 0 / 0 px** |
| Blurred difference, 0..255, HUD excluded (two same-eye frames: 0.1) | 4.9 | 5.4 |
| Low-frequency shading, render minus resampled: top half / bottom half | **-5.0** / -0.1 | **-8.1** / -0.8 |
| Frames paired to their eye, side check | all, 0 failures | all, 0 failures |

- **The geometry is exact**: the picture is the off-axis projection of the same scene, each eye's
  image covers its own -54.6..46 deg (16% more pixels per degree), and the pairing and the eye tag work
  off-axis. Each eye culls to its own frustum: consecutive L/R frames differ by 14 draws of 950
  (symmetric 3.8).
- **A haze component differs** (the shading check fails): the off-axis render has less haze on distant
  geometry in the upper half (the facade 20-30 m away, up to -20 of 255 over a 240x180 px region,
  `png/offaxis_*_shading_x8.png`), while near geometry, its lighting and its shadows match (bottom half
  within 1). It grows with distance and is not a shift of the geometry, so probably some pass still
  assumes a symmetric frustum, building its view rays or its own projection from the camera's FOV and
  aspect without the P20 term instead of through the two hooked builders (a volumetric fog or light
  scattering volume would fit). Such a pass takes each pixel's direction 4-10 deg (0.18 in tangent)
  away from its true one. Not found yet; until it is, **the symmetric frustum stays the default** and
  off-axis is an experiment (`BB_VR_STEREO_OFFAXIS=1` + the hook group).

## 7. Running it

### 7.1 Build, install

Both binaries change (bridge ABI 6) and ship together:
```
ninja -C out/gpu bb-probe vr-stereo-test vr-stereo-hooks-test   (MSYS2 CLANG64; BB_BUILD_LOCK = the shared build lock)
ninja -C out/xr-bridge bbport_xr bbport_xr_stereo_test
copy, with the game closed: out/gpu/bb-probe.exe -> out/bb-probe.exe, out/xr-bridge/bbport_xr.dll -> out/bbport_xr.dll
```
A new bb-probe works with an ABI 5 bridge (no eye images: mono), an old bb-probe with the ABI 6 bridge.

### 7.2 Hooks

`BB_HOOKS="VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context);VR LOD Fix (SetCamera)"`
- **H1** (required): the eye camera (head, first person, eye offset, eye tag).
- **H2** (recommended): the main context gets the same eye camera (its consumer may be the HUD markers).
- **LOD fix** (recommended): the game's LOD distances at the 110 x 89 deg frustum; both eyes share it.
- With first person (`BB_VR_FP=1`, findings/16) add `;VR FP Camera Heading (in-game step)`.
- Optional, experimental: `;VR Stereo Off-Axis (projections)` with `BB_VR_STEREO_OFFAXIS=1`: per-eye
  off-axis frusta (section 6: the geometry is right, distant haze differs).

### 7.3 Environment

| Variable | Effect |
|---|---|
| `BB_XR_MODE=alt` | alternate-eye stereo (headset with `BB_VR=1`; flat: the pairing, log and trace only) |
| `BB_XR_OVERSCAN=<v>[,<h>]` | mono/alt: render (and submit) a frustum larger than the eyes' union by v (vertical, default 0.15) and h (horizontal, default 0.10) of the tangents; 0 = off |
| `BB_VR_STEREO_IPD=<m>` | IPD without a headset, or when the runtime's eye offsets are implausible (0.063) |
| `BB_VR_STEREO_SCALE=<s>` | world scale of the camera offset (1) |
| `BB_VR_STEREO_H2=eye` | H2's main context at the eye instead of the center (default center: section 5) |
| `BB_VR_STEREO_OFFAXIS=1` | per-eye off-axis frusta; needs the hook group "VR Stereo Off-Axis (projections)" (section 6) |
| `BB_VR_STEREO_LOG=1` | H1: 1 line/s (eye, offset, aspect, schedule, presenter feedback); presenter: 10 s summaries (on by default with `BB_VR=1`); frame dumps log their shadow cascades |
| `BB_VR_STEREO_CONTROL=<file>` | live: `alt= ipd= scale= invert= tag= offaxis= eyel= eyer= h2=` (`vr_stereo_math.h`) |
| `BB_VR_STEREO_TRACE=<csv>` | one line per finished frame: frame, time, kind, eye, seq, lag, side, reason, draws, s1, s2 |
| `BB_FRAME_DUMP_COUNT=<n>` | n consecutive frames per `BB_FRAME_DUMP_TRIGGER` |
| `BB_PATCHES=...` | `Disable Motion Blur (perf increase);Disable AA;Disable DoF;Disable Chromatic Aberration` (section 5) |
| `BB_UPSCALER=off` | no temporal AA (not `none`: that stops the camera tracking the pairing needs) |
| `BB_FPS_LIMIT=72` | the game at the headset rate with the uncap preset (section 4) |

### 7.4 The headset command

PowerShell in `C:\bloodborne_vr` (the game closed, then the binaries copied as in 7.1):
```powershell
$env:BB_VR='1'; $env:BB_XR_MODE='alt'; $env:BB_VR_HEAD='1'; $env:BB_VR_HEAD_LEVEL='1'
$env:BB_HOOKS='VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context);VR LOD Fix (SetCamera)'
$env:BB_PATCHES='Disable Motion Blur (perf increase);Disable AA;Disable DoF;Disable Chromatic Aberration'
$env:BB_UPSCALER='off'; $env:BB_FPS_LIMIT='72'
python run.py
```
Second try: `$env:BB_FPS_LIMIT='60'` (or remove it and set `$env:BB_FPS='60'`). First person: add
`$env:BB_VR_FP='1'` and `;VR FP Camera Heading (in-game step)` to `BB_HOOKS`. Mono again:
`$env:BB_XR_MODE='mono'`. In Pimax Play turn **Smart Smoothing off** (section 8).

### 7.5 What to look for

- Log: `VR: mode alt`, `VR: left eye images 1920x1080`, `VR: first alternate-eye frame on the
  headset`, every 10 s `VR stereo: ... L n R n, ... same eye twice 0, ... side check failures 0` and
  `VR: alt: ... age in display frames L mean ... both old ...%`.
- Depth: the hunter about 4 m ahead and the stairs/railings in 3D, the sky at infinity. The HUD sits at
  infinity (zero disparity, section 8).
- **No swap**: the world must not look inside-out (near things far away). Close one eye, then the
  other: switching from the left eye to the right eye, near objects jump to the left, distant ones stay.
- Expected artifacts: moving enemies and the game camera turning with the right stick show doubled
  edges or false depth (temporal disparity, section 8); compare `BB_FPS_LIMIT` 72 vs 60.

## 8. Risks and known artifacts

1. **Temporal disparity (inherent to alternate eyes).** The two eyes show different game ticks, one
   frame apart (16.7 ms at 60 FPS, 13.9 ms at 72). Sideways motion becomes false depth: an enemy
   crossing at 3 m/s moves 5 cm per frame, close to one IPD, so it looks nearer or farther than it is,
   with doubled edges; the game camera turning (right stick, the follow camera's own rotation, lock-on)
   gives the whole world a disparity offset while it turns (90 deg/s: 1.5 deg per frame at 60 FPS).
   Head rotation is not affected: each eye is reprojected from its own pose. First person with snap
   turn: the snap frame pair shows one eye before and one after the turn for one frame. R.E.A.L. needed
   its AER v2 for this; Phase 4 removes it.
2. **Per-eye rate.** 30 Hz per eye at 60 FPS, 36 Hz at 72 (section 4). The stale eye is reprojected
   rotationally only (no depth layer yet): head translation parallax is up to two display frames old,
   animation judders at the per-eye rate. Some players see a 30 Hz alternation as flicker on
   high-contrast edges.
3. **Motion smoothing** (Pimax Smart Smoothing, ASW/SSW): these estimate motion per eye from
   consecutive submissions; each eye's image changes only every other frame, so the estimate sees
   alternating zero and double motion. Turn it off for alt mode (findings/09 4.3).
4. **Temporal effects reprojecting from the other eye** (section 5): motion blur, the game's AA and the
   port's TAA/FSR history all take the previous frame, which is the other eye's. Off by `BB_PATCHES`
   and `BB_UPSCALER=off`; the cost is aliasing (no AA at all).
5. **HUD at zero disparity.** The HUD is drawn into each eye at the same screen position, so it
   appears at infinity; nearer geometry then occludes something that looks farther away (a depth
   conflict) [M: hud 0 px]. Phase 5's HUD layer fixes it.
6. **Resolution.** The eye images are the presenter's frame (the window size, 1920x1080 here) stretched
   over 110 x 89 deg: about 17 px/deg horizontally, 12 vertically (the runtime recommends 3832x3076 per
   eye). Same as mono; see section 9.
7. **Pairing edge cases.** FOV changes (cutscene cameras): the old views of the same eye fail the tag,
   which is harmless (counted only when the other eye's view is rejected). A camera that H1 did not set
   (the debug free camera, a pass that rebuilds the scene constants from another camera) falls back to
   mono after 0.5 s. A scene camera without H1 (BB_HOOKS without H1): mono. `BB_UPSCALER=none`: no
   scene camera, mono.
8. **Starvation.** The present thread presents every flip in order (driver.cpp), so a systematic 2:1
   loss of one eye's frames should not happen; the guard and pair mode cover it if it does [U], and
   the 10 s summary's "same eye twice" and "longest run" show it.
9. **Culling/LOD/shadows** differ slightly between the eyes (section 5): about one draw in 900, and the
   shadow cascades follow the eye position. Not seen as flicker flat; watch shadow edges and small
   props at the edge of the view in the headset.
10. **Not exercised in the headset yet [S]:** the ABI 6 layer on the Pimax runtime (two swapchains, one
    released per frame), the age statistics there, comfort.

## 9. Notes for Phase 4

- **Pairing by camera works and carries over.** The presenter recognized every frame by its camera
  (rows, position, P00) with no failure in 17299 frames; Phase 4 (both eyes per tick) can use the same
  ring to tag D-dup's eye-R images, and the bridge's per-eye chains (`AcquireEye`/`ReleaseEye`, one
  release per eye per frame) already form a full stereo layer: with both eyes released every frame the
  age log shows "both new" 100%.
- **The eye tag in the projection** (a relative 4e-5 on one eye's x scale) is a cheap, invisible way to
  carry a per-image identity through the whole GPU pipeline when positions coincide; usable for any
  per-view identity (quad views too).
- **Measured inter-eye differences of a CPU camera offset** (what Phase 4's `M_eye` epilogue avoids by
  construction): culling about 1 draw in 900 (3-5 in 700 while moving), shadow cascade constants
  change with the eye position (section 5). Phase 4 renders shadows once from the center camera, as
  planned.
- **Per-eye temporal history is needed already** for any AA: the port's TAA/FSR/DLSS history and
  `CameraMotion`'s previous camera must be keyed by eye (`EyeState x2`, findings/09 3.4); the game's
  own AA and motion blur use the previous draw camera (RendMan +0x2980) and stay off.
- **Resolution:** take the eye image from the output-size display buffer (`TemporalUpscaler` display)
  rather than the window-size frame, and render at the eye frustum's aspect (1.43 for the Dream Air
  union frustum; findings/09 3.2 asks for a non-16:9 `BB_RENDER_RES` test).
- **Latency:** flat, the frame reaches the flip 1 game frame after H1 (the trace's lag); the mono
  headset log reported 3. The bridge predicts the head for its latest frame; H1 could ask for the views
  predicted 2-3 frames ahead (findings/09 open question 5).
- **Off-axis frusta** (section 6) belong to `M_eye` (exact for any frustum), not to the CPU camera.
  Either way, a pass that draws the distant haze apparently assumes a symmetric frustum (section 6:
  the geometry was exact, the haze was not): find it, and give it the eye's frustum, before any
  asymmetric projection is relied on.

## Lobby looked double, screen distance (headset run 1)

- **Cause [C, from code and log; not yet re-run in the headset]:** in alt mode every non-eye frame
  (title, menus, loading: "held ... no camera 771, mono 742" in `out/vr_all_alt.log`) took the *mono
  projection layer* of the bridge: the window image stretched over the 110 x 89 deg frustum of the
  newest render view, world-locked at the head pose of render time, with each eye at its own offset.
  A flat 2D image as a wide projection in two eye poses reads as double. Entering the game then
  switched projection (mono) -> projection (eyes), a second visible jump.
- **Fix:** the bridge uses the mono projection only for `BB_XR_MODE=mono`. In alt mode flat frames
  go to the cinema screen (cylinder or quad, `BbXr` screen layer): one image, identical in both eyes,
  head-placed. `AltEyes` already shows the eyes only once both have an image newer than the last
  flat one, so the transition is flat screen -> eyes without a mixed frame. 3D gameplay is unchanged.
- **Screen distance:** `BB_XR_SCREEN_DIST` (m) already existed. Default 2.6 -> 3.38 m (+30%), and
  `BB_XR_SCREEN_M` (width) 3.2 -> 4.16 m so the angular size stays the same (the screen is farther,
  not smaller); set `BB_XR_SCREEN_DIST=2.6 BB_XR_SCREEN_M=3.2` for the old placement.

## 9. Black spots when pitching (overscan)

Report (Dream Air, 72 Hz, 36 Hz per eye): black spots when looking up and down. Log: the runtime's
per-eye FOV is -55/46/45/-45 deg (L) and -46/55/45/-45 (R); the submitted union is 110.2 x 89.1 deg, so
the FOV is not too small (the union is the headset's). Cause: each image covers exactly the display
frustum at its render pose; the compositor rotates a 1-2 display-frame-old image (age max 2-6 in the
log) and the uncovered border is black. Fix: the head hook widens the union by `BB_XR_OVERSCAN`
(default 15% vertical, 10% horizontal of the tangents, about 126 x 98 deg here; mono and alt); the bridge
already submits the rendered view's FOV, so the compositor crops. Start log:
`VR head: render fov ... display fov ...`. Cost: the frame stays 1920x1080 (the game's frame size),
so the pixel density drops by 1/1.1 horizontally and 1/1.15 vertically; no extra shading. Needs a
headset run to confirm.
