# 09: Stereo renderer design (PLAN Phase 3 alternate-eye, Phase 4 "Vulkan geo-11")

Date: 2026-10-09. Read-only design pass: no code was changed, built or run (the headset and GPU were in use).
Inputs: PLAN.md, findings 01–08, the `vr` branch at `bb741e9` plus the uncommitted working tree of that moment
(bridge rewrite with a mono projection layer and `GetViews`, `src/hooks.h`). Paths are relative to
`gpu/shadps4/` unless they start with `gpu/`, `src/` or `docs/`.

Legend: **[C]** confirmed in the code or docs this session. **[L]** likely. **[S]** design proposal or speculation.
**[E]** estimate (numbers derived from stated assumptions; measure before trusting). **[K]** prior knowledge,
not re-checked this session (headset panels, GPU bandwidth).

---

## 0. Summary of decisions

1. **The remap is dynamic, not a pipeline variant per eye.** [S] One session-wide vertex shader variant
   (`HwVertexRuntimeInfo::stereo_remap`) multiplies `gl_Position` and the motion varyings by a 4×4 read from a new
   special storage buffer, `BufferType::StereoView`, bound per draw like the existing `ClipPlanes` buffer. A draw
   picks identity, `M_L`, `M_R` or a sky matrix by descriptor offset. There is no eye bit in pipeline keys and
   `PushData` stays at 128 bytes. The variant is cacheable on disk, unlike the BDA motion variants.
2. **D-dup first, as a pass-batched "record once, execute twice".** [S] Eye L renders into the guest images
   (all existing machinery, guest-visible state, readbacks). Eye R renders into **twin images** kept outside the
   texture cache and keyed by image uid. A pass's recorded commands run a second time with eye-R attachments and
   descriptors. Which draws replay is decided by **taint inside the scene region**: anything that writes the
   scene depth or an image already twinned.
3. **Multiview (D-mv) does not fit this renderer yet.** [C/S] It needs both eyes in host-only 2-layer images,
   which is incompatible with eye L living in single-layer guest images. It also needs recompiler layer
   selection for every sampled stereo image, per-view constant blocks, a `viewMask` in every pipeline key chosen
   on stage A, and the `multiview` device feature, which is not enabled today. Revisit it as an optimisation
   (or for quad views) once D-dup works.
4. **Phase 3 should render the eye with the CPU camera, not `M_eye`.** [S] Use findings 08's render-side hooks
   H1/H2 (`0x22de113`/`0x22de386`) with a ±IPD/2 offset and a symmetric per-eye frustum. The game then builds
   every view-dependent constant itself, so no renderer classification is needed. `M_eye`-AER is kept as a dev
   mode and as the **oracle** that validates Phase 4's remap and constant patching.
5. **Parallel eye frusta and the same near/far for both eyes.** [S] View-space directions then match between
   the mono camera and both eyes, so view-space G-buffer normals stay valid. Only translation and projection
   differ, and the depth values of a point are identical in both eyes.
6. **The 864-byte camera block is patched per eye only for PS/CS stages that read stereo (depth-derived)
   images.** [S] The VS stage always sees the mono block, and the epilogue applies `M_eye`.
7. **Prerequisite: turn the guest memcpy shader `3d5ebf4e` into image-level copies.** [S] It copies render-target
   memory (the scene depth into an R32F texture, and others; `docs/parallel_gpu.md:542-547`), so a twin cannot
   follow it. The change is also a mono performance win.
8. **Ranked risks:** (1) GPU cost at headset resolution with two per-eye upscales, (2) memory-level aliasing of
   render targets, (3) shaders that carry clip or screen position in varyings, or reconstruct with diagonal-only
   inverse projections. Section 5.

---

## 1. Vertex position output, caching, multiview

### 1.1 Where `gl_Position` is finalised today [C]

`shader_recompiler/backend/spirv/emit_spirv_special.cpp:189-203`. The IR emits an `Epilogue` op at every
`Return` (`frontend/structured_control_flow.cpp:745-748`), so this runs at every exit:

```cpp
void EmitEpilogue(EmitContext& ctx) {
    if (Sirit::ValidId(ctx.motion_out_cur)) { EmitVertexMotion(ctx); }     // stores/loads clip pos by BDA
    if (Sirit::ValidId(ctx.motion_frag_out)) { EmitFragmentMotion(ctx); }
    if (ctx.hw_stage == HwStage::Vertex && ctx.runtime_info.hw.vs.emulate_depth_negative_one_to_one)
        ConvertDepthMode(ctx);
    if (ctx.hw_stage == HwStage::Vertex && ctx.runtime_info.hw.vs.clip_disable)
        ConvertPositionToClipSpace(ctx);                                    // screen-space (clip disabled) draws
}
```

- `output_position` is the `BuiltIn::Position` output (`spirv_emit_context.cpp:587`). `EmitVertexMotion`
  (`:73-152`) reads it **before** any conversion. It atomically stores this frame's clip position, loads the
  previous one, and writes the varyings `motion_cur`/`motion_prev` (locations 30/31, `runtime_info.h:84-94`).
- **Only `HwStage::Vertex` is covered.** Geometry and tessellation stages emit positions elsewhere
  (`EmitEmitVertex` is in the same file). Whether BB draws scene geometry with ES/GS or LS/HS is unknown;
  milestone M0 checks `stage_enable` per scene draw.

### 1.2 Push constants are full [C]

`resource.h:212-237`. `PushData` is laid out as offsets 0..127: 4 viewport floats, 16 user-data regs, 40 B of
`buf_offsets`, `motion_param` and `scene_size` (`spirv_emit_context.cpp:775-810`), with
`static_assert(sizeof(PushData) <= 128)`. Every graphics layout pushes `sizeof(PushData)` to all graphics stages
(`renderer_vulkan/vk_graphics_pipeline.cpp:44-48`). Desktop GPUs allow 256 B, but growing it is unnecessary
(1.3).

### 1.3 The precedent to copy: user clip planes [C]

`ir/passes/lower_user_clip_planes.cpp:19-86` is an IR pass (`recompiler.cpp:116`) that:
- appends a special `BufferResource{.buffer_type = BufferType::ClipPlanes}` to the VS `Info`;
- finds the last `SetAttribute(Position0)` and emits loads from that buffer after it.

