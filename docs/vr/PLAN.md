# Bloodborne VR: deep plan (OpenXR, Quest + Pimax)

Date: 2026-10-09. Target: https://github.com/Supermedo/bloodborne_pc (branch `windows`, read at e0761196). It is a native Windows build: the game's x86-64 code runs natively (loaded by `src/probe.c`), and the shadPS4-derived Vulkan 1.3 renderer draws it. License GPL-2.0-or-later.

Research: `findings/01..06` (5 parallel agents plus a cross-check pass). File:line references below point into that repo unless they say otherwise.

---

## 0. Does Bloodborne VR already exist? No.

Everything labelled "Bloodborne VR" turned out to be one of these:
- **Flat virtual screens.** The YouTube "Bloodborne VR [shadPS4] [Quest 2]" video is Quest Link desktop mode. PSVR cinematic mode and Remote Play are the same.
- **Flat first-person mods.** Garden of Eyes (Nexus #11) and a shadPS4 one (Nexus #287).
- **A failed attempt.** vorpX cannot hook shadPS4's Vulkan (vorpX forum thread, Apr–May 2025).

shadPS4 issue #3156 (VR) is an idle feature request. Luke Ross R.E.A.L. supports Elden Ring, DS Remastered and DS2, but not Bloodborne. **We would be first.**

Prior art we can reuse:

| Project | What it gives us | License |
|---|---|---|
| **AstroQuest** (bigmak94), shadPS4 fork | Working OpenXR host, `src/core/vr/openxr_host.cpp`: shared Vulkan device, pose-tagged submit, 20 ms prediction. Tested on VD, SteamVR and Pimax. (It only runs PSVR games, which already render both eyes.) | GPL-2+, **same as ours** |
| **hookmanuk/rpcs3 "RPCS3 VR"** | Demon's Souls VR on an emulator. vr8 = **multiview stereo**, 130° culling, motion blur/DoF off, cutscene camera smoothing | GPL-2 |
| **Sekiro-VR** (junglejuz) | The best FromSoft camera teardown: inject head pose after the camera copy and restore it at director entry, widen culling, HUD on a head-locked layer | open |
| **ds3fps / erfps2** (Dasaav) | First-person camera: head bone, head and face mesh hiding, near-fade off, lock-on, audio listener, HUD markers | open |
| **vrframework** (elliotttate) | An engine-agnostic VR core plus a 17-part guide. Its `IEngineAdapter`/`StereoView` design and per-eye TAA history model our VR layer | MIT |
| PPSSPP `Common/VR/` | Projection swap, HUD detection by matrix, per-game `compatvr.ini`, multiview | GPL-2+ |
| mbucchia VDXR / OpenXR Toolkit / Quad-Views-Foveated | Runtime-side reference code, foveation | MIT |
| ERVR (Elden Ring) | Second eye rendered with the simulation frozen, motion-controlled attacks. **Source not public** (404) | — |
| UEVR | Design ideas only. **All rights reserved**, not open source | — |

---

## 1. The strategy we reuse from Elden Ring / Dark Souls / Sekiro VR

Every good FromSoft VR mod separates **head pose** from **stereo**:

1. **Head pose goes into the CPU-side camera, once per game tick (the Sekiro-VR seam).**
   - Hook right after the game copies the active camera into the render camera.
   - Save the original values, write `game_cam ∘ head_pose`, then restore the saved values when the director function is entered again.
   - The game logic (movement relative to camera yaw, lock-on) therefore never reads our head pose.
2. **Stereo** is generated one of three ways:
   - geo-11 draws everything twice with a clip-space shift (Sekiro-VR)
   - a second engine pass with the simulation frozen (ERVR)
   - alternate eyes (R.E.A.L.)
3. **HUD on its own OpenXR quad layer.** Sekiro-VR spent most of its roughly 160 builds learning this.
4. **Post effects that break in VR are disabled:** DoF, motion blur, CA, camera shake.
5. **Culling is widened** to the union of both eye frustums.
6. **Gamepad first.** Motion controls are last, and only as triggers for the game's own attacks (the ERVR approach), never physics-driven swords.

**Bloodborne fits this template.** Disassembling the existing "Restore Debug Camera" patch (`patches/Bloodborne.xml:2865`, hook `0x01972998`, cave `0x01de3329`) shows the render camera has **the same layout as Sekiro's `SprjPersCam`**:
- a 4×4 matrix at `+0x10..+0x4F`
- FOV at `+0x50`
- probably aspect/near/far at `+0x54..+0x5C`

`0x01972998` is the active-camera selection function. That is our seam.

### What's different about Bloodborne, and our advantage

**There is no geo-11 for Vulkan, but we own the shader recompiler.** shadPS4 recompiles every PS4 shader to SPIR-V, and `EmitEpilogue` (`shader_recompiler/backend/spirv/emit_spirv_special.cpp:188-203`) already rewrites `gl_Position` in every vertex shader. So we build the "Vulkan geo-11" into the renderer:

```
clip_eye = M_eye · clip_mono,   M_eye = P_eye · V_eye · V_mono⁻¹ · P_mono⁻¹
```

This is exact for any eye pose and FOV, including asymmetric frustums and **Pimax canted displays**. It does not need to know where the game stores its matrices, which matters because the 416-byte per-object constants also follow the camera (`vk_rasterizer.cpp:1027`). That rules out patching only the scene constants. `V_mono`/`P_mono` are already captured every frame by `CameraMotion::OnConstants` (`vk_camera_motion.cpp:221`).

---

## 2. Architecture

```
┌──────────── bloodborne_pc (GPL-2+) ────────────┐        ┌──── xrbridge.dll (MIT, ours) ────┐
│ probe.c  ── host-call patch ──► camera seam    │        │ OpenXR instance/session          │
│          (0x01972998: inject/restore pose)     │◄─pose──│ xrWaitFrame/Begin/End, predict   │
│ runtime_pad.c:sample ◄────────── pad state ────│◄─input─│ action sets → virtual DS4        │
│ renderer                                       │        │ swapchains: eyes, HUD quad,      │
│  EmitEpilogue: pos = M[eye]·pos (scene draws)  │        │   menu quad, depth (ASW/SSW)     │
│  SceneTargets: per-eye proxies                 │──imgs─►│ Vulkan enable2 helpers           │
│  TemporalUpscaler: per-eye TAA/DLSS history    │        │ comfort: snap turn, vignette     │
│  ui_image cleared transparent → HUD layer      │        │ per-game profile (JSON)          │
│  Presenter::Present → xrbridge submit          │        └──────────────────────────────────┘
└────────────────────────────────────────────────┘
```

- **xrbridge** is the reusable **VR layer for every headset**: OpenXR, so it covers Quest (Link, Air Link, Virtual Desktop VDXR, Steam Link) and Pimax (Pimax Play OpenXR or SteamVR).
- It is a separate DLL with an ABI-versioned `GetApi()` struct, the same pattern as the port's MIT DLSS bridge (`vk_dlss.cpp`). The game-specific parts stay in the fork, and the bridge can be reused by other flat games we patch (the vrframework adapter idea).
- **Vulkan setup:** create the instance and device through `XR_KHR_vulkan_enable2`, at the existing DLSS hook points `vk_platform.cpp:293` and `vk_instance.cpp:405`. The runtime chooses the physical device, which replaces the sort in `vk_instance.cpp:106`.
- **Single queue** (`vk_instance.cpp:396`). xrEndFrame must hold `Scheduler::submit_mutex`, or we create a second queue.
- **Swapchains:** pick the sRGB format carefully, or gamma gets applied twice.
- **Pimax:** never assume parallel eye views; use the per-view pose and FOV that `xrLocateViews` reports.

---

## 3. Phases

Each phase ends in something playable or measurable. Effort is rough, for one focused developer.

### Phase 0: Setup and ground truth (1 week)
- Fork bloodborne_pc. Build it with MSYS2 CLANG64 as its README describes. Run it with **the user's own** decrypted CUSA03173 v1.09 dump.
- **Disassemble the real eboot** around `0x01972998`, the camera-controller cluster `0x01836000–0x01842000`, and look for the CPU frustum/culling builder. Every hook site is currently inferred from patch bytes only (06 §3 gap 2).
- **Capture a frame** using the existing "object motion research" dump (`vk_rasterizer.cpp:2103-2115`) while moving only the camera. Find which of the 416-byte model constants change.
- **Test the 120 FPS movement limit:** set the 1/120 constant at `0x04D25844` to 1/240 and measure run speed at 144–165 FPS. This is a correctness check, not a balance run.
- **Measure GPU command-thread headroom** in a heavy scene (the port reaches 57–94 FPS there; the thread is the bottleneck).

### Phase 1: xrbridge plus cinema mode (1–2 weeks). Plays on Quest and Pimax.
- xrbridge DLL: instance, session, swapchains, frame loop driven from `VideoOutDriver::PresentThread`, submit from `Presenter::Present`. Port AstroQuest's `openxr_host.cpp` pattern.
- The flat game shows on a large curved quad, either head-locked or in world space.
- **ImGui overlay** goes to its own quad swapchain (today it is drawn on the swapchain image, `vk_presenter.cpp:699`).
- OpenXR controllers map to the DS4 in `runtime_pad.c:276`, and haptics come from `pad_vibration` (349).
- **Exit:** Bloodborne playable as a cinema screen on a Quest 3 (via VD) and on a Pimax, both at 90 Hz without dropped compositor frames.

### Phase 2: Head tracking (2–3 weeks)
- **Status (2026-10-09, `findings/15`):** head rotation injected at H1 (draw-step PersCam copy; H2 kept for
  consistency), verified flat with a fake head (`tools/vr_head_test.py`); wider FOV widens CPU culling by itself;
  optional LOD fix hook; mono shows each image with the view it was rendered with (bridge ABI 5).
- Add a **host-call patch type** to `apply_patches` (`probe.c:316`). Patches today are static byte writes only, so this lets a patch call back into our own code.
- Hook the camera seam at `0x01972998` with the Sekiro-VR inject/restore pattern. Head rotation and position are composed with the game camera, and the body and camera yaw stay with the gamepad.
- Set the FOV at `+0x50` to the HMD vertical FOV, and **widen CPU culling** (the hook location comes from Phase 0).
- Apply the existing effect patches: motion blur `0x026C2549`, DoF `0x025D7BBC`, CA `0x0269faa8`, game AA off. Disable camera auto-rotation (`0x0183c6e8`).
- Still mono: one rendered image goes to both eyes as a projection layer with **depth submitted** (`XR_KHR_composition_layer_depth` from `camera_motion->Depth()`), so ASW/SSW can reproject.
- **Exit:** look around Yharnam with correct parallax-free 3DoF+6DoF, no judder from head motion, and gameplay movement unaffected.

### Phase 3: First real stereo, alternate-eye rendering (2 weeks). The first true VR.
- **Status (2026-10-09, `findings/22`):** `BB_XR_MODE=alt` works flat: the H1 camera is moved to the eye (the
  runtime's eye offsets, after the head and first person), each finished frame is paired with its eye by its
  camera and an eye tag in the projection (0 swaps in 14822 frames, parallax measured to the pixel), bridge
  ABI 6 shows each eye's latest image with its own view; pacing best at 72 FPS (`BB_FPS_LIMIT=72`); temporal
  effects off (motion blur smears every frame). First person works too (after a scene-camera tracking fix).
  Optional off-axis frusta through projection hooks: geometry exact, one distant-haze pass still assumes a
  symmetric frustum, so off by default. Not yet run in the headset. The camera route (not `M_eye`) was
  chosen, per findings/09 §4.1.
- Per frame: `M_eye` applies the ±IPD/2 offset and per-eye projection (same epilogue as Phase 4, but one eye per frame).
- TAA, DLSS and FSR history and the previous-VP are **keyed by eye parity**. The port already owns these (`vk_camera_motion.cpp`, `TemporalUpscaler`).
- The game runs at 90–120 FPS (45–60 per eye, under the movement limit). The other eye is the last frame reprojected by the runtime using depth.
- **Exit:** comfortable 3D in light scenes. Known artifacts: some ghosting, and eye swap when FPS dips. This is the R.E.A.L.-mod quality bar, already a release candidate.

### Phase 4: Renderer-side synchronous stereo, the "Vulkan geo-11" (6–10 weeks). The quality target.
- **D-dup first:**
  - Scene-classified draws are recorded a second time into eye-R proxy targets.
  - The classifier reuses the TAA jitter predicate (`vk_rasterizer.cpp:1100-1107`: depth == scene depth, more than 6 indices or instanced).
  - Per-eye targets come from `SceneTargets` proxies (`vk_scene_resolution.h:18-60`) at headset resolution.
  - Per-eye copies of the 864-byte constants for full-screen and compute passes (SSAO, fog, lighting) come through `vk_constant_ring.h`.
  - **Eye 2 must never write guest memory:** no labels/EOP, no readbacks, no compute writes to guest-visible buffers.
- Then **D-mv (multiview):** 2-layer targets with `viewMask=0b11`, and the recompiler selects layers for sampled scene targets. This is what RPCS3-VR vr8 does, and it costs the least at runtime.
- Special cases:
  - **Shadow maps** are rendered once (mono).
  - The **sky** gets rotation-only `M_eye` (deadinside28 issue #68: the sky uses a clamped copy of the view).
  - **Particles** facing the mono camera are acceptable.
- **Push-constant budget:** `PushData` is exactly 128 bytes (`shader_recompiler/resource.h:212-238`). Fetch `M_eye` through BDA the way `EmitVertexMotion` does (`emit_spirv_special.cpp:69-147`), or pack an eye bit.
- **Pipeline cache:** the stereo epilogue must be part of the shader cache key.
- **Exit:** both eyes from the same simulation tick, correct stereo on all scene geometry, ≥72 Hz in average scenes on an RTX 4080-class GPU.

### Phase 5: HUD and UI (2–3 weeks; can run alongside Phase 3 or 4)
- Use the TAA/DLSS/FSR4 path only. FSR3 draws straight into `ui_image`, so it needs an extra copy.
- **Clear `ui_image` to transparent** instead of copying the scene into it (`vk_temporal_upscaler.cpp:1641-1661`, `RunUiOnly` 1461), then submit it as a curved quad layer.
- Verify that Scaleform alpha gives proper coverage. Keep the scene-depth copy into UI depth, because the stencil masks need it.
- **World markers** (lock-on dot, enemy HP bars, damage numbers) are inside the HUD movies at mono screen positions:
  - first, render the HUD at the correct depth
  - later, find Bloodborne's marker-position code (its equivalent of erfps2's `CSFeManImp`) and re-project per eye
- Menus and loading screens go to a world-space flat panel.
- Cutscenes go to theater mode by default. Detect them with the movie VS hashes (`ui_composition.h:27`) and the real-time "remo" camera.

### Phase 6: Comfort, polish, first-person (ongoing)
- Snap or smooth turn, an optional vignette while sprinting or dodging, and a "lock horizon" option for camera shake.
- **Third person is the default** (a floating-head camera behind the hunter), with world scale tuned.
- **First-person mode (optional):**
  - hide the head with the `EquipParamProtector` `invisibleFlag` bits
  - put the camera on the head bone (as ds3fps/erfps2 do) and turn off the near-camera fade
  - borrow the hooks from the Garden of Eyes mod
- **Foveation on Pimax Crystal and Quest Pro** with quad views (mbucchia Quad-Views-Foveated).
- **Motion controls:** gestures trigger the game's own attack or input events (a swing = R1), never physics.
- **Arms (2026-10-09, `findings/24`):** the sandbox's arm IK is now shared (`src/vr_arm_math.h`); the game hook
  `VR Arms IK (AnimationToModel)` (`BB_VR_ARMS=1`, off by default) poses the player's arms (rest pose or scripted
  grips); the controllers need `bbgpu_vr_hands` (findings/24 §6). Interim: `up=`/`forward=` camera offset.
- **Online:** off by default in VR builds; a modified client on shadNet is a risk.

---

## 4. The reusable "VR layer for all headsets", and future games

`xrbridge` and the adapter interface are written once. Each new game only fills in an adapter:

| Adapter hook | Bloodborne (this plan) | Other shadPS4-based games | Our own engines |
|---|---|---|---|
| `GetCameraSeam()` | `0x01972998` inject/restore | per-game RE, same Sekiro-VR pattern | Unity XR rig (Castle Fight already uses OpenXR 1.18) |
| `Stereo` | recompiler `M_eye` epilogue | **same code, free**: it lives in the renderer, not the game | native |
| `ClassifyDraw()` | jitter predicate + sky/shadow rules | per-game `compatvr.json` (PPSSPP-style) | — |
| `HudLayer()` | `ui_image` | per-game UI shader hashes | — |
| `Input` | DS4 via `runtime_pad.c` | same | — |

**The big reuse:** once Phase 4 works, stereo is a renderer feature. Any PS4 game on a shadPS4 renderer gets roughly 70% of its VR for free; only the camera seam, culling and HUD are per-game.

---

## 5. Risks

1. **GPU command-thread budget.** The port's GPU command thread is already at 100% in heavy scenes, at 57–94 FPS mono. Phase 4 adds a second record per scene draw, not a second PM4 decode, so it's cheaper than double-rendering, but 90 Hz may still need the port's parallel-GPU work to land. **Mitigations:**
   - target 72 Hz on Quest
   - rely on ASW/SSW through the depth layer
   - prefer multiview (D-mv)
2. **Duplicating eye 2 safely.** Compute passes, readbacks and guest-visible writes. The full list is unknown until Phase 4 starts.
3. **CPU culling not located yet.** Without it, the edges of the wider VR view pop in.
4. **Every hook site is inferred.** Phase 0, with the user's own eboot, turns these into facts.
5. **HUD world markers and Scaleform alpha.** Visual testing needed.
6. **The 120 FPS movement limit** blocks alternate-eye rendering above 2×60 Hz. The `0x04D25844` constant is the prime suspect.
7. **Legal.** No game files ship; the user supplies their own dump, as upstream already requires. The fork stays GPL-2+. xrbridge is MIT, so it can link into a GPL-2+ host. **No code from ERVR (GPL-3, unavailable) or UEVR (proprietary).**

## 6. Decisions (user, 2026-10-09)
- **Fork:** https://github.com/laughtt/bloodborne_pc, cloned to `<repo>` (remote `upstream` = Supermedo). Work happens on branch `vr`.
- **Pimax is the first headset.** Consequences:
  - The test runtime is Pimax Play's OpenXR, with SteamVR as a second check.
  - Handle canted views from day one: per-view pose and FOV from `xrLocateViews`, never parallel eyes.
  - The per-eye resolution is large (Crystal ~2880×2880), so foveated quad views (Phase 6) move earlier, right after Phase 4.
  - Wide FOV makes culling widening (Phase 2) mandatory, not optional.
- **First person is in scope for v1.** Phase 6's first-person items move to straight after Phase 3:
  - head-bone camera
  - `EquipParamProtector` head hiding
  - near-fade off
  - lock-on behaviour
  - Garden of Eyes as reference
