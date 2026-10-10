# 03: bloodborne_pc repo internals (code-level), for an OpenXR VR mod

Track: read-only code investigation. Nothing was built or run.
Clones (shallow):
- `scratchpad/bbvr/repos/supermedo/bloodborne_pc`: branch `windows`, commit `e0761196` "Online play, Add to Steam (1.6)", 2026-10-08.
- `scratchpad/bbvr/repos/deadinside28/bloodborne_pc`: branch `master`, commit `daa71654` "0.4: FSR 4.1.1 ...", 2026-10-07.

Paths below are relative to the Supermedo clone. Line numbers are for that commit.
"PS4 VA" means a patch-XML address (eboot base 0x400000).

---

## 1. Architecture

### 1.1 Loading and running the game (no emulator, no CPU translation)
- **Offline preparation:** `scripts/prepare.py`, `scripts/link_modules.py` and `scripts/link_libc.py` turn `eboot.bin` into a flat memory image (`BBPROBE1..5` format). PS4 libc and libSceFios2 are linked in as native code.
- **Loader:** `src/probe.c` `main()` (line 380 onward).
  - Reads the boot file (lines 463-570) and allocates the image with `allocate()` at any host address. It is not mapped at 0x400000; patches use image offsets.
  - Calls `bbgpu_init()` (line 575): window, Vulkan device, presenter and GPU command processor.
  - Builds import trampolines and resolves imports through `runtime_resolve()` (the HLE runtime in `src/runtime_*.c`) or `bbgpu_resolve()` (the vendored shadPS4 libs: GnmDriver, VideoOut and others).
  - **Applies patches** with `apply_patches()` (line 316, called at line 606) **before** page protections are set (lines 609-617).
  - Jumps to the guest entry (line 646, Windows).
- **Guest ABI:** the guest is SysV. Host functions exported to the guest are tagged `ABI` = `__attribute__((sysv_abi))` (`src/runtime.h:18`).
  - Windows TLS: `fs:[0]` loads are rewritten to `gs:[0x1480+8*slot]` (`patch_tcb_loads`, `probe.c:181`).
- **Host/guest boundary:** the C API is `gpu/bbgpu.h`: `bbgpu_init`, `bbgpu_resolve`, `bbgpu_handle_fault`, `bbgpu_overlay_captures_input` and others.
  - On Windows the GPU library is the static lib `libbbgpu.a`, linked into `bb-probe.exe`.

### 1.2 Patch and mod system
- **Patch format:** shadPS4/GoldHEN XML (`patches/Bloodborne.xml`, 3438 lines), compiled by `scripts/patches.py` into `out/patches.bin` (`BBPATCH2`: base, count, then `(offset,len,data)*`).
  - Only literal writes are supported: `bytes`, `bytes16/32/64`, `float32/64`, `utf8`, `utf16`. Mask (pattern) patches are rejected (`patches.py` docstring and `encode()`).
  - Addresses are checked against the eboot segments (`compile_patches`).
  - User-selectable patches are listed in the `EFFECTS` dict (`patches.py:28-50`), keyed from `bbport.ini`.
  - The resolution patch is rewritten in place (`resolution_writes`, scene W/H at PS4 VA 0x2196A6B/0x2196A7A).
  - Third-party XMLs come from the data dir's `patches/` folder (`external_patches`/`compile_external`), with an enable list in `patches.json`.
- **Hooking mechanism:** there are no runtime inline hooks and no detour library. Everything is static byte patches applied once at load: NOPs, rewritten immediates, and hand-assembled code caves that reuse dead code.
  - Examples: "Restore Debug Camera" writes about 200 lines of code at PS4 VA 0x1de3328-0x1de368b, and "60/90 FPS++" replace function pointers.
  - `BBPATCH2` carries the patch base so the loader can rebase written pointers (`probe.c:313-350`).
  - **Implication for VR:** we can add our own write that installs a `jmp`/`call` to a host `ABI` function. The loader controls the image, so a "host-call" patch type is about 20 lines in `apply_patches`. This is the cleanest head-tracking hook.