`Rasterizer::BindBuffers` fills that binding per draw from the stream buffer (`vk_rasterizer.cpp:2059-2077`).
The stereo matrix should be supplied the same way. It costs one descriptor per draw (descriptors are already
push descriptors written per draw, `vk_pipeline_common.cpp:26-88`) and no push-constant space.

### 1.4 Proposed recompiler change [S]

```cpp
// runtime_info.h, HwVertexRuntimeInfo (next to motion_vectors, :115)
bool stereo_remap{};        // bbport VR: pos = M * pos, M per draw from BufferType::StereoView

// resource.h, BufferType (append after ClipPlanes; appending keeps serialized values stable)
StereoView,                 // std430 { mat4 cur; mat4 prev; } 128 bytes

// ir/passes/lower_stereo_remap.cpp (new), called next to LowerUserClipPlanes (recompiler.cpp:116)
if (info.hw_stage == HwStage::Vertex && runtime_info.hw.vs.stereo_remap)
    info.buffers.push_back({.used_types = IR::Type::F32, .buffer_type = BufferType::StereoView});

// emit_spirv_special.cpp, EmitEpilogue: after EmitVertexMotion, before ConvertDepthMode
if (ctx.hw_stage == HwStage::Vertex && ctx.runtime_info.hw.vs.stereo_remap && !clip_disable) {
    // M_cur: rows 0..3 at dword 0..15; M_prev at 16..31 (stereo_view_index set in DefineBuffers
    // like flatbuf_index, spirv_emit_context.cpp:876-882)
    pos         = M_cur  * pos;          // OpStore output_position
    motion_cur  = M_cur  * motion_cur;   // only if motion varyings exist
    motion_prev = M_prev * motion_prev;  // keep .z (valid flag) untouched
}
```

- **Session-wide flag.** Set `stereo_remap` for every `HwStage::Vertex` shader while stereo mode is on, in
  `PipelineCache::BuildRuntimeInfo` (`renderer_vulkan/vk_pipeline_cache.cpp:166-176`). Pipelines are selected on
  **stage A** (`Rasterizer::Draw` → `GetGraphicsPipeline`, `vk_rasterizer.cpp:933`). Render targets, and with them
  the stereo classification, are only known on **stage B** (`PrepareRenderState`, `BeginRendering`). A
  per-draw variant would couple the two threads; a per-draw *binding* does not.
  - Draws that must not move (outside the region, full-screen passes, UI) bind the identity block.
  - The cost is one extra descriptor per draw and four vec4 loads per vertex (eight with motion varyings).
    [E] negligible
- **Motion history stays mono.** `EmitVertexMotion` stores the *pre-remap* position, so the 128 MiB history
  (`vk_object_motion.h:71-85`) is shared by both eyes.
  - Each eye transforms both the current and previous mono positions with its own matrix.
  - Eye R must not store a second time: it uses a separate motion parameter slot without `FlagStore`.
- **Clip-disabled draws** (`ConvertPositionToClipSpace`, screen-space coordinates) are never remapped.
- **User clip planes** are computed on the mono position in IR. They stay correct, because mono clip space and
  world space are related by an invertible affine map [L].

### 1.5 The math in this game's conventions

Facts [C]:
- Camera space is LH: rows right, up and forward, position at +0x40 (08 §3.1).
- The projection is D3D [0,1] with `sx = cot(fov/2)/aspect`, `sy = cot(fov/2)`, `A = f/(f-n)`, `B = -n·f/(f-n)`
  (08 §3.2).
- In the 864-byte scene block: view 3×4 at floats 8-19, inverse projection at 36-51, projection at 52-67
  (`52,57,62,63` = sx, sy, A, B), shadow cascades at 112-175, inverse view at 180-191 (`docs/upscaler.md:104-116`).
- `CameraMotion::OnConstants` captures the first G-buffer block per frame (`vk_camera_motion.cpp:221-241`).

Per eye E, with **parallel** eye frusta (summary item 5), `T_E` = translation by the eye offset `e_E` (OpenXR VIEW-space eye
position with z negated: RH to LH), and `P_E` = the eye's off-axis projection built from the four FOV tangents
with the **same A, B** as `P_mono`:

```
M_E    = P_E · T_E · P_mono⁻¹          (exact for every vertex the game transformed with the mono camera,
                                        homogeneous, so w<0 is fine; P_mono⁻¹ maps (xc,yc,zc,wc) to
                                        (xc/sx, yc/sy, wc, (zc - A·wc)/B))
M_sky  = P_E · P_mono⁻¹                (rotation-only: no parallax for the sky)
M_prev = P_E(n-1) · T_E · P_mono(n-1)⁻¹ (= M_E while FOVs are constant)
```

- With `e_z = 0`, the eye depth `z_e = zc`. **Both eyes write identical depth values for a point**, which keeps
  depth-based shaders, the XR depth layer and TAA depth rejection consistent. [C math]
- `P_mono` must be the **union** frustum, because CPU culling uses this exact projection (08 §6). Phase 2 sets
  it through H1/H2's FOV/aspect. Compensate the LOD scale at `ctx+0x150` (08 §6).
- **Why parallel frusta** [S]: view-space vectors (normals in the G-buffer, light directions) are computed with
  the mono rotation. With canted eye views they would be wrong by the cant angle in each eye, in opposite
  directions, which gives eye rivalry.
  - Rendering a parallel frustum that bounds a canted eye's FOV costs [E] ≈15–30% more pixels for 5–10° of cant.
  - The XR layer is submitted with that parallel pose and FOV, and the runtime warps it to the canted panel.

### 1.6 Pipeline and SPIR-V caching [C unless marked]

- **Permutations.** `StageSpecialization` compares `runtime_info` (`shader_recompiler/specialization.h:204`).
  A new flag produces a new permutation in `Program::modules` (`vk_pipeline_cache.cpp:903-930`). Its
  `stage_hashes = HashCombine(pgm_hash, perm_idx)` goes into `GraphicsPipelineKey` (`vk_graphics_pipeline.h:32-70`),
  so stereo pipelines are distinct automatically. Draw-preparation workers look up the same `spec`
  (`vk_pipeline_cache.cpp:857-869`).
