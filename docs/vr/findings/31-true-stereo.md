# 31: True stereo (Phase 4): both eyes from every game frame

Date: 2026-10-10. Branch `vr`. Plan: findings/23 section 5 (P4.1 to P4.8). Everything here is behind
`BB_XR_MODE=stereo` (or `BB_STEREO=1`). `alt` (findings/22, the default) and `mono` are unchanged:
every new path checks the stereo switch first, and the scheduler's capture hook is a null pointer
test that never fires outside a doubled pass.

Legend as in 23: **[M]** measured in the running game (flat, fake head, `BB_VR=0`), **[U]** unit test,
**[C]** confirmed in code, **[S]** design or estimate.

## Status

| Step | State | Evidence |
|---|---|---|
| P4.1 finish the shift | **done** | GS remapped (2 GS, 40 and 8 stores); clip distances on the unshifted x; per-shader class log; oracle at the noise level (0.57-0.67 vs noise 0.59); a stereo-permutation binding bug fixed (2.4) [M] |
| P4.2 `3d5ebf4e` as image copies | **measured; nothing to build for mono** | 2 of its 5 copies per frame already are image copies; 1 (depth to R32F) misses by length only; 2 write memory with no image: these need twin buffers in P4.4 (section 3) [M] |
| P4.3 record once / execute twice | **done** | identity check bit-exact on all 8 G-buffer targets (398 draws, 15 render passes); stereo warp residual 1.66 = the prototype's 1.67; eye R 0.57 ms GPU, +0.74 ms per frame against the prototype's +1.76 [M] |
| P4.4 twins for the whole region | **started: 2 slices green** | write guard with negative control; eye-R copies of the copy shader's buffer targets (section 5) [M] |
| P4.5 to P4.8 | not started | section 6 |

## 1. P4.1: the shift, finished

Files: `shader_recompiler/ir/passes/lower_stereo_remap.cpp`, `shader_recompiler/runtime_info.h`,
`video_core/renderer_vulkan/vk_pipeline_cache.cpp`.

- **Geometry shaders.** `HwGeometryRuntimeInfo::stereo_remap` (new, at the struct's end; the
  `HwInfo` union is sized by the fragment info, so `RuntimeInfo` and the disk cache layout do not
  change: a `static_assert` guards it). The pass takes every `SetAttribute(Position0.x)` of the GS (one
  per `EmitVertex`), requires every stored w to come from memory, and shifts each distinct x
  definition once. Two of the game's GS were compiled in the test runs: `1635154c` (40 position
  stores) and `8e3f8dc4` (8), both perspective [M].
- **Clip distances on the unshifted x.** User clip planes (`LowerUserClipPlanes`, which runs first)
  and guest-exported clip distances are `plane . clip` with the plane given in the center camera's
  clip space. Evaluated on the eye's clip position they would move the plane with the eye; on the
  center's position they give the same distance per point for both eyes, which is exact. Uses of x
  that only feed `SetAttribute(ClipDistance)` (through `FPMul`/`FPFma`/`FPAdd`) keep the unshifted x.
  (23 section 3.2 described the prototype's behavior, shifted, and noted the planes were off in the
  frames seen.)
- **Disk cache.** `stereo_remap` is now a revision number (`StereoRemapRevision = 2`) instead of a
  bool. Permutations the prototype (revision 1) left in the disk cache never match, so no stale
  stereo shader is loaded. Mono and alt permutations (0) are untouched: no `ShaderMetaVersion` bump,
  no cache rebuild for the user.
- **Per-shader log.** In stereo mode every compiled VS/GS prints its class:
  `Stereo: vs <hash> perspective (1 x, 3 uses shifted)`, `... screen-space (w not from memory)`,
  `... not remapped (position stores | immediate x)`. One session's tally: 58 VS perspective, 26
  screen-space, 2 GS perspective (only shaders compiled in that session).

## 2. P4.3: record once, execute twice

