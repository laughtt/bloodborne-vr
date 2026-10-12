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
| P4.4 twins for the whole region | **started: 2 slices green, slice 3 built** | write guard with negative control; eye-R copies of the copy shader's buffer targets (section 5) [M]; slice 3, dispatches per eye and eye-R descriptors: unit-tested, not yet run in the game (5.3) [U] |
| P4.6a hand-off (XR plumbing) | **built, unit-tested; not yet on a headset** | section 8 |
| P4.5, P4.7, P4.8 | not started | section 6 |
| Hybrid stereo (B, shortcut to a first headset build) | **built, unit-tested; not run in the game yet** | section 9 [U]; warp measured in the game (9.4, 9.7); per-eye DLSS (9.8) [M] |
| Full stereo (eye R rendered from the G-buffer to the display pass) | **built, measured flat; not on a headset yet** | section 10: same=1 byte-identical, eye R vs the oracle at the noise floor (0.12-0.23 vs noise 0.13; hybrid 2.6), +3.3 ms/frame [M] |

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

### 5.3 P4.4 slice 3: compute per eye and eye-R descriptors [C][U]

The foundation for running lighting, AO and post per eye (D). Files: `vk_scheduler.{h,cpp}`
(dispatch capture, eye flag), `vk_stereo_substitute.h` (new, the substitution, device-free),
`vk_pipeline_common.cpp` (the push-descriptor command), `vk_stereo.{h,cpp}` (a separate block
"P4.4 slice 3" plus small hooks), `vk_rasterizer.{h,cpp}` (two calls per dispatch path),
`texture_cache/image_view.{h,cpp}` (each view keeps the format, aspect and type it was created with).
Control: `compute=0|1` (default 1) switches all of it; `compute=0` is slice 2's behavior.

**Eye-R descriptors (the mechanism).**
- Data: `Stereo::DescriptorSubstitute` = one (guest image view -> twin view + layout) or one
  (guest buffer + descriptor offset -> twin buffer + offset) pair. The draw or dispatch being bound
  builds its table (`Rasterizer::StereoCollectSubstitutes`): for every bound image that a twin
  doubles in this frame, each of its views the descriptors use gets a twin view made to match
  (same format, aspect, type, swizzle; mip 0, layer 0; cached on the twin); for every bound buffer
  that lies inside a twin buffer of this frame (slice 2), the same range of the twin.
- Recording: `Pipeline::BindResources`, only while the scheduler captures (a pointer test, so zero
  cost outside stereo), takes the table (`Stereo::TakeSubstitutes`) and copies it into the arena
  next to the writes. The push-descriptor command then holds both. Without push descriptors, a
  second descriptor set is written instead and the command binds one or the other.
- The eye flag: `RecordArena::Execute(cmdbuf, eye_r)` sets a thread-local
  `RecordArena::ExecutingEyeR()` for the duration of one execution (restored after; nested
  executions keep their own). Eye L's execution (in place, `FinishCapture`) pushes the recorded
  writes; eye R's (the capture-end callback) pushes a copy with the table applied
  (`SubstitutedWrites`: sampled, storage and combined images, storage buffers; samplers, other
  views and other buffers unchanged).