- **Disk cache.** The spec serializes `runtime_info` (`vk_pipeline_serialization.cpp:463/485`).
  - Adding the field changes the layout: bump `ShaderMetaVersion` and `PipelineKeyVersion` (`:14-17`), as was
    done for `needs_native`.
  - Motion VS variants are refused from disk because they embed session BDA addresses (`:118-122`). The
    descriptor-bound `StereoView` variant has no such problem, so stereo pipelines persist across sessions [S].
- **No eye in any key** for D-dup and AER. For D-mv the key would need a `view_mask` (the
  `PipelineRenderingCreateInfo` at `vk_graphics_pipeline.cpp:282-292` sets none today), plus a ViewIndex VS
  variant.

### 1.7 Multiview versus double recording, against this renderer

| Aspect | D-dup (record twice) | D-mv (VK multiview) |
|---|---|---|
| Render targets | Eye L = guest images as today. Eye R = twins of the same size and format. | Both eyes need **2-layer host images**. Guest images are single-layer texture-cache images (`texture_cache.cpp:499+`), so eye L would have to be resolved back to them for every native read or readback. |
| Dynamic rendering | Unchanged. A second `vk::RenderingInfo` with twin views (`vk_scheduler.cpp:97-108`). | `RenderState` gains a `view_mask`; `layerCount` is ignored. |
| Pipelines | Same pipeline for both eyes. | `viewMask` in pipeline creation and key, decided on **stage A** before the targets are known. |
| Device | Nothing new. | `multiview` is **not enabled** (`vk_instance.cpp:475-478` only enables 16-bit storage and draw parameters); GS/tess also need `multiviewGeometryShader`/`multiviewTessellationShader`. |
| Shaders | VS epilogue only. | VS reads `ViewIndex`. **Every PS/CS sampling a stereo image must sample layer = ViewIndex**: an `ImageSpecialization` bit plus 2D→2DArray rewriting (precedent: `SharpFetchPostOp::ConvertCubeTo2DArray`, `resource.h:60, 134-137`). Texel loads, gathers and size queries too. |
| Camera constants | Swap one descriptor for eye R. | Both blocks must be readable in one draw, indexed by ViewIndex: a per-binding recompiler rewrite. |
| Compute passes | Dispatch twice with eye-R descriptors. | No multiview in compute, so D-dup anyway (per-layer 2D views). |
| Guest shaders writing Layer/ViewportIndex | Unaffected. | Must stay on the non-multiview path (check the multiview VUIDs before relying on it). |
| CPU cost | +[E] 0.4–0.8 ms/frame on stage B: ~1160 replayed scene draws (`docs/parallel_gpu.md:420`) × ~0.3–0.6 µs of substitution. Stage B is the limit today at ~85% busy and ~2.8 µs/packet (`:495-496`). The Vulkan recording thread runs the second record; it is ~40% busy (`:268-269`). | Lowest (one record). |
| GPU cost | 2× raster plus pass begin/end; per-pass batching keeps the extra begins to ~1 per pass. | 2× raster; vertex work may be shared by the driver. |

**Verdict [S]:** D-dup first. D-mv later and only for the hottest passes (G-buffer and lighting), once stage B
time or quad views (four views) make it worth the recompiler work.

---

## 2. Pass classification

### 2.1 The frame and its boundaries [C]

From `docs/upscaler.md:86-101`:
1. a copy of the previous UI target into the display buffer;
2. shadow 4096² D32;
3. G-buffer (6 MRT + D32S8);
4. light volumes into two B10G11R11 targets;
5. scene color RGBA16F with forward and transparent draws;
6. volumetric fog (compute, reads linear depth R32F);
7. half-resolution effects;
8. bloom and post;
9. tonemap to RGBA8;
10. game AA;
11. Scaleform UI.

Code markers:

| Boundary | Where |
|---|---|
| Frame start (display pass) | `DrawRecord`, `vk_rasterizer.cpp:959-973`: `FrameCapture::IsDisplayBuffer`, then `upscaler->OnFrameStart`, `object_motion->OnFrameStart` |
| G-buffer pass (main camera) | `gbuffer_draw = popcount(mrt) >= 5 && depth` (`vk_rasterizer.cpp:265-270`) → `CameraMotion::OnGBufferPass` sets `Depth()` |
| Scene color | RGBA16F at depth size with scene depth and ≤2 MRT (`:274-295`) |
| `scene_started` | ≥5 MRT and `EligibleScene(depth)` (`:2684-2685`). **1920×1080 only**, so do not use it as the stereo trigger. |
| End of 3D / start of UI | `TemporalUpscaler::OnDraw`: Scaleform VS hashes (`ui_composition.h:27-30`) → `RunScaled`/`RunUiOnly`, `ui_phase = true` (`vk_temporal_upscaler.cpp:1304-1344`) |
| Existing jitter predicate | depth == scene depth and (indices > 6 or instanced) (`vk_rasterizer.cpp:1103-1107`); applied as a viewport offset (`:3261-3266`) |

### 2.2 Proposed classifier [S]

**Region R.** It opens when the frame's scene depth image is first bound as a depth target after the display
pass. Its identity is learned from the previous frame's `gbuffer_draw` depth, and on the first frame from the
≥5-MRT pass itself. This catches a depth pre-pass or stencil pass before the G-buffer, which `scene_started`
would miss. R closes at the first UI draw (`OnDraw` → Temporal/Copy). Everything outside R is mono and runs once.

**Stereo set S.** Images whose eye-R twin is valid this frame, meaning their last write was a replayed write.
Inside R, for each draw or dispatch D:

```
reads_S   = some sampled/storage binding is in S (or a buffer V# overlaps the memory of an S image)
replay(D) = attaches the scene depth or an S image
         || (reads_S && every written image is "screen-sized", >= W/16 x H/16)
on replay : D runs for eye L (guest bindings) and eye R (twin bindings); written images join S;
            written guest buffers go to eye-R scratch twins (2.3)
otherwise : D runs once with eye-L bindings; written images leave S (twin invalid, NativeWrite)
M_eye(D)  = replay(D) && graphics && !clip_disable && (num_indices > 6 || instanced)
            && !MovieShader(vs)            // extends the jitter predicate (vk_rasterizer.cpp:1104)
remap     = Sky if the VS is listed as sky (or binds a camera block with a zero view translation,
            deadinside28 #68) else Eye
patch_camera_block(stage s of D) = replay(D) && s in {PS, CS} && s reads S
```

Small outputs such as auto-exposure luminance become **mono by rule**, computed from eye L and shared. That is
deliberate: independently adapted exposure per eye would cause rivalry.

| Pass | Class | Why |
|---|---|---|
| Shadow maps (4096² D32), before G-buffer | **shared** | outside R; never touches S; cascades fit the union frustum |
| GPU vertex work read by the CPU (FaceGen; `BB_READBACKS` Relaxed, `gpu/shim/core/emulator_settings.h:30-32`) | **shared** | camera-independent; outside R or not reading S |
| Copy shader `fefebf9f` (HLE, constants; `vk_shader_hle.cpp:19-245`) | **shared** | buffer constants, no S |
| G-buffer, light volumes, scene color, forward and transparent draws, particles | **per eye** + `M_eye` | attach depth/S |
| Directional light, fog composite, full-screen post | **per eye**, no `M_eye` | ≤6 indices; PS reads S, so patched block |
| Volumetric fog (CS, reads linear depth) | **per eye** (CS replay) | reads S |
| `3d5ebf4e` memcpy of depth/G-buffer memory | **per eye after the HLE** (M1) | memory-level, see 2.3 |
| Half-res chain, bloom pyramid (one image with mips) | **per eye** | taint |
| Exposure/luminance reductions (tiny) | **mono, eye L** | size rule |
| Tonemap, game AA (disabled for VR) | **per eye** | taint |
| Reflections (SetCamera has reflection callers, 08 §6) | **shared by default** | another camera; `M_eye` assumes `V_mono`. Verify. |
| UI (Scaleform), display pass | **mono** | after R |

### 2.3 Guest-visible writes: safe by construction, and what still needs work

- **Safe [C].** Labels, fences and memory writes are PM4 packets, not draws: `EventWriteEos/Eop`,
  `WriteData` and `DmaData` go through `RunInOrder` (`video_core/amdgpu/liverpool.cpp:1129-1181`). Replay
  happens at draw/dispatch record level, so these execute exactly once. Occlusion counters are faked
  (`liverpool.cpp:1116-1124`).
- **Eye-R write rule [S].** An eye-R replay writes only host-only resources: twin images and scratch twin
  buffers. Never note them as pending guest writes (`NotePendingGpuWrite`, `vk_rasterizer.cpp:441`). Never let
  `DownloadImageMemory` (`texture_cache.cpp:74-114`) or buffer readbacks see a twin.
- **Needs handling [S]:**
  1. Storage-buffer writes by replayed dispatches or pixel shaders: redirect them to scratch twins, and read
     those only in eye-R replays later in R.
  2. Guest memcpy of render-target memory (`3d5ebf4e`): convert to image copies (M1).
  3. Indirect draws whose arguments come from a GPU pass reading S (Hi-Z culling). M0 lists them. If they
     exist, their argument buffers need twins.
  4. Texture-cache overlap copies of an S image: `ResolveDepthOverlap` → `CopyColorAndDepth` (`:181-248`),
     `ExpandImage` → `CopyImage` (`:480-497`), left-overlap `CopyMip` (`:467-472`). Mirror them on twins or
     invalidate the twin.

### 2.4 Camera-block patching rules [S]

- **Where.** In `BindBuffers` (`vk_rasterizer.cpp:2027-2156`), on both the ring path (`:2034-2043`) and the V#
  path (`:2095-2144`).
  - A binding qualifies when its size is 864 and it carries the `OnConstants` signature *and* the view rows of
    the current `V_mono`. The second check excludes shadow cameras, which use the same layout (comment at
    `:265-266`).
  - Bind a per-frame, per-eye patched copy from the stream buffer (`GetStreamBuffer().Copy`, as `ClipPlanes`
    does), cached by source address and frame.
- **Both eyes are patched.** Eye L is not the mono camera either.
- **Never patch the VS stage.** If a VS builds clip = P·V·world from the block itself, patching would double the
  transform with `M_eye`.
- **Patch PS/CS only when they read S.** Deferred lights and full-screen passes reconstruct position from depth
  with floats 36-51 and 180-191, which must be the eye's.
  - G-buffer PS keep the mono block, consistent with their VS varyings. The cost is mono-camera specular, a
    small flatness.
- **Two patch modes, chosen by the oracle test (M3):**
  - **A, "eye view".** `V' = T_E·V_mono` and `P' = P_E`, symmetric if needed. Works when shaders reconstruct
    with a ray scaled by linear depth using only the diagonal of the inverse projection. Wrong by `e`
    (≈32 mm) wherever the CPU passed mono-view-space data, such as view-space light positions.
  - **B, "eye projection".** `V' = V_mono` and `P' = P_E·T_E`. Exact for every mono-view-space quantity, but
    only correct if shaders use the full 4×4 inverse projection.
- **Unknown slots.** Floats 20-35 and 68-111 are undocumented. SetCamera keeps view·proj next to view and
  projection (08 §3.1 `+0x50`), so one of them is probably VP. M0 identifies every slot numerically (V, P, VP,
  VP⁻¹, P⁻¹, V⁻¹, cascades) so the patcher rewrites all V/P-derived matrices.
  - If the cascade matrices at 112-175 take *view* rather than world positions, they need `·V_mono·V'⁻¹` too.

### 2.5 Special cases

- **Sky** [S]: `M_sky` (rotation only). Detect it by VS hash from M0 (compat JSON), or by a bound camera block
  with zero view translation. Verify zero disparity at the horizon.
- **Varyings that copy the clip position** [S]. Decals and soft particles may compute screen UVs from a varying
  rather than `FragCoord`. Those UVs must be remapped too.
  - Extend the IR pass: Param outputs that are the *same SSA value* as a Position component get the same
    transform.
  - PS4 deferred shaders that use `FragCoord` (`emit_spirv_context_get_set.cpp:121-138`) are already correct
    per eye.