Files: `vk_scheduler.{h,cpp}` (capture), `vk_stereo.{h,cpp}` (the doubled pass),
`vk_rasterizer.{h,cpp}` (one hook in `DrawRecord`, the `StereoView` binding).

### 2.1 Design (as 23 section 3.4, with two changes)

```
DrawRecord (a draw of the doubled pass, after its inputs are resolved)
  StereoBeforeEmit
    new render pass?  EndRendering (finishes the previous capture: eye R of it is recorded there)
                      FlushBarriers if needs_barrier (as DrawRecord would)
                      eye slot <- StereoView(eye L)       (updateBuffer + 2 barriers, vertex stages)
                      dynamic state invalidated            (the first draw sets all of it)
                      BeginRendering(guest targets)        (into the command stream)
                      Scheduler::BeginCapture(arena)       Record()/RecordData() now go to the arena
  the draw's commands  vertex/index buffers, push constants, push descriptors, dynamic state,
                       pipeline + draw: into the arena
Scheduler::EndRendering (any reason: next pass, dispatch, copy, barrier, submission)
  arena executed in place (eye L), endRendering
  callback (capture off): eye slot <- StereoView(eye R), BeginRendering(twins), the same arena
                       executed again, EndRendering; GPU profile label "stereo: eye R ..."
```

- **The arena** (`RecordArena`) is a list of the scheduler's own `RecordChunk`s. Its commands are
  executed without being destroyed (`RecordChunk::ExecuteKeep`), twice, then freed with the last
  closure that holds it. `RecordData()` during a capture allocates in the arena, so the descriptor
  writes, vertex bindings and barrier lists the commands point at live as long as both executions.
- **What is eye-dependent and how.** Only the attachments (the eye-R `BeginRendering` gets the
  twins' views) and the `StereoView` value. **Change from 23:** 23 proposed a per-eye descriptor
  ("the eye-R descriptor write is a stream copy"). The recorded push-descriptor command is the same
  bytes for both executions, so instead every draw of a doubled pass binds one fixed device buffer
  (the eye slot, `EyeSlot()`), and the slot is rewritten with `vkCmdUpdateBuffer` before each eye's
  execution, outside the render pass, between barriers on the vertex stages. P00 comes from the
  frame's scene constants as before; one value per pass.
- **State independence.** In Vulkan the bound state (pipeline, dynamic state, vertex/index buffers,
  descriptors) carries over from one render pass instance to the next in a command buffer. Eye R
  must therefore not depend on what eye L's last draw left bound. Every draw already records its
  vertex/index buffers, push constants, push descriptors and pipeline; the dynamic state is
  redundancy-filtered (`DynamicState::CommitWith`), so the capture invalidates it at the pass start
  and the first draw records all of it. The identity check below proves the result.
- **Commands that must not be captured** bypass it (`Scheduler::CaptureBypass`):
  `RecordHostCopy` and `SignalAfterHostCopies`. Both are waited on by the CPU (`WaitHostCopies`,
  `WaitDeferredSignals`); in an arena not yet handed to the recording thread they would deadlock.
  Their order relative to recorded commands does not matter (CPU copies into staging before the
  submission).
- **A direct `CommandBuffer()` call** during a capture would put commands out of order. It ends the
  capture early: the arena is executed in place, the pass continues uncaptured, and eye R lacks that
  pass (counted as "cut short"; 0 in every run).
- **Change from 23: the hook point.** 23 placed the capture in `Scheduler::EndRendering` only. The
  first draw of a pass records its state *before* `scheduler.BeginRendering` (in `DrawRecord`), so the
  capture has to begin earlier, in the rasterizer: `StereoBeforeEmit` runs after the draw's inputs
  are resolved (resolving may upload, which ends rendering) and opens the pass itself. The end stays
  in `Scheduler::EndRendering`, the single choke point.
- **Pass structure equals mono.** The hook flushes pending barriers only when the draw sets
  `needs_barrier`, as `DrawRecord` does. (A first version flushed always and split the G-buffer pass
  into 398 one-draw render passes; with the fix it is 15, the 14 HLE copy dispatches inside the pass
  being the split points, as in mono.)

### 2.2 Controls

`BB_STEREO_CONTROL` file (23 section 4.1) gains `replay=0|1` (default 1: record once / execute
twice; 0: the prototype's per-draw second draw) and `same=1` (eye R at eye L's position: the
identity check). `dup=<n>` and `dupall=1` work with both.

### 2.3 Evidence [M]

`python tools/vr_true_stereo_test.py replay` (flat, `BB_VR=0`, fake head, `out/gpu/bb-probe-stereo.exe`),
one session in Iosefka's clinic (the save's location), three doubled G-buffer passes of different
frames (`out/vr_true_stereo/replay3/report.txt`):