- **Mods (files):** loose-file overlay of `dvdroot_ps4/` (`scripts/mods.py`, `docs/MODS.md`). Folders use symlinks or junctions on Windows. Files only, no code.

### 1.3 Launcher and settings
- **Windows launcher:** `launcher/bbport_launcher_win.py` (Tkinter, frozen with PyInstaller into `Bloodborne.exe`), with 13 languages in `bbport_lang.py`.
  - Writes `bbport.ini` and environment variables such as `BB_FPS_LIMIT`, `BB_UPSCALER` and `BB_OUTPUT_RES`, then runs `run.py`.
  - `run.py` runs `patches.py` and starts `bb-probe.exe`.
  - Cheats page rows are at lines 142-160; the frame-cap list is at 204-207.
- **In-game overlay:** Dear ImGui, `gpu/shim/bbport_overlay.cpp`.
  - `BbOverlay::Init` (line 449) uses `ImGui_ImplVulkan`; `Menu()` is at line 156.
  - `HandleEvent` (line 524) handles SDL events: Insert or L3+R3.
  - `Render(cmdbuf, view, extent)` (line 629) is called from `Presenter::Present` **directly onto the swapchain image** (`vk_presenter.cpp:699`), after the game frame blit.
  - Live settings are in `gpu/shim/bbport_settings.{h,cpp}` (`BbSettings::Get()`). Debug A/B bits are in `bbport_toggles.h`.

## 2. Camera, FOV and frame time

### 2.1 Camera patches (CPU side, PS4 VA, v1.09)
- **"Increased camera distance"** (Kyo), `Bloodborne.xml:1213-1220`:
  - `0x01836940` = `30 c9 90` (`xor cl,cl; nop`)
  - `0x05527a94` = `00` (data)
  - `0x0184107f` = `c7 43 04 12 27 00 00` (`mov dword [rbx+4],0x2712`)
  - `0x018410b1` = `c7 43 08 12 27 00 00` (`mov dword [rbx+8],0x2712`)
