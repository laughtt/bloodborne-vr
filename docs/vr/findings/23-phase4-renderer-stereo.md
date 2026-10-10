# 23: Phase 4, renderer stereo: the camera's path to the GPU, the approaches, the design, a proof of concept

Date: 2026-10-09. PLAN Phase 4: both eyes from the **same** game frame, at full rate, with correct
parallax. Phase 3 (alternate eyes, `BB_XR_MODE=alt`, findings/22) runs in parallel and is untouched.

Inputs: findings 03, 08, 09 (the earlier design, made without live data), 10, 11, 15, 16, 22; the eboot
(`C:/games/CUSA03173`, read-only) and its RE index from 08; the game's shader binaries
(`dvdroot_ps4/shader/*.shaderbnd.dcx`); the port's shader cache (`user/cache/CUSA03173`, 688 SPIR-V
modules, decompiled with spirv-cross); flat runs of the game on this PC (RTX 4090, Windows 11).

Code added (all inert unless enabled):
- **Research tool** `gpu/shadps4/video_core/renderer_vulkan/vk_stereo_census.{h,cpp}`: with
  `BB_STEREO_CENSUS=1` the frame that `BB_CAPTURE_TRIGGER` records is also written draw by draw as JSON
  lines (shaders, targets, sampled images, every bound buffer with its bytes).
- **Prototype** `vk_stereo.{h,cpp}` and `shader_recompiler/ir/passes/lower_stereo_remap.cpp`, on with
  `BB_XR_MODE=stereo`: a per-draw eye shift in the vertex shaders, and one frame's G-buffer pass drawn a
  second time into twin images (section 4).
- **Tools** `tools/vr_shader_reflect.py` (the game's shaders by hash, with their constant buffer
  layouts), `tools/vr_stereo_census.py` (census reader), `tools/vr_stereo_poc.py` (the two PoC checks).
- Hooks into core files, all additive: `vk_rasterizer.{h,cpp}` (calls next to the existing
  `FrameCapture` hooks, a `BufferType::StereoView` branch in `BindBuffers`, 3 member declarations),
  `vk_pipeline_cache.cpp` (1 line), `resource.h`, `runtime_info.h` (a bool in existing padding: the
  serialized layout and the disk cache are unchanged), `recompiler.cpp`, `ir_passes.h` (1 line each).

Legend: **[M]** measured in the running game (census, frame dumps, GPU profile). **[R]** read from the
reflection data of the game's shader binaries. **[C]** confirmed in code, decompiled shaders or the
disassembly. **[S]** design or estimate.

---

## 0. Summary

1. **The camera reaches the GPU through about 30 kinds of constant buffer, not one.** The game's shader
   binaries carry their reflection data, so every constant has a name [R]:
   - `cbSceneParam` (864 bytes, in 2359 of the 3382 shaders): view, inverse view-projection, inverse
     projection, projection, the four shadow cascades **premultiplied with the main camera**
     (`FC_ProjSpaceToShadowMatrix`), inverse view and a **camera-relative** view-projection
     (`VC_MatrixViewProj`).
   - **Per-draw CPU-premultiplied matrices**: `cbMatrixWVP` (79 shaders), `cbMatrixVP` (54),
     `cbMatrixCamera` (482), `cbTessScene` (175), the particles' billboard and camera matrices,
     decals' `FC_ProjToDecalSpace`.
   - **CPU-projected screen data**: light-volume rectangles (vertex data), light positions in view
     space (`cbLightAccLightParam`), the light-shaft screen center.
2. **Every perspective vertex shader can be moved to an eye with one add** [C][M]. The eyes share the
   center camera's rotation and symmetric projection P, and sit at e along its right axis, so
   `P·T(e)·P⁻¹` is `clip.x -= P00·e`, exact whatever buffer the matrices came from. A vertex shader is
   perspective iff its clip w is computed from buffer data: 197 VS + 8 TES + 3 GS of the cache; 69 are
   screen-space (full-screen, light rectangles, post, UI) and keep their positions [C].