- **Billboards and particles** facing the mono camera are fine under `M_eye` (PLAN). CPU SFX gets the camera
  matrix (08 §3, `0x22de3c7`).
- **Screen-space sprites without `M_eye`** (sun glare) land at zero disparity, which is infinity for parallel
  frusta. That is correct for glare and wrong for world markers (Phase 5).

---

## 3. Resource plan

### 3.1 Twins, not proxies, for eye R (v1) [S]

**Why not `SceneTargets` proxies** (`renderer_vulkan/vk_scene_resolution.h:18-114`) [C]:
- Proxies keep the native guest image as the coherence anchor. Native reads resolve proxy to native
  (`NativeAccess`, `.cpp:122-135`, called from `Runtime::Transit`, `vk_runtime.cpp:199`).
- Several kinds of read need the native image: texel loads, size queries and offsets (`needs_native`,
  `ir/passes/resource_patching_pass.cpp:246`), all **depth** sampling (`ProxyCurrent` excludes depth,
  `.cpp:452-458`), and memory-level copies.
- There is one native image per guest address, so eye R cannot resolve into it.

**Twin design:**
- A twin is a full `VideoCore::Image` built from the source's `ImageInfo`, never registered in the
  texture-cache address or page tables. It reuses `FindView` and `GetBarriers`/per-subresource states for free
  (`texture_cache/image.h:86-196`).
- Twins are owned by a stage-B `StereoTwins` store keyed by `image_uid`. Staleness is detected with
  `TryGetImage(id, uid)`, as `SceneTargets::Get` does (`vk_scene_resolution.cpp:387-416`).
- **Coherence** follows `SceneResolution::Coherence` (`scene_resolution.h:24-32`):
  - a replayed write makes the twin valid;
  - a mono write invalidates it, after which eye R reads the native image;
  - twins persist across frames, so a game-side temporal read of last frame's image stays per eye;
  - a twin is freed with its source (`FreeImage`) or after N idle frames.
- **Barriers.** In R, every `Runtime::Transit(native, layout, ...)` also emits the same target layout for the
  valid twin, from the twin's own tracked state. Replays execute directly after their eye-L pass and before the
  next barrier, so mirrored barriers order correctly.
- **Descriptor substitution.** After `BindTextures` builds `image_infos` (`vk_rasterizer.cpp:2358-2447`), each
  image in S maps to the twin's view of the same `ImageViewInfo`. Storage images, feedback-loop views
  (`:2403-2412`) and the attachments in `BeginRenderingFull` (`:2753-2902`) are substituted the same way.
- **VR v1 runs with live reduced proxies off.** Use the `scaled_session` path (`BB_RENDER_RES`, already
  disables `SetSize` and `RasterScaling`: `vk_temporal_upscaler.cpp:264-266, 319-324`).
  - Eye resolution is the guest render size set by the resolution patch (`scripts/patches.py:149-167`), and the
    upscaler produces the headset size.
  - "Twin proxies" (reduced-size per-eye proxies) can come later.

### 3.1.1 Replay mechanics: record once, execute twice [S]

**Anchors [C]:**
- `DrawRecord` (`vk_rasterizer.cpp:950-1146`) runs `BindResources` (`:989`, building `set_writes`), then
  `BeginRendering` (`:996`, a `RenderState`), then `pipeline->BindResources(set_writes, push_data)` (`:1100`:
  push constants and push descriptors, `vk_pipeline_common.cpp:26-88`), then `scheduler.BeginRendering(state)`
  (`:1110`), then a recorded closure that binds the pipeline and draws (`:1121-1128`).
- About 90% of draws continue the open pass (`docs/parallel_gpu.md:458`).
- `RecordChunk::Execute` runs and **destroys** each closure (`vk_scheduler.h:617-626`).

**Proposal:**
- **On stage B**, for a replayed draw, build `set_writes_R` by substituting twin views, the eye-R camera block
  and the eye-R `StereoView` offset in a copy of `set_writes`/`image_infos`/`buffer_infos`, and build `state_R`
  from twin attachment views with the same clears.
  - Record the two eye-dependent commands as one "eyed" closure holding both variants: push descriptors and
    `beginRendering`.
  - Everything else (dynamic state, vertex and index binds, push constants, draws) is eye-independent.
- **In the scheduler**, a stereo pass collects its closures in a side arena while they also go to the normal
  stream.
  - `Scheduler::EndRendering` (`vk_scheduler.cpp:128-134`) is the single choke point. While a stereo pass is
    open it records: end L, then the arena again with a thread-local `record_eye = R` (eyed closures pick their R
    variant), then end R. Destroy the arena afterwards.
  - Because the flush happens inside `EndRendering`, whatever ended the pass (a barrier, a copy, a dispatch, a
    `SceneTargets::Transition`, a direct `CommandBuffer()` user) is recorded *after* eye R.
  - Mono resources they modify are therefore seen identically by both eyes.
- **Dispatches** (`DispatchRecord`, `:1286-1336`) are outside passes: record the dispatch twice right away.
- **Do not replay** draws with PS storage writes to non-twinned buffers, or `FilterDraw` special paths
  (`:118-163`), until they are mirrored explicitly.
- **Memoization.** `BeginSignature`/`begin_memo` (`vk_rasterizer.h:173-196`) and the texture-set memo must
  include the stereo state.

### 3.2 Render size and aspect [S/C]

- `M_E` maps the eye frustum onto the whole guest render target, whatever the mono aspect. For square pixels
  the guest render size should match the eye's tangent aspect: Pimax per-eye FOVs are roughly square [K].
- **Test `BB_RENDER_RES` with non-16:9 sizes** (for example 1920×1920) in M0. Notes:
  - The game's projection takes aspect from PersCam +0x54, not from the render size [C, 08 §3.2].
  - Format-B width/height are hard-coded 1920×1080 at `0x21166d5` [C, 08 §8.3]; consumers are unknown.
  - The upscaler's `RenderTarget` allows only +8 px alignment slack (`vk_temporal_upscaler.h:150-154`), which
    suits any size.