- **"Disable Camera Auto Rotation via Movement"** (Imedved, Kyo), lines 1205-1212: 9-byte NOPs at `0x0183c6e8`, `0x0183c870`, `0x0183c984`, `0x0183dde6`.
- **What this tells us:** the third-person player-camera controller (FromSoft "ChrCam"/"CSPlayerCam"-like) lives in about **PS4 VA 0x01836000-0x01842000**. Both patches hit it. This is where to look for the per-frame camera update to override.
- **"Restore Debug Camera"** (Lance McDonald, GoldHEN), lines 2865-3060: a code cave at `0x01de3328`, entered from `0x019729ab`.
  - I disassembled the bytes by hand. They show the **camera object layout**:
    - `mov rcx,[r12+0xd0]` gets the target camera.
    - `vmovaps xmm,[r12+0x50..0x80]` → `[rcx+0x10..0x40]` copies a **4x4 float matrix at camera+0x10** (camera-to-world).
    - `vmovss [r12+0x4c]` → `[rcx+0x50]` copies the **FOV float at camera+0x50**.
    - The else branch copies `[rdx+0x50..0x5c]` (4 floats: fov, aspect, near, far as in DS3's FieldInsCamera) plus the matrix from `[r12+0x10]`.
  - This matches the DS3 camera layout (matrix @0x10, fov @0x50). **[inferred, high confidence]**
  - **This is the best head-tracking injection point:** after the game writes the camera matrix, multiply it by the HMD pose (and per-eye offset) before rendering and culling read it.
- **The port has no FOV setting.** FOV is visible only on the GPU side: `CameraMotion::VerticalFov()` (`vk_camera_motion.cpp:155`), `2*atan(1/proj[1])` (about 43.0° vertical at 16:9).

### 2.2 GPU-side camera (already located by the upscaler work)
- The **864-byte "scene constants" buffer**, documented in `docs/upscaler.md:104-118`:
  - Signature: `[0]=3000` (far), `[1]=1/3000`, `[4]=W`, `[5]=H`.
  - View 3x4 at floats 8-19, inverse projection at 36-51, projection rows at 52-67 (`52,57,62,63` non-zero: x 1.42799, y 2.53865, near 0.05, far 3000, D3D 0..1 depth).
  - Shadow cascades at 112-175, inverse view at 180-191.
- **Detection:** `Rasterizer::BindBuffers` (`vk_rasterizer.cpp:2027`).
  - A constant buffer of size 864 bound during a G-buffer draw goes to `CameraMotion::OnConstants` (lines 2036-2040 for the ring path, 2097-2101 for the V# path).
  - `OnConstants` (`vk_camera_motion.cpp:221-241`) checks the signature and takes **the first match per frame = main camera**.
  - The G-buffer pass is recognised as ">=5 color targets + depth" (`PrepareRenderState`, `vk_rasterizer.cpp:266-270`).
- **Caveat for GPU-side stereo:** `vk_rasterizer.cpp:1027` says *"model constants follow the camera"*. Per-object vertex constants change when the camera moves, which suggests camera-relative or premultiplied object matrices.
  - So patching only the 864-byte buffer would **not** move all geometry. Stereo must be done on the **CPU camera** (2.1), or by also rewriting per-draw constants. **[S, needs a capture]**
- **Jitter is applied as a viewport offset, not a projection change:**
  - `draw_jitter` is set in `DrawRecord` (lines 1101-1107) only for draws using the scene depth with more than 6 indices.
  - It is applied in `UpdateViewportScissorState` (lines 3261-3264).
  - Halton(2,3) is generated in `TemporalUpscaler::OnFrameStart` (`vk_temporal_upscaler.cpp:347-356`).
  - **This mechanism could also do per-eye asymmetric shifts.** A viewport offset can only translate the image, which is not a true stereo projection.

### 2.3 Frame time, FPS unlock and the 120 FPS limit
- FPS presets (`patches.py:17`): `30`, `60 FPS++`, `90 FPS++`, `Uncap FPS++`. Uncap is at `Bloodborne.xml:1016`, around PS4 VA 0x2434xxx. These are Lance McDonald and Kyo's DS3-derived delta-time patches.
- **Pacing:** `VideoOutDriver::PresentThread` (`gpu/shadps4/core/libraries/videoout/driver.cpp:585`).
  - In uncapped mode the vblank runs at 480 Hz, and a frame-limit slot gates each flip (lines 595-628).
  - Settings are in `gpu/shim/core/emulator_settings.h:33-53`: `GetVblankFrequency`, and `GetFrameLimit` = `BB_FPS_LIMIT` or `min(display Hz, 120)`.
- **Why movement slows above 120:** nothing in the code explains it. The comments only say *"above ~120 FPS movement timing breaks: running slows"* (`emulator_settings.h:36`, `bbport_launcher_win.py:204-205, 830`).
  - It is a limit of the game and the community delta-time patch, perhaps a clamped minimum dt or per-frame constants. **Unexplained in the repo.**
  - **For VR:** 90 or 72 Hz per eye-pair is fine. Alternate-eye rendering at 2×90 = 180 game frames/s would exceed the 120 limit and break movement.
  - Prefer two renders per game tick (synchronized sequential), or one game frame with two camera passes.
- **xrWaitFrame placement:** the flip pacing loop in `PresentThread` (or the GnmDriver flip/EOP path) is where the guest is throttled. `xrWaitFrame` should replace the frame-limit slot there.

## 3. Renderer

### 3.1 PS4 command translation
- **GnmDriver** → `AmdGpu::Liverpool` (`gpu/shadps4/video_core/amdgpu/liverpool.cpp`).
  - `Process` is at line 105; the PM4 decode switch with `DrawIndex2`/`DrawIndexAuto`/indirect is at lines 213-230.
  - `ApplyGraphicsRegisterPacket` is at line 479.
- **bbport two-stage pipeline** (`vk_draw_pipe.h`, `docs/parallel_gpu.md`):
  - Stage A (Liverpool thread) decodes PM4 and writes draw packets.
  - Stage B (recording thread) runs `Rasterizer::RunDrawPacket` (line 701) → `DrawRecord` (line 950).
- **Rasterizer** (`vk_rasterizer.cpp`, 3593 lines):
  - `PrepareRenderState` (line 220) resolves render targets and depth through the texture cache.
  - `BindResources` (line 1526) and `BindBuffers` (line 2027) bind resources; `BeginRendering` is at line 2614.
  - The draw itself is recorded by `scheduler.Record(...drawIndexed...)` (lines 1113-1120).
- **Replaying draws twice:**
  - **Conceivable at Vulkan level.** Each draw is a closure `scheduler.Record(cmdbuf→bind+draw)` with pipeline, descriptors and push data prepared just before.
  - In principle each scene draw could be recorded twice into two render targets (or once with multiview), with a different viewport or a patched constant.
  - **Blockers:**
    1. Per-object constants depend on the camera (2.2 caveat). They live in guest memory read through V#s, so a second eye would need a second copy of every camera-dependent buffer.
    2. CPU culling and LOD are done for one camera.
    3. Lighting, fog, SSR and post are full-screen passes keyed to one set of targets. The whole frame graph, not just G-buffer draws, would need duplicating.
    4. Guest compute passes and readbacks.
  - **Lower-risk alternative:** run the guest frame twice per tick with a CPU camera offset ("synchronized sequential"). That needs game-side control (freeze sim time for the second pass). Or accept AFR.
  - **Depth-reprojection fallback:** depth and camera matrices are already available every frame (see 3.2), so a reprojected second eye is cheap.

### 3.2 Depth, motion vectors, jitter and scene color (all already located)
- **Scene depth:** the G-buffer pass's D32S8 (`CameraMotion::OnGBufferPass`, `vk_rasterizer.cpp:266-270`), exposed as `camera_motion->Depth()`.
- **Scene color (HDR RGBA16F):** `TemporalUpscaler::OnSceneColor` (called at lines 273-282). It is "the RGBA16F target same size as scene depth, at most 2 MRTs".
- **LDR (tonemapped RGBA8) target:** `OnColorTarget` (`vk_temporal_upscaler.cpp:1291`).
- **Motion vectors:** computed by `CameraMotion::RecordMotion` (`vk_camera_motion.cpp:179`).
  - Compute shader: `camera_motion.comp`. It uses depth + previous view × current inverse view + projections, plus an object-motion image from `vk_object_motion.cpp` (vertex replay with the previous frame's positions).
- **Upscaler entry:** `TemporalUpscaler::RunScaled` (`vk_temporal_upscaler.cpp:1528`). It dispatches TAA, DLSS or FSR4 (lines 1617-1690), then FSR3 below.
  - It writes `output_image` and **blits it into `ui_image`** (lines 1641-1661), then sets `ui_phase = true`.
  - The 1080p path upscales the HDR scene before game post. Other outputs upscale the tonemapped scene at the first Scaleform pass (`docs/upscaler.md` "Live resolution").
- **DLSS:** `vk_dlss.cpp` loads `bbport_dlss.dll` (`gpu/dlss_bridge`, MIT) through `LoadLibraryW` + `BbDlssGetApi`. Inputs are color, depth, render-pixel MVs and jitter (`RecordDlss`, `vk_temporal_upscaler.cpp:1989-2015`).
  - **This bridge-DLL pattern is the template for an OpenXR bridge.**

### 3.3 Render-target tracking
- Through shadPS4's `VideoCore::TextureCache` (`texture_cache/texture_cache.cpp`): `FindTargetMemoized` (`vk_rasterizer.cpp:166`), with ImageIds per color buffer (`cb_descs`) and depth (`db_desc`).
- `vk_scene_resolution.cpp` / `scene_resolution.h` (`SceneTargets`) create **host-side proxies at a different resolution** than the guest allocation (live-resolution path).
  - **Directly useful:** per-eye render size (for example 2016x2240) can differ from the guest's 1920x1080 without patching the game.
  - Non-16:9 aspect still needs the game's projection or a resolution patch.

### 3.4 Present path
- Guest flip → `VideoOutDriver::Flip` (`driver.cpp:292`) → `Presenter::PrepareFrame` (`vk_presenter.cpp:354`).
  - It uses `upscaler.DisplayOverride` (the output-size display image) when upscaled; otherwise the guest display buffer via the texture cache.
- `Presenter::Present` (`vk_presenter.cpp:570-740`):
  - acquire → clear → **`blitImage(frame->image → swapchain)`** (line ~670)
  - → ImGui overlay (line 699)
  - → submit (waits `frame->ready_semaphore`) → `swapchain.Present()` under `Scheduler::submit_mutex`.
- **For OpenXR:** replace or augment this with the eye images copied into XR swapchain images, then `xrEndFrame`, keeping the desktop mirror.
  - All queue submits must take `Scheduler::submit_mutex` (`vk_scheduler.h:875`), because the device has **one queue** (`queueCount=1`, `vk_instance.cpp:396-400`).

### 3.5 HUD/UI separation
- UI is Scaleform. Its draws are recognised by vertex-shader hash: `UiComposition::MovieShader` (`ui_composition.h:27-30`: 0x34e8a281, 0x81d336ce, 0x09957251, 0x24042a9b, 0xa400228b), or by a 1920x1080 native viewport.
- On the first UI draw, `TemporalUpscaler::OnDraw` (line 1304) runs `RunScaled()` or `RunUiOnly()` and **redirects all following UI draws into the output-size `ui_image`** (with a copied UI depth, `PrepareUiDepth` at line 1403). The scene is blitted in first as background.
- **So the 3D/UI boundary is known exactly.**
  - `output_image` holds the scene without HUD (when TAA, FSR4 or DLSS ran). `ui_image` holds scene + HUD.
  - For a VR quad layer, clear `ui_image` to transparent instead of blitting the scene into it. A small change at lines 1641-1661 and in `RunUiOnly`.
  - Then `ui_image` becomes a pure HUD layer with alpha, and `output_image` is the eye image.
- Caveats:
  - The game keeps data in alpha. The display view forces alpha=1 (`vk_presenter.cpp` view swizzle).
  - Some UI may use premultiplied blending against the scene, so check its look on a transparent target.
  - Menus with full-screen movies also go through this path.

## 4. Vulkan version, instance and device
- **Vulkan 1.3 required:** `TargetVulkanApiVersion = VK_API_VERSION_1_3` (`vk_platform.h:21`).
  - Uses dynamic rendering, `synchronization2` (`pipelineBarrier2`), push descriptors and VMA. The FSR4 path needs int8 dot products and related features.
- **Instance:** `CreateInstance` (`vk_platform.cpp:258-425`), `vk::createInstanceUnique` at line 421.
  - Uses the vulkan-hpp dynamic dispatcher with its own `DynamicLoader`.
  - App name is "shadPS4". Extensions come from `GetInstanceExtensions(window_type)` plus layers.
  - **Hook already present:** `Dlss::AppendInstanceExtensions(extensions)` (line 293).
- **Physical device:** chosen in `Instance::Instance` (`vk_instance.cpp:106-170`) by sorting (Vulkan 1.3 support, discrete, most VRAM), or `BB_GPU_ID`.
  - For XR_KHR_vulkan_enable2 it must instead use `xrGetVulkanGraphicsDevice2KHR`.
- **Device:** `createDeviceUnique` (`vk_instance.cpp:629`), with a DLSS device-extension hook at lines 405-416.
- **Plan:** either append the runtime's required extensions in the same place the DLSS hook does (XR_KHR_vulkan_enable style), or route both creates through `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` (enable2).
  - Both are about 50-line changes in `vk_platform.cpp` and `vk_instance.cpp`.
  - The DLSS `CollectExtensions` helper (`vk_dlss.cpp:137-175`) can be reused.

## 5. Input
- PS4 pad HLE: `src/runtime_pad.c`. `scePadReadState` → `pad_read_state` (line 313) → `sample()` (line 276) → `sample_host()` (line 92).
  - `sample_host()` reads an SDL3 gamepad, falling back to the keyboard. `PadData` is the OrbisPadData layout (lines 32-49, 120 bytes, including `orientation[4]`, `acceleration`, `angular_velocity`).
- An injection layer already exists, `injected` (lines 155-200, `BB_PAD_FILE`), merged in `sample()` (lines 282-290).
  - **OpenXR controller input** goes in the same place: an `ABI` callback or a shared struct filled by the XR thread, OR-ed into `d->buttons` and the sticks.
  - The overlay already neutralises input with `bbgpu_overlay_captures_input()`.
  - Motion fields could carry controller pose, though BB ignores the gyro.
- Rumble: `pad_vibration` (line 349) → `SDL_RumbleGamepad`. It can be mirrored to `xrApplyHapticFeedback`.

## 6. Licenses
- **Repo:** GPL-2.0-or-later (`LICENSE`; README "License"). The same applies upstream (deadinside28 `LICENSE`, GPLv2 text).
- **Vendored shadPS4 core:** GPL-2.0-or-later (`gpu/VENDOR.txt`, from shadPS4 commit b4e7ae77). Files carry `SPDX-License-Identifier: GPL-2.0-or-later`, and local changes are marked `bbport:`.
- **Other components:**
  - MIT: `gpu/dlss_bridge` (`SPDX MIT`, `LICENSE.txt`), FSR-Vulkan, the FidelityFX SDK, Dear ImGui, LibAtrac9, magic_enum, miniz, VMA.
  - BSD-3: xbyak. zlib: SDL3.
  - The NVIDIA DLSS SDK and `nvngx_dlss.dll` are under NVIDIA's license, loaded at runtime only.
  - Online play uses shadp2p code.
  - AMD FSR4 DLLs and models are not distributed.
- **For VR:**
  - The OpenXR loader is Apache-2.0. Linking it into GPL-2.0-or-later code is acceptable under GPL-3 terms ("or later"). Loading it dynamically, or keeping a separate bridge DLL like DLSS, is cleanest.
  - The repo itself has no "all rights reserved" parts. The game patches are community-authored byte patches with credits.
- **Upstream note:** deadinside28's tree is Linux-first and has no DLSS bridge and no `compat_win.c`. The Windows port, DLSS and launcher are Supermedo's additions.

---

## VR injection points

| Feature | File : function (lines) | Difficulty |
|---|---|---|
| Head pose → game camera (true 6DoF, culling-correct) | New host-call patch type in `src/probe.c:apply_patches` (316) + hook in the player-camera update around PS4 VA 0x01836000-0x01842000, or at the debug-cam copy site writing `cam+0x10` matrix / `+0x50` FOV (`Bloodborne.xml:2865`, cave 0x01de3328) | Hard (RE needed); highest value |
| FOV / per-eye asymmetric projection | Camera `+0x50` FOV (CPU). The GPU projection is in the 864-B scene constants floats 52-67 (`vk_camera_motion.cpp:221`); asymmetric needs the off-diagonal terms or a per-eye viewport offset (`vk_rasterizer.cpp:3261`) | Medium |
| Read camera matrices every frame (for reprojection, ASW, debug) | `CameraMotion::OnConstants` (`vk_camera_motion.cpp:221`): `current.view`, `inv_view`, `proj` | Easy (exists) |
| Depth buffer + motion vectors for the XR depth layer / reprojection | `camera_motion->Depth()`, `CameraMotion::RecordMotion` (179), motion image in `RunScaled` (`vk_temporal_upscaler.cpp:1528`) | Easy (exists) |
| Stereo via a second render of the frame | Guest-side: run the sim tick once and render twice (needs game RE). GPU-side replay in `Rasterizer::DrawRecord` (950) / `scheduler.Record` is blocked by camera-dependent per-object constants (line 1027) | Very hard |
| Depth-reprojected second eye (fallback) | New compute pass next to `CameraMotion::RecordMotion`, inputs depth + `inv_view` + `proj` | Medium |
| Per-eye render resolution | `vk_scene_resolution.cpp` `SceneTargets` proxies + `TemporalUpscaler` output size (`BB_OUTPUT_RES`) | Medium |
| Scene image without HUD → XR projection layer | `output_image` in `TemporalUpscaler::RunScaled` (`vk_temporal_upscaler.cpp:1617-1690`) | Easy-Medium |
| HUD as a separate quad layer | Clear `ui_image` transparent instead of the scene blit (`RunScaled` 1641-1661, `RunUiOnly` 1461); UI draws are detected by `UiComposition::MovieShader` (`ui_composition.h:27`) | Medium |
| XR swapchain submit / xrEndFrame | `Presenter::Present` (`vk_presenter.cpp:570`), under `Scheduler::submit_mutex` | Medium |
| Frame pacing (xrWaitFrame/xrBeginFrame) | `VideoOutDriver::PresentThread` (`driver.cpp:585`), frame-limit slot; `EmulatorSettings::GetFrameLimit` | Medium |
| Vulkan instance/device through OpenXR (enable2) | `CreateInstance` (`vk_platform.cpp:258`, DLSS hook at 293); `Instance::Instance` device pick (`vk_instance.cpp:106`) + create (629, DLSS hook at 405) | Medium |
| OpenXR controllers → PS4 pad | `src/runtime_pad.c:sample` (276), next to the `injected` merge | Easy |
| Haptics | `runtime_pad.c:pad_vibration` (349) | Easy |
| VR settings UI | `gpu/shim/bbport_overlay.cpp:Menu` (156) + `bbport_settings.h`; launcher `bbport_launcher_win.py` (Cheats/Advanced pages) | Easy |
| Bridge DLL packaging (OpenXR loader kept separate) | Copy the pattern of `vk_dlss.cpp` (`LoadLibraryW` + `GetApi` ABI-versioned struct) | Easy |
| Disable effects that break in VR (DoF, motion blur, CA, game AA) | Already patch toggles: `scripts/patches.py` `EFFECTS` | Trivial |

## Cross-links
- `01-existing-bloodborne-vr.md`: AstroQuest / Farpoint PC VR (GPL shadPS4 forks with an OpenXR host). Their Vulkan-instance, XR-swapchain and pacing code maps onto the injection rows above (`vk_platform.cpp`, `vk_presenter.cpp`, `driver.cpp`). Its camera-address list (section 3) matches the PS4 VAs found here (0x0183xxxx camera controller, debug cam cave 0x01de3328).
- `04-flat2vr-tech.md`: UEVR's "synchronized sequential" mode is the analog of the "render twice per tick" option in 3.1. The UI-to-quad-layer approach matches the `ui_image` separation in 3.5. Its OpenXR-on-Vulkan notes apply to section 4 (enable2 requires the runtime to pick the physical device, replacing the sort in `vk_instance.cpp:106`).
- `05-bloodborne-engine-modding.md`: DS3-lineage class layouts (camera @0x10 matrix / @0x50 FOV) support the layout inferred from the debug-camera cave in 2.1. Its frame-timing section expands on the 120 FPS limit (2.3).