| Check | Result |
|---|---|
| **identity** (`same=1`): eye R replay vs eye L, all 8 targets (4 x RGBA8, 2 x RGBA16F, B10G11R11, D32S8) | **bit-identical**, every byte |
| replay log | 398 draws, 14 dispatches (HLE copies, not doubled), **15 render passes executed twice, 2781 commands, 0 cut short, 0 draws outside a capture** |
| stereo (eyes at -/+ 31.5 mm), record once / execute twice | disparity median 9.5 px, max 27.2; same surface in both eyes 98.2%; albedo \|R - warp(L)\| mean **1.66** (p95 6.0; 12.6 unwarped); depth of a point R vs L 3e-5 |
| the same with the prototype's per-draw replay | 9.4 / 27.2 px; 98.2%; **1.67** (p95 6.0; 12.5 unwarped); 3e-5 |

The identity check is the strong one: the replayed commands reproduce the guest pass exactly, so
nothing eye R draws depends on state eye L left behind, on the order of execution or on data that
changed in between. The stereo result equals the prototype's to two decimals, and 23 section 4.3's
1.58 in Hunter's Dream (the residual is nearest-pixel resampling at edges).

### 2.4 Bug found on the way: stereo permutations bound with the wrong resources

The first runs of this build showed an empty world in stereo (fog and the hunter's cape only;
depth at the far plane on 99% of pixels; **0 to 1 remapped draws per frame** against 240 in 23's
session). The A/B (`tools/vr_true_stereo_test.py look`, `out/vr_true_stereo/look_grid.png`):

| Binary, mode | World | Remapped draws/frame |
|---|---|---|
| this build, `BB_XR_MODE=stereo` | missing | 0 |
| this build, `mono` | normal | (no remap) |
| this build, stereo, `BB_VR_HEAD_POS=0` | missing | 0 |
| **the prototype's binary (23)**, stereo | missing | 0 |
| this build with the fix, stereo | **normal** | 210-440 |

So it was not P4.1 but a latent bug of the prototype that the user's growing shader cache exposed.
`PipelineCache::GetProgram` returns the program's single `Info` (the resources of its first
permutation) for every permutation. The pipeline-cache warm-up now loads the mono/alt permutations
of the game's shaders from disk first; a stereo permutation then becomes permutation 1+ and was
bound with permutation 0's `Info`, which has no StereoView buffer: the binding the shader reads its
offset from got nothing (or another buffer), and every perspective draw went off-screen. In 23's
session the stereo permutations happened to be first.