- **Fallback.** Keep 16:9 (for example 2560×1440) and accept anisotropic pixels: vertical resolution is ≈56% of
  horizontal per angle. Upscale anisotropically.

### 3.3 Memory and bandwidth [E]

**Per-pixel footprint of one eye's stereo set (render size), from the frame table:**

| Item | B/px |
|---|---|
| G-buffer (3× RGBA8, sRGB RGBA8, B10G11R11, RGBA16F) | 28 |
| D32S8 depth | 5–8 |
| 2× B10G11R11 lighting | 8 |
| RGBA16F scene color | 8 |
| R32F linear depth | 4 |
| Half-res targets and post chain | 6 |
| LDR and game-AA ping-pong | 12 |
| Object-motion attachment (RGBA32F) | 16 |
| Upscaler motion (RG16F) | 4 |
| **Total** | **≈90** |

Per-eye output: about 45 B/px (output RGBA16F 8, plus history: TAA 2× RGBA32F, or DLSS/FSR4 internal ≈20–40).

Panels [K]: **Pimax Crystal 2880×2880 per eye (8.29 Mpx)**, **Dream Air 3840×3552 per eye (13.6 Mpx)**. Plan with
the runtime's recommended size: the working-tree bridge logs `recommendedImageRect` at Init
(`gpu/xr_bridge/bbport_xr_bridge.cpp` ~:257-275, uncommitted).

| Config (render → output, per eye) | VRAM both eyes (render + output) | Of which new for eye R | Traffic/frame (≈300 B/px render + 60 B/px output) | At 90 Hz |
|---|---|---|---|---|
| mono today 1920×1080 | 0.19 GB | — | — | — |
| Crystal 1920×1920 → 2880² | 0.66 + 0.75 = 1.41 GB | 0.71 GB | 3.2 GB | 289 GB/s |
| Crystal 2880² → 2880² (DLAA) | 1.49 + 0.75 = 2.24 GB | 1.12 GB | 6.0 GB | 537 GB/s |
| Dream Air 1920×1776 → 3840×3552 | 0.61 + 1.23 = 1.84 GB | 0.92 GB | 3.7 GB | 331 GB/s |
| Dream Air 2560×2368 → 3840×3552 | 1.09 + 1.23 = 2.32 GB | 1.16 GB | 5.3 GB | 474 GB/s |

RTX 4080 has 717 GB/s and RTX 4090 1008 GB/s [K]. Rendering each eye at native panel resolution is not viable at
90 Hz. Render about 3.5–4 Mpx per eye and upscale.

**GPU time [E].** Today native 1080p is 3.67 ms GPU busy on an RX 7800 XT (`docs/parallel_gpu.md:521`). FSR 4
at a 4K-class output is ≈4.0–4.2 ms per run (`docs/upscaler.md:190, 287`). Assume ~1.0 ms shared work plus
~1.3 ms/Mpx of per-pixel scene work.
- Crystal 1920² per eye: ≈1.0 + 2×3.69×1.3 ≈ 10.6 ms, plus 2 upscales to 2880² (FSR 4 ≈ 2×4 ms on that card,
  DLSS ≈ 2×1–2 ms on a 4080). That is **≈19 ms on a 7800 XT with FSR 4** and ≈10–12 ms on a 4080 with DLSS.
- So 72–90 Hz is plausible on 4080/4090-class cards with DLSS. FSR 4's cost on AMD favours TAA or FSR 3 per
  eye, or a lower render scale.
- Measure with `BB_GPU_PROFILE` and eye-tagged labels before deciding anything.

### 3.4 Temporal upscaler and per-eye history [C → S]

**Today everything is single-instance** (`vk_temporal_upscaler.h:188-284`): the FSR 3 portable `context`, one
`fsr4`, `motion_image`, `output_image`, `taa_history[2]`, `jitter_index`, `reset`, `frame_id`. The DLSS bridge
has **one feature** (`CreateFeature`/`ReleaseFeature` with no handle: `gpu/dlss_bridge/bbport_dlss_bridge.h:65-68`,
`vk_dlss.h:62-66`).

Proposed changes:
- Add `struct EyeState { contexts, taa_history, motion_image, output_image, jitter_index, reset, frame_id }` and
  keep `eyes[2]`.
- `RunScaled(eye)` at the UI boundary (`:1528-1694`) runs once per eye (D-dup) or for the current eye only (AER).
  Inputs: eye L uses guest `ldr_target` and `Depth()`; eye R uses their twins.
- **DLSS ABI change:** feature handles (two NGX features).
- **FSR 4:** one `Fsr4Upscaler` per eye. Its view registry holds eight images (`docs/upscaler.md:155`), and
  twins double the image count.
- **Motion per eye** (`CameraMotion::RecordMotion`, `vk_camera_motion.cpp:179-219`):
  `reproject = V'_E,prev · V'_E,cur⁻¹`, `proj/prev_proj = P'_E`.
  - `Camera previous` becomes per eye: under AER it holds the same eye two frames back.
  - Object motion: under D-dup the mono history plus eye matrices in the epilogue (1.4). Under AER the history
    is keyed by parity (two `Motion::History`).
- **Jitter.** D-dup can use the same phase for both eyes (one viewport offset, `vk_rasterizer.cpp:3263-3264`).
  AER needs per-eye Halton indices, since each history only sees its own frames.
- **Output.** VR copies each eye's `output_image` into the XR eye swapchain instead of blitting it into
  `ui_image` (`:1641-1661`).
  - The HUD goes to its own transparent layer (PLAN Phase 5, coupled here).
  - The output size must be the eye swapchain size, not `BbSettings::OutputWidths` (`:302-303`).

### 3.5 XR handoff (bridge ABI 4) [S]

**Today.** HEAD submits a quad (`gpu/xr_bridge/bbport_xr_bridge.cpp:385-433`; `xrEndFrame` under the host queue
lock at `:431-433`). The working tree adds a mono projection layer using `flat_views` captured at `ReleaseFlat`,
and notes pose tagging at the camera seam as still to do.

**Needed:**
- per-eye `Chain`s for color and depth (`XR_KHR_composition_layer_depth`; nearZ 0.05007, farZ 3000, depth range
  0..1);
