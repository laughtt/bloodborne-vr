# 06 — Cross-check (wave 2): contradictions, verified claims, gaps, stereo strategy

Date: 2026-10-09. I read all of 01–05 in full. Code evidence comes from the read-only clones under `bbvr/repos/` (nothing was built or run). Paths without a prefix are relative to `repos/supermedo/bloodborne_pc` (commit e076119).

For disassembly I used capstone 5.0.7 through scripts in `bbvr/xcheck/`: `dis.py`, with output in `xcheck/debugcam.txt` and `xcheck/uncap.txt`. A copy of the current upstream `shadps4-emu/ps4_cheats` `PATCHES/Bloodborne.xml` is at `xcheck/upstream/Bloodborne.xml`.

The game eboot is **not** in any clone. Only the patch bytes could be disassembled, not the original code around them.

---

## 1. CONTRADICTIONS

| # | Conflict | Resolution (evidence) |
|---|---|---|
| C1 | 04 says there are no maintained RPCS3 VR forks; 01 and 02 say hookmanuk/rpcs3 exists. | **01 and 02 are right.** GitHub API: `hookmanuk/rpcs3` is a fork of RPCS3/rpcs3, GPL-2.0, pushed 2026-10-09T00:26Z, 10 stars. Releases v0.0.42-vr4 (2026-09-28, "Demon's Souls added (BLUS30443)", "Wider view (VR culling) 130°", motion blur off because it "followed the game camera") through **v0.0.43-vr8 (2026-10-03): "both eyes are drawn in one pass (multiview)"**. The notes do not say how head pose is injected. The source is public and current, so it is the best reference for "emulator-side multiview of a flat FromSoft game" (strategy D-mv below). This also refutes 04's argument that programmable-shader consoles have no VR forks. |
| C2 | 02: ERVR is "open source, GPL-3.0+" but the repo is unreachable. Is there a current location? | **No public source found.** `github.com/IlyaMez/EldenringVR` returns 404. GitHub API user `IlyaMez` lists only KHARVOX (DOOM 2016 VR, MIT fork), SkyroadsPrototype, SpidyVR and VISION. Repo searches `elden ring vr openxr` and `ERVR elden` return 0 results. Web searches turn up only the Nexus page (eldenring/mods/10711) and press. `repos/ilyamez-ervr` is an **empty directory** (the clone failed). Treat ERVR's "second engine pass with simulation time frozen" as an unverified author claim. Even if the source appears, its GPL-3 could not be pasted into a GPL-2-or-later tree unless relicensed (the "or later" lets the *combined* work go GPL-3, which is a project decision). |
| C3 | 01 describes "Increased camera distance" as a hook at `0x0183AE68` → cave `0x04D3BC30` plus floats at `0x0183A47D...`; 03 and 05 describe it as `0x01836940=30c990`, `0x0184107f mov [rbx+4],0x2712`. | **Both are real, but they are different versions of the patch.** 01 read the **upstream** ps4_cheats XML (`xcheck/upstream/Bloodborne.xml:1776-1792`). The cave disassembles as `vmovss xmm1,[rsi]; vmulss xmm1,[rip+0x24]` (×1.5) `; vmovss xmm2,[r13+0x188]; jmp 0x183ae75`, with floats 3.0 and 6.0 written into immediates at 0x0183A47D etc. 03 and 05 read **bbport's** `patches/Bloodborne.xml:1213-1220`, which forces LockCamParam row 10002 (0x2712) and clears `cl`. Both point at the same camera cluster, 0x01836000–0x01842000. For bbport, the in-repo version is what ships. |
| C4 | 01: "Restore Debug Camera" was removed from the XML after the DMCA and survives only in git history (8419e97f). 03 and 05 cite it at `Bloodborne.xml:2865`. | **Both true.** It is absent from the current upstream XML (grep finds nothing) but **present in bbport's own XML** (lines 2865–~3060, `isEnabled="false"`). There is no need to dig through git history. |
| C5 | 02 table: RPCS3 VR "multiview since vr8". 01: "stereo method unknown". | 02 is right per the vr8 release notes (C1). The head-pose mechanism is still unknown; read the source. |
| C6 | 03 and 05 say "GPU constant patch + replay" is a viable "one sim step, two eyes" path (05's checklist rates it "Low (novel)"); 03 says it is blocked. | **03's blocker holds for *constant-only* patching** (§2b). It does **not** apply to a clip-space remap in the vertex shader (strategy D), which is independent of where the game keeps its matrices. 05's "patch view 3x4 floats 8-19 per eye" alone would leave every object with camera-dependent model constants in the wrong place. |
| C7 | 02 and 05: the HUD/menus are "Scaleform-era" and BB "probably" has a PersCam layout like DS3/Sekiro. 03: confirmed from the debug-cam bytes. | Confirmed for the *target* camera object (§2a). The class names (ChrCam/PersCam) remain DS3/Sekiro analogies; BB is a PS4 ELF with no RTTI checked. |
| C8 | 03 says `RunScaled` "writes `output_image` and blits it into `ui_image`" as the general behaviour. | Only on the **TAA/FSR4/DLSS** branch (`vk_temporal_upscaler.cpp:1618-1688`). The FSR3 branch writes **directly into `ui_image`** (`info.output = Describe(vk::Image(ui_image)...)`, line 1710), so there is no HUD-free copy there. See §2c. |

## 2. LOAD-BEARING CLAIMS VERIFIED

### a. Camera object layout (matrix at +0x10, FOV block at +0x50): CONFIRMED for the target camera. Field names are inferred.

Disassembly of bbport's "Restore Debug Camera" (`patches/Bloodborne.xml:2865+`, full listing in `xcheck/debugcam.txt`).

Hook site at `0x01972998`. The original code picks one of two camera candidates by comparing a priority field `[rdx+0x20]`, so this is an **active-camera selection function** (a director-like seam):
```
1972998 je 19729b0 ; 197299a cmp [rdx+0x20],rcx ; mov rcx,rax ; jg ; mov rcx,rdx ; cmp rcx,rax ; je ; jmp 0x1de3329
```
Cave at `0x1de3329`, where `r12` is the debug-camera object:
```
1de3397 mov rcx,[r12+0xd0]                 ; target (render) camera
1de33ab vmovaps xmm0,[r12+0x50] -> [rcx+0x10] ; +0x60->+0x20, +0x70->+0x30, +0x80->+0x40
1de33e6 vmovss xmm0,[r12+0x4c] -> [rcx+0x50]
; else-branch (debug cam off): rdx=[r12+0x10] (the game's own camera)
1de33fc..141f  [rdx+0x50],[+0x54],[+0x58],[+0x5c] -> rcx same offsets (4 floats)
1de3424..3447  [rdx+0x10..0x40] (4 x xmmword) -> [rcx+0x10..0x40]
```
- **Verdict.** The target camera has a 16-byte-aligned 4×4 float block at +0x10..+0x4F (`vmovaps`, so it is aligned) and 4 floats at +0x50..+0x5C. This is byte-for-byte the shape of SKVR's measured `SprjPersCam` (rows +0x10/+0x20/+0x30, position +0x40, fov/aspect/near/far +0x50..+0x5C; `sekiro-vr/src/src/sk_camera.cpp` reads `m=(float*)(c+0x10)`, `pos=m[12..14]`, `fov=*(float*)(c+0x50)`).
- Calling +0x50 "FOV" is supported by the debug cam writing only that float (its zoom). Calling +0x54/+0x58/+0x5C aspect/near/far is **analogy only**. It is consistent with the GPU scene constants (near 0.05, far 3000, vertical 43°; `docs/upscaler.md`), but nothing here proves it.
- The debug object's *own* layout differs (matrix at +0x50, fov at +0x4c), so not every camera class shares the layout.
- **The `0x01972998` function is the strongest seam candidate**, because the cave runs exactly where the active camera is copied into the render camera. That is where SKVR hooks Sekiro ("after the director's copy").
- **Camera-distance and auto-rotation patches.** The bbport camera-distance patch writes only immediates (row id 0x2712 into `[rbx+4]`/`[rbx+8]`) and `xor cl,cl`. Upstream reads a distance float `[rsi]` and `[r13+0x188]`. The auto-rotate NOPs are 9-byte NOPs over unknown originals.
  - **None of these touches the camera matrix.** They only show that the follow-cam controller lives in 0x01836000–0x01842000.
  - "Uncap FPS++" also writes **camera smoothing fields** in that cluster: `0x1840799 mov [r13+0x238],0.5236 (π/6); mov [r13+0x23c],0.03`.
- **Unclear:** whether game logic reads the render camera back (movement relative to camera yaw, lock-on). Apply SKVR's restore-at-entry rule by default.

### b. "Model constants follow the camera" (`vk_rasterizer.cpp:1027`): CONFIRMED as an empirical observation by the port authors. Medium-high confidence.
- Context (`vk_rasterizer.cpp:1018-1060`): object-motion gating hashes the **bone palette**, not the model constants, "since last frame (model constants follow the camera, the palette does not)". `motion_history.h:39-41` identifies the G-buffer VS constant buffers as "864-byte scene constants (camera), **416-byte model constants** and palettes of 3x4 bone matrices".
- **Reading.** The 416-byte per-object block (26 vec4) changes when only the camera moves. So it holds camera-dependent data: a world-view(-projection) matrix, a camera-relative translation, or both.
  - This comes from development of the motion heuristic, not from a documented layout, hence "medium-high".
  - It is enough to rule out "patch only the 864-byte buffer and replay" (strategy C).
- **Settle it cheaply.** `BindBuffers` already has a capture hook that prints the G-buffer VS buffers (`vk_rasterizer.cpp:2103-2115`, "Object motion research"). One capture with a camera-only move would show which floats of the 416-byte block change.
- **Corollary.** Strategy D (clip-space remap) does not care *where* the matrices are, because it operates on the final `gl_Position`.

### c. HUD isolation via ui_image/output_image: CONFIRMED, with conditions.
- `TemporalUpscaler::OnDraw` (`vk_temporal_upscaler.cpp:1303-1341`) classifies the first UI draw. The trigger is a VS hash in `UiComposition::MovieShader` (`ui_composition.h:27-30`), or a native 1920x1080 viewport in a scaled session, plus an RGBA8 render target that is not the display buffer.
  - It then runs `RunScaled()` (temporal path) or `RunUiOnly()` (copy path).
  - After that, `ui_phase=true`, and later UI draws are redirected to `ui_color`/`ui_image` at output size.
- TAA/FSR4/DLSS path (`:1617-1688`): the upscaled scene goes to `output_image`, then `blitImage(output_image→ui_image)`, then the HUD draws into `ui_image`. **So `output_image` really is the HUD-free scene**, and `ui_image` is the scene plus HUD.
- **Conditions and caveats:**
  - (1) It only happens when the upscaler is `Scaled()`. With the upscaler off, the UI goes straight into the guest target.
  - (2) The FSR3 path renders into `ui_image` directly (line 1710). VR must use the TAA/DLSS/FSR4 branch, or add a copy.
  - (3) `RunUiOnly` blits the background with the scene (menus, loading). For a transparent HUD layer, replace both blits with a clear to (0,0,0,0).
  - (4) `PrepareUiDepth` copies **scene depth** into the UI depth, and Scaleform uses stencil masks. Keep that copy, since masks need stencil, but depth-tested HUD pieces would then test against eye-specific depth.
  - (5) Alpha: the game keeps data in alpha, and Scaleform blend states may not produce coverage alpha. Needs a visual test.
  - (6) **World markers** (lock-on dot, enemy HP bars, damage numbers) are almost certainly drawn by the same Scaleform movies. They would land on the head-locked HUD layer at screen positions computed from the mono camera, so they need re-projection (the erfps2 `CSFeManImp` analogue; BB's equivalent is unknown).

### d. Who drives a frame; can "frame N done" be observed? Render-twice feasibility
- **The game drives itself.** probe.c jumps to the guest entry, and guest threads are native host threads (`runtime_thread.c`, `pthread_setname_np` at line 246). The port never calls the game's update or render. Guest threads include the main thread and **"GXWorker"** threads (`docs/parallel_gpu.md:66-67`): FromSoft's GX library builds command buffers on workers.
- **The game imports** (`src/import_names.inc`): `sceGnmSubmitCommandBuffers`, `sceGnmSubmitAndFlipCommandBuffers`, `sceGnmSubmitDone`, `sceGnmRequestFlipAndSubmitDone`, `sceGnmInsertWaitFlipDone`, `sceVideoOutSubmitFlip`, `sceVideoOutAddFlipEvent`, `sceVideoOutGetFlipStatus`, `sceVideoOutSetFlipRate`.
- **Observable boundaries:**
  - `sceGnmSubmitAndFlipCommandBuffersForWorkload` (`gnmdriver.cpp:2258-2309`) takes `m_submit_lock`, calls `WaitGpuIdle()`, patches the flip packet into the last DCB (`PatchFlipRequest`) and submits. **This marks the end of frame N's guest command stream.**
  - The GPU-side completion comes back through the EOP flip IRQ, then `VideoOutDriver::SubmitFlip(..., is_eop=true)` (`driver.cpp:535-559`), then `SubmitFlipInternal` → `presenter->PrepareFrame` (561-583), then `PresentThread` → `Flip` → `Presenter::Present` (`driver.cpp:292-296`, 585+).
  - `sceGnmSubmitDone` increments `frames_submitted` (`gnmdriver.cpp:2355-2365`).
  - So yes: both "frame N's commands submitted" and "frame N's GPU work done/flipped" are observable host-side.
- **What is *not* available:**
  - A callable "render the scene again" entry. The render and update split lives inside guest code (main thread plus GXWorkers), which has not been reverse-engineered.
  - Re-running guest render code with frozen simulation means finding and re-invoking FromSoft's draw-list build on the GX workers. Comparable work exists only in ERVR (closed). Nobody has located BB's.
  - The global frame-delta `dt` is known: pointer at PS4 VA `0x059404F8`, `dt` at `+0x264`. It is read at `0x1bf9cba`, `0x21bc18b`, `0x2377cf6` and `0x2418e4d` (rip-relative targets computed from the Uncap patch). Zeroing it would freeze most dt-scaled systems, but it does not give a render-only call.
- **Performance blocker for A and B** (`docs/parallel_gpu.md:63-69`, `:145-150`, `docs/ROADMAP.md`): "The GPU command thread is back at ~100%: it is the limit again". The port reaches 57–94 FPS in heavy scenes (up to ~166 in light ones). Anything that **doubles the guest command stream** (A: game renders twice; B with a doubled game rate) doubles the load on the bottleneck thread, giving roughly 30–47 FPS per eye pair in heavy scenes. Duplicating at Vulkan record time (D) reuses one PM4 decode, bind and prepare per draw. Its extra CPU cost is a second `bindPipeline/draw` plus descriptor and push-constant writes.

### e. Sekiro-VR director seam: CONFIRMED (`sekiro-vr/src/src/sk_camera.cpp`)
- **Inject.** A mid-function MinHook on the `call` right after ChrCam's copy-out (expected 0x140735355) goes through `sk_stub.asm` to `skvr_on_chrcam(rdi=ChrCam)` (header 1-20, `inject()` ~280).
  - It `memcpy`s the pristine `ChrCam+0x10..+0x5F` (0x50 bytes) into `g_prist`, composes the head pose on top, and writes back.
- **Restore.** `hkDirector` hooks the director entry (expected 0x140734790, lines 245-260). If the camera still holds exactly what SKVR wrote (`memcmp(c+0x10, g_written)`), it copies `g_prist` back before the game's own logic runs.
  - It also has a pause-menu `repose` path from the Present thread (~277-287).
  - Culling hooks `hkCull<I>` on 0x140F00DF0/0x140F00E80 feed a fake copy with the true render FOV and aspect (~113-135).
- **Two eyes.** SKVR does **not** offset the camera per eye (`xr.cpp:121-126`: "no AER alternation, no camera eye-offset (geo-11 applies the separation inside the projection)").
  - geo-11, a 3Dmigoto-family **D3D11 proxy**, draws every draw twice and composites side by side. Separation and convergence live in geo-11's settings object (`sk_geo11.cpp:7-15`).
  - `xr.cpp:836` notes that `x += S(w-C)` "is an off-axis term (S*w) plus the eye translation (-S*C)".
  - Lesson from `xr.cpp` ("TILTFILL"): an off-centre projection "broke geo-11's light/decal fixes". Screen-space passes need per-shader correction.
- **Vulkan equivalent.** No geo-11 exists for Vulkan (Vk3DVision is closed). **We own the SPIR-V emitter, so we can build it in** (see §4 D).

### f. 120 FPS movement slowdown: partly explained, still unverified
- **Uncap FPS++** (`patches/Bloodborne.xml:1016+`) contains a global dt clamp, written into `0x2418E3D` by all FPS++ presets (lines 368, 730, 926, 1157):
  ```
  cmp dword [rbx+0x18],1/30 ; je ; mov [rbp-0x38],1/30
  movss xmm0,[rbx+0x264] ; maxss xmm0,[rbx+0x18] ; minss xmm0,[rbp-0x38]   => dt = clamp(dt, minDt, 1/30)
  ```
  - `minDt` (`[r12+0x18]`) is set to **1/240** (`41C74424188988883B` at 0x2434840/85A/87E/8B3). So the global floor is 240 FPS, not 120.
  - `0x1bf9cba..` clamps another consumer to `min(dt, 1/60)`. `0x1972a7d` puts **1/240** into a local of the camera-select function at 0x01972xxx, the same function as the debug-cam seam.
- **Prime suspect.** The data constant **`0x04D25844`** is set to **1/120** (`0x3C088889`) by Uncap only (line 1164). The 60-no-dt, 60++ and 90++ presets set it to 1/60 (lines 536, 702, 898). It is the only 1/120 value in the patch.
  - A subsystem with a fixed or minimum step of 1/120 that drops the remainder, or a per-fps table like Sekiro's sprint-speed table that SKVR had to re-point (02 §3.3), would slow movement above 120 FPS.
  - **Test:** set `0x04D25844` to 1/240 at 144–165 FPS and measure run speed. This is a correctness check, not a balance run.
- The repo has no explanation beyond comments (`emulator_settings.h:36`, launcher:205/830). The ROADMAP row says "running does not slow above 120 FPS" only *because* of the min(monitor,120) cap.
- **VR consequence.** AER at ≥2×72 game FPS is blocked until this is fixed. AER at a 90/120 game rate (45/60 per eye) is fine.

## 3. GAPS (no agent answered; the plan needs them)

1. **CPU frustum, LOD and animation culling.** SKVR needed two culling-builder hooks. For BB nothing is known beyond `LockCamParam.camFovY=43°`. Widening the camera FOV at +0x50 may widen culling too, but whether culling uses +0x50/+0x54 or a separate copy is unknown. This is required for any approach that renders a wider or second view than the game culled for (D, E, and canted Pimax views).
2. **The original game bytes.** No agent had the eboot. Every hook site above is inferred from patch bytes. The plan needs the user's decrypted v1.09 eboot (or bbport's prepared image `out/` from their install) to disassemble 0x01972998, 0x01836000–0x01842000 and the culling code.
3. **Shadow cascades.** Floats 112–175 in the 864-byte buffer; 4096² D32 maps. Are they fit to the camera frustum (would need widening for a union frustum)? Can both eyes share them? D must classify shadow draws (depth target ≠ scene depth, square 4096²) as mono.
4. **Occlusion queries:** answered here. They are **faked as always visible** (`liverpool.cpp:1116-1124`, `ZpassDone`/`PixelPipeStatDump` writes an incrementing counter), so per-eye occlusion mismatch is not a risk.
5. **Guest-visible GPU writes in a duplicated eye pass.** Readbacks exist ("Readbacks Relaxed", FaceGen, `docs/ROADMAP.md`). `vk_scene_resolution.h:18-19` keeps native images "for unmodified guest compute, integer texture loads, copies and CPU readbacks". An eye-2 replay must suppress every guest-memory write: EOP and labels, compute writing V#-backed buffers, readback copies. Nobody has enumerated these.
6. **Compute passes** (volumetric fog reads linear depth, SSAO, half-res effects, bloom). They need per-eye dispatch with per-eye constants. Which pass uses the 864-byte block versus its own matrices is unknown.
7. **Sky and other camera-locked draws.** deadinside28 issue #68: the sky draws "a clamped copy of the game view via ConstantRing". Skyboxes usually drop translation, so a clip remap with translation gives the sky parallax. Needs a classifier: no translation for sky, or a per-eye view built from rotation only.
8. **Particles and billboards** oriented to the mono camera. These are acceptable under D; just note it.
9. **HUD world markers** (§2c-6) and where BB computes their screen positions (an FeMan analogue).
10. **Cutscenes.** Remo is real-time; does it use the same render camera and seam? Per-shot FOV changes? FMV is detected through the movie VS hashes and goes to a flat quad. Untested.
11. **ImGui overlay.** It is drawn **directly on the swapchain image** in `Presenter::Present` (`vk_presenter.cpp:699`), not into any game image. For VR it must render into its own quad-layer swapchain, a separate path from the Scaleform HUD.
12. **Push-constant budget.** `Shader::PushData` is exactly 128 bytes (4 floats + 16 ud regs + 40 B offsets + motion_param + scene_size; `shader_recompiler/resource.h:212-238`, `static_assert(sizeof(PushData) <= 128)`). An eye index or clip-remap data needs a raised limit (256 on all desktop GPUs, with the assert changed), a bit packed into `motion_param` or `scene_size`, or a small per-eye UBO or BDA read like `EmitVertexMotion` does.
13. **Pipeline-cache keying.** Pipelines and shaders are cached by `pgm_hash` plus runtime info. A stereo epilogue must be in all VS variants or keyed in, or stale cached SPIR-V will lack it.
14. **Single Vulkan queue** (`queueCount=1`, `vk_instance.cpp:396-400`). OpenXR's runtime submits on the same queue, so it must take `Scheduler::submit_mutex`, or the device must be created with a second queue.

## 4. RECOMMENDED STEREO STRATEGY (evidence-based ranking)

Head pose is a **separate** problem from stereo in every option. One CPU-side injection per simulation tick at the 0x01972998 camera-select seam (§2a) gives rotation, position and yaw decoupling, using the SKVR inject-after-copy / restore-at-entry pattern. The options differ only in how the two eye images are produced.

### Rank 1: D, renderer-side stereo through a clip-space remap in the VS epilogue, then draw duplication or multiview ("Vulkan geo-11", built in)

**Why it works:** clip_eye = P_e·V_e·V⁻¹·P⁻¹·clip.
- For any vertex the game transformed with the main camera, the per-eye clip position is a **fixed 4×4 linear map M_eye of the mono clip position**. Homogeneous coordinates, no divide, so it is exact for w<0 too.
- This generalizes 3Dmigoto's `x += S(w−C)` (the special case of an eye translation along x plus an off-axis shift) to **arbitrary per-eye pose and FOV**: asymmetric frusta and Pimax canted displays included.
- It does **not** need to know where the model or world matrices live, which sidesteps §2b. SKVR's plan states the same identity (`P_eye·T_eye·P_c⁻¹`).
- P and V for the frame are already captured per frame by `CameraMotion::OnConstants` (864-byte block: view at floats 8-19, projection at 52-67; `vk_camera_motion.cpp:221-241`).

**Code anchors:**
- `EmitEpilogue` (`shader_recompiler/backend/spirv/emit_spirv_special.cpp:188-203`) already rewrites `ctx.output_position` in every vertex shader.
  - Examples: `ConvertDepthMode`, `ConvertPositionToClipSpace`, and bbport's own `EmitVertexMotion` (lines 69-147). That one loads per-draw parameters by buffer device address from a push-constant index, which is exactly the mechanism needed to fetch M_eye.
  - Insert `pos = M[eye] * pos` there, gated by a per-draw flag.
- Gating: the same predicate bbport uses for TAA jitter (`vk_rasterizer.cpp:1100-1107`: depth attachment == `camera_motion->Depth()` and more than 6 indices or instanced). That selects G-buffer and forward scene geometry, and excludes shadow maps, full-screen quads and UI.
- Full-screen and compute passes that reconstruct position read the 864-byte block. Small constants are already **copied into a host ring** by stage A (`vk_constant_ring.h`, bound at `vk_rasterizer.cpp:2036-2044`), so eye-2 can bind a second ring copy with P_e·V_e and inverses patched. No guest-memory write is needed.
- Per-eye targets: the `SceneTargets` proxy machinery (`vk_scene_resolution.h:18-60`) already substitutes host-side images for guest targets at attachment and sample time, and resolves for native access. It is the natural place for an eye-R proxy of every scene-sized target, and for per-eye size (for example 2064×2208) independent of the guest's 1920×1080.

**Two implementation variants:**
- **D-dup.** Record each scene-classified draw and dispatch a second time into the eye-R proxies, as a second command stream fed by the same prepared draw. Passes would be replayed per eye rather than per draw, to avoid render-pass thrash. The CPU cost is a second record, not a second PM4 decode.
- **D-mv** (what RPCS3-VR did in vr8). Scene targets become 2-layer arrays, rendering uses `viewMask=0b11` (dynamic rendering supports it; Vulkan 1.3 is already required, `vk_platform.h:21`), and `gl_ViewIndex` selects M_eye. Fragment and compute reads of those targets must select the layer, which means recompiler changes to sampled-image types. This is the cheapest at runtime and the most invasive.

**Costs and risks:**
- About 2× GPU raster work.
- Every screen-space pass must run per eye with per-eye constants.
- Guest-visible writes must be suppressed on eye 2 (Gap 5).
- Culling must be widened (Gap 1); the game's projection should be set to the union frustum.
- Sky and skybox exceptions (Gap 7).
- No sub-frame timing problems, no AER ghosting, and TAA keeps per-eye history naturally (each eye is a full frame).

### Rank 2: B, alternate-eye rendering at a ≤120 FPS game rate (first playable milestone)
- Only needs the CPU camera injection plus a per-frame ±IPD/2 offset and per-eye projection at +0x50 (or the same M_eye trick with one eye per frame).
- TAA, motion vectors and history must be keyed by eye parity (vrframework guide 10). The port already owns history and previous-VP (`vk_camera_motion.cpp`, `TemporalUpscaler`).
- Each eye updates at 45–60 Hz, and the stale eye depends on runtime ASW or our own reprojection.
- Game rate 90–120 stays under the movement limit (§2f). It also fits the GPU-command-thread budget only when the scene sustains 90 FPS mono, which the port does in light scenes but not heavy ones.
- Lowest engineering cost; known artifacts (ghosting, eye flip when the frame rate dips).

### Rank 3: E, depth reprojection (fallback and comfort layer)
- Depth, inverse view and projection are already available every frame (`camera_motion->Depth()`, `OnConstants`).
- Submit `XR_KHR_composition_layer_depth` so Meta ASW and VDXR SSW work, and optionally synthesize eye R from eye L in a compute pass next to `RecordMotion`.
- Cheap; disocclusion halos and weak parallax. Use it for menus, loading, under-budget frames, and as the gap filler for B.

### Rank 4: A, CPU-camera double render per simulation tick (ERVR-style). Not recommended now
- It needs an unlocated guest "render again" entry inside FromSoft's GX worker pipeline, with simulation frozen (no BB prior art; ERVR's source is unavailable).
- **It doubles the guest PM4 stream on the bottleneck GPU command thread** (§2d). Doubling it would likely give about 30–47 FPS per eye pair in heavy scenes.
- Revisit only if the port's GPU-thread throughput roughly doubles.