3. **Recommendation: record-level double rendering with that eye shift** (approach (a) at the Vulkan
   record level, with (d)'s split): shadows and other camera-independent work once, the scene region
   (G-buffer to the end of post) recorded once and executed twice into twin targets, the UI once.
   Multiview is a later optimization; game-side double rendering is ruled out (it doubles the port's
   bottleneck, the PM4 thread).
4. **Proof of concept** [M]:
   - **Oracle.** The whole scene shifted by the vertex shaders as if the camera stood 31.5 mm right
     differs from the CPU camera really moved 31.5 mm (H1) by a mean **0.27-0.34/255** (3 runs) on
     non-animated pixels: noise is 0.22-0.27, the shift itself 9.8. With an 8x exaggerated 0.25 m: 1.13 against 17.3.
   - **Same frame.** One frame's G-buffer pass (240 draws, 8 targets) drawn twice into twins at
     ±31.5 mm: disparity median 18 px, max 36 px at 1080p. Eye R's albedo equals eye L's warped by depth
     to a mean 1.6/255 (16.0 unwarped). Depth per point is identical in both eyes (1e-4).
   - **Cost.** The remap shaders cost nothing measurable (GPU busy 3.5 ms/frame either way at 1080p).
     The naive per-draw replay of the G-buffer adds 0.8 ms at 1080p, 1.4 ms at 1440p (0.63/0.81 ms for
     the pass itself).
5. **Cost estimate** [S]: about 1.7 ms of camera-independent GPU work plus 0.9 ms per megapixel per eye
   (measured scaling) gives about 8.5 ms at 3.5 Mpx per eye, plus two upscales (DLSS ~1 ms each on this
   4090): **about 10.5 ms, inside 72 Hz** (13.9 ms). The CPU limit stays the PM4 thread, as in mono.
6. **Next** (section 5, about 8 to 10 weeks): record once and execute twice in the scheduler, twins
   for the whole region, compute replays, then per-eye temporal state and the eye submission that
   Phase 3 already built (`AcquireEye`/`ReleaseEye` twice per frame).

---

## 1. The camera's path to the GPU

### 1.1 CPU side

```
SprjDrawStep 0x22ddf30 (game thread)    H1 0x22de113 (PersCam copy) / H2 0x22de386 (findings/15)
  SetCamera 0x2691c30                     draw context: +0x10 view, +0x50 view*proj, +0x90 proj,
                                          +0x160 world planes (culling), +0x150 LOD scale (findings/08)
  CSLod::Update 0x2141d60                 LOD from the main context (H2's camera) (findings/22 sec. 5)
  draw-list build (GX)                    one scene-constant copy per draw record          [M]
GXRenderThread (object ctor 0x25b6620,    reads the records, writes Gnm command buffers,
  thread "GXRenderThread", 128 KiB stack) copies constants into the frame ring              [C]
frame ring (~0x1043400000..0x1049xxxxxx,  per command buffer: one GPU copy of cbSceneParam (17 per
  rewritten every frame, parallel_gpu.md) frame) the V#s of the draws point at              [M]
port stage A (PM4 decode)                 small read-only constants copied into the constant ring
                                          (vk_constant_ring.h, CollectRingBindings)          [C]
port stage B (DrawRecord/BindBuffers)     binds the ring copy per stage (VS and PS separately)
```

- **787 copies per frame** [M]. A memory scan of the guest heap during a census
  (`BB_STEREO_CENSUS_SCAN=1`) found the main camera block's first 80 bytes (far, screen size, view
  matrix) 787 times. They sit at 0x1032.. to 0x1034.., mostly 0x31d0 bytes apart: one per draw record of
  the engine's draw lists (the frame has ~980 draws and dispatches). The GPU then sees 17 ring copies
  with identical contents, one per command buffer.
- **The writer** of the per-draw copies was not pinned down statically: they are struct copies, not
  named. A write watchpoint on one record address from the scan finds it. **No stereo approach in this
  document needs it.**
- **The render thread** is created in 0x25b6620 (thread create 0x248afe0, name string 0x4d4022c,
  object vtable 0x575fbc0) with three events (0x247e690 init): the classic producer/consumer between
  the draw step and the GX command build.

### 1.2 The constant buffers, by name [R]

`tools/vr_shader_reflect.py` reads the reflection data of all 3382 shader binaries: member records of
36 bytes (flags, offset, size, ..., name), keyed by the `BinaryInfo.shader_hash` after "OrbShdr". That
hash is shadPS4's `pgm_hash`, so every hash in a log, capture or the shader cache now has a file name
and a constant layout.

**`cbSceneParam`** (also named `cbPerScene` in 6 filters), 864 bytes, floats. Values are from a
Hunter's Dream frame [M]:

| floats | member | value / meaning |
|---|---|---|
| 0-3 | FC_FarClipInfo | (3000, 1/3000, 0, 0) |
| 4-7 | FC_ScreenSize | (1920, 1080, 1/1920, 1/1080) |
| 8-19 | FC_MatrixView | 3x4, world to view (rows: right, up, forward; translation in .w) |
| 20-35 | FC_MatrixInvViewProj | clip to world, camera-relative (G-buffer PS: world position from the clip varying) |
| 36-51 | FC_MatrixInvProj | diag(1/P00, 1/P11), row 3 = (0, 0, -19.97, 19.97) |
| 52-67 | FC_MatrixProj | P00 1.428, P11 2.539, A 1, B -0.0501: D3D [0,1], near 0.05 |
| 68-71 | FC_DepthComputeParam | (1, -1.67e-5, -0.0501, 0): depth linearization |
| 72-111 | shadow parameters | FC_ShadowMapParam, Color, Dir, VC_ShadowMapMatrix, kernel scales |
| 112-175 | FC_ProjSpaceToShadowMatrix[4] | **main-camera clip space to cascade i**: P⁻¹·C·S_i (findings/22 sec. 5) |
| 176-179 | FC_CascadeSelectDist | (2, 8, 15, 0) m |
| 180-195 | FC_MatrixInvView | 4x4, view to world; **the camera position is floats 183, 187, 191** |
| 196-199 | VC_ClipPlane | (0, 0, 0, 1) |
| 200-215 | VC_MatrixViewProj | **P·R: no translation** (row 3 = the forward axis) |

The shadow pass binds the same layout with each of the four cascade cameras (far 103 to 140,
orthographic) [M].

**Other camera-carrying buffers** (member name, float offset, shaders using it) [R]:

| Buffer | Camera members | Shaders | Used by (this frame) |
|---|---|---|---|
| `cbMatrixCamera` | VC_MatrixCamera 0-15 | 482 FLVER | forward pass |
| `cbMatrixWVP` / `cbPrevObjMatrix` | VC_MatrixWVP 0-15, VC_PrevMatrixWVP 16-31, VC_NowToPrevView 32-47 | 79 / 47 | velocity pass (motion blur), half res |
| `cbMatrixVP` | (VP) | 54 | |
| `cbTessScene` | g_f4x4ViewProjection, g_f4Eye, g_f4ViewVector, g_f4ViewFrustumPlanes | 175 | tessellated FLVER |
| `cbLightAccParam` (208 B) | g_InvProjMatrix 0-15, g_ViewMatrix 16-27, g_ScreenSize 28-31, g_CameraMatrix 32-47 | 88 | deferred lights |
| `cbLightAccLightParam` | g_aPointLight[] {PositionAndRadius (view space), DiffColor, ...} | 84 | deferred lights |
| `cbLightAccVtxParam` | g_vtxParam (light rectangle depth) | 1 | light volumes VS |
| decal params | FC_ProjToDecalSpace 0-15, FC_EyeInDecalSpace 64-67 | 63 | deferred decals |
| particles (gxffxshader) | g_ds_viewProjectionMatrix, m_gBillBoardMatrix, m_gCameraMatrix, m_gCameraPosition, g_psViewMatrix, g_LightShaftScreenCenter, ... | 11 to 127 | particles, light shafts |
| post (gxposteffect) | g_mCurToPrevScreen, g_mInvProj, g_mView, g_vScreenSize | 3 to 27 | motion blur, SSAO |
| sky | g_mInvWorldViewProjection | 1 | |