- `AcquireEyes`/`ReleaseEyes` with the pose and FOV each eye was rendered with, and an `updated[2]` mask (AER
  releases one eye; the other chain keeps its last released image and pose);
- presenter-side copies next to `CopyToFlat` (`renderer_vulkan/vk_presenter.cpp:895-903`), with the same handling
  of sRGB values stored in UNORM.

**Buffering.** Keep per-eye images per guest display buffer, as `DisplayOverride` does
(`vk_presenter.cpp:369`, `TemporalUpscaler::displays`). Frame N+1's upscale then cannot overwrite N's eyes before
the present copy.

**Pose tags** (needed since Phase 2). H1 records `{seq, eye, injected camera}`. The renderer matches `V_mono`
from `OnConstants` against that ring (eye L and eye R differ by the IPD, so they are distinct) [L]. A fallback is
an LSB tag in the far plane, but that breaks the exact `[0]==3000` signature, so the signature check would need
adjusting.

---

## 4. Phase 3: alternate-eye (stepping stone)

### 4.1 Two ways to render one eye per frame

| | **A. CPU eye camera (recommended)** | **B. `M_eye` per frame** |
|---|---|---|
| How | H1/H2 (08 §7): `M' = M_game·T(head)·R(head)·T(±IPD/2)`, per-eye FOV and aspect (symmetric bounding frustum; off-centre later via the tile path of `0x25aa290`) | Center camera with the union frustum (Phase 4 setup); the epilogue applies `M_E`; patched camera blocks |
| Shading consistency | Exact: the game builds V, P, VP, cascades, culling and LOD for the eye | Subject to all of section 2's rules |
| Canted views | Native (the camera can rotate) | Parallel frusta only |
| Renderer work | None for geometry | Recompiler pass + classifier |
| Value for Phase 4 | Shares XR, per-eye history and pose tags | **Oracle mode:** render the same eye with A and B and diff them |

Phase 3 = A. B is a dev toggle used in M3 to validate Phase 4's remap and patching before twins exist.

### 4.2 What Phase 3 needs from the shared seams

- XR projection layer with per-eye swapchains, depth and pose tags (3.5).
- `EyeState ×2` in the upscaler; per-eye `Camera previous`; parity-keyed object motion; DLSS feature handles
  (3.4).
- Pacing at headset rate: game rate = HMD Hz, so 90 or 120 FPS. The real 120 FPS limit is still unlocated; it
  is not `0x04D25844` (08 §4).
- **Eye parity by rendered-frame counter**, not by display frame index. A missed vsync then delays both eyes
  equally instead of starving one.
- Desktop mirror: eye L.

### 4.3 Comfort issues

1. **Temporal disparity.** Each eye shows a different sim time, 1 frame apart. Lateral motion becomes false
   depth (Pulfrich effect), with double edges on fast enemies and rolls. This is inherent to AER; R.E.A.L.
   needed AER v2 and optical flow for it.
2. **Head translation.** The stale eye shows old parallax. Positional reprojection needs the depth layer; the
   runtime's rotational warp alone hides rotation only.
3. **ASW/SSW/Smart Smoothing.** These estimate motion from consecutive submissions *per eye*. Under AER an eye's
   image changes only every other frame, so their estimate sees alternating zero and double motion: warping and
   judder [L].
   - Recommend motion smoothing **off** in AER mode, and rely on per-view pose tags plus the depth layer.
   - Re-enable it for D-dup, where submissions are normal.
   - Test on Pimax Play, SteamVR and VDXR.
4. **Frame drops** stretch one eye's age. Keep alternation strict and each view's own pose, so geometry stays
   rotationally stable.
5. **History over two sim ticks.** Larger motion vectors give more TAA/DLSS ghosting. `frame_ms` passed to the
   upscalers doubles.
6. **Rivalry** from per-eye exposure or TAA state. Exposure comes from the game per frame, so it is identical
   anyway under A.

---

## 5. Risks, ranked

| # | Risk | Likelihood / impact | Mitigation | Detected by |
|---|---|---|---|---|
| 1 | **GPU time** at headset resolution with two upscales (3.3) | high / high | Render about 3.5–4 Mpx per eye; DLSS or TAA; foveation (Phase 6) later; 72 Hz targets | `BB_GPU_PROFILE` with eye labels (M6) |
| 2 | **Memory-level aliasing**: the `3d5ebf4e` memcpy of RT memory, texture-cache overlap copies, pool reuse | high / high (eye R shows eye-L or stale content) | M1 HLE to image copies; mirror or invalidate twins in `ResolveOverlap`/`ExpandImage`/`ResolveDepthOverlap` | shift test, mono-write audit (M4) |
| 3 | **Shader space mismatches**: clip position in varyings, diagonal-only inverse projection, view-space data from the CPU, VP slots not patched | medium-high / medium (halos, shifted decals, light offset) | Parallel frusta; patch modes A/B; position-copy varyings; slot identification (M0) | oracle diff (M3) |
| 4 | **Classification errors**: missing `M_eye`, `M_eye` on non-camera geometry, sky, reflections | medium / medium | Taint rules; compat JSON; live per-VS toggle in the overlay | shift test, oracle |
| 5 | Unknown 864-block slots and shadow cascades in view space | medium / low-medium | M0 numerical slot map | oracle |
| 6 | **Replay mechanics**: a stereo pass ending in odd places (`SceneTargets::Transition`, direct `CommandBuffer()` users), feedback loops, special draws (`FilterDraw`: fast-clear eliminate, resolve, depth copy, `vk_rasterizer.cpp:118-163`) | medium / high (corruption) | Flush the eye-R pass inside `Scheduler::EndRendering`; mirror special draws or keep them out of R | identity test (M4) |
| 7 | Per-eye temporal state: DLSS single-feature ABI, FSR 4 registry | high / low | ABI change, per-eye instances | visual, logs |
| 8 | **Guest-visible writes from eye R** | low (by construction) / high | Rule in 2.3; `BB_WRITE_LOG` audit | M4 audit |
| 9 | AER comfort (temporal disparity, smoothing) | high / medium (Phase 3 only) | Smoothing off, depth layer, strict parity | user test protocol |
| 10 | Non-16:9 `BB_RENDER_RES` breaks something in the game | unknown / medium | Anisotropic 16:9 fallback | M0 |
| 11 | GS/tess-emitted scene positions (epilogue only covers `HwStage::Vertex`) | unknown / medium | Remap before `OpEmitVertex` | M0 stage census |
| 12 | Stage B CPU at 90–120 Hz | low at 90, medium at 120 / medium | Per-pass batching; D-mv later | `BB_FRAME_STATS` "Draw pipe" |