- Layouts: between operations every twin rests in its attachment layout. The twins one capture's
  eye R uses through descriptors go to GENERAL just before eye R's execution and back just after
  (outside the render pass). A twin that is an attachment of the captured render pass is never
  substituted (eye R renders into it; it stays eye L's view, counted as "kept").

**Dispatches per eye.** `Rasterizer::StereoBeginDispatch` (direct and indirect dispatches, after
the resources are bound and any render pass ended) decides per dispatch in the doubled region:

| Dispatch | Plan (`PlanDispatch`) | Eye R |
|---|---|---|
| reads no twin | once (both eyes share it) | twins of what it writes become stale (readers then use the guest's contents) |
| reads twins, writes only twins: storage images of the region's size (the doubled pass's depth size, 2D, one mip, a format with storage and attachment use) or ranges inside twin buffers | captured (`Scheduler::BeginComputeCapture`/`EndCapture`) and executed again | its descriptors substituted; a fresh storage twin starts as a copy of the guest image (like the attachments' twins) |
| reads twins, writes anything else: buffers (exposure `1f2b3c9e`, `CS_VFX_*` particles, counters), GDS, storage images of another size or not twinnable (LUT) | eye L only (shared) | as "once"; counted per category |

The write guard of slice 1, for dispatches: before a dispatch is captured, every written storage
image and buffer descriptor must have a substitute; otherwise it runs for eye L only. Eye R
therefore writes only twins. `guardtest=1` makes every dispatch that would run twice count as
writing shared memory (the negative control). A dispatch capture cut short by a direct
`CommandBuffer()` or a `BeginRendering` gets no eye R (counted; its written twins go stale).

**Log** (the doubled pass's line, appended): `compute: n dispatches executed twice, n once (no twin
read), n cut short, n twin targets; eye L only (shared): n buffer writes, n GDS, n storage images
not region size, n guardtest; n twins stale; eye-R descriptors: n views, n buffers substituted, n
views kept`. GPU profile: eye R of dispatches is its own label ("stereo: eye R of doubled
dispatches").

**Verified so far.** Builds clean. `stereo-substitute-test` [U] (`ninja -C out/gpu stereo-substitute-test`,
no device, no game): sampled, storage, buffer and array substitution with the originals
untouched; the eye flag per execution and its restore; the write policy table incl. the negative
control. **Not yet run in the game:** the doubled window is still the G-buffer pass, whose
dispatches are HLE copies and the copy shader (section 3), so the replay session is expected to
show 0 dispatches executed twice there; the graphics passes' substitution is live in it (a pass
sampling an earlier pass's target). `python tools/vr_true_stereo_test.py replay` now runs five
doubled passes: identity (must stay bit-identical with `compute=1`), stereo, per-draw, guard
(also requires 0 dispatches executed twice) and identity with `compute=0`; it fails when the log
lacks the compute counts or a dispatch capture was cut short. Real coverage comes with D (the
region extended past lighting, AO and post), where `same=1` must still give eye R == eye L.

**Risks.** Twins of formats with storage support are now created with storage use (possible loss
of color compression on some GPUs; the cost test shows it). Copies that bypass dispatches
(`IsComputeImageCopy`, HLE copies, clears) do not mark twins stale yet. A twin whose slot is
replaced (same address and format, new size) while a capture holds it would leave a dangling
pointer in that capture's list (the game's targets do not change size). Views of a twinned image
in another format than the twin's (sRGB written as UNORM storage) are kept, not substituted.

## 6. Next steps

0. **P4.4 slice 3, compute per eye** (built, section 5.3; run `replay` to confirm). Dispatches inside the region that read twinned images or twin
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
        python tools/vr_true_stereo_test.py warp [--ipd 0.063] [--yaw D]   hybrid stereo (section 9)
        ninja -C out/gpu stereo-substitute-test && out/gpu/stereo-substitute-test.exe
                                                              slice 3 unit test (no game, no device)
        (each refuses to start while another bb-probe or bb-play runs)
env:    BB_IMAGE_COPY_DEBUG=1  why a 64-thread two-buffer dispatch is or is not an image copy
        BB_STEREO_HYBRID=0     hybrid stereo off (stereo mode default: on)
        BB_STEREO_IPD=<m>      eye distance until the runtime gives one (default 0.063)
```

## 8. P4.6a hand-off: both eyes of a frame as one pair

Date: 2026-10-12. Status: **plumbing built and unit-tested [U]; nothing here ran on a headset yet.**
Everything is behind `BB_XR_MODE=stereo`; `alt` and `mono` take the same code paths as before (the new
presenter code is guarded by `Frame::pair_on`, the bridge's by `Bridge::pair`, H1's by `vh.stereo`).

### 8.1 Data flow

```
H1 (game thread)   head + overscanned union frustum from ONE snapshot (GetViewsAt, 8.3), no eye offset;
                   BbVrRenderView: seq, head pose, frustum, camera rows, both eyes' offsets from the head
Presenter flip     PrepareFrame -> PreparePair: Xr::MatchRenderView(scene camera rows) -> PairDecider
                   (Eye / Hold / Mono); eye R's final image from StereoEyeR::Take (8.4) or nothing
Present            Eye: AcquireEye(0), AcquireEye(1); copy eye L's frame image and eye R's; submit;
                   Xr::ReleaseEyes(PairView)   <- ONE call
                   Hold: nothing (both eyes keep the pair); Mono: AcquireFlat -> the cinema screen
Bridge             ReleaseEyes releases both chains under one state_mutex hold; the frame loop builds its
                   layers under the same lock, so it shows the previous pair or this one, never half
```

- **Poses.** `xrstereo::PairEyePoses` (xr_stereo.h): both eyes get the head's orientation (the image is
  rendered along the head's axes with the union frustum, as mono does, not along a canted eye axis) and the
  position `head + R(head) * eye_offset[eye]`. The offsets are `vrs_pair_offsets` (vr_stereo_math.h): the
  located eye positions in the head's frame, from the same `xrLocateViews` snapshot as the head. So the
  baseline is the runtime's IPD whatever the head does, and the two poses cannot come from two snapshots.
  Without eye offsets (fake head, implausible runtime offsets) the bridge shows the images with the views
  current at release (logged once), as alt does.
- **Frustum.** One union frustum with the overscan of alt (`BB_VR_HEAD_FOV=hmd` is the default in stereo,
  `BB_XR_OVERSCAN` applies), the same for both eyes.
- **Pairing.** No eye tag, no schedule: the finished frame's scene camera (CameraMotion) is matched to the
  render view whose camera rows equal it (`Xr::MatchRenderView`, 2e-3 as `ReleaseFlat`). A frame that matches
  none is held (both eyes keep their last pair); after 30 held frames in a row the flat screen takes over
  until a frame matches (`xrstereo::PairDecider`, as alt's `fallback_frames`). A cutscene
  (`bbgpu_vr_set_cinema`, H1's `vr_fp_cutscene`) goes flat at once. Flat frames (menus, title, loading,
  cutscenes) go through `AcquireFlat`/`ReleaseFlat`: the cinema screen, same image in both eyes, and the
  bridge invalidates the pair (`AltEyes::ReleaseFlat`) until the next one.
- **HUD layer** is on in stereo (`xr_hud.h` `ParseMode`): the UI is its own quad over both eyes.
- **Half pair.** If one `AcquireEye` fails (the chain is not free), the other eye is still released: the
  bridge releases what is writing, counts a half pair and logs it once. Both eyes are acquired before any
  copy, so this only happens when the runtime has no free image.

### 8.2 Bridge ABI 10

`BBPORT_XR_BRIDGE_ABI` 9 -> 10 (9 was the mirror UI). The struct only grew; hosts built against 9 or less
work unchanged.

| Entry | What |
|---|---|
| `ReleaseEyes(const BbXrEyeView view[2])` | releases both eye chains under one `state_mutex` hold; per eye pose + frustum; `pose.valid` 0 uses the current views |
| `GetViewsAt(int64 display_time, BbXrViews*)` | `xrLocateViews` + head at any XrTime (any thread, serialized with Shutdown by `g_locate_mutex`); the screen and pointer come from the latest `GetViews`; falls back to it on failure |
| `GetLatency(BbXrLatency*)` | valid, smoothed release -> display in display periods, display period, **XrTime now**, pairs, half pairs; reads only the leaf `g_views_mutex` (never blocks behind the frame loop's `xrEndFrame`) |

`BB_XR_MODE=stereo` sets `alt` internally (the same two chains and layer: a pair is two eye images with
their poses), plus `pair` for the rest. `AcquireEye` returns -1 unless the mode is alt or stereo. The
runtime extension `XR_KHR_win32_convert_performance_counter_time` is enabled when listed (the instance is
created again without it if that fails) to know the XrTime now; without it `GetLatency.now_xr` is 0 and
the latency prediction below is off (H1 keeps `bbgpu_vr_views`).

The bridge's 10 s log in stereo replaces the alt line:
`stereo: N of M display frames showed the pair; pairs released P (half H); one eye new a%, both new b%,
both old c%; release to display X display periods (measured | not enough samples yet, n pairs measured)`.
**Both new should be near 100% when the game runs at the headset rate**, and "one eye new" must be 0.

### 8.3 Latency item 1: views for the real display time

Before: H1 used the views predicted for the bridge's current frame, so a camera set up now was shown 2-4
headset frames later with a pose that old (findings 22 section 4).

- The frame loop samples, the first time it shows a pair, `predicted display time - XrTime at ReleaseEyes`
  into `xrstereo::LatencyTracker` (smoothed 0.1, stalls over 8 periods rejected, valid after 8 samples).
- The presenter measures its own part: H1's `SetRenderView` -> `ReleaseEyes` wall time (smoothed).
- `bbgpu_vr_display_ahead` returns `PredictDisplayTime(now_xr, camera->release, latency * period)`
  clamped to [now, now + 8 periods]; H1 asks `bbgpu_vr_views_at(that)` in stereo mode only. Mono and alt
  call `bbgpu_vr_views` as before. H1 logs the figures once (and each second with `BB_VR_HEAD_LOG=1`):
  `VR head: stereo: views for X ms ahead (camera -> release a ms + release -> display b periods of c ms)`.
- `GetViewsAt` is a plain `xrLocateViews`: a runtime may refuse a time far from the predicted one; the host
  then gets the latest snapshot (head and eyes still from one snapshot).
- Not done here: items 2-6 of the latency list (phase-lock the game to the headset, copy off the desktop
  present, depth layer, late latching).

### 8.4 The eye R interface (for the stereo renderer)

`gpu/shadps4/video_core/renderer_vulkan/vk_stereo_eye_r.h`:

```cpp
namespace Vulkan::StereoEyeR {
struct Request { u8 slot; u32 width, height; vk::Format format; vk::Image eye_l; u64 view_seq; u64 display_buffer; };
struct Image   { vk::Image image; vk::ImageLayout layout; };
using Provider = std::optional<Image> (*)(const Request&);
void SetProvider(Provider);                     // vk_stereo.cpp registers once
std::optional<Image> Take(const Request&);      // the presenter, once per paired frame
}
```

`Presenter::PrepareFrame` calls `Take` on the GPU thread right before it flushes the frame's commands, so
a provider records into the draw scheduler and the frame's ready tick covers its work. The image is eye
R's FINAL picture at the stage of `Frame::image` (upscaled like eye L, no HUD), `width x height` of the
request, in the request's format (same channel order is enough; the presenter blits), valid and unchanged in
`layout` until that presenter frame's `present_done` (ring-buffer by `slot`). No provider, or `nullopt`:
the presenter copies eye L's image into eye R (the identity case: it must look 2D), counted as "eye L's
image again" in the 10 s presenter line
`VR stereo: N frames: P pairs, H held, C cutscene; eye R from the stereo renderer X, eye L's image again Y`.

### 8.5 Tests [U]

| Test | Covers |
|---|---|
| `bbport_xr_stereo_test` (375 checks) | `ParseMode` words; `PairEyePoses` (rotated head, canted 2 mm, baseline constant under 8 head rotations, no offsets: no pose); pair atomicity (200 pairs, counters move together, 0 one-eye frames, flat frames invalidate, stale pairs stop showing, half releases wait); `PairDecider` (29 held, then flat, a match brings the pair back, a cutscene is flat at once); `LatencyTracker` (valid after 8, smoothing, convergence, stalls and bad data rejected); `PredictDisplayTime` (math, clamps, no XrTime gives 0, end to end with a simulated 72 Hz loop) |
| `bbport_xr_math_test` | HUD layer on in `stereo` |
| `vr-head-test` (2110 checks) | `vrs_pair_offsets`: the offsets of six head poses rebuild the located eyes exactly, implausible snapshots give the default IPD split |

### 8.6 What a headset test should look for

1. Bridge log at start: `mode stereo (BB_XR_MODE=stereo, experimental): true stereo ...`, then
   `first true stereo frame on the headset` and the 10 s `stereo:` line with both new near 100%, one eye
   new 0%, half 0.
2. Presenter log: `VR stereo: ... pairs ... eye L's image again` (until the renderer registers a provider
   every frame is "eye L's image again": the world must look flat 2D, but head-tracked and steady, with no
   doubling and no eye swimming; that is the identity case). With the renderer's eye R (`BB_STEREO_CONTROL`
   offsets) the pair carries the disparity.
3. H1 log: `VR head: stereo: views for N ms ahead (...)`. Turning the head fast should trail less than
   `alt` or `mono`; if the line is absent the runtime has no time conversion (the bridge's mode line says
   `no time conversion`).
4. Menus, title and loading on the flat cinema screen after ~30 held frames; the pair back when the
   world loads; cutscenes flat at once.
5. The HUD as a quad over the scene in both eyes.

### 8.7 Not done / limits

- Eye R content is eye L's image until the stereo renderer (vk_stereo.cpp) registers a provider.
- No headset run; the time conversion and `xrLocateViews` at a future time are runtime-dependent
  (untested here).
- `BB_VR_STEREO_SCALE` and `BB_VR_STEREO_IPD` do not apply (the renderer shifts per eye; the layer poses
  use the runtime's offsets).
- DLSS runs as one stream (eye 0) on the eye L image in stereo; per-eye temporal state is P4.5.

Plug-in (task B's accessor, branch worktree-agent-a6ee7c81dbcd10082): register a provider that returns
`Stereo::EyeRForDisplay(request.display_buffer)` as `{image, layout}` (its layout is always GENERAL;
its format may be RGBA8 sRGB/UNORM, which the presenter's copy blits). `display_buffer` is the flipped
display buffer's guest address, the key B tags its eye R ring with.

## 9. Hybrid stereo warp

The shortest path to a true-stereo headset image without P4.4 slice 3 (compute and sampled-view
substitution for eye R): the geometry is drawn for both eyes, everything after it once, and
eye R's final image is warped from eye L's with eye R's own depth.

Files: `vk_stereo.{h,cpp}` (the frame logic, `RegionEnd`, `Rasterizer::StereoWarpAtFlip`, the accessor),
`vk_stereo_warp.h` (the math, header-only), `host_shaders/stereo_warp.comp`,
`tests/test_stereo_warp.cpp` (`stereo-warp-test`), `tools/vr_true_stereo_test.py warp`.

### 9.1 What a frame does (`BB_XR_MODE=stereo`, hybrid on)

1. **G-buffer for both eyes, every frame** (what `dupall=1` did): eye L at -ipd/2 into the guest
   targets, eye R at +ipd/2 into twins, record once / execute twice (section 2). No dumps unless
   `dup=<n>` asks for them (a new `dump_pass` flag replaces the `dup_all` checks).
2. **The rest of the 3D region runs once, for eye L at -ipd/2** (`hybrid_frame`: every remapped
   draw of the region gets eye L's offset, the oracle-validated offset path of section 2.5).
3. **At the region's end** (before the first Scaleform draw, `UiComposition::MovieShader`, or at
   the display pass for a 3D frame without UI) the frame's warp inputs are kept: eye L's depth,
   eye R's depth twin, P00 and the depth constants, the eye positions.
4. **At the frame's flip** (`Presenter::PrepareFrame` calls `Rasterizer::StereoWarpAtFlip` with
   the display buffer being flipped, same GPU thread and scheduler) one compute pass writes eye R
   from the **display buffer** (the game's finished frame, HUD-free with the HUD layer; 9.7 says
   why not `ldr_target`): for each eye-R pixel, eye R's depth twin gives z, eye L's source is
   `u = x_R + shift / z` (sub-pixel), sampled linearly between columns `floor(u)` and
   `floor(u) + 1`, each tap depth-tested (`|z_L - z_R| <= 0.02 z_R + 0.01`, the oracle test's
   rule; one failing tap: the other alone). Both failing (a disocclusion, or outside eye L's
   image): the nearest direct pixel along the row on the **farther** side is the background the
   foreground uncovers (search radius `|shift| / 0.2 m`, at most 96 px); its source is mirrored
   into the hole (`k` px from the background pixel -> `k` px past its source) and averaged over
   three columns depth-tested against the background's z, so the hole continues the background's
   texture instead of repeating one column. With eye R's albedo twin a second dispatch then
   recolours the hole pixels from eye R's own G-buffer (9.10). Skipped (logged once, eye L in both
   eyes) when a newer frame's region already started or the display buffer is not 8-bit RGBA.
5. Eye R goes into a ring of 3 images (by frame) for the presenter (9.3).

Off: `BB_STEREO_HYBRID=0` or `hybrid=0` in the control file, and any frame with `offset` not 0
(the oracle). `tools/vr_stereo_poc.py` sets `BB_STEREO_HYBRID=0` so its checks and those of
`replay`/`cost`/`oracle` keep their meaning; `warp` turns it back on.

### 9.2 The disparity, exactly

From the remap pass: eye at m metres along the camera's right axis draws `clip.x - P00 * m`
(`Stereo::ViewAt`), clip w = view z and depth unchanged. Pixel `x = (ndc.x + 1) / 2 * W`, so a
point at view depth z lands at `x_center - P00 * m / z * W / 2`, and

```
x_L = x_R + P00 * (m_R - m_L) * W / 2 / z = x_R + shift / z
    = x_R + P00 * ipd / z * W / 2          (m_L = -ipd/2, m_R = +ipd/2)
```

`shift` is computed per frame from the same P00 the vertex shift used (scene constants float 52),
W is the scene target's width. z comes from the depth buffer through the projection's own
constants (floats 62, 63: `depth = zs + zo / z`, `z = zo / (depth - zs)`, as
`camera_motion.comp`). The game's projection is **standard Z** (D3D, 0 near, 1 far; zs = f/(f-n),
zo = -fn/(f-n), the constants `tools/vr_stereo_poc.py` uses). Cleared depth 1 gives z = far
(3000 m, under 0.03 px of disparity), so the sky needs no special case; the same formula would
also hold for a reversed projection (its zs/zo), nothing assumes the direction. With `same=1`
`m_R = m_L`, shift is 0 and the shader copies eye L's own pixel (identity, byte for byte);
an inverted IPD flips the sign of shift.

### 9.3 For the presenter (task A)

```cpp
namespace Vulkan::Stereo {
struct EyeRImage { vk::Image image; vk::ImageView view; vk::Format format; u32 width, height;
                   vk::ImageLayout layout /* eGeneral */; u64 frame; VAddr display_buffer; };
u64 Frame();                                          // renderer frame counter
std::optional<EyeRImage> EyeRFinal(u64 frame);        // none: that frame has no eye R
std::optional<EyeRImage> EyeRForDisplay(VAddr buf);   // newest frame whose display pass wrote buf
void SetIpd(float metres);                            // the runtime's IPD (control "ipd=" wins)
}
```

- The image is `R8G8B8A8Unorm` storage (mutable format) holding the display buffer's bytes as
  stored; `format`/`view` are eye L's view of the display buffer (`GetFrameViewFormat`, e.g.
  `B8G8R8A8Srgb` for the game's `A8R8G8B8Srgb`, alpha swizzled to 1), so the presenter samples
  eye R exactly as it samples eye L.
- Layout is always `GENERAL`; a consumer reads it there and does not change it.
- Written by a compute shader on the graphics queue at the frame's flip, followed by a barrier to
  all later commands of the queue. Reused 3 frames later.
- Pairing: the display pass records its display buffer address on the pending warp, the flip of
  that buffer runs it; a repeated flip of X takes the same image (`EyeRForDisplay(X)`).
- `BB_STEREO_EYE_R_FLAT=1` (tests): eye R's presenter chain also runs for frame dumps of flat
  frames, dumped as `output_r_<n>` (next to `output_<n>`) and `eyer_warp_<n>`.

### 9.4 Evidence

- **[U] `stereo-warp-test`** (`ninja -C out/gpu stereo-warp-test`, CPU, the header the shader
  mirrors): a synthetic scene (a 0.4 m post at 1.5 m before a wall at 10 m, sky past it) drawn
  per eye the way the remap does. Zero eye distance gives eye L's own pixel everywhere (one tap,
  weight 1: byte-exact); at 63 mm every matched pixel is the same surface, two-tap pixels (654)
  hit the surface point to 0.1 footprint (mean error 0.0000, the colors are linear in x), one-tap
  pixels within one; the disparity has the sign of `m_R - m_L`, inverted IPD flips it, the
  region the post uncovers (26 of 960 px) is filled from the wall, never from the post, with 11
  distinct sources (mirrored, not one stretched column); at 200 mm the fill still finds the
  wall. PASS.
- `stereo_warp.comp` compiles with the build (glslang) and passes `spirv-val` (Vulkan 1.3).
- bb-probe builds without warnings (`out/stereo_b/bb-probe-stereoB.exe`).
- **[M] In the game** (`out/stereo_ab/run3/warp/report.txt`, 2026-10-12, display-buffer source,
  sub-pixel taps): warps at 100% of flips, 0 skipped; `same=1` byte-identical; same surface at
  `x + d` 89.9% vs `x - d` 43.3%, `|R - L|` 3.79 vs 19.55; inverted IPD flips; eye R vs the
  +ipd/2 oracle on static pixels: noise 0.49, signal 16.50, warp 5.27 / 5.33, residual / signal
  0.32 (PASS; the previous build, nearest taps from `ldr_target`, gave 0.27 in its own session:
  the flat display buffer carries the HUD, which the warp shifts). On the same dumps (warp2 vs
  the oracle, static pixels) the old nearest + stretch algorithm, emulated, gives a mean error
  of 5.67 (4.02 without the HUD band), the new two-tap + mirror 5.33 (3.75), the GPU's own image
  5.33 (3.74; it matches the emulation to 0.18); disoccluded pixels 18.0 vs 18.2 (the mirror
  removes streaks rather than lowering the error). `python tools/vr_true_stereo_test.py warp --probe <exe>` checks, in one
  flat session: warps in >= 90% of frames;
  `same=1` byte-identical; the depth of the two eyes agrees at `x + d` more than at `x - d`
  (independent of the warp) and eye R's color likewise; inverted IPD flips the logged shift;
  eye R against the oracle (the whole region at +ipd/2, `offset=+ipd/2 hybrid=0`) on static
  pixels next to the noise and the signal, PASS below 0.35 x the signal.

### 9.5 Cost [S]

Eye R's G-buffer +0.74 ms (measured, section 4). The warp: one 1920x1080 pass, 3 fetches and a
store per pixel, the ~2% disoccluded pixels up to 2 x 96 more pairs: about 0.1-0.2 ms. So about
+0.9 ms GPU per frame over mono at 1080p, no CPU cost beyond the replay's. A second DLSS
feature for eye R costs much more than that: 3.3-3.7 ms at 1080p -> 2160p (9.8). The G-buffer
-guided hole fill adds about 0.27 ms (9.10).

### 9.6 Known artifacts (expected, to check in the headset)

- **View-dependent shading is eye L's**: specular highlights, reflections, rim light move with
  the surface instead of shifting between the eyes (subtle at 63 mm).
- **Transparent and screen-space effects sit at the opaque depth behind them**: particles, fog,
  light shafts, decals drawn after the G-buffer, the weapon's trails. Their disparity is the
  background's; a sparkle in front of a far wall looks painted on the wall.
- **Disocclusions** (about 2% of pixels, bands up to ~25 px at 1080p next to near objects) get
  eye R's own albedo times the background's lighting (9.10); where that has no match (no
  background of the same depth and facing nearby, sky) they keep the background mirrored into
  the hole (3-tap average). The edge band eye L does not see falls back to the clamped column.
- **Sub-pixel sampling** is linear along x only (y has no disparity); the taps are depth-tested,
  so a silhouette never blends foreground into background. Depth is nearest-sampled at render
  size, so a silhouette follows depth-pixel steps.
- **Without the HUD layer** (`BB_XR_HUD` off, or the flat tests) the display buffer holds the HUD,
  which is then warped with the scene depth behind it.
- Anything opaque drawn after the G-buffer pass (forward-lit objects, if any) is missing from
  eye R's depth: it warps with the depth behind it or gets filled.
- With an upscaler, eye R is warped from the (upscaled) display buffer with render-size depth (the
  depth is sampled scaled). With `BB_XR_UPSCALE=dlss` each eye has its own DLSS feature and
  history (9.8); eye R falls back to FSR 1 only when its feature cannot be created.
- Indirect draws and passes the write guard skips (section 6, risks) leave eye R's depth without
  them; the warp then fills those pixels as disocclusions.

### 9.7 Eye R bluish: the warp read `ldr_target`, not the display buffer [M]

Headset report (stereo, hybrid, DLSS on eye L): eye R bluish, eye L normal. Flat frame dumps
(`out/stereo_ab/run3/colour1`, same=1 so eye R = eye L's pixels; region 1520x880):

- The scene target the warp read is `R8G8B8A8Unorm`; the display buffer is a `R8G8B8A8Srgb`
  texture-cache image the presenter samples through a `B8G8R8A8Srgb` view (`A8R8G8B8Srgb`). Byte
  i of the scene correlates with byte i of the display buffer (chroma b0-b2 r = 0.95), so both
  hold the PS4 layout (byte 0 = blue). Eye R's view read byte 0 as red: **R and B swapped**
  (displayed R-B of eye L vs old eye R r = -0.93); a warm scene turns blue.
- The `Unorm` view also skipped the sRGB decode, so the post pass encoded twice (old eye R mean
  RGB 123.0 / 122.3 / 121.4 vs eye L 66.7 / 66.0 / 66.1).
- The game's display pass applies a **brightness curve** after `ldr_target` (the same in every
  channel, about `y = x^0.85`: 16 -> 24, 64 -> 78, 128 -> 142, 200 -> 208), which eye R missed.

Fix: eye R is warped at the flip from the display buffer itself and sampled with eye L's view
format (9.1, 9.3). After (`run3/colour2`, `BB_STEREO_EYE_R_FLAT=1`, presenter outputs): same=1
eye L and eye R final images byte-identical (RGB 66.98 / 66.21 / 65.97 both); at 63 mm the
channel means differ by -0.21 / +0.37 / +0.58 (disparity), displayed R-B correlation 1.00.

### 9.8 Eye R's own DLSS [U, M]

`BB_XR_MODE=stereo` with `BB_XR_UPSCALE=dlss`: both eyes go through DLSS every frame, each with its
own NGX feature and history (bridge slots 1 and 2, the per-eye machinery of alt mode, findings/37
section 3). Alt and mono are unchanged (no `pair` flag, no eye offset, no new profiler segments).

Per frame, in `Presenter::PrepareFrame` after the warp:

1. **Eye L** (`XrDlss::Run`, `Input::pair`, `Input::eye_m` = the warp's eye L offset): the display
   buffer, the G-buffer depth, the one-stream tracker (history, resets, cuts, menus as before).
   New: its camera is the game camera moved to eye L (`XrDlssLogic::EyeCamera`), so its motion
   vectors include the eye's lever arm under head rotation (the game camera's reprojection is off
   by offset x rotation: 2.6 mm, over a pixel at 1 m, for a 5 degree turn [U]).
2. **Eye R** (`XrDlss::RunEyeR`, right after, every frame eye L was evaluated and this flip's warp
   is current, held and flat-test frames included so both histories stay in step):
   - colour: the warp's eye R image (render size, the display buffer's bytes, eGeneral), read
     through the same UNORM view format as eye L's (`DlssColorFormat`, sampled-only view);
   - depth: eye R's G-buffer depth twin (`EyeRImage::depth_*`, eGeneral; valid while
     `Frame() == frame + 1`, checked);
   - camera: eye L's game camera moved to the warp's eye R offset; motion vectors from it to eye R's
     **previous** camera (`XrDlssLogic::PairEyeR`), through the same `camera_motion.comp` pass on the
     twin depth;
   - jitter: the same as eye L (below);
   - arm/weapon mask (`BB_XR_DLSS_MASK`): the same pass on eye R's depth with eye R's projection
     (radial distance from eye R);
   - history: reset when eye L's resets (cut, menu, mono/held gap, settings), on eye R's first frame,
     after more than 4 frames without an eye R evaluation, and when its feature is (re)created.