`cbInstanceData` (416 bytes, VC_InstanceData, 2481 shaders) holds per-draw data. Its dwords 0-11 change
with the camera [M] (read by some G-buffer PS and as the TES's 4x4 at 0-15). Its dword 20 is the base
index into the instance and bone palettes.

### 1.3 How the vertex shaders place geometry [C]

All 284 vertex-stage shaders of the cache (272 VS, 9 TES, 3 GS: the game's working set after a few
runs), decompiled. The expression tree behind `gl_Position` was traced to its buffer reads, then joined
with the census (which buffer, which size, is bound at each binding):

| Class | Shaders | Position | Examples |
|---|---|---|---|
| Scene geometry | 176 VS | `clip = VC_MatrixViewProj · (p_world − cam)`; `p_world` from the instance matrix (12 dwords) or 4 bones (48 dwords) of world-space palettes; `cam` = cbSceneParam 183/187/191 | G-buffer, forward, shadow (cascade blocks), sky dome |
| Per-draw WVP/VP | 12 VS + 8 TES | a 4x4 at dwords 0-15 of a per-draw buffer (400 B particles, 192 B velocity pass, 416 B TES) | GXFfxsoftTexture, *_Velo, GXFfxtessellate* |
| Other perspective | 9 VS + 3 GS | a 4x4 at 48-63 (distortion), dynamically indexed matrices | GXFfxdistortion |
| Screen space | 69 VS + 1 TES | x, y, w from vertex data or constants; offsets at 20-21 (post), a 2x4 at 8-15 (Scaleform) | full-screen quads, light rectangles, post, UI |

- **The perspective criterion: w depends on buffer data.** That holds for 197 VS + 8 TES + 3 GS and
  never for the screen-space ones (6 VS write the position as a whole vector and were not classified)
  [C]. The light-volume VS (GXLightAcc.vpo) takes x, y, w from the
  vertex buffer (a rectangle the CPU projected) and only z from `g_vtxParam`. Post shaders read x
  offsets from a buffer but w = 1.
- **The clip position travels on.** 248 of the 284 copy position components into a varying unchanged
  (the G-buffer PS multiplies them by `FC_MatrixInvViewProj`). **31 compute a screen coordinate from the
  clip x in the vertex shader** (soft particles, distortion, Scaleform) [C].
- **Geometry shaders** exist (3 in the cache) but none drew in the captured frame. Tessellation: 37
  draws (tessellated particles), where the TES is the hardware VS stage and gets the same treatment.

### 1.4 The frame and which passes depend on the view [M]

Census of a Hunter's Dream frame (982 draws and dispatches, 1080p, names from 1.2):

| Draws | Pass | Camera input | Stereo class |
|---|---|---|---|
| 0-1 | display copy, depth clear | none | mono (frame start) |
| 2-119 | shadow 4096² (`*_DptA`): 4 cascades | 4 cascade cbSceneParam blocks, **fitted to the main camera** | **shared** (center camera) |
| 120-144 | 6 x 64x64 depth + `Sample13Weight` blur + CS 8b355b5a/a040875f | none found | shared |
| 145-384 | G-buffer (6-7 MRT + D32S8), `GXFlver_*_Gbuf*`, 240 draws | VS and PS: cbSceneParam | **per eye** |
| 385, 388 | CS `3d5ebf4e` (guest memcpy of depth/G-buffer memory, 8.4 and 12 MB) | none | per eye (as image copies) |
| 390-393 | `DepthScale2x2_Direct`, `ScreenSpaceAO2_WithNormalReProj` (temporal), `..._BilateralUpScale` | AO params (272/336 B) | per eye (+ per-eye AO history) |
| 394-398 | `GXScreenSpaceCascadedShadow_PCF9`, blur, `CompositeShadowMask` | cbSceneParam (cascades) | per eye |
| 400-402 | CS 8b355b5a, `GXLightDeferredAO` | cbSceneParam | per eye |
| 402-696 | 295 light volumes `GXLightAcc_*Light*` | cbLightAccParam, view-space lights; rectangles from the CPU | per eye (rectangles unshifted) |
| 697-702 | `GXLightCmbAlbedo`, `GXSssBlurX/Y`, `GXLightComposite` | cbSceneParam | per eye |
| 704-707 | half/quarter depth (`DepthScale2x2OutputDepth`) | none | per eye |
| 708-743 | forward `GXFlver_*_Fwd*` + sky | cbSceneParam, cbMatrixCamera | per eye |
| 744-804 | particles `GXFfxsoftTexture*`, tessellated FFX (37 TES draws) | per-draw 4x4, TES 416 B | per eye |
| 805-807 | `CS_VFX_SparkParticleEmit`, `CS_VFX_StandardParticleCounter...` (GPU particles; read the linear depth) | | **mono, once** (side effects) |
| 809-813 | light shafts `GXFfxLightShaft*` (+ `3d5ebf4e` copies) | g_LightShaftScreenCenter | per eye |
| 814-815 | CS a040875f (1920x1080), `GXFfxdistortionBump` | 400 B matrix at 48-63 | per eye |
| 817-863 | velocity pass `*_Velo` 960x540 + `MotionBlurDilateVelocity` (motion blur) | cbMatrixWVP + previous | per eye, or gone with motion blur off |
| 864 | CS `LutCopy2DTo3DCs` | none | shared |
| 865-923 | CS 9a9cf8a9 (YEBIS combine), bloom/glare pyramid, tonemap, AA | none | per eye |
| 911, 979-981 | CS 1f2b3c9e (luminance, 2048x2), 306ce628 | none | **mono** (one exposure for both eyes) |
| 924- | Scaleform UI | | mono (HUD layer, findings/10) |

About 720 of the ~980 draws are per eye; 240 to 254 per frame are perspective (the rest are
full-screen and screen-space draws).

### 1.5 What changes with the camera [M]

Three census captures in one session (`BB_STEREO_CENSUS=1`, H1 fake head): yaw 0, yaw 10°, yaw 0 two
seconds later. A buffer read by a shader is a camera input when its bytes change from yaw 0 to 10 but not
over the same time with the camera still (`tools/vr_stereo_census.py camdep`):

- cbSceneParam: every view-derived member. The four cascade blocks change too (the fit follows the
  camera).
- `cbLightAccParam` dwords 16-26 / 32-42 (view and camera matrices) and the light arrays' positions
  (dwords 0-2, 16-18, 32-34, ...: view space).
- The particles' 400-byte block 0-27, the velocity pass's 192-byte 0-31, the TES's 416-byte 0-27,
  cbInstanceData 0-11, a light-shaft/fog 464-byte block 68-83.

So **patching constants per eye means about ten layouts**, several premultiplied on the CPU (WVP,
P⁻¹·C·S). Moving the geometry with constant patches is not general. Moving the clip position is
(section 3.2).

---

## 2. The approaches

| | (a1) PM4 double replay | (a3) record-level double render | (b) multiview | (c) game-side double render |
|---|---|---|---|---|
| What runs twice | stage A decode, stage B binding, recording, GPU | recording of the region's commands, GPU | GPU raster (one record) | the draw step, the GX render thread, stage A, stage B, GPU |
| Camera per eye | must patch ~10 constant layouts | **clip.x shift in the VS** (1.3) + optional PS patches | same shift, picked by `gl_ViewIndex` | the game builds it (exact, culling per eye) |
| Guest-visible side effects | EOP/labels/WriteData would run twice; constants already overwritten (the frame ring is rewritten every frame, EOPs signal at record time: parallel_gpu.md) | none: labels and fences are PM4 packets, run once | none | doubles everything the game writes |
| Images | need a second guest image set | host twins of the region's images | every region image a 2-layer host image; guest images single-layer (texture cache): conflicts with readbacks and native access | the same guest images, used twice per frame |
| Compute | runs twice incl. side effects | replay region dispatches without guest writes; particles once | no multiview in compute: per-layer dispatches anyway | twice |
| Recompiler | — | one IR pass (built) | `ViewIndex` in VS; every sampled region image a 2D array, layer = view (PS and CS), size queries | — |
| CPU cost | ~2x the port's bottleneck (PM4 thread at ~100% at 120-137 FPS here) | +~1-2.4 µs per replayed draw on the recording side (measured 2.4 naive) | lowest | ~2x the game's draw step and render thread, and ~2x the port's PM4 thread: ~half the frame rate |
| GPU cost | 2x everything | 2x per-eye passes only | ~2x raster, shared VS work | 2x everything incl. shadows |
| Engineering | large and fragile | 6-8 weeks (09's M1-M5) | +6-10 weeks on top of (a3) | large RE: re-entrancy of the draw step and GX lists, sim frozen for the second render |
| Verdict | no | **yes** | later, G-buffer only, if CPU-bound | no |

Notes:
- **(c) in detail.** The draw step culls, picks LOD and builds per-draw records that each carry the
  camera (787 copies per frame, 1.1). A second camera means a second set of records and a second
  render-thread pass, and the port then decodes twice as much PM4. That thread already limits the frame
  here (stage A at ~100%, 120-137 FPS mono at 1080p); in heavy scenes mono runs 57-94 FPS
  (parallel_gpu.md), so doubling halves it. Phase 3 measured the CPU-offset camera: about 1 draw in 900
  differs per eye and the cascades refit per eye (findings/22 sec. 5). (a3) avoids both by construction.
- **(b) in detail.** Multiview helps only when the recording thread limits. Here it does not: the
  doubled G-buffer replay left the frame rate unchanged (137 FPS, 4.4). It also needs host 2-layer
  images for everything in the region, which fights the texture cache's single-layer guest images and
  native readbacks (09 sec. 1.7). Keep it for later, for the hottest passes or quad views.
- **(d) hybrids.**
  1. **Recommended:** shared shadows, the 64x64 passes, GPU particles, exposure and the LUT; the region
     per eye; mono UI.
  2. **Cheaper fallback, a depth-reprojected second eye (2D + depth):** one render, eye R warped from
     eye L's color and depth, holes filled. Disocclusion holes at every depth edge and wrong
     transparencies and particles: not true stereo. Worth keeping as a performance mode for weak GPUs.
- **Composition with the rest, for (a3):**
  - first person and the head pose come from H1's center camera, and the eyes derive from it (the
    shift uses the camera's own right axis, so head roll is handled);
  - the HUD is outside the region (mono, its own layer);
  - Phase 3's pairing and per-eye swapchains carry over (section 6).

---

## 3. The recommended design

```
H1 (game thread)       center camera: head pose, first person, the symmetric frustum containing both
                       eyes (as mono/alt today); publishes the view (ring) and the eye offsets e_L, e_R
GPU (one game frame)   shadows, 64x64 passes, particle sim, LUT, exposure: once
                       region G-buffer .. post: recorded once, executed for eye L (guest images) and
                       eye R (twins); perspective VS: clip.x += -P00*e (StereoView buffer per draw)
                       UI: once, into the HUD layer
present                eye L / eye R images (per-eye upscale) -> AcquireEye/ReleaseEye x2, the same
                       frame's view each (pose = center ± e, the shared frustum)
```

### 3.1 The math

Both eyes keep the center camera's rotation R and symmetric union projection P (Dream Air: ±55°
horizontal, ±45° vertical, 110 x 90 deg as findings/22 measured). They sit at e = ±IPD/2 along the
camera's right axis. In this game's LH view space with D3D [0,1] depth, a point at view x seen from the
eye has `x_e = x − e`, so

```
clip_e = P · T(−e) · P⁻¹ · clip   ⇒   x_e = x − P00·e ;  y, z, w unchanged
```

- **Exact** for every vertex the game transformed with the center camera, whatever buffer its matrix
  came from (VP·(p−cam), per-draw WVP, the TES matrix).
- **Depth per point is the same in both eyes**, so depth-based effects and the XR depth layer stay
  consistent [M: 1e-4].
- **Far objects get zero disparity**, as parallel eyes should.
- **Asymmetric eye frusta later.** An off-axis P_E per eye turns the shift into
  `x' = a·x + b·w − P00_E·e`, still exact, but the PS-side constants then differ by more than a
  translation (3.6). Phase 3 measured 13.7% of the symmetric image outside each eye's view
  (findings/22 sec. 6). Symmetric is v1.

### 3.2 The vertex shader change (built)

- **Runtime info.** `HwVertexRuntimeInfo::stereo_remap` is set for every clip-enabled hardware VS
  stage (VS, or TES under tessellation) while `BB_XR_MODE=stereo`. It is a separate permutation that is
  cacheable on disk: no session addresses, no eye in any pipeline key.
- **IR pass `LowerStereoRemap`** runs after the user clip planes.
  1. It finds the single `SetAttribute(Position0.x)` and requires w to depend on a buffer or constant
     load (1.3).
  2. It appends a `BufferType::StereoView` buffer and inserts `x' = x + load(StereoView[0])` right
     after x's definition.
  3. It points **every** use of x at x': the position, plain varying copies, and screen coordinates
     the VS derives itself (the 31 shaders of 1.3).
- **Binding.** `BindBuffers` copies `{−P00·e, 0, 0, 0}` per draw into the stream buffer, like
  `ClipPlanes`. P00 comes from the main camera's cbSceneParam (float 52), recognized as
  `CameraMotion::OnConstants` does: float 0 within 100 of 3000 (it moves with the near plane: 3001.49
  at first person's 0.2 m). e is 0 outside the region and for eye L/R the eye's offset.
- **Edge cases:**
  - geometry shaders need the add before each `EmitVertex` (none drew in this frame);
  - user clip planes (evaluated on the shifted x; disabled in the frames seen);
  - draws with x not from one SSA value (none found).

### 3.3 Region and classes

- **Region start:** the first draw into the scene depth after the frame start. The prototype uses the
  first draw with 5+ color targets and a depth, which is the G-buffer.
- **Region end:** the first Scaleform draw (`UiComposition::MovieShader`).
- **Inside the region, everything runs per eye, except:**
  - **GPU particles** (`CS_VFX_*`): one simulation, reading eye L's linear depth;
  - **exposure and luminance** (1f2b3c9e, 2048x2; and other small reductions): computed from eye L
    and used by both (09's size rule, against rivalry);
  - **the LUT** (`LutCopy2DTo3DCs`).
- **Outside the region:** shadows. The cascades are fitted to the center camera, which is what both
  eyes need, and they are texel-snapped (findings/22 sec. 5).
- **Light rectangles stay unshifted** (screen space). They bound each light's influence, so eye R
  loses at most a margin of e/R of the rectangle's half-size: ~1.5% for a 2 m light. Nothing visible in
  the oracle at 8x the offset.

### 3.4 Record once, execute twice (to build)

The prototype draws eye R right after each eye-L draw, switching render passes per draw: simple and
correct, but +0.2 ms of switching on top of the eye-R work at 1080p (4.4). The production version is
09's design, refined by the prototype:

- **Capture.** A stereo pass's closures go to a side arena while they also go to the normal stream.
  `Scheduler::EndRendering` is the single choke point: it executes the arena again with
  `record_eye = R`, inside `BeginRendering(twins)`.
- **Eye-dependent closures.** Only the attachment views and the StereoView descriptor (built: the
  eye-R descriptor write is a stream copy, `Stereo::NoteViewWrite`) depend on the eye.
- **Dispatches inside the region** (a040875f, 9a9cf8a9, AO, light shafts) are recorded twice with twin
  storage and sampled views.
- **HLE constant copies inside the pass** (11 in the G-buffer pass, `copy shader fefebf9f`) run once:
  they write guest buffers that both eyes read.
- **The `3d5ebf4e` guest memcpy** of render-target memory (depth to R32F, G-buffer, half-res targets:
  4 per frame; with its downloads and uploads 0.9 ms of GPU) must become image copies first (09's M1),
  or eye R reads eye L's data.

### 3.5 Twins

- **What.** Host images of the same format and size as the region's guest targets, kept outside the
  texture cache and keyed by image (the prototype keys by guest address and view format).
- **Initialization.** At the region start, each twin is copied from its guest image (built: the scene
  depth was cleared by an earlier pass, draw 1, so eye R must start from it).
- **Coherence.** A replayed write keeps the twin valid; a mono write invalidates it (09 sec. 3.1).
- **Sampled images in the region.** The eye-R replay of a pass substitutes the twin's view for every
  sampled image that has a valid twin (not built: the prototype doubles the G-buffer pass only, whose
  pixel shaders sample material textures).

### 3.6 Pixel and compute shader constants

- **v1 keeps them mono.** With a symmetric shared P, every reconstruction from depth with the center
  camera's matrices lands exactly e from the true point: a 31.5 mm translation, for lighting, shadow
  lookups, fog and decals alike.
- **Measured:** the whole frame (lighting, shadows, fog, SSS, particles, post) shifted by the vertex
  shaders differs from the CPU camera really moved by 0.27-0.34/255 on average, at most 0.1 above the
  noise (4.2).
- **v2 patches the PS and CS stage bindings only** (the VS keeps the mono block, or the shift would be
  applied twice):
  - cbSceneParam: `FC_MatrixView` and `FC_MatrixInvView` (translation by e),
    `FC_MatrixInvViewProj` and `FC_ProjSpaceToShadowMatrix[i]` (times `M_e⁻¹`);
  - cbLightAccParam: `g_ViewMatrix`, `g_CameraMatrix`;
  - the light arrays' view-space positions (−e);
  - decals' `FC_ProjToDecalSpace` (times `M_e⁻¹`).
  Each needs knowing which bound buffer is which: by size and signature, and by shader hash with the
  reflection index (1.2).

### 3.7 Per-eye temporal state

TAA/FSR/DLSS history, `CameraMotion`'s previous camera (per eye: the eye's own previous view), object
motion (store once from eye L: the positions are mono before the shift; apply each eye's shift to both
the current and the previous clip position), the temporal SSAO (`ScreenSpaceAO2_WithNormalReProj`)
history if it is kept in guest images (then twinned like any target). The game's motion blur and AA
stay off (findings/22 sec. 5).

---

## 4. Proof of concept

### 4.1 What was built

| Piece | File | State |
|---|---|---|
| Eye shift in perspective VS/TES, all uses of clip x | `ir/passes/lower_stereo_remap.cpp`, `runtime_info.h`, `resource.h`, `recompiler.cpp` | works on the whole frame |
| Per-draw offset (StereoView), P00 from cbSceneParam, region tracking, live control | `vk_stereo.{h,cpp}`, `BindBuffers` branch, `vk_pipeline_cache.cpp` | works |
| One frame's G-buffer pass drawn twice into twins (copied from the guest images first), both eyes dumped | `vk_stereo.cpp` (`Rasterizer::StereoBeginDraw/AfterDraw`) | works: 240 draws, 8 targets, 11 HLE copies in the pass not doubled (correct) |
| Census and memory scan | `vk_stereo_census.{h,cpp}` | research tool |

Environment: `BB_XR_MODE=stereo` (or `BB_STEREO=1`) turns the remap shaders on.
`BB_STEREO_CONTROL=<file>` is re-read every frame when it changes: `offset=<m>` shifts the whole region
by an eye at <m>; `ipd=<m>`; `dup=<n>` (a new n) doubles the next G-buffer pass and dumps both eyes to
`BB_DUMP_DIR`; `dupall=1` doubles every frame's G-buffer without dumps (cost).

### 4.2 Oracle: the shift against the CPU camera moved (H1) [M]

One session, Hunter's Dream, upscaler none, motion blur, DoF, CA and game AA off. Each setup was
captured twice so that animated pixels (foliage, the idle hunter, the doll) can be excluded: about 86%
of pixels are static. Mean absolute difference per pixel and channel (0-255) on those:

| Offset | Noise (same setup twice) | Signal: center vs CPU camera moved | **Remap vs CPU camera moved** | Pixels > 8 |
|---|---|---|---|---|
| 31.5 mm (half the IPD) | 0.22-0.27 | 9.76 (39.5% > 8) | **0.27-0.34** (p99 1.7-2.3), 3 runs | 0.02-0.13% |
| 0.25 m (8x) | 0.22-0.27 | 17.3 (66.5% > 8) | **1.13** (p99 6.0) | 0.41% |

The residual is spread over lit surfaces in proportion to their brightness, with no edges: the mono
pixel-shader constants (3.6), not geometry. Images: `out/vr_stereo_p4/oracle_mono_remap_cpu.png`,
`oracle_residual_static_x16.png`, `oracle_diff_remap_cpu_x4.png`, `oracle_diff_noise_x4.png`.

### 4.3 Both eyes from the same game frame (G-buffer) [M]

The G-buffer pass of one frame was drawn for eye L (−31.5 mm) into the guest targets and for eye R
(+31.5 mm) into twins, in the same command stream. Eye R's albedo was compared with eye L's warped by
eye R's depth and the expected disparity `P00·IPD/z·W/2`, on pixels where both eyes see the same
surface:

| IPD | Disparity (median / max) | Same surface | \|R − warp(L)\| mean, p95 | \|R − L\| unwarped | Depth of a point, R vs L |
|---|---|---|---|---|---|
| 63 mm | 18.1 / 36.0 px | 94.4% | **1.58**, 6.3 | 16.0, 52 | 1e-4 relative |
| 0.5 m | 145 / 301 px | 81.9% | **1.84**, 7.0 | 26.8, 73 | 1e-4 |

The residual is resampling (nearest pixel) at edges. Phase 3 measured the same 63 mm parallax
independently: +20-22 px for the hunter 4 m ahead (findings/22). Images in `out/vr_stereo_p4/`:
`gbuffer_albedo_LR_ipd63mm.png`, `gbuffer_albedo_anaglyph_ipd63mm.png` (red left, cyan right),
`gbuffer_depth_LR_*`, `gbuffer_rgba16f_LR_*`, `gbuffer_R_vs_warpedL_ipd0.5m.png` (R, warped L, error).

### 4.4 Cost on this PC (RTX 4090) [M]

Hunter's Dream, upscaler off, uncapped, effect patches on, GPU profile (`BB_GPU_PROFILE=1`; busy = the
profile's total minus "GPU idle between submissions"). The frame is CPU-bound (PM4 thread) at 121-138
FPS in every case:

| Render size | Setup | GPU busy ms/frame | G-buffer pass labels | FPS |
|---|---|---|---|---|
| 1920x1080 | mono build, stereo off | 3.66 | 0.63 | 121-136 |
| 1920x1080 | `BB_XR_MODE=stereo`, offsets 0 (remap shaders, StereoView per draw) | 3.52 | 0.63 | 136-137 |
| 1920x1080 | + G-buffer doubled every frame (per-draw replay) | 4.62 | 1.45 | 137-138 |
| 2560x1440 | stereo, offsets 0 | 4.98 | 0.81 | 133-139 |
| 2560x1440 | + G-buffer doubled | 6.75 | 2.20 | 130-132 |

- **The remap costs nothing measurable.**
- **Doubling the G-buffer the naive way** costs 0.82 ms at 1080p (pass 0.63 ms) and 1.39 ms at 1440p
  (pass 0.81 ms); the rest is 480 render-pass switches per frame. The recording thread absorbed the
  extra 240 draw records with no frame-rate loss.
- **Scaling:** 0.90 ms per Mpx of render size, plus 1.65 ms that does not scale (shadows, HLE copies,
  the 64x64 passes).
- **Estimate for stereo** [S]: 1.65 + 2 × 0.90 × R + ~0.5 ms of per-pass overhead, plus 2 upscales.
  - R = 3.5 Mpx per eye (e.g. 2240x1568 at the union frustum's aspect 1.43, upscaled to the panel):
    ≈ 8.45 ms + DLSS ≈ 2 × 1 ms = **≈ 10.5 ms: 72 Hz fits** (13.9 ms), 90 Hz is tight (11.1 ms).
  - The Dream Air's native 3832x3076 per eye (11.8 Mpx) is out of reach without upscaling or
    foveation.

### 4.5 Limits of the prototype

- Only the G-buffer pass is doubled. Lighting, forward, particles and post for eye R need sampled-image
  substitution and compute replays (3.4, 3.5), which is the main Phase 4 work.
- It switches render passes per draw.
- Object motion (upscaler on) would store eye R's positions too. The motion attachment is dropped from
  eye R's pass.
- The oracle shifts the whole frame of one eye, so it validates the shift, the classification and the
  mono-constant approximation. The doubled pass validates the replay mechanics and same-frame parallax.
- One run (BB_UPSCALER=off, during a load) stopped on "Unimplemented PM4 type 0" before any stereo
  draw. The next 8 runs were clean. Not attributed. If it repeats, check it without
  `BB_XR_MODE=stereo`.
- The census frame is slow: about 3 s with the memory scan (`worst frame 3129 ms`).

### 4.6 Running it

```
# build: ninja -C out/gpu bb-probe (under the shared build lock), then copy to out/gpu/bb-probe-p4.exe
python tools/vr_stereo_poc.py oracle --offset 0.0315 --out out/vr_stereo_p4/oracle
python tools/vr_stereo_poc.py dup --ipd 0.063 --out out/vr_stereo_p4/dup
python tools/vr_shader_reflect.py --json out/vr_stereo_p4/shaders.json --show 3deaa573
# census: BB_STEREO_CENSUS=1 BB_CAPTURE_TRIGGER=<file> BB_CAPTURE_DIR=<dir> (+ BB_STEREO_CENSUS_SCAN=1)
python tools/vr_stereo_census.py passes <census.jsonl> --index out/vr_stereo_p4/shaders.json
python tools/vr_stereo_census.py camdep <yaw0> <yaw10> <yaw0_later> --index ...
```

---

## 5. Implementation plan

Effort for one developer who knows this code [S]:

| Step | Work | Check | Effort |
|---|---|---|---|
| P4.1 | Productize the shift: GS `EmitVertex`, clip planes on the unshifted x, a log of the perspective/screen-space split per shader, `ShaderMetaVersion` bump if the struct ever grows | identity (offsets 0: same image as off), oracle at 31.5 mm ≤ 0.5/255 | 3 days |
| P4.2 | M1 of 09: `3d5ebf4e` to image copies (also a mono speed-up: 0.9 ms of downloads and uploads at 1080p) | screenshots identical, GPU time down | 1 week |
| P4.3 | Record once, execute twice in the scheduler (3.4); StereoView descriptor per eye; region detection by the scene depth | the doubled G-buffer bit-identical to today's prototype output; render-pass switches gone | 2 weeks |
| P4.4 | Twins for the whole region: sampled/storage view substitution, compute replays, coherence, overlap copies (09 sec. 2.3), the mono list (particles, exposure, LUT) | **shift test** (eye R = eye L + 8 px NDC everywhere), **write audit** (no guest write from eye R), dump of eye R's final image vs the oracle | 2-3 weeks |
| P4.5 | Per-eye temporal state (TAA/FSR/DLSS ×2, DLSS feature handles, camera motion per eye), output images per eye at the eye size | headset: no ghosting, stable | 1-2 weeks |
| P4.6 | XR: both eyes per frame through Phase 3's `AcquireEye`/`ReleaseEye` (section 6), depth layer per eye | headset check, pacing log "both new 100%" | 3-5 days |
| P4.7 | Performance: render scale and aspect per eye (non-16:9 `BB_RENDER_RES` test, 09 open question 4), DLSS, `BB_GPU_PROFILE` per eye | ≥72 Hz in average scenes on the 4090 | 1 week |
| P4.8 | v2 constants (3.6) and off-axis eyes (3.1), only if the headset shows a lighting or decal offset | oracle residual at noise | 1-2 weeks |
| (later) | Multiview for the G-buffer, quad views | CPU/GPU A/B | — |

---

## 6. Merging with Phase 3

Phase 3 (findings/22) already built most of what Phase 4 needs outside the renderer:

- **Camera and frustum.** H1's center camera with the both-eyes frustum is exactly Phase 4's camera.
  Phase 4 keeps it at the center and does not alternate, so Phase 3's per-eye offset step is skipped.
  First person and the head pose compose before it, as today.
- **Pose pairing.** The ring of views H1 publishes (`bbgpu_vr_set_eye_view`) and the pairing in
  `PrepareFrame` (by camera rows, position, P00) identify the frame and its view. Phase 4 publishes the
  center view plus both eye offsets; the eye tag is not needed (one frame, both eyes).
- **Submission.** The bridge's per-eye chains (ABI 6) show each eye's latest image with the view it was
  rendered with. Phase 4 calls `AcquireEye(0)`/`ReleaseEye(0, view_L)` and
  `AcquireEye(1)`/`ReleaseEye(1, view_R)` for the same frame, with pose = center ± e (the runtime's eye
  offsets, as Phase 3 reads them) and the shared frustum. Phase 3 notes this gives "both new" 100%. No
  ABI change is needed. ABI 4's `AcquireStereo`/`ReleaseStereo` assumes the host renders with the views
  it hands out at acquire time, which a game camera set a frame earlier cannot do. If it is used later,
  add a "rendered with" view per eye, as ABI 5 and 6 did for flat and eye images.
- **Mode switch.** `BB_XR_MODE=stereo` (Phase 4) next to `alt` (Phase 3) and `mono`. Menus and loading
  screens go mono as in alt.
- **Shared lessons.**
  - Phase 3's motion-blur and TAA findings: per-eye history (3.7).
  - LOD and culling stay at the center: Phase 4 has no per-eye culling difference, and the center
    camera's frustum is the union.

---

## 7. Risks (ranked)

| # | Risk | Likelihood / impact | Mitigation | Detected by |
|---|---|---|---|---|
| 1 | GPU time at headset resolution (2 eyes + 2 upscales) | high / high | 3-4 Mpx per eye, DLSS, 72 Hz; foveation (quad views) later; the depth-reprojection fallback mode | `BB_GPU_PROFILE`, headset timing |
| 2 | Memory-level aliasing: `3d5ebf4e` copies, texture-cache overlap copies, downloads | high / high (eye R shows eye L data) | P4.2 first; mirror or invalidate twins on every copy path | shift test, write audit |
| 3 | Replay mechanics: passes ending in odd places, feedback loops, `FilterDraw` paths, HLE copies inside passes (seen: 11 in the G-buffer) | medium / high | flush in `EndRendering`; HLE copies run once (built) | identity and shift tests |
| 4 | Mono PS constants visible somewhere (close point lights, decals, screen-space shadows near the camera) | medium / low | v2 patch list (3.6), guided by the oracle per pass | oracle in Yharnam streets (lamps, blood decals) |
| 5 | Shaders that build screen positions in ways the IR pass misses (not from the single Position.x SSA value), geometry shaders | low / medium | the census lists them; GS hook | oracle diff edges |
| 6 | Compute with side effects in the region (GPU particles, counters) run twice by mistake | medium / medium | explicit mono list by CS hash (named: `CS_VFX_*`) | census, particle counts |
| 7 | Light rectangles too tight for eye R (close small lights) | low / low | grow rectangles by the max disparity in eye R | visual |
| 8 | Per-eye temporal state, DLSS single-feature ABI | high / low | P4.5 | visual |
| 9 | CPU: recording ×2 for ~720 draws | low (recording thread had slack) / medium | batched replay; multiview later | `BB_FRAME_STATS` |
| 10 | Non-16:9 render size untested with this game | unknown / medium | test early (P4.7); 16:9 fallback with anisotropic upscale | dumps |

## 8. Open questions

1. Does the game render correctly at a non-16:9 `BB_RENDER_RES` with H1's aspect (09 question 4)? A
   2560x1440 run worked; 3840x2160 needs `BB_DMEM_MB` (it crashed without it).
2. Where exactly the per-draw records are written (1.1). A watchpoint answers it; nothing in the
   design depends on it.
3. The velocity pass (`*_Velo`): does it disappear with motion blur off? It saves per-eye work.
4. Temporal SSAO (`ScreenSpaceAO2_WithNormalReProj`): where its history lives (guest image, then a
   twin), and whether `Disable SSAO` is preferable in VR.
5. The 64x64 passes (draws 120-144): what they are. They read no camera, so they are shared either way.

## Cross-links

- **09** (the earlier design) is confirmed and sharpened. The "M_eye" remap is the clip x shift here
  (symmetric shared P). The StereoView buffer is built as 09 proposed. D-dup stays first and
  multiview later. The unknown cbSceneParam slots are named (1.2). The classification rules gain the
  w-from-buffer criterion. "Varyings that copy the clip position" turn out to be the common case,
  handled by rewriting every use of clip x.
- **22** (Phase 3): pairing, per-eye chains, shadows texel-snapped per eye, temporal effects. Section 6.
- **10** (HUD): the region ends at the first Scaleform draw; the UI stays mono.
- **15/16**: H1's center camera, first person and the head pose are unchanged.