**Fix** (`vk_pipeline_cache.{h,cpp}`, stereo mode only): a permutation whose resources differ from
the program's `Info` (`SameResources`: buffer, image, sampler counts and buffer types) keeps its own
`Info` (`Program::Module::own_info`). `GetProgram` returns it (with the draw's user data and
bindings), the draw-preparation workers leave such draws to the GPU thread, and such permutations
are not written to the disk cache (its entries carry the program's `Info`). Outside stereo mode
`own_info` is never set: mono and alt are unchanged.

### 2.5 P4.1 check: the oracle [M]

`python tools/vr_true_stereo_test.py oracle` (23 section 4.2's method at 31.5 mm, with the fix),
Iosefka's clinic, 61.5% of pixels static (candles and the doll animate):

| | mean | p99 | > 8 |
|---|---|---|---|
| noise (center camera twice) | 0.59 | 2.7 | 0.00% |
| signal (center vs CPU camera moved 31.5 mm) | 7.15 | 52.0 | 26.0% |
| **remap vs CPU camera moved** | **0.67, 0.57** (2 runs) | 3.7, 3.3 | 0.07%, 0.08% |

The shift is at the noise level (23: 0.27-0.34 against 0.22-0.27 noise in Hunter's Dream).

## 3. P4.2: the `3d5ebf4e` copies

**Finding [C]:** the port already turns this pattern into image copies. `Rasterizer::IsComputeImageCopy`
(upstream shadPS4) matches a 64-thread compute shader with two formatted buffers, one read and one
written, each the exact guest memory of an image (`FindImageFromRange`) with equal guest size, pitch
and bits per texel. It then records `Runtime::CopyColorAndDepth` (`vkCmdCopyImage`, or a copy through
a staging buffer for depth to color) and skips the dispatch. `3d5ebf4e` is such a shader (decompiled
from the shader cache: `dst[i] = src[i]` for `i < ud_regs0.x`, local size 64, binding 0 read,
binding 1 written).

**Measured [M]** with `BB_IMAGE_COPY_DEBUG=1` (new: per shader, every 10 s, how often each outcome
happened; `out/vr_true_stereo/look_copies2/session.log`). Per frame, `3d5ebf4e` runs 5 times:

| # | Source image | Destination | Bytes | Today |
|---|---|---|---|---|
| 1, 2 | RGBA8 1920x1080 | RGBA8 1920x1080 image | 8.4 MB each | **already an image copy** |
| 3 | D32S8 1920x1080 (depth plane guest size 0x900000) | R32F 1920x1080 image (guest 0x900000) | **0xb70000**: the depth plane plus the stencil plane behind it | memory path: misses only because the copy is longer than the image (`guest_size != buffer size`) |
| 4 | RGBA16F 1920x1080 | memory with no image (16.7 MB) | 0xff0000 | memory path (image downloaded into the buffer) |
| 5 | RGBA8 1920x1080 | memory with no image | 0x7f8000 | memory path |

The other 64-thread two-buffer dispatches that pass through the check (`8b355b5a`, `a224d59e`,
`a361a1e5`) are not copies (buffer 0 not formatted).

**Consequences.**
- **Mono:** copy 3 could be an image copy of the depth plane (the layouts match: same tile mode,
  pitch, guest size) plus the stencil plane's bytes. That would save a 9.4 MB download and upload
  per frame. It changes the mono path, so it is not done here (alt and mono stay as they are). It is
  a candidate for the performance track once someone checks what reads the 2.4 MB behind the R32F
  image.
- **Stereo (P4.4):** copies 1-3 follow the eye with a second image copy between twins (for 3, the
  depth twin into an R32F twin). Copies 4 and 5 write guest memory that no image covers: whatever
  reads it (as a buffer, or as an image created later) reads eye L's data. Eye R needs a twin
  *buffer* there, filled by an image-to-buffer copy from the source's twin, and the readers' buffer
  bindings substituted, like images. This is the memory-level aliasing risk of 23 (risk 2), now
  located: two copies per frame, 25 MB.

## 4. Cost [M]

`python tools/vr_true_stereo_test.py cost` (`BB_GPU_PROFILE=1`, `BB_UPSCALER=off`, 1080p, the clinic;
busy = GPU time per frame minus "idle between submissions"; three 5 s profile windows per phase;
`out/vr_true_stereo/cost3/report.txt`):

| Phase | GPU busy ms/frame | G-buffer passes | eye-R label |
|---|---|---|---|
| stereo shaders, nothing doubled | 5.55 | 0.67 | - |
| G-buffer doubled every frame, per-draw (23's prototype) | 7.31 (+1.76) | 1.97 | - |
| **G-buffer doubled every frame, record once / execute twice** | **6.29 (+0.74)** | 0.66 | **0.57** |
| nothing doubled, again | 5.61 | 0.66 | - |

- **Eye R costs 0.57 ms, less than eye L's 0.66** (eye L's pass label includes the pass start and the
  twins' initial copies), and the total rises by 0.74 ms. The prototype paid 1.76 ms for the same work
  (480 render-pass switches per frame in this scene).
- **CPU:** the frame stays CPU-bound on the PM4 thread, about 95 FPS in every phase (with the GPU
  profile on). Recording: 2781 commands per frame are recorded once and executed twice; nothing on
  the GPU thread is doubled.
- Added barriers: 2 per eye per doubled render pass (30 per frame), on the vertex stages only.

## 5. P4.4, first slices

Still inside the doubled G-buffer window (`dup`/`dupall`), so every slice is checked by the same
`replay` session (`out/vr_true_stereo/replay_s3/report.txt`).

### 5.1 Slice 1: the eye-R write guard [M]

Eye R must never write memory the game can see.
- **Attachments.** Eye R's attachments are twins by construction. Each captured pass now also
  checks that no eye-R attachment view is one of eye L's (guest) attachment views.
- **Shader writes.** A captured draw whose shaders write a buffer or a storage image (a non-special
  buffer with `is_written`, or GDS; any image with `is_written`) would write guest memory a second
  time, with eye R's values.
- **What the guard does.** Such a pass is executed for eye L only and gets no eye R. It is counted
  ("write guard: n passes without eye R (guest writes), m (attachment not a twin)" in the doubled
  pass's log line). `BB_STEREO_WRITE_AUDIT=1` names the first writing shaders.
- **Result.** The G-buffer pass: 0 and 0 in every run.
- **Negative control.** `guardtest=1` (control file) makes every captured draw count as writing.
  The guard then left all 15 passes without eye R (0 executed twice), and 7 of eye R's 8 targets
  differ from eye L's (they keep the pre-pass contents). So the counter is live and the skip works.

### 5.2 Slice 2: eye-R copies of the copy shader's buffer targets [M]

Copies 4 and 5 of section 3 write memory that no image covers. In the doubled window the RGBA8 one
(8.4 MB) runs; the RGBA16F one (16.7 MB) comes after the window's end.
- `Rasterizer::StereoAfterDispatch` runs after every dispatch (inert outside stereo).
- For `3d5ebf4e` inside a doubled pass, whose destination has no image and whose source image has a
  twin this frame, it makes eye R's version of the destination bytes:
  1. the twin is copied into a staging buffer in rows of the guest pitch;
  2. it is tiled into a per-target **twin buffer** (`TwinBuffer`, keyed by the guest address) with the
     texture cache's own tiling shader (new `TileManager::TileLinear`, mip 0, additive).
- That is exactly how the guest copy obtains its source memory (download, tile, then the dword
  copy).
- Dumps: the guest buffer after the dispatch (eye L) and the twin buffer (eye R), as target 100+.

**Identity (`same=1`):** of 8,355,840 bytes, 53,699 (second run: 53,720) differ. They are 15,359 dwords, all in the
surface's last 64-row tile band: at most the 15,360 padding texels (rows 1080-1087 of the 1088-row
tiled surface, 8 x 1920). Those padding texels exist only in the guest's download staging, which
holds leftovers there; eye R's staging has none. Every texel of the image matches. The test checks
exactly this (`replay`: "at most the padding texels").

Nothing reads the twin buffers yet. Their readers come after the window (lighting, AO), with slice 3
and the extended region.

## 6. Next steps

0. **P4.4 slice 3, compute per eye.** Dispatches inside the region that read twinned images or twin
   buffers are captured like a render pass (Scheduler::BeginCapture around the dispatch's commands)
   and executed again for eye R. Eye R gets its own descriptors: the push-descriptor command
   recorded in a capture carries (guest view or buffer, twin) pairs and substitutes them on the
   second execution. The same substitution serves the graphics passes after the G-buffer
   (sampled twins). The write guard of slice 1 applies unchanged (a dispatch that writes guest
   memory gets a twin target or no eye R).


1. **P4.4 twins for the whole region.** The capture works per render pass; what is missing:
   - **sampled and storage view substitution for eye R.** The recorded push-descriptor command is the
     same bytes for both executions, so eye R's image views cannot be patched like the StereoView
     value. Plan: the push-descriptor closure (`Pipeline::BindResources`, recorded while capturing)
     carries a small table of (guest view, twin view) pairs, and the arena's second execution runs
     with an "eye R" flag that makes it substitute them. Only captured commands carry the table.
   - **compute replays**: dispatches between the region's passes (AO, light shafts, a040875f,
     9a9cf8a9) recorded the same way (a capture around the dispatch), with twin storage images;
     mono list: GPU particles `CS_VFX_*`, exposure `1f2b3c9e`, the LUT.
   - **the `3d5ebf4e` copies between twins** (section 3) and twin buffers for the two copies whose
     destination is not an image.
   - twin coherence (a mono write invalidates) and the write audit (no guest memory write from eye R).
   The region then runs from the G-buffer to the first Scaleform draw, and the `same=1` identity
   check extends to it directly: eye R's final image must equal eye L's bit for bit.
2. **P4.5 per-eye temporal state.** Research notes from the coordinator
   (`scratchpad/research_vr_injectors.md`, `research_cyberpunk_vr.md`, `research_dlss_vr.md`):
   - TAA/DLSS/FSR history and jitter strictly per eye; never let one eye sample the other's history
     (UEVR: native stereo keeps TAA working, alternate-eye modes ghost);
   - one DLSS feature per eye with its own color, depth, motion vectors and jitter, not one
     side-by-side evaluation; reset on scene cuts;
   - prefer DLSS/DLAA over FSR 3 in VR; no frame generation;
   - the 16:9 render size against the about 5:4 eye aspect (P4.7).
3. **P4.6** both eyes per frame through `AcquireEye`/`ReleaseEye` (23 section 6).

### Risks

- Stereo permutations with their own `Info` are bound on the GPU thread, not by the
  draw-preparation workers: some CPU cost in stereo mode (not visible at 95 FPS here).
- Indirect draws inside a doubled pass do not pass `StereoBeforeEmit`. If one lands in a captured
  pass it is replayed with eye L's offset; none was seen (the identity check would show it as a
  difference only if the eyes differ; the count of "draws outside a capture" covers direct draws).
- A `CommandBuffer()` caller inside a pass cuts the capture (counted; 0 so far).
- The non-threaded recording mode (`ThreadedRecording` toggle off) cannot capture: those passes
  get no eye R (counted as "outside a capture").
- The flat tests now press cross until the world loads (the title asks about the network first
  when offline); `tools/vr_stereo_poc.py` changed accordingly.

## 7. Running it

```
build:  ninja -C out/gpu bb-probe (MSYS2 CLANG64, under the shared build lock), then copy
        out/gpu/bb-probe.exe to out/gpu/bb-probe-stereo.exe (never over out/bb-probe-all*.exe)
tests:  python tools/vr_true_stereo_test.py replay [--yaw D]   P4.3 identity + stereo + prototype
        python tools/vr_true_stereo_test.py cost              GPU ms: off / per-draw / replay
        python tools/vr_true_stereo_test.py oracle            P4.1: 23's oracle at 31.5 mm
        python tools/vr_true_stereo_test.py look [--xr-mode M] [--env K=V]   views at 4 yaws (A/B)
        (each refuses to start while another bb-probe or bb-play runs)
env:    BB_IMAGE_COPY_DEBUG=1  why a 64-thread two-buffer dispatch is or is not an image copy
```