### Rank 5: C, GPU scene-constant patch plus command replay. Rejected as stated
- The 416-byte per-object constants follow the camera (§2b), so patching the 864-byte block moves only some of the geometry.
- It survives only as a sub-component of D (per-eye 864-byte copies for full-screen and compute passes).

**Suggested order:**
1. OpenXR host and swapchain (AstroQuest pattern) with a flat head-locked cinema layer.
2. CPU head-pose injection at 0x01972998, plus FOV and culling widening.
3. B (AER at 90) with per-eye TAA history, and a depth layer for ASW. This is the first true VR.
4. D-dup prototype on G-buffer draws only, using the existing jitter predicate, then extend pass by pass.
5. HUD layer from `ui_image` cleared transparent (TAA/DLSS/FSR4 path), then marker re-projection.

## Cross-links
- 01: C1, C3, C4 resolved here. The AstroQuest host remains the plumbing base.
- 02: the SKVR seam and two-eye mechanism are confirmed in §2e. The ERVR source is still not found (C2).
- 03: §2a, §2b and §2c confirmed with corrections (C6, C8). New: GPU-thread bottleneck, push-constant limit, ConstantRing per-eye path, faked occlusion.
- 04: its option "(1) clip-space formula" is upgraded to the main path here, in its exact 4×4 form. Its RPCS3 row is wrong (C1).
- 05: §2f adds the dt-clamp disassembly, the 1/120 suspect constant and the dt global at 0x059404F8+0x264.

Sources (web): https://api.github.com/repos/hookmanuk/rpcs3 , https://github.com/hookmanuk/rpcs3/releases , https://api.github.com/users/IlyaMez/repos , https://github.com/IlyaMez/EldenringVR (404) , https://www.nexusmods.com/eldenring/mods/10711 , https://raw.githubusercontent.com/shadps4-emu/ps4_cheats/main/PATCHES/Bloodborne.xml