3. Eye R's DLSS output goes through eye L's post-DLSS chain (the same RCAS settings) and the post
   pass into the frame's eye R image (`RecordEyeRChain` with `dlss_view`; the warp image is not
   touched).

**Jitter.** The G-buffer pass is recorded once and executed twice, so both eyes' G-buffers carry
the same viewport shift (and the warp runs on eye L's jittered frame). The warp samples eye L at
`u = x_R + shift / z` with two depth-tested taps: interpolating the jittered samples keeps the
jittered sample position, so eye R's pixel x shows the scene at `x + 0.5 - j_x`, the position DLSS
is told (`stereo-warp-test`: with jitter 0.25, -0.4, 0.4375 px both eyes, eye R matches eye R
rendered with the jitter to 0.0000 footprints and is off by ~|j| from eye R rendered without [U]).
y is untouched (no disparity). What x loses is detail: the two taps are a linear filter of weight
`frac(u)`, which varies with depth, so eye R's input is slightly softer in x than eye L's (flat:
gradient sharpness of eye R 9% below eye L at 63 mm, equal at zero IPD). The depth twin is
jittered identically, and the motion pass removes the same jitter [U].

**Fallback.** If eye R's feature cannot be created (video memory) or fails to evaluate, eye R
goes back to FSR 1 (the old chain), eye L keeps DLSS, one line is logged (`VR DLSS: eye R's own
DLSS feature could not be created ...`), until DLSS is switched off and on. Unusable eye R inputs
(size, depth format) log once and use FSR 1 for that frame.

**Logs.** `VR DLSS: eye R feature created (...): render WxH -> output WxH, ..., device memory +N
MB`, the once line `VR DLSS: stereo pairs: eye R has its own feature ...`, the 10 s line plus
`VR DLSS: 10 s: stereo pairs: eye R own feature N frames (history resets N), eye R on FSR 1 N`,
`VR stereo: eye R chain: dlss (own feature) ...`. With `BB_GPU_PROFILE=1`: segments
`VR DLSS: eye L (motion, mask, evaluate)` and `VR DLSS: eye R (...)`.

**Evidence** (`out/stereo_ab/run5/dlss1`, `bb-probe-stereoAB4.exe`, flat, fake head yaw 0,
`BB_STEREO_EYE_R_FLAT=1 BB_XR_UPSCALE=dlss BB_GPU_PROFILE=1`, 1920x1080 -> 3840x2160 performance,
arm mask 0.9 m) [M]:
- both features created on the same frame, 600-612 evaluations per eye per 10 s, eye R never on
  FSR 1, 100% of frames warped, no history resets after the first;
- zero IPD (`same=1`, three captures): the warp input is byte-identical to the display buffer;
  final images: channel means equal to 0.00, `|R - L|` mean 0.145-0.148, 0.4% of pixels above 2
  (spread over the frame: the two features' histories, not the inputs, which are identical; eye
  L against itself 6 s later differs by 3.66). Not bit-identical: two NGX features fed the same
  frames do not converge bit for bit;
- 63 mm (two captures): R - L channel means -0.56 / -0.12 / +0.13 and -0.63 / -0.21 / +0.03 (the
  FSR 1 eye R gave -0.21 / +0.37 / +0.58, 9.7); disparity, not a colour change;
- cost: eye R's DLSS (motion + mask + evaluate) 3.26-3.68 ms/frame, eye L's 2.83-3.29 ms (RTX
  4090, flat, 4K output); each feature +469 MB of device memory (VK_EXT_memory_budget around
  `CreateFeature`). So true stereo with DLSS on both eyes costs about +3.5 ms GPU and +0.47 GB
  over DLSS on eye L only; a lower output size (`BB_XR_DLSS_QUALITY=quality`, 2880x1620) is the
  lever if the headset frame budget is short.
- Unit tests: `xr-dlss-test` (`EyeCamera`: offset along the camera's x axis, inverse consistent,
  the remap's `clip.x - P00 * m`; eye R motion vectors against projected ground truth, the game
  camera's error, jitter removed; `PairEyeR` reset rules), `stereo-warp-test` (jitter kept).

**Not checked yet:** the headset (sharpness and stability match between the eyes in motion, any
eye-R-only shimmer from the warp's x filter or disocclusion fill, ghosting on the fill), the GPU
cost inside the VR frame budget.

### 9.9 Eye R streaks into the sky, drag beside near objects [M]

Headset report (stereo, hybrid; recordings of eye R): looking at the skyline, building and gate
edges smear tens of pixels sideways into the sky in horizontal streaks; the held weapon and
near edges "drag" when moving.

- **Sky (bug, fixed)**: in every flat warp dump (`out/stereo_ab/run3/warp/dump/warp1-4`) eye L's
  depth never reaches 1.0 (max 0.999974, about 1200 m: the sky is drawn into the guest depth
  after the doubled G-buffer pass), while eye R's twin keeps the cleared 1.0 (3000 m) on the
  same 6.5% of pixels. The 2% z rule failed on 100% of sky pixels, so the whole sky went to the
  disocclusion fill, which took the nearest direct pixel within 96 px, the building beside it:
  the streaks. Now two depths are also the same surface when their disparities differ by at
  most 0.5 px (`TolPx`, `|shift| * |1/z_L - 1/z_R|`): sky failures 100% -> 2.1% (warp2/3; the
  rest are real edges), all failures 14.8% -> 8.3%. Near surfaces keep the 2% rule (at 0.4 m
  0.5 px is far stricter). `stereo-warp-test` checks it (sky at 0.99997 in eye L only: direct,
  never filled; the 2% rule alone filled all of it). In the game (`out/stereo_drag/warp`, GPU
  dumps, flat): eye R vs eye L on sky pixels (about 0 disparity) mean |R - L| 40.3 -> 3.4,
  pixels off by more than 24: 57% -> 3%; the oracle residual / signal 0.32 -> 0.29 (PASS).
- **Stale eye R (defensive fix)**: the presenter takes `EyeRForDisplay(buffer)`, the newest eye R
  of that display buffer. A frame that wrote the buffer but got no warp (region end without a
  twin, a skipped flip) was paired with the eye R of the buffer's previous frame, two or three
  frames old: eye R lagging eye L. The display pass now invalidates the buffer's old eye R; a
  frame without a warp shows eye L in both eyes. (The headset log had 100% warps in play,
  0 skipped, so this was not the visible artifact.)
- **Near objects (inherent)**: shift = P00 0.6347 x 0.0626 m x 1920 / 2 = 38.1 px m. A weapon at
  0.4 m has 95 px of disparity, the background at 2-3 m 13-19 px: a band of about 80 px
  (4% of the width) beside it that eye L never saw, filled with the mirrored background and
  re-invented every frame as the hand moves (the smear beside the weapon). Alpha-tested
  foliage fails the depth test pixel by pixel (its per-eye coverage differs) and is filled
  from neighbours: the speckled plants. Removing this needs eye R to render what it sees:
  the near field for real (P4.4 slice 3 limited to the arms and the held weapon) or the full
  per-eye lighting path.

### 9.10 G-buffer-guided hole fill [U, M]

Headset report on d524f86: the sky streaks are gone; streaks beside the held weapon and near
objects remain (9.9, near objects: the band eye L never saw, filled from neighbouring pixels).
Eye R's own G-buffer has the right background in those pixels; only its lighting is missing.

**Which twin is what** (`warpdump` now also dumps eye R's G-buffer color twins by MRT slot,
`warp<n>_f<frame>_slot<i>_R_<format>`, and the slots are logged once: `Stereo: G-buffer color
slots ...`; `out/stereo_ab/run7/warp/dump/warp2`, the world's G-buffer pass, 6 targets):

| slot | format | content |
|---|---|---|
| 0 | R8G8B8A8Unorm | world-space normal x 0.5 + 0.5 (floor green, walls blue/red by facing) |
| 1 | R8G8B8A8Unorm | material ids / masks (R and B channels, flat per object) |
| 2 | R8G8B8A8Srgb | **albedo** (base colour: unlit textures, R, G, B in memory order) |
| 3 | R8G8B8A8Unorm | specular / roughness (grey) |
| 4 | B10G11R11Ufloat | (lighting accumulation, not used) |
| 5 | R16G16B16A16Sfloat | (not used) |

The albedo is the only R8G8B8A8Srgb target (as in the P4 dumps, `out/vr_stereo_p4`,
`gbuffer_albedo_*`); the warp finds it by format. The normals are slot 0 of the world's layout;
a menu frame logged another layout (slot 0 RGBA16F), so the normal is used only when slot 0 is
R8G8B8A8Unorm (else the fit runs without normals). The twins of a frame are written only by the
doubled G-buffer pass, so at the flip they still hold the frame exactly when the depth twin does
(the `current` check; each twin's `frame` is checked too): no copy needed.

**The fill.** A hole pixel p (both direct taps fail, its fill found the background `k0` px away
on side `s`) at eye-R view z `z_p`, albedo `A_p`, normal `n_p`. Its neighbours: 5 rows
(`y - 8 .. y + 8`, step 4) x 16 columns (`x + s (k0 + 0, 3, ..., 45)`), i.e. past the hole on
the background side; a neighbour counts when eye R's depth there is within 8% of `z_p` (weight
`1 / (1 + (rel / 0.02)^2)`), its normal within about 45 degrees of `n_p` (`n . n_p >= 0.7`) and
eye L sees it (DirectSample's two taps; its colour `C_q` is eye L's two-tap sample). In linear
light (the display value through the display pass curve `x^(1/0.85)` and the sRGB decode, 9.7;
re-encoded after):

```
r_c      = sum(w A_q,c C_q,c) / sum(w A_q,c^2)                 per channel: lighting colour, level
L_Y(n)   = l0 + l . n   fitted by weighted least squares on  Y(C_q) = Y(A_q) L_Y(n_q)
           (Y = channel mean; ridge 0.05 sum(w Y(A)^2) on l)    luminance lighting's shape
s(n_p)   = L_Y(n_p) / (sum(w Y(A) Y(C)) / sum(w Y(A)^2))        1 for alike normals, clamp 0..4
colour_c = A_p,c * r_c * s(n_p)
```

So `colour_R(p) = albedo_R(p) * L(p)` with `L` the background's lighting ratio, refined by the
normal (the shading a normal map adds; the plain ratio paints a bumped wall flat). Fallback to
the mirrored fill: no albedo twin (`guided=0` in the control file, `BB_STEREO_GUIDED=0`), sky
(z >= 1000 m), less than about 2 neighbours of weight. Zero eye distance never fills, so the
identity stays byte-exact (checked).

**GPU shape.** The warp dispatch (8x8 tiles) appends each hole pixel (x, y, side, k, z) to a
buffer (one atomic per subgroup), then a second dispatch runs `GuidedFill` one thread per listed
pixel: inline in the warp, the 80-120-neighbour loop ran in every tile that had a hole pixel and
cost 0.86-1.12 ms; listed, full warps of hole pixels. The list holds a quarter of the pixels
(8.3 MB at 1080p; more holes keep the mirrored fill). `vk_stereo_warp.h` (`GuidedNeighbour`,
`GuidedSolve`, `GuidedShade`, `GuidedFill`, `WarpColor`) is the CPU mirror: on the real dumps
it reproduces the GPU's eye R to 1/255 (mean 0.02, max 1).

**Evidence** (`out/stereo_ab/run7`, `bb-probe-stereoAB6.exe`, flat, `vr_true_stereo_test.py
warp`, which now also dumps two frames with `guided=0` and scores the hole pixels alone against
the +ipd/2 oracle camera, the image that really renders the background behind near objects) [M]:

| hole pixels (static, sky excluded, ~63,600 px, 7.1% of the frame) | mean | median | p90 |
|---|---|---|---|
| mirrored fill (before), 2 frames | 19.35 / 19.28 | 12.33 | 47.7 |
| guided fill (after), 2 frames | 15.52 / 15.54 | 10.33 | 35.3 |

Hole error -20% (0.80 x), p90 -26%; the whole static image's residual / signal 0.29 (PASS),
`same=1` byte-identical, 100% of frames warped. On the pixels the fill actually guides (71% of
the filled ones) the error drops 28% (18.2 -> 13.1, CPU mirror on warp2); the other 29% (no
neighbour of the same depth and facing: alpha-tested foliage, thin ironwork, the frame's edge)
keep the mirrored fill. How far the model can go: even with the oracle's own lighting averaged
over a 17 px box the ratio model leaves 13.0 on these pixels, the normal-shaped one 9.0 (what
the albedo cannot explain: specular, AO, fog, the HUD in flat runs). Variants measured on the
same dumps: plain ratio 0.84-0.96 x, the shape fitted per channel 0.81 x (more registers, 2x the
cost), anchored on the hole's background pixel instead of each hole pixel 0.94 x (too many hole
pixels belong to another surface than the anchor).

**Cost** (`warpcost`, BB_GPU_PROFILE, flat 1080p, RTX 4090): warp 0.12-0.14 ms (unchanged) +
guided fill 0.25-0.28 ms; total 0.39 vs 0.12 ms with `guided=0` (120 neighbours: 0.42 ms for the
fill pass). The fill scales with the number of hole pixels (more with the weapon close).

**Unit tests** (`stereo-warp-test`) [U]: the synthetic post-before-wall scene, shaded (wall
albedo a texture per channel, lighting with a gradient, display = B, G, R through the display
curve): every wall pixel the post uncovers is guided; mean error 0.013 (display units 0..1) vs
0.121 for the mirrored fill, distinct values along the row (no streak); uniform lighting: 0.0005
(exact to the two-tap blend); a normal-mapped wall lit from the side: 0.041 with the shape in
the normal vs 0.066 for the plain ratio and 0.153 mirrored; zero eye distance with the fill on:
eye L's own value bit for bit; the display encoding round-trips.

**For the headset:** the band beside the weapon should now show the real background texture with
roughly the right brightness instead of smeared stripes; what is left there is lighting detail
the albedo does not carry (specular, AO) and pixels whose background has no neighbour of the same
surface (they keep the old fill).

## 10. Full stereo: eye R rendered to the display pass

Date: 2026-10-12. Branch `stereo-full` (from `stereo-ab` plus the P4.4 slice 3 branch, merged).
Status: **built, unit-tested, measured flat [M][U]; not yet on a headset.** On by default in stereo
mode; `BB_STEREO_FULL=0` (or `full=0` in the control file) gives the hybrid warp of section 9.

The hybrid (section 9) rendered eye R's G-buffer but warped eye L's finished image: disocclusion
bands beside near objects (the weapon, the arms) and eye L's view-dependent lighting in both eyes.
Full stereo renders everything of the 3D region for eye R as well: lighting, AO, the shadow mask,
the light volumes, forward objects and the sky, particles, light shafts, distortion, post and the
bloom pyramid, then the game's display pass. The UI is drawn once (the HUD layer carries it).

### 10.1 What a frame does

Files: `vk_stereo_region.h` (new: the region's state machine and the twin rules, header-only),
`vk_stereo.cpp`, `vk_rasterizer.{h,cpp}` (three hooks: compute image copies, compute image clears,
indirect draws).

```
Idle    --first G-buffer draw (full allowed)--> Scene    doubled: every render pass, dispatches per eye
Scene   --first Scaleform draw--------------->  Ui       eye L only (the UI; with the HUD layer its own image)
Scene/Ui --the display pass------------------>  Display  doubled: the display pass again for eye R, into a
                                                         twin of the display buffer = eye R's final image
Display --the next non-display draw, or the flip--> finish: eye R's display pass recorded, handed to the flip
```

- **Full allowed:** a hybrid frame (stereo on, `offset` 0), replay and per-eye dispatches on, the eye
  slot buffer, and the scene rasterized at the guest size (no reduced proxies; VR runs the upscaler
  "off", which only hosts the HUD layer). Otherwise the hybrid path of section 9 runs unchanged.
- **Every render pass** of the region is the P4.3 capture (record once, execute twice) with eye R's
  attachments the twins and eye R's sampled and storage views substituted (slice 3). 122-127 render
  passes per frame in the measured scene, about 9,100 commands, 0 cut short.
- **At the flip** (`StereoWarpAtFlip`) eye R's display-buffer twin is copied into the eye-R ring image
  (bitwise, R8G8B8A8 sRGB into UNORM, as the warp copies the display buffer's bytes). Everything
  downstream is unchanged: `EyeRForDisplay`, the presenter's `RecordEyeRChain`, and eye R's own DLSS
  feature (9.8) now gets eye R's real final colour with the same depth twin, camera and motion vectors.
- **Fallback per flip:** no rendered eye R for the flipped frame (the display pass got no eye R, an
  upscaled display, size or format) -> the warp runs as before, counted ("warp fallback").

### 10.2 Design details

- **Twins rest in GENERAL.** Eye R renders into, samples and stores to a twin in one layout, so a pass
  that depth-tests against the depth it also samples (light volumes, decals) reads eye R's depth twin;
  the guest does this in read-only or feedback layouts. Before each eye-R execution one global memory
  barrier (the guest images' barriers recorded for eye L do not cover the twins).
- **Keys.** Twins are keyed by the guest image's address, view format and size: render targets that
  alias one address at different sizes get separate twins, and a twin is never destroyed while
  recorded commands use it (a replaced one is retired; 0 in the runs). 97 twins, about 300 MB, plus
  3 twin buffers (33 MB).
- **Twin coherence.** Twins are tagged with the region's frame (`twin_frame`, which stays the region's
  through its display pass although the frame counter advances there). First write in a region: the
  twin starts as a copy of the guest image, skipped when the pass clears the target (the same
  meta-clear test `BeginRendering` uses for its load op). **Read before written** (temporal history):
  last region's eye-R twin is used if nothing wrote the guest since (1 such read per frame here). An
  eye-L-only write marks the twins stale and eye R then reads the guest's: dispatches that read no
  twin (eye-independent output), compute clears, copies not redone, passes the write guard skips,
  draws into a mip/layer/MSAA target (0 here).
- **Copies.** The compute image copies (section 3, copies 1 and 2, RGBA8 1080p) and the copy shader's
  depth to R32F copy (copy 3, through memory) are redone between the twins (depth aspect into the R32F
  twin through a scratch buffer). Copies 4 and 5 keep slice 2's twin buffers.
- **Dispatches** follow slice 3's write policy; in a full region every plain 2D color storage image
  from 64x64 gets a twin (half-resolution AO, blur and bloom targets), not only the region's size.
- **Indirect draws** now pass the stereo hooks like direct ones (resolve, capture, then record the
  binds): they are doubled in the region (1 per frame here) and no longer bind the eye slot with the
  last eye's value outside a doubled pass (a latent bug of the hybrid path too).
- **Per-eye constants:** none patched. As section 2.5's oracle showed, the vertex shift alone
  (`clip.x - P00 m`) renders the moved camera: every reconstruction from depth with the center
  camera's matrices lands m from the true point (31.5 mm), below the noise (10.4).

### 10.3 Controls and logs

| | |
|---|---|
| `BB_STEREO_FULL=0` / `full=0` | full stereo off: the hybrid warp (section 9) |
| `BB_STEREO_HYBRID=0` / `hybrid=0` | neither (only the stereo shaders; eye R = eye L's image) |
| `audit=<n>` (control file, a new value) | lists the next full region: every render pass (shaders, targets), dispatch decision, copy and leak |
| `BB_STEREO_WRITE_AUDIT=1` | per shader the dispatch decisions, the first leaks and not-doubled draws |
| `BB_STEREO_DUMP_SLOTS=1` | `warpdump` also dumps the G-buffer slots (off now: 65 MB per dump) |

The 5 s line gains `full stereo on: N regions, eye R rendered at N flips, warp fallback at N`, followed
by the last region's counters (`Stereo: full region of frame ...`: not-doubled draws, image copies
redone, copies left to eye L, history reads, leaks, format aliases, indirect draws, shared reads) and
the doubled-pass line of section 5. GPU profile labels: `stereo: eye R of doubled passes`, `... of
doubled dispatches`, `stereo: eye R twin init copies`, `stereo: eye R image copies`, `stereo: eye R
full stereo (display twin to the eye R image)`.

### 10.4 Evidence [M]

`python tools/vr_true_stereo_test.py full --probe out/gpu/bb-probe.exe` (flat, fake head, in the
world near the Cathedral Ward gate, `BB_UPSCALER=off BB_XR_HUD=force`: the display buffers carry no
HUD, as in the headset; `out/stereo_ab/run8/full1`):

| Check | Result |
|---|---|
| flips with eye R rendered | 100% of full regions in every 5 s window (257/257, 260/260, ...), 0 warp fallbacks |
| `same=1` (eye R at eye L): eye R's final image vs eye L's | **byte-identical** (the hybrid's too) |
| region counters | 1150 draws, 53 dispatches, 123-126 render passes executed twice, 0 cut short, 0 draws outside a capture, 0 write-guard skips, 1836 eye-R views substituted, 3 image copies redone, 0 copies left to eye L, 1 history read, 0 leaks (second run, refined count), 1 indirect draw doubled |
| disparity sign (normal, inverted ipd) | same surface at x + d 96.4% / 95.9% vs x - d 74.0% / 74.4%; `|R - L|` at x + d 4.0 vs 21.0 |

Eye R against the oracle (the whole region rendered from the camera moved +ipd/2, `offset=+ipd/2`),
static pixels (55.5%), the hybrid warp of the same session and settings next to it:

| static pixels | mean | p99 | > 8 |
|---|---|---|---|
| noise (+ipd/2 camera twice) | 0.13 | 1.7 | 0.00% |
| signal (eye L vs +ipd/2) | 16.59 | 75.3 | 59.0% |
| **full: eye R vs +ipd/2** (two dumps) | **0.12, 0.23** | 1.7, 2.3 | 0.01% |
| hybrid: eye R vs +ipd/2 (two dumps) | 2.60, 2.61 | 27.0 | 6.5% |

Residual / signal: **full 0.014, hybrid 0.157** (section 9.10's 0.29 was with the HUD in both
images). Full stereo is at the noise floor: what is left (`png/full0_vs_oracle_x8.png`) are the
animated plants, the character, particles and flickering lights, which differ between any two
captures; the static architecture is black. On the pixels the hybrid had to invent (both direct
taps fail, about 10,400 static non-sky pixels):

| hybrid's hole pixels | mean | median | p90 |
|---|---|---|---|
| full stereo (two dumps) | **0.19, 0.42** | 0.00 | 0.7, 1.3 |
| hybrid, guided fill (two dumps) | 13.44, 13.38 | 8.0, 7.7 | 23.7, 23.3 |

A second session (`run8/full2`) started while a headset game was launched and its frames were white
(a clash of two game processes); its image metrics are void, its counters agree (identity
byte-identical, 100% rendered, 0 leaks).

### 10.5 Write audit

From the listed region (`audit=1`, `run8/full1/audit.txt`) and the per-shader decisions:

| Writer | Eye R | Notes |
|---|---|---|
| 123 render passes (G-buffer 15, light accumulation and composite, SSS, forward/sky, particles, light shafts, distortion, post, bloom/glare pyramid down to 5x5, tonemap, AA, 960x540 blurs, the display pass) | both eyes, into twins | write guard: 0 passes with shader memory writes, 0 attachments not a twin |
| ~8 attachment-less passes (vs `89cda9e4`) | both eyes | no attachments, no memory writes (guarded): no effect |
| cs `9a9cf8a9` (post combine) | both eyes (twin storage target) | once when it reads no twin (eye-independent) |
| cs `e2c27f0c` | once (reads no twin) | writes a region-size storage image: eye-independent; its twins stale, eye R reads the guest's |
| cs `3d5ebf4e` copy shader | once + eye-R copies | copies 1, 2 as image copies redone between twins; copy 3 depth -> R32F redone; 4, 5 twin buffers |
| cs `1f2b3c9e` (luminance/exposure), `306ce628`, `200820a1`, `10bc83cb`, `baad65ce` | once (read no twin) | small shared images (2048x2, 1025x1, 16x16): one exposure for both eyes |
| cs `a509af23`, `8b355b5a` | once | buffers and GDS (particle emit, counters), read no twin |
| cs `42f2a521` | **eye L only** (reads twins, writes a buffer and GDS) | the GPU particle simulation against eye L's depth: one simulation, both eyes draw it (by design) |
| the UI (Scaleform) | eye L only | the HUD layer; flat without the layer eye R has no HUD |

**Risks to eye L:** eye R writes only twins, twin buffers and the eye-R ring image: render passes are
checked per pass (no eye-R attachment is a guest view), dispatches per dispatch (every written
storage image and buffer substituted, else eye L only), copies go twin to twin. The eye slot is a
host buffer rewritten between the eyes. Found and fixed on the way: indirect draws read the eye slot
outside doubled passes (they could have drawn eye L with eye R's offset). **Leaks** (eye R reading an
image eye L wrote, without a twin): 0 per region after counting eye-independent writers correctly.

### 10.6 Cost

Flat, 1080p render, RTX 4090, same session (`run8/full1/session.log`, frame stats): hybrid 60-62 FPS,
full stereo 51-52 FPS, the oracle phase (nothing doubled) 64 FPS. The flat game is CPU-bound at about
62 FPS (the PM4 thread); with full stereo the GPU becomes the limit (`blocked: GPU ticks 8.3%`).
So full stereo costs about **+3.3 ms per frame** over the hybrid at render size (eye R's region:
the region is about as heavy as eye L's minus shadows, the 64x64 passes and the UI). Eye R's DLSS
is unchanged (9.8: 3.3-3.7 ms at 4K output, less at 2880x1620).

**Estimate for the headset** (2880x1620 DLSS output per eye, render 1920x1080, hybrid measured at
59-62 FPS): about 50-52 FPS if the extra 3.3 ms adds in full, i.e. the same drop as flat. Not yet
profiled with labels (`vr_true_stereo_test.py fullcost` is ready: mono / hybrid / full, A B C x2,
with the `stereo: eye R ...` labels).

### 10.7 Limits and what remains

- **Not run on the headset.** Look for: eye R as sharp and stable as eye L, no streaks beside the
  weapon, correct specular per eye; the presenter line `eye R chain: dlss (own feature)`; the 5 s
  line's warp fallback at 0.
- `fullcost` (labelled GPU cost) and the regression sessions `replay` and `warp` with this build were
  stopped by a clash with a headset run and are still to do.
- Shared by design: GPU particle simulation (eye L's depth for collisions), exposure, the LUT, the UI.
- Not covered: the game upscaler's scaled presets and reduced raster (full stereo stands down; the
  warp runs), MSAA or mip/layer render targets inside the region (eye L only, counted; none seen).
- Twin memory about 330 MB of device memory on top of eye R's DLSS feature (0.47 GB).

### 10.8 The white screen in stereo: a disk shader cache pairing (fixed) [M]

Headset reports (run8 full stereo, and run7 / dlss-nr hybrid builds): both eyes white "at full
brightness" in the world, nondeterministic (some launches at once, some after minutes, others never),
never in alt or mono, persisting after switching the upscaler. Reproduced flat
(`out/stereo_ab/run8`, sessions of the `repro` wrapper with the new probe line, BB_UPSCALER=off,
BB_XR_HUD=force):

| Flat sessions, one shared (accumulated) shader cache | white |
|---|---|
| stereo, full (default) | 5 of 8, 6 of 7, ~40% over all batches |
| stereo, hybrid only (`BB_STEREO_FULL=0`) | seen |
| stereo shaders only, nothing doubled (`BB_STEREO_HYBRID=0`) | 1 of 6 |
| renderer stereo with the XR side mono (`BB_XR_MODE=mono BB_STEREO=1`) | 2 of 8 |
| mono | 0 of 8 |
| stereo, full, the shader cache wiped before each session | **0 of 6** |

So it is not the doubling and not the XR/head side: it needs the stereo shader permutations and an
accumulated disk shader cache. **Where it goes white** (probe chain, `BB_STEREO_PROBE_CHAIN=1`,
`BB_STEREO_PROBE_PS=<hash>`): the G-buffer, lighting, the HDR scene and every post input are normal;
the first bloom prefilter draw (ps `4b766f62`, 960x540) writes NaN everywhere from a clean HDR
input, the bright pass (ps `4eb5a42f`) turns it into ~64000, the bloom pyramid saturates and the
composite is a uniform 245/252/255 (`ldr_target` already white, the display pass copies it). The
draw reads its resources through the wrong bindings.

**Cause.** `PipelineCache::CompileModule` stored every permutation's SPIR-V in the disk cache under
`<hash>_<permutation index>`, including stereo permutations with their own Info (P4.1/2.4: the
StereoView buffer changes the resource layout), whose meta is deliberately not stored. Permutation
indices are positional per session. A stereo permutation compiled at index i overwrote the binary of
the permutation a previous session had stored (with its meta) at index i; a later session loaded
that meta with the stereo binary: a module whose bindings do not match the Info it is bound with.
Whether a session hits it depends on which permutations were preloaded and which were compiled in
which order, so it is nondeterministic and changes as the cache evolves; once a draw has the
mismatched module it keeps it for the session (and the NaN feeds the frame's bloom every frame).
Mono and alt create no own-Info permutations.

**Fix** (`vk_pipeline_cache.cpp`, `vk_pipeline_serialization.cpp`): a new permutation's binary is
stored only together with its meta (never for an own-Info permutation); binaries are keyed with the
version (`<hash>_<index>_b8`) and the shader binary and meta versions go 7 -> 8, which drops every
entry a cache already paired wrongly and keeps builds of other versions sharing the store (older
stereo builds) from overwriting this build's binaries. One-time cost: the shader cache is rebuilt
on the first launch.

**Verified** on the polluted cache of the failing sessions (restored), 16 consecutive sessions with
the fix, the cache accumulating (full, hybrid and mono mixed): **0 white**; the `full` session after
it PASS (same=1 byte-identical; eye R vs the +ipd/2 oracle 0.31 / 0.14 vs noise 0.34, residual /
signal 0.020, hybrid 0.161). VRAM was never short (2.3 GB used of a 19.3 GB budget, flat).

**Also in this build:**
- Indirect draws of a doubled pass now pass the write guard (`StereoAfterIndirectDraw`); before, an
  indirect draw whose shaders write memory would have run twice.
- A probe line every 10 s in stereo mode (`BB_STEREO_PROBE_SECONDS`, 0 = off): `Stereo: probe frame
  N: eye L display r/g/b (white %); eye R ...; exposure ...; display input ...; VRAM usage X MB of
  budget Y MB; flip id ... display pass target id ...`. White 100% in both eyes with a white display
  input points at the game's frame; eye R alone at the stereo path.
- Diagnostics (off by default): `BB_STEREO_PROBE_CHAIN=1` (every image of the last full region in
  pass order: mean, NaN/Inf), `BB_STEREO_PROBE_PS=<hash>` (the images and constants of a pixel
  shader's first draw per frame), `BB_STEREO_TWIN_BUFFERS=0` (slice 2's twin buffers off).

### 10.9 The fps counter and the "Pace:" line (what limits the frame rate) [U]

The headset shows 72 Hz and the game delivers about 62 fps in stereo, with the GPU 74 % busy and
the CPU at 25 %. Two tools tell where the time goes. Neither starts the game by itself; both were
only built and unit tested (tests/test_xr_fps.cpp, `ninja -C out/gpu xr-fps-test`), the headset
check is open.

- **Counter in the headset.** `BB_XR_FPS=1`, or `fps=1` in the `BB_XR_IQ_CONTROL` file (live,
  `fps=0` hides it again; the corner is `BB_XR_FPS_POS=tl|tr|bl|br` / `fps_pos=`, default `tr`).
  A small dark panel in the HUD layer with two lines of bitmap text: the game frames delivered per
  second over the headset Hz (`62/72`), and the frame time with the limit (`16.1MS GPU`). Green at
  >= Hz - 1, yellow at >= 80 % of Hz, red below (grey while the Hz is unknown). The rate is
  sampled every 0.5 s and smoothed (new weight 0.5; a gap of 2 s starts the average over), the
  verdict every 2 s. The Hz is the runtime's predicted display period from the bridge
  (`GetLatency`, ABI 10), so no bridge change. It is drawn into the HUD image after the UI copy
  (`vkCmdClearAttachments` rectangles, no pipeline), so it is never in an eye image and shows in
  alt, mono and stereo; it needs the HUD layer (on by default in those modes, `BB_XR_HUD=off`
  hides it) and appears whenever the HUD does (not on full-screen menus on the cinema screen).
- **Bottleneck line**, every 10 s in the log (`BB_PACE_LOG=0` mutes it), flat or headset:
  `Pace: 10 s: N frames, X fps (72 Hz): F ms/frame = guest wait a + command thread work b + GPU
  wait c + free-frame wait d; GPU n/a (BB_GPU_PROFILE=1); flip slot wait e, presenter p
  (xr/swapchain x) => <verdict>`. The terms are per delivered game frame, taken from running
  counters (`BbStats::pace_*`, always on, a few relaxed atomics per frame):
  - *guest wait*: the GPU command thread idle, waiting for the guest's commands (the emulation
    thread is the slow one);
  - *command thread work*: the rest of the frame on that thread (translation, recording);
  - *GPU wait*: the `BB_FRAMES_AHEAD` wait for the GPU to finish the frame before (the GPU is
    behind);
  - *free-frame wait*: `GetRenderFrame` waiting for a presentation frame the presenter has not
    freed (limiter `BB_FPS_LIMIT`, headset, swapchain);
  - *flip slot wait*: a finished flip waiting for its slot (frame limiter / vblank);
  - *presenter / xr/swapchain*: the swap thread's time per frame, and what of it is swapchain
    acquire and present, queue submit and the bridge's Acquire/Release calls.
  `GPU ms` is the existing profiler's per-frame sum and only exists with `BB_GPU_PROFILE=1` (its
  timestamps split render passes, so it is not on by default); the GPU wait is the cheap signal.
- **Verdict** (`xr_fps_logic.h`, `Analyze`): at the display rate (>= Hz - 1) *pacing-bound*; GPU
  wait >= 20 % of the frame and more than the free-frame wait, or measured GPU time >= 90 % of the
  frame: *GPU-bound*; free-frame wait >= 20 %, or the presenter busy >= 90 %: *pacing-bound*;
  otherwise *CPU-bound* (guest or command thread). How to read it at 62 of 72: GPU-bound means
  the frame needs more GPU time than 13.9 ms even though the average utilisation looks low
  (dependent passes, eye R after eye L); CPU-bound with a large guest wait means the emulation
  thread; pacing-bound with a large free-frame or flip slot wait means the limiter or the
  presenter, not the game.

### 10.10 The command thread's CPU cost: profile and fixes (branch `stereo-perf`) [M][U]

Headset (run10, Pimax 72 Hz): 62-66 fps, `Pace:` CPU-bound, command thread work 12-15 ms/frame.
Flat sessions at the headset's settings (`BB_XR_MODE=stereo`, full stereo, `BB_UPSCALER=off`,
`BB_XR_HUD=force`, uncapped, Cathedral Ward gate, `out/stereo_ab/run13/sessions/`), measured
with a new always-on `Cmd:` line after `Pace:` (TSC counters per thread: thread A = the PM4
thread, B = the draw recording thread, the Vulkan recorder) and an external stack sampler
(dbghelp stack walks of the three threads, ~300 Hz, symbolized from the exe's DWARF).

**Where the time went (before, base1/base2, full stereo, ms per frame).** The Pace "command
thread work" (12.5-14.0) is thread A, but A is mostly *waiting for B* (draw pipe 9-10); B is
the limit (busy 11.7-13.1, 92%). The recorder was busy 6.7 (eye L/R arenas 0.9/0.6).

| Stage B, sampled | share | |
|---|---|---|
| `Stereo::OnSceneConstants` | 29.5% | read the 864-byte scene constants back from the constant ring, which is write-combined device memory (stereo mode only, every frame) |
| `WaitHostCopies` (yield) | 9% (33% once the above was fixed) | WriteData/EOS wait for the recorder's fence writes, which blocked in `runtime_memory_write_backing`'s rwlock (winpthreads, kernel wait: 28% of the recorder) |
| page protection (`Protect`) | 9% | base path (GPU page watchers), not changed |
| `StereoCollectSubstitutes` | 7% (1.4 ms by TSC) | per bound image a scan of all ~100 twins |
| other stereo hooks | ~0.7 ms | begin draw, emit |
| probe readback, fps counter, logs | ~0 | the probe runs every 10 s (`probe 0.00` ms/frame) |

**Changes.**
1. Stage A parses an 864-byte block's scene constants while copying it into the ring (the bytes
   are in its cache) and passes them in the ring binding; stage B applies them in draw order.
   Same bytes, same order: no change in what eye R sees.
2. Twins indexed by source image (rebuilt when a twin is added, retired or changes source, in
   the map's iteration order, so the same twin wins as before); no per-draw heap allocations in
   `StereoBeginDraw`.
3. `runtime_memory_write_backing` (guest writes through the backing view: the GPU's fence labels)
   takes the thread's region cache for writes up to 64 bytes, as the lookups do, instead of the
   region table lock. The cache holds for one table generation (odd while a writer holds the
   lock); it now also keeps the region's backing offset. This is the base path: mono and alt
   gain too.

**Numbers (flat, same scene; FPS from the 10 s Pace windows).**

| build | full stereo fps | Pace work ms | stage B busy | stereo hooks on B | stereo shaders only (`hybrid=0`) fps |
|---|---|---|---|---|---|
| before (perfBase, two sessions) | 70-79 | 12.5-14.0 | 11.7-13.1 | 2.05-2.20 | 81-89 |
| 1+2 (perfExp2) | 77-84 | 11.2-12.6 | 10.3-11.1 | 0.92-0.97 | 83-84 |
| 1+2+3 (perfExp3) | 100-115 | 6.8-9.1 | 6.6-8.3 | 0.95-1.00 | 117 |
| final, merged with `vr` (stereoPerf1) | 90-114 (GPU-bound from the 2nd window: GPU wait 3.8 ms) | 6.9-7.3 | 7.2-10.0 | 0.92-0.98 | hybrid 115 |

The command thread's work per frame halved (13.5 -> 7 ms); flat full stereo is now GPU-bound.
In the headset the 13.9 ms budget then has ~7 ms of CPU margin; the next limit is the GPU
(both eyes' DLSS at 2880x1620) and the guest thread.

**Correctness** (`vr_true_stereo_test.py full`, final build, `run13/sessions/full_check`): PASS;
`same=1` and `samehybrid=1` byte-identical; eye R vs the +ipd/2 oracle mean 0.09 / 0.47 vs noise
0.42 (residual / signal 0.032, hybrid 0.133); 0 leaks, 0 cut short, 100% of flips rendered.
Unit tests: stereo-substitute, stereo-warp, xr-fps, xr-dlss, xr-dlssnr, vr-stereo, vr-mem-cache,
vr-hook-prof pass.

**What remains.** Stage B still waits for the recorder at WriteData/EOS (host copies in order
with the recorded commands); the GPU page-protection syscalls (~9% of B); the substitution scan of
`referenced()` per view (0.35 ms). Not tried: eye R recorded on another thread or as a secondary
command buffer (the recorder is now at ~3.5 ms, not the limit).