---

## 6. Milestones and verification

| M | Work | Verify (capture / measure) | Exit |
|---|---|---|---|
| **M0** Census | Extend `BB_CAPTURE_TRIGGER` (`renderer_vulkan/vk_frame_capture.h`) to log per pass: written images and buffers (address, size, format), `stage_enable`, indirect draws and their argument producers, PS storage writes, `clip_disable`, VS hash, depth bound. Add a slot-identifier for the 864 block (match V, P, VP, inverses, cascades numerically). Run `BB_RENDER_RES` non-16:9 flat. | Captures in Hunter's Dream, Central Yharnam, Forbidden Woods (fog), Nightmare Frontier | A classification table that names every pass in R, with no unknown writers |
| **M1** memcpy HLE (mono win) | HLE `3d5ebf4e`: image→image copy when source and destination ranges match images (`docs/parallel_gpu.md:542-547`) | Screenshots identical (PSNR ∞ or ≥ 50 dB); GPU ms for that label drops; no "Scene native read" for it (`BB_SCENE_DEBUG`) | Merged with no visual change |
| **M2** Remap plumbing | `stereo_remap` flag, `StereoView` buffer, epilogue, per-frame blocks, version bumps | **Identity test:** stereo variant + identity matrices give frames bit-identical to flag off. FPS cost ≤1%. Disk cache reload works. | Regression-free |
| **M3** One eye via `M_eye` (desktop, then AER-B) | Center camera with the union FOV (H1/H2), `M_L` + patched blocks (modes A/B) | **Oracle:** the same eye rendered via CPU camera (AER-A) and via `M_eye` on consecutive frames of a frozen scene: zero the global frame delta (`[0x059404F8]+0x264`, 06 §2d), plus the debug "freeze culling camera" byte 0x5940DD4 (08 §6) so both frames cull the same objects. Per-pass diffs from dumps (`BB_DUMP_TRIGGER`); PSNR per region. | ≥35 dB overall; no visible lighting, fog or decal offsets; mode A or B chosen |
| **M3.5** Phase 3 ship (AER-A) | XR eye chains + depth + pose tags; `EyeState ×2`; parity history | Head-rotation swim test (static scene: no drift); 10-minute comfort run; pacing log (frame age per eye); smoothing on/off A/B | "R.E.A.L. bar" (PLAN Phase 3 exit) |
| **M4** Twins + replay with eye R = eye L | `StereoTwins`, Transit mirroring, descriptor and attachment substitution, eye-R pass flush in `EndRendering`, CS replays, overlap-copy mirroring | (a) **Identity:** IPD 0, `M_R = M_L`: eye R bit-identical to eye L (catches sync and corruption). (b) **Shift test:** `M_R = M_L` + 8 px NDC x-shift: eye R = shift(eye L) everywhere except the border (catches fallbacks to native and missed replays). (c) **Write audit:** `BB_WRITE_LOG`, plus a CRC of GPU-written guest regions with stereo on vs off (IPD 0) | All three pass in the four M0 scenes |
| **M5** Real D-dup stereo | `M_R`, per-eye upscale, SBS debug view on the monitor, then the XR projection layer | Disparity check on known geometry (lamp posts at measured distances); sky zero disparity; stage B busy % and µs/packet (`BB_FRAME_STATS`) | Correct stereo on all scene geometry |
| **M6** Performance | Render scale, DLSS/TAA choice, 72/90 Hz | `BB_GPU_PROFILE` (eye-tagged), VRAM via VMA stats, headset frame timing | ≥72 Hz in average scenes on a 4080 class (PLAN exit) |
| **M7** (opt.) D-mv | Multiview for G-buffer and lighting | CPU and GPU A/B vs D-dup | Only if it is a measurable win |

---

## 7. Open questions

1. Does BB use GS or tess for scene geometry, or indirect draws fed by GPU culling? (M0)
2. Which 864-block slots are V/P-derived, and are the cascades in view or world space? (M0)
3. Do lighting shaders use the full inverse projection or only the diagonal? This decides patch mode A or B (M3).
4. Does the game render correctly at non-16:9 `BB_RENDER_RES`? (M0)
5. On which thread does H1 run relative to `PresentThread` (pose latency, 08 §8.4)? This affects the pose-tag
   ring depth.
6. Do Pimax Play, SteamVR and VDXR accept a render-size depth image with an output-size color image in
   `XrCompositionLayerDepthInfoKHR`? The spec maps depth through its own `imageRect` [L]. Verify per runtime.

## Cross-links

- **PLAN.md Phases 3/4.** This doc replaces three PLAN specifics:
  - "fetch `M_eye` through BDA or pack an eye bit" → a descriptor-bound `StereoView` buffer;
  - "per-eye 864-byte copies through `vk_constant_ring.h`" → stage-B stream-buffer copies, because stage A cannot
    classify;
  - "per-eye targets from `SceneTargets` proxies" → native-size twins in v1.

  It also recommends CPU-camera AER for Phase 3.
- **06 §4 Rank 1.** The math and anchors are confirmed. Gap 5 (guest-visible writes) is resolved by construction
  for PM4 writes and narrowed for storage writes (2.3). Gap 12 (push constants) is avoided. Gap 13 (cache keys)
  is covered in 1.6.
- **08.** Projection convention, union-frustum culling and LOD (§3, §6) feed 1.5. H1/H2 feed Phase 3-A and the
  pose tags.
- **03 §3.1 blockers.** "Camera-dependent per-object constants" is handled by the clip-space remap. "Full-screen
  passes keyed to one set of targets" is handled by twins and replay (3.1).
