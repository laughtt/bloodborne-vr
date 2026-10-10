# 02 — Teardown of existing FromSoftware VR and first-person mods

Research date: 2026-10-09. Track: every VR and first-person mod for FromSoftware games, plus what carries over to Bloodborne (bloodborne_pc: native x86-64 game code with a shadPS4 Vulkan renderer).
Evidence tags: **[SRC]** read in cloned source (path:line given), **[AUTH]** the author's own page or README, **[PRESS]** news coverage, **[INF]** my inference.

Repos cloned read-only (shallow, never built) under `bbvr/repos/`:
`sekiro-vr/src` (junglejuz/Sekiro-VR), `dasaav-erfps2/src`, `dasaav-erfps/src`, `dasaav-ds3fps/src`.
Tried but not public: IlyaMez/EldenringVR (the ERVR Nexus page links it, but GitHub returns 404; the author's public repos are KHARVOX (DOOM 2016 VR) and SpidyVR).

---

## 0. Summary table

| Mod | Game | Author | Source | Runtime | Stereo method | Head/camera | Motion controls |
|---|---|---|---|---|---|---|---|
| **R.E.A.L. VR** | Elden Ring, Dark Souls Remastered, Dark Souls II (+ ~30 non-FromSoft) | Luke Ross | Closed. Patreon, framework free since 2025 | **OpenVR** (ships `openvr_api.dll`; injects through `dxgi.dll` + `RealVR64.dll`) | **AER** (alternate eye), AER v2 since Mar 2023, "full-framerate AER" profile Jul 2025 | Camera hook per game, 6DoF, 1st/3rd/"Closer"/"R.E.A.L. Close" | None (gamepad or KB+M only) |
| **ERVR** (Ilya's Elden Ring VR) v0.4.0 | Elden Ring 2.6.2 / 2.7.1 | IlyaMez | Says "open source, GPL-3.0+", repo not reachable | **OpenXR** (own session, as a ReShade 6.8 add-on, dinput8 proxy) | **True double render**: the second eye is "a second engine pass with simulation time frozen" | First person on the head bone, 6DoF, head hiding, reduced bob | **Yes.** Physical swings trigger the game's attack event, two-handing by grip, posture-based shield |
| **SKVR** Sekiro-VR v0.2.1 / 0.3.0-pre | Sekiro 1.6.0.0 | junglejuz (JJ) | Public, no license file | **OpenXR** (VDXR / Link / SteamVR) | **True stereo through geo-11** (3Dmigoto driver-style stereo: every draw is issued per eye with a clip-space shift) | 3rd person kept, head pose injected into ChrCam, 6DoF | No (gamepad) |
| DS3VR | Dark Souls III | junglejuz | Private (named in the SKVR source as `ds_hud.cpp`, `geo11conv.cpp`) | OpenXR | geo-11 | same as SKVR | No |
| AKVR | Batman Arkham Knight (UE3) | junglejuz | Public | OpenXR | geo-11 | — | — |
| **First Person Souls** ds3fps | DS3 | Dasaav | MIT | flat | — | 1st person + head-bone tracking | — |
| **erfps / erfps2** | Elden Ring 1.17.1 (2.7.1.0) | Dasaav | MIT / Apache-2.0 (erfps2) | flat | — | 1st person, head tracking, stabilizer, barrel-distortion shader | — |
| Garden of Eyes FP | Bloodborne 1.09 (PS4 eboot patch, Nexus #11) | Garden of Eyes, with help from Zullie | Patch bytes | flat | — | 1st person, "Great One Hat" trigger | — |
| BB FP for shadPS4 | Bloodborne (Nexus #287) | ? | patch XML | flat | — | see sibling 01 | — |
| RPCS3 VR | **Demon's Souls** BLUS30443 (and others) | hookmanuk | GPL-2.0 fork | OpenXR | Emulator stereo (multiview since vr8) | head tracking | — |
| DSR first-person | DS Remastered (Nexus #1110) | ? | file replacement | flat | — | camera inside head, meshes hidden | — |
| DS2 / DS3 Cheat Engine FP | DS2/DS3 | various | CT script | flat | — | freezes the AimCam zoom (fake first person) | — |
| vorpX | (generic) | Animation Labs | closed, commercial | own | Geometry 3D / Z3D / DirectVR | — | — |

No Armored Core 6 VR mod exists (only flat mods: performance, co-op). No Demon's Souls Remake VR. For Sekiro there is also a flat Nexus first-person mod (GameStar coverage), with floating hands and visual glitches **[PRESS]**. The GitHub repos reylsyssup/EldenRingVR, JosepLlimona/SekiroVR (Unity) and atg99/EldenRingVRProject (Unreal) are fan **recreations** and not mods; WetDog53/elden-ring-vr is a 7 KB design scaffold. All are irrelevant.

---

## 1. Luke Ross — R.E.A.L. VR (Elden Ring, Dark Souls Remastered, Dark Souls II)

- **URLs:** patreon.com/realvr (posts such as "Optimized AER v2 with support for Elden Ring", id 77878465); Helix guide https://helixmod.blogspot.com/2024/10/elden-ring-realvr-vrto3d.html ; coverage at mixed-news.com and uploadvr.com.
- **Status:** Elden Ring support has been WIP since May 2022. DS Remastered was released Jun 2022. DS II is listed under "AER v2 only". The framework went free in 2025 **[PRESS]**.
- **Runtime:** OpenVR/SteamVR. The ERVR install notes tell users to delete `dxgi.dll, RealVR64.dll, openvr_api.dll, cudart64_12.dll, RealVR.ini` **[AUTH, ERVR page]**. It hooks DXGI and has its own CUDA dependency (the optical-flow estimator and DLSS work).
- **Rendering: Alternate Eye Rendering.** Each engine frame is rendered for one eye only. The camera is shifted half the IPD left, then right, on alternate frames, and each eye is shown at half the game rate. **AER v2** (Mar 2023) reduces ghosting, probably by reprojecting the stale eye; **v19.0.0** adds an optional, heavier "optical flow estimator" for AER v2 **[PRESS]**. "Full-framerate AER" (Jul 2025 Elden Ring profile) needs a steady ~120 fps or the eyes "flip" **[AUTH via Helix]**.
- **TAA:** Ross's own explanation is that TAA/DLSS "need precise motion vectors", and the engine's motion vectors do not include the per-eye horizontal shift. Disocclusions differ between the eyes, so history matching fails and you get "shimmering halos". This is why DLSS needs an engine-level fix. For Elden Ring, DLSS works only with a third-party DLSS mod plus his "DL(S)SS" patch **[AUTH, Patreon excerpt]**.
- **Post effects:** the community setup guide says motion blur off, DoF off, RT off, windowed, render target 3920x3920 (1:1). The 2D game window is forced to 1:1 while the headset gets 16:9 **[AUTH/community]**.
- **Camera:** first person follows the character's head through rolls and scripted animations "keeping the horizon level". Modes are 3rd person / Closer / R.E.A.L. Close / 1st person. "Tourist mode" makes NPCs ignore you. A VR quick menu pauses the game (Elden Ring cannot pause natively) **[PRESS]**.
- **Input:** gamepad only. Ross considers motion controllers unnecessary **[PRESS]**.
- **Known complaints:** armor clips into the view; camera vs attack direction disconnect in first person; disorientation from rolls; ghosting and eye-flip when fps is unstable; a 2080 Ti can't hold 120 **[PRESS / Helix comments]**.
- **Hook names:** not published (closed source).

## 2. ERVR — Ilya's Elden Ring VR (Nexus eldenring/mods/10711)

Source of everything below: the Nexus description fetched through a text proxy **[AUTH]**. Code is not reachable.
- **Architecture:** a ReShade 6.8.0 add-on (`dxgi.dll`, unmodified ReShade) loaded by a `dinput8.dll` proxy. It creates its **own OpenXR session** (Meta Link, VDXR or SteamVR). Offline only, EAC off. Verified address maps exist for exe 2.6.2.0 and 2.7.1.0.
- **Stereo:** "Full stereo renders a separate image per eye; the second eye is rendered in a **second engine pass with simulation time frozen**" (my summary). This is the **synchronized-sequential** technique (the UEVR term): both eyes show the same simulation instant, so there is none of AER's ghosting or TAA confusion between eyes. The cost is about 2x the render.
- **Fallback "cinema mode":** menus, cutscenes, loading screens and *unsafe transitions* switch to a head-locked flat screen that renders once.
- **Camera:** placed on the character's head, 6DoF, with headset rotation on top. Optional head hiding and an optional reduced-bob view.
- **HUD:** rendered at headset resolution onto a **separate floating VR panel** (lock = body/head/world, with distance/width/height/opacity settings). The desktop mirror loses the HUD unless the MirrorPanel option is on.
- **Motion-controlled combat (the most advanced in any FromSoft mod):** Touch inputs are mapped to XInput. A physical swing **fires the game's own attack event**, so damage, stamina and status rules all stay intact. Swing speed scales damage and poise, and weapon weight and two-hand torque are simulated. You two-hand by gripping near the hilt. Shields guard by posture. Ashes of War replay recorded effects. Spells and bows aim along the hand. Includes haptics. Movement direction can follow the head, hand, camera or body, with smooth or snap turn.
- **Known limitations (author):** about 2x GPU and slowdowns when the frame target is missed. Physical melee plays no player attack animation, so AI that reads animations reacts oddly, and you can roll mid-swing. Some Ashes and charged follow-ups are incomplete. There is no in-headset menu (settings live in ERVR.ini). Tested mainly on Quest 3.

## 3. SKVR — Sekiro VR (junglejuz/Sekiro-VR) — **deepest open-source reference** [SRC]

Repo: https://github.com/junglejuz/Sekiro-VR (pushed 2026-10-04, C++, no LICENSE file, so treat it as all-rights-reserved for code reuse and reuse the *techniques*). It ships as `dinput8.dll` + geo-11 0.7.11 + the community "Sekiro_geo11_fix" by masterotaku. The same author has a private DS3VR and a public Batman-Arkham-Knight-VR built on the same pattern.

### 3.1 Architecture (`SEKIRO_PLAN.md` §1–2, `src/hooks.cpp`, `src/xr.cpp`)
- `game → geo-11 (draws BOTH eyes each frame into a side-by-side "Katanga" shared surface) → SKVR Present hook → OpenXR swapchains`. The SKVR camera hook only puts the **head pose** into the game camera, and geo-11 then offsets each eye in the vertex shaders (`x += S·(w − C)`, xr.cpp:836 comment; SEKIRO_PLAN line ~321 shows it equals `P_eye·T_eye·P_c⁻¹`).
- Bootstrap: DllMain patches the exe IAT slots for `CreateDXGIFactory`/`D3D11CreateDevice` so geo-11 loads before the device exists (the SteamStub entry point can't be hooked). Code is scanned only after the first Present because SteamStub decrypts `.text` at runtime (SEKIRO_PLAN §1–2).
- Pitfalls hit: ReShade installed as an implicit OpenXR API layer broke `xrCreateSession`, fixed with `DISABLE_XR_APILAYER_reshade_1=1`. A 10-bit backbuffer (R10G10B10A2) needed a shader blit into 8-bit sRGB eye swapchains (`src/sk_blit.cpp`). FromSoft mutes when unfocused, so a focus-keeper WndProc is installed (hooks.cpp:1874–1921).

### 3.2 Camera seam — names and layout (`src/camera.h:1-82`, `src/sk_camera.cpp`)
- RTTI classes: `SprjCam → SprjPersCam → ChrCam` (director), `ChrExFollowCam` (chase), `ChrAimCam`, `FallDieCam`, `BallistaAimCam`, `DbgCamera`, `SprjDebugCam`, `CameraMan`; render side `GameRend::GameRendCameraSet`, `SprjCameraImp`, `SprjCameraStep` (SEKIRO_PLAN §3).
- **`SprjPersCam` layout:** `+0x10/+0x20/+0x30` rotation rows (right/up/forward, with +0x30 the view axis), `+0x40` position, `+0x50` FOV (radians, **vertical, referenced to 16:9**), `+0x54` aspect (1.7778), `+0x58` near (0.08), `+0x5C` far (10000) (measured, SEKIRO_PLAN run 12). FromSoft/Havok world units are metres, +Y up.
- **The seam:** the ChrCam director (fn 0x140734790) ends by copying the *active* camera (`[ChrCam+0x78]`) into its own +0x10..+0x5C. SKVR hooks the **next instruction after the copy** (0x140735355, a `call`, through a MASM stub `sk_stub.asm`) and composes the head pose onto the pristine copy. It also **hooks the director entry to restore the game's values**, so game logic never reads the head-modified camera back (sk_camera.cpp:1–30; "playbook A2.3 don't read your own output back"). The heading follows the game camera's yaw, pitch is kept (`pitchkeep`, default level horizon), and the headset owns pitch and roll.
- **FOV law** (run 13): the game's +0x50 is a vertical FOV for a 16:9 frame, so `fov = 2·atan(tan(hmd_halfH)/(16/9))` for any render aspect. Writing +0x54 has no effect on the picture.
- **Culling camera** (sk_camera.cpp:113–135, 490–520): fns 0x140F00DF0 / 0x140F00E80 build a frustum from +0x50/+0x54 for **animation culling and visibility**. With the tall VR frustum, objects at the top and bottom animated choppily, so both builders are hooked and given the true render FOV and aspect. Found by the AOB `F3 0F 10 5B 54 F3 0F 10 53 50 BA 01 00 00 00`.
- Aspect: the game carves a 16:9 rect (fn 0x14012ABD0, `+0x218` aspect lock). This is patched out (Lyall's je→jmp), and the windowed resolution list is patched to offer 2560x2184 (SEKIRO_PLAN runs 8–10).
- Cutscenes: untested (HANDOVER_2026-10-01:422). A heartbeat of director Hz is logged to detect when ChrCam stops being the rendered camera.

### 3.3 Frame pacing (`src/xr.cpp`, `src/hooks.cpp`)
- **Pose delay = 3 Presents** (xr.cpp:189–199, 2019). They measured that the camera update and Present run on the same thread, one update per Present: the camera consumes the pose published one Present earlier, and the image is shown two Presents after its camera update. The submitted `XrCompositionLayerProjection` pose must be the one the frame was rendered with. If they differ you get jitter ("POSETRACE", SEKIRO_PLAN run 6).
- FPS cap: an immediate `mov [rbx+18],3C888889` (1/60) at 0x1411AB54D is rewritten to 1/cap live, and the sprint speed table is re-pointed (uberhalit's method, hooks.cpp:736, 1289–1333). The headset refresh is switched to a multiple of the cap via `XR_FB_display_refresh_rate`. In practice 45 fps on a 90 Hz Quest with SSW.

### 3.4 HUD (`src/sk_hudsplit.cpp`, `src/sk_hudlayer.cpp`, `xr.cpp:893,1409-1540`) — about 100 of 160 runs went into HUD
- Sekiro's HUD is Scaleform (`01_000_fe.gfx` etc.), drawn into its own transparent screen target. They tried three approaches in turn: (1) per-shader convergence in geo-11 to place the HUD at infinity or a fixed depth; (2) live HUD scale and depth through ini constants; (3) final: **redirect the bottom-HUD Scaleform draws into SKVR's own texture and submit it as a head-locked projection layer in VIEW space** (`XR_REFERENCE_SPACE_TYPE_VIEW`, xr.cpp:475). The reason: HUD baked into a 3-frame-old image gets reprojected by the compositor and "exaggerates micro head movements".
- **World markers** (lock-on dot, enemy health bars, grapple points) stay in the world picture. Problems: they trail by one frame because they are positioned with the previous camera, and the game clamps them to the middle 80% of a 16:9 screen, so they detach near the view edges (known issue in the README).
- Full-screen effects (hurt and crouch vignettes, death screen) were replaced by the mod's own quads (`HURTLAYER`), and menus go to a floating screen. The "binocular panel fit" rule says HUD corners must sit within about ±35° so both eyes see them.

### 3.5 Post / temporal
- README tells users: **motion blur off, DoF off**, AA on (Sekiro's AA is not TAA-heavy). Because both eyes render each frame from the same simulation state, there are **no AER-style temporal artifacts**. geo-11 duplicates every draw, so screen-space passes (SSAO, volumetrics) depend on the community fix's per-shader corrections (`[TextureOverrideAllNonSquareRT] StereoMode=1`; square RTs are treated as mono, which lost depth on square renders).

## 4. Dasaav — First Person Souls: ds3fps (DS3), erfps / erfps2 (Elden Ring) [SRC]

These are the mods VR authors build first-person modes on.
- **ds3fps** (https://github.com/Dasaav-dsv/ds3fps, MIT, C++ + asmjit + `mem` pattern scanner, `dinput8` proxy). AOB-scanned singletons: `GameDataMan, WorldChrMan, FieldArea, LockTgtMan, SprjFlipper, SprjRemo, SprjEventFlagMan, UserInputManager, FrpgHavokMan, SoloParamRepository, NewMenuSystem, MsgRepository, CSPcKeyConfig` (`src/AOB.cpp:82–98`). Camera control works through **~20 mid-function patches on the instructions that store camera matrix rows** (`CamCodeAOB01..21`, AOB.cpp:122–135, e.g. `66 0F 7F 4F 30` = `movdqa [rdi+30],xmm1`, a store to the forward row, and `0F 29 46 40` = a store to position +0x40, the same PersCam layout as Sekiro). Additional hooks: lock-on code, strafe checks, TAE (animation-event) fetch, head-size scaling (`HeadSizeCodeAOB`, which shrinks the head bone to hide it), fall-death camera, aim fade, Havok run state, and menu-value injection for settings in the game's own menu.
- **erfps2** (https://github.com/Dasaav-dsv/erfps2, MIT/Apache-2.0, **Rust**, uses `fromsoftware-rs` bindings and `me3` loader). Hook list in `src/hooks.rs` and RVAs in `src/rva.rs`:
  - `CAMERA_STEP_UPDATE_RVA` → `update_camera`: in first person it **replaces** the camera step by writing `CSCam.pers_cam_1.matrix` and `ChrCam.pers_cam.matrix` directly (core.rs:339–363).
  - `MMS_UPDATE_CHR_CAM_RVA` (MoveMapStep update) → per-frame logic, then `update_chr_cam` (core.rs:365–403): hides the face model, hides sheathed weapons and makes the player transparent during dodges.
  - `UPDATE_FOLLOW_CAM_RVA` / `FOLLOW_CAM_FOLLOW_RVA`: sets `ChrExFollowCam.reset_camera` to kill attach-point interpolation and zeroes `locked_on_cam_offset`.
  - `UPDATE_LOCK_TGT_RVA`: forces lock-on state so strafing follows the camera. Soft lock-on.
  - `UPDATE_FE_MAN_RVA` (`CSFeManImp`): **re-projects HUD world markers** (`lock_on_pos`, enemy and friendly tag `screen_pos`) through the custom distortion (hooks.rs `update_fe_man`). This is the exact place to fix VR marker placement.
  - `SET_WWISE_LISTENER_RVA`: moves the audio listener to the head. `CHR_ROOT_MOTION_RVA`: makes root motion directional. `POSTURE_CONTROL_RIGHT_RVA`: hand posture.
  - `USES_DITHERING_RVA`: disables the near-camera dither-fade of character meshes (shaders.rs:87–97). `ADD_PIXEL_SHADER_RVA`: swaps in a custom `ToneMap_PostOETFPS` shader (barrel distortion, crosshair, and the CA/vignette params visible in the ToneMap cbuffer, shaders.rs:28–60). `CB_FISHEYE_HOOK_RVA`, `GX_FFX_DRAW_*`: VFX fade near the camera.
  - Head tracking (`core/head_tracker.rs`, `core/stabilizer.rs`): the camera sits at the **head dummy-poly** matrix (`player.head_matrix()`) with a positional stabilizer window and eased rotation, and the horizon is damped by head pitch and roll (core.rs:305–337). Model masks are toggled to hide ears and hood (player.rs:147–154).
- **Garden of Eyes Bloodborne FP** (2021) was based on Zullie's DS3 first-person work: same camera-offset approach, PS4 eboot patch, Great One Hat trigger, weapon-handling animation tweaks.

## 5. Other relevant projects

- **RPCS3 VR (hookmanuk/rpcs3)** has **Demon's Souls BLUS30443** in VR at headset refresh. Its VR patches are directly analogous to what Bloodborne needs: *Disable Motion Blur* ("it followed the game camera, not your head"), *Wider view (VR culling)* at 130° (43 = flat), DoF off ("blurred the floor and the edges"), *Unlocked frame rate* tying game time to the real clock, and *Smooth cutscenes* (interpolates the cutscene camera and object tracks, which are stepped at 30 Hz). Pre-rendered videos go on a fixed screen. vr8 renders both eyes in one pass (multiview) (release notes). Sibling 01 covers it in more depth.
- **Helix / 3Dmigoto / geo-11 fixes** exist for DS3, Sekiro and Elden Ring. VRto3D (oneup03, an OpenVR driver) is used with RealVR for 3D monitors.
- **vorpX:** Geometry 3D (double render at shader level), Z3D (depth reprojection) and DirectVR (memory-hooked camera). It has had DS2-era profiles. Low quality for Souls games compared with the above **[PRESS]**.

---

## 6. Cross-cutting: how each problem was solved

| Problem | R.E.A.L. | ERVR | SKVR | erfps2 / ds3fps | RPCS3-DeS |
|---|---|---|---|---|---|
| Stereo | AER (alternating) | 2nd engine pass, sim frozen | geo-11 per-draw duplicate | — | emulator multiview |
| Per-eye camera | game camera hook, ±IPD/2 | engine camera | vertex-shader `x += S(w−C)`, head pose in ChrCam | write `pers_cam.matrix` | VS/CB |
| Projection/FOV | set to HMD | ? | FOV law vertical@16:9 + culling hooks + aspect-lock patch | FOV + barrel shader | culling patch 130° |
| HUD | ? | separate OpenXR panel | Scaleform redirect → head-locked VIEW layer; markers in world | FeMan marker re-projection | flat HUD at depth (HUD Depth setting) |
| TAA/temporal | breaks; AER v2 + optical flow | n/a (same instant) | n/a | n/a | — |
| Motion blur / DoF | user turns off | ? | user turns off | — | patched off |
| Frame pacing | needs steady fps, half-rate fallback | slows if target missed | 45 fps + SSW, pose delay 3, fps-cap patch, FB refresh ext | — | VR frame rate, game time to real clock |
| Body/head hiding | clipping complaints | head-hide option | n/a (3rd person) | face model off, ear/hood masks, head-size, dither off | — |
| Lock-on | game | stick click | markers trail 1 frame | forced soft lock-on, `locked_on_cam_offset=0` | — |
| Cutscenes | follows camera | cinema screen | untested; heartbeat | — | interpolated 30→rate |
| Input | gamepad | motion controls → XInput + attack events | gamepad | — | gamepad |
| 6DoF | yes | yes | yes | — | yes |

---

## Reusable strategy for Bloodborne (ranked)

1. **Synchronized sequential stereo, not AER (ERVR's approach).** Run the game simulation once per frame, then render twice with simulation frozen. In bloodborne_pc this maps naturally to *replaying the frame's render submission* with a per-eye camera, since we own both the x86 game code and the shadPS4 Vulkan backend. Reasons: AER breaks Bloodborne's TAA-like temporal passes and needs a high, steady fps (Ross's own diagnosis). SKVR and ERVR show that true per-frame stereo removes all eye-ghosting and lets the compositor reproject properly. The cheaper alternative is the SKVR/geo-11 trick done in our shader recompiler: emit each world draw twice, or use multiview, with the clip-space shift `x' = x + S·(w − C)` per eye. RPCS3-DeS went to multiview in vr8. Fall back to a head-locked "cinema screen" for menus, loading and pre-rendered cutscenes (both ERVR and RPCS3 do this).

2. **Inject the head pose at the "director copies active camera" seam, and restore before the next game update.** Bloodborne sits between DS2 and DS3 and almost certainly has the same `ChrCam / ChrExFollowCam / ChrAimCam / *PersCam` family. Look for a PersCam-like block with rows at +0x10/+0x20/+0x30, position at +0x40, and FOV/aspect/near/far at +0x50..+0x5C (DS3 ds3fps AOBs store to +0x30/+0x40; Sekiro is measured). Kyo's camera cluster at eboot 0x0183A000–0x01841100 (siblings 01/05) is where to start. Two rules from SKVR: (a) write the head-composed matrix **after** the game's final copy into the render camera; (b) **put back the game's own values at the director entry** so movement and lock-on logic never read head yaw. Yaw follows the game camera, and the headset owns pitch and roll. Also patch out "camera auto-rotate" (Kyo's NOPs) for comfort.

3. **FOV/projection and culling together.** Replace the projection with the asymmetric OpenXR frustum. In SKVR the game's FOV field was a vertical FOV referenced to 16:9, and its aspect field fed only culling. Without widening the **culling/animation-LOD frustum** builders (SKVR hooks two, AOB in §3.2; RPCS3 "Wider view (VR culling)" 130°), objects pop in at the edges and animate choppily. Remove any 16:9 aspect lock so the render target can match the headset (about 1:1.1).

4. **Pose bookkeeping.** Tag every pose with an id. Submit to `xrEndFrame` the exact pose the frame's camera used, then measure the pipeline depth instead of guessing (SKVR measured 3 Presents). Use `XR_FB_display_refresh_rate` to pick a refresh that is a whole multiple of the game rate. Make game time follow the real clock at 60/72/90 using Lance's/Kyo's FPS patches plus the speed-table fixes, as SKVR does for the sprint table.

5. **HUD on its own OpenXR layer from day one.** SKVR spent about 100 builds learning that a baked HUD wobbles under reprojection. Bloodborne's HUD is also Scaleform-era FromSoft menus (sibling 05 §5). In the shadPS4 renderer, classify the HUD/menu draws (ortho projection or the HUD render target) and render them into a separate texture. Submit it as a quad or VIEW-space layer (ERVR has body/head/world lock options). Re-project **world markers** (lock-on dot, boss and enemy bars) from the *current* per-eye camera, the way erfps2 rewrites `CSFeManImp.lock_on_pos` / tag `screen_pos`. Widen the game's on-screen clamp, which in Sekiro was the middle 80% of 16:9. Replace full-screen vignettes and damage overlays with mod-drawn layers.

6. **Kill or fix the effects that follow the game camera.** Turn off motion blur and DoF; RPCS3 patched both out for Demon's Souls. Disable chromatic aberration, vignette and camera shake (RPCS3 "Disable camera shake (VR)"). For TAA in true stereo, keep **separate history buffers per eye** and give each eye its own previous view-projection, since the port already computes camera motion vectors (sibling 05 §6). The alternative is to switch to FXAA/SMAA.

7. **First-person mode as an option, borrowing erfps2/ds3fps mechanics.** Anchor the camera to the head dummy-poly. Hide the face, head and hood with model masks or head-bone scale (the ds3fps HeadSize hook). Disable the near-camera dither fade. Move the audio listener to the head. Zero the lock-on camera offset and force a soft lock-on so strafing follows the view. Smooth the head bone with a stabilizer window. Default to **third-person with 6DoF head look**: SKVR's choice, and the most comfortable for rolls and visceral attacks. Ross's first-person mode drew complaints about rolls and armor clipping. The existing Bloodborne FP patches (Garden of Eyes, Nexus #287) give v1.09 camera offsets to diff against.

8. **Motion controls last, using the ERVR model.** Map Touch to the gamepad first. Later, a physical swing can fire the game's own attack request so the damage, stamina and poise rules stay intact. Accept ERVR's known downside: there is no attack animation, so AI that reads animations behaves differently.

9. **Operational lessons.** Watch for implicit OpenXR API layers (ReShade, OpenXR Toolkit) that break session creation. Handle 10-bit or HDR swapchain formats with a blit. FromSoft games mute or pause when unfocused, so add a focus keeper. Log a camera heartbeat to detect when cutscenes stop using the player camera. Never ship config files that the mod rewrites at runtime (SKVR 0.2.1 lesson).

## Cross-links
- **01-existing-bloodborne-vr.md**: no Bloodborne VR exists. It lists the BB first-person mods (Garden of Eyes #11, shadPS4 FP #287) and the v1.09 camera patch addresses (Kyo's cluster 0x0183A000–0x01841100, auto-rotate NOPs, Lance's debug free-cam), plus AstroQuest (shadPS4 OpenXR host) and RPCS3 VR. These supply the concrete hook targets for strategy items 2 and 7. Its RPCS3-Demon's Souls notes match my §5.
- **04-flat2vr-tech.md**: UEVR/REFramework synchronized-sequential vs AFR, the 3Dmigoto clip-space formula, and Dolphin/PPSSPP projection heuristics. These support strategy items 1 and 5. Its recommendation (native camera hook first, CB/VS patch as fallback) agrees with SKVR and erfps2 practice here.
- **05-bloodborne-engine-modding.md**: confirms DS3/ER class names (`FieldArea → ChrCam → ChrExFollowCam/AimCam/DbgCam`, `*PersCam`) as the BB template, the LockCamParam row 10002 in Kyo's patch, and that the port already keeps previous view/projection for motion vectors. That matters for per-eye TAA history (item 6).
- **03-repo-renderer-internals.md** gives **strong confirmation of strategy item 2**. The bytes of Lance's debug-camera code cave show a Bloodborne camera object with a **4x4 matrix at +0x10 and fov/aspect/near/far at +0x50..+0x5C**, the same `PersCam` layout SKVR measured in Sekiro and ds3fps patches in DS3. So the SKVR "write after the director's copy, restore at entry" seam pattern should port directly. 03 also warns that GPU-side camera-CB patching alone won't move all geometry ("model constants follow the camera"), which favours the CPU camera hook plus a full second render pass (item 1). Its `ui_image` split in the TemporalUpscaler (Scaleform VS hashes, `RunUiOnly`) is the ready-made way to put the HUD on its own layer (item 5). The existing Halton jitter and motion-vector code are where per-eye TAA history would go (item 6).
