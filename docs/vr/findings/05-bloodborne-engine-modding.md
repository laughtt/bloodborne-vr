# 05: Bloodborne engine internals and the modding scene, for VR

Track: what the Souls modding community knows about the BB engine that matters for a VR mod on the
native PC port (Supermedo/bloodborne_pc, fork of deadinside28/bloodborne_pc, game CUSA03173 v1.09).

Legend: **[V]** verified here (read source, data file or primary page). **[R]** reported by
secondary press or wiki, not checked against code. **[S]** speculation or inference.

Local clone used: `scratchpad/bbvr/repos/supermedo/bloodborne_pc` (default branch `windows`).
Paramdex defs downloaded to `scratchpad/bbvr/tmp05/`.

---

## 1. Engine lineage and tools

- BB (2015) is a FromSoftware in-house engine of the DeS/DS1/DS2 to DS3 line, often called "Dantelion"
  (the FD4 framework, with Havok physics). Its internal project code is **SPRJ**: debug screenshots
  are named `SPRJ-win64`. **[R]** https://wccftech.com/bloodborne-pc-evidence/amp/
  DS3 is built on BB's codebase. Lance McDonald back-ported DS3's PS4 Pro variable-timestep code into BB
  for the 60fps patch "since both games share the same engine". **[R]**
  https://www.thegamer.com/bloodborne-60fps-patch/ , https://stevivor.com/news/bloodborne-60fps-lance-mcdonald-ps5
  Consequence **[S]**: DS3 reverse-engineering knowledge (class layouts and singletons such as
  WorldChrMan, FieldArea, the camera classes, and many `Sprj*` class names) is the best template for BB.
  Offsets differ, and BB is a PS4 ELF, not a PE.
- Asset formats are shared with DS3: DCX compression, BND4 archives (`*.chrbnd.dcx`, `*.partsbnd.dcx`), FLVER2
  models, TAE animations, HKX (Havok) skeletons, cloth and ragdoll data, PARAM/PARAMDEF, GPARAM/drawparam
  lighting, MSB maps, EMEVD events, FMG text. The port's mod loader replaces files under
  `dvdroot_ps4/{chr,parts,map,menu,msg,param,sfx,sound,font}`. **[V]** `docs/MODS.md` in the repo.
- Tools that apply:
  - **Smithbox** (successor to DSMapStudio and Yapped) lists Bloodborne support: Param Editor, Map Editor,
    GPARAM editor. **[R]** https://www.nexusmods.com/site/mods/2025 , https://soulsmodding.com/
  - **Paramdex** (soulsmods/Paramdex) ships BB defs. **[V]** I listed `BB/Defs` with the GitHub API. Relevant
    entries: `LockCamParam`, `ThrowParam`, `EquipParamProtector`, `SkeletonParam`, `RagdollParam`, `WindParam`,
    `LodBank`, `MenuPropertyLayoutParam`, `ResidentFxParam`.
    BB has **no** separate `CamParam`, `GraphicsConfig` or `PostEffect` param in Paramdex. Camera
    tuning lives in `LockCamParam`, and post-FX lives in drawparam/GPARAM or code.
  - Other DS3-era tools: Yabber/WitchyBND (BND/DCX unpack), FLVER editors, Soulstruct (EMEVD).
- People and groups: Lance McDonald (manfightdragon: 60fps patch, debug camera, enemy control,
  region unlock), Kyo (most shadPS4 XML patches: FPS++, post-FX toggles, resolution, camera
  distance), illusion (skip intro, CA off), Whitehawkx and auser1337 (debug menu restore), stagvant
  (Enemy Control, BBDBG GoldHEN debug plugin, now archived or gone), Zullie the Witch (DS3 first person,
  helped with the BB first-person camera), Garden of Eyes (BB first-person mod), Dasaav, emoose.
  The port's own Discord is https://discord.gg/yTMG8c4Bqm . **[V]** Patch authors are read from
  `patches/Bloodborne.xml`.

## 2. Community patch catalogue (Bloodborne.xml in the repo)

**[V]** `patches/Bloodborne.xml` holds shadPS4/GoldHEN-format eboot byte patches for v1.09, 3438 lines.
`scripts/patches.py` applies them, and its FPS presets are `{'30':[], '60':['60 FPS++'], '90':['90 FPS++'],
'uncap':['Uncap FPS++']}`. Uncap is the default. Patches that touch the relocation table are
relocated (`docs/MODS.md`). Addresses are eboot-image offsets, so the native port maps them 1:1.

Entries relevant to VR:

| Patch | Author | Addresses | VR relevance |
|---|---|---|---|
| Disable Motion Blur | Kyo | `0x026C2549 = 00` (also disables the velomap render) | **Must for VR** |
| Disable AA | Kyo | `0x026C254A = 00` | Game TAA/AA smears in stereo; disable it and let the port's own AA or upscaler run per eye |
| Disable SSAO | Kyo | `0x026C2548 = 00` | Optional perf |
| Disable Dynamic Light Shadows | Kyo | `0x026C2538 = 00` | Perf (shadows are 4096² D32) |
| Disable DoF | Kyo | `0x025D7BBC` jmp | **Must for VR** |
| Disable Chromatic Aberration | illusion | `0x0269faa8` | **Must for VR** |
| 50% Text scale | Kyo | float at `0x04D2913C` | HUD/menu legibility on a VR quad |
| Uncap FPS++ / 60 / 90 FPS++ | Lance McDonald, Kyo | `0x0243xxxx` frame-pacing block; floats `0x3B888889` (1/240), `0x3C888889` (1/60) at many sites | Variable timestep, see section 4 |
| 60FPS (no deltatime) | Kyo | fixed-step variant | Shows the engine supports both fixed and variable step |
| Restore Debug Camera | Lance McDonald | large code cave at `0x01972998`, `0x01de3328..0x01de368b` | "Hold X + L3 cycles freecam modes". A freecam already exists in code and can serve as a reference for camera write points |
| Restore Debug Menu | Whitehawkx, auser1337 | `0x2199EEE...`, needs debug font files from Nexus | Dev menu: likely has HUD and draw toggles **[S]** |
| Increased camera distance | Kyo | `0x01836940`, `0x05527a94`, and `mov dword [rbx+4]/[rbx+8], 0x2712` at `0x0184107f/0x018410b1` | 0x2712 = 10002 looks like a **LockCamParam row ID** being forced. This would locate the camera-param selection code **[S]** |
| Disable Camera Auto Rotation via Movement | Imedved, Kyo | NOPs at `0x0183c6e8/0x0183c870/0x0183c984/0x0183dde6` | **Must for VR**: stops the game turning the view on its own (comfort). Also shows the player-camera update code sits around `0x0183xxxx` |
| Enemy Control | stagvant | debug "chr switch" | Not needed |
| Skip Intro | illusion | | Avoids the logo FMVs |

The player-camera code cluster around `0x01836940-0x01841100` (auto-rotate, distance, param-ID select)
is the obvious first place for the VR camera override. **[S]** This is inferred from the patch addresses
only, not disassembled.

## 3. Camera

- **LockCamParam (BB paramdef) [V]** has these fields (default in parentheses):
  `camDistTarget` (4 m), `rotRangeMinX` (-40°), `lockRotXShiftRatio` (0.6), `chrOrgOffset_Y` (1.42 m, the
  pivot height in character space), `chrLockRangeMaxRadius` (15), **`camFovY` (43°)**, and dark-area
  variants. Source: https://github.com/soulsmods/Paramdex/blob/master/BB/Defs/LockCamParam.xml
  - Cross-check **[V]**: the port's scene constants (`docs/upscaler.md`) give projection y = 2.53865, so
    tan(fovY/2) = 0.39391 and fovY = 43.0°. This matches `camFovY` = 43. Near = 0.05, far = 3000, D3D depth 0..1.
  - With params alone you can get camDist ≈ 0.1 m (min 0.1) and chrOrgOffset_Y ≈ eye height. That is a
    "pseudo first person" but camera collision, the auto wall recovery and the body mesh remain. The
    params are per area and state (rows are selected by code, see the 10002 note above), so you would edit
    every row. **[S]**
  - The param FOV is irrelevant for VR. We replace the projection matrix per eye (see section 6).
- **First-person mods (existing work) [R]**:
  - DS3 (Zullie the Witch, 2016): a Cheat Engine script that repurposes the **AimCam** (the bow, crossbow
    and binocular aim camera). It exposes offsets (L/R, U/D), centre, lens distance, rotation limits and speed, and
    FOV. It hides the body and moves stowed weapons in front of the camera.
    https://nexusmods.com/darksouls3/mods/20 , https://www.pcgamesn.com/dark-souls-3/mod-first-person
  - BB (Garden of Eyes, with Zullie's help, 2021): an eboot patch plus a "Great One Hat" headpiece that
    switches to first person when worn. It adds disembodied hands and "close-up weapons" and reworked
    weapon-handling animations. Zullie found the camera offsets on PC (DS3). It is compatible with Lance's
    60fps patch and needs a decrypted eboot. It is reported to run on shadPS4 (Nexus 11 for PS4 and Nexus 287 for the
    shadPS4 build). https://www.nexusmods.com/bloodborne/mods/11 (403 to the fetcher),
    https://www.dsogaming.com/mods/you-can-now-play-bloodborne-in-first-person-on-pc-via-shadps4/ ,
    https://www.nme.com/news/this-bloodborne-mod-makes-the-game-first-person-3020190 ,
    https://www.tomsguide.com/news/bloodborne-first-person-mod
    → **Best prior art for VR.** Its byte patches are XML/eboot patches for v1.09, so they likely load in this
    port's patch system as-is. **[S]** Check whether it uses `mask` lines, which the port skips.
    The hat trick shows how head hiding is done: a protector row whose invisible flags hide the head and body parts.
- **Hiding head and body [V]**: `EquipParamProtector` has `invisibleFlag00..invisibleFlagNN` bitfields
  (the DS3-style model mask). These hide FLVER mesh groups of the player model per worn armour piece. A
  "VR head" item, or editing the hat row the player wears, can mask the head, hair and face. Alternatively,
  at render level, skip the player's head draw by bone or mesh at the GPU layer. **[S]**
- **Camera collision and auto-recovery**: handled in code (the auto-rotation NOPs above). The DS-series option
  "Camera Auto Wall Recovery" exists in FromSoft games. **[R]** For BB I could not confirm its exact
  options-menu labels.
- **Free or debug camera [V]**: the Lance McDonald "Restore Debug Camera" patch is in the XML. BBDBG
  (stagvant, GoldHEN) had an overworld debug camera through the dev menu "[SYSTEM] → MoveMapStep". **[R]**
  https://github.com/stagvant/BBDBG (now 404).
- **DS3 and Elden Ring camera structures [S]**: the community uses names such as `FieldArea → ChrCam →
  ChrExFollowCam / AimCam / DbgCam` and an `*PersCam` render camera holding the world matrix plus FOV, aspect, near and far.
  I could not find a public citable page for these names in this pass, so treat them as leads to verify
  in DS3 CE tables (e.g. igromanru Dark-Souls-III-Cheat-Engine-Guide forks) and The Grand Archives ER table.
  Practical alternative already proven in this port: **find the camera per frame from GPU
  constants**. View 3x4 is at floats 8-19, inverse view at 180-191, projection at 52-67, in an 864-byte scene
  constant block with signature `[0]=3000,[1]=1/3000,[4]=1920,[5]=1080` (`docs/upscaler.md`). **[V]**

## 4. Frame timing (alternate-eye or stereo cadence)

- Vanilla BB ties logic to a 30 Hz step. The built-in debug menu "doubles framerate" but also game speed.
  **[R]** https://kotaku.com/bloodborne-modder-will-release-60-fps-hack-if-sony-fail-1845352238
- Lance's patch makes "most gameplay logic run at arbitrary update speeds" using DS3's delta-time
  code. He hand-patched cloth physics, particles and SFX, wind, enemy patrol pathing, motion-blur sampling and
  elevator speed. **[R]** https://stevivor.com/news/bloodborne-60fps-lance-mcdonald-ps5 ,
  https://mp1st.com/news/unofficial-bloodborne-60fps-patch-has-finally-released
- In the XML **[V]**, "Uncap FPS++" rewrites the frame-pacing block at `0x024347F3..0x02434D98` (vsync and frame-skip
  flags) and sets many float timestep constants to 1/240 (`0x3B888889`) and some to 1/60. The likely reading **[S]** is
  that clamps or minimum timesteps limit how small a step subsystems accept. Kyo's note says "a lot of the timestep fixes would vary
  with fluctuating frame cap".
- **The >120 FPS slowdown [V/S]**: the README says "Above about 120 FPS the game's movement slows down (a limit
  of the game itself)". ROADMAP says "Vblank 480 Hz + frame limit min(monitor,120)". **[S]** The cause is
  probably a min-dt clamp or fixed sub-steps in movement or Havok character control, where frames shorter than the
  clamp lose time. Not verified in disassembly.
- **VR implications [S]**:
  - Do **not** run the game loop once per eye at 2x rate. That would need ≥180-240 sim Hz, above the 120 limit.
  - Preferred: **one simulation step, two eye renders**. Either (a) re-submit the frame's GPU command
    buffers twice with patched view/projection constants (renderer-level stereo, only possible because
    shadPS4 sees whole frames), or (b) alternate-eye rendering at 90/120 Hz with each eye at 45/60 Hz
    and reprojection. The game's own sim and render are one thread pipeline, and no community patch
    decouples them.
  - Keep the sim at 90 or 120 (≤120 is safe per the README), matching headset refresh. 72/80/90 should all
  behave since the timestep is variable.

## 5. HUD and menus

- **[V]** The port's docs say the UI is drawn by **Scaleform** ("tonemapped scene proxies at the first
  Scaleform pass, then draw HUD/menu geometry into the output-size target"). The frame order is tonemap → game AA
  → **UI: stencil-masked draws over the same RGBA8 1920x1080 target**. The first pass of each frame copies
  the previous UI target to the display buffer, and movie (FMV) shaders are detected separately
  (`docs/upscaler.md`).
  → For VR, redirect the Scaleform passes to a separate transparent render target, then show it as an
  OpenXR quad or cylinder layer. The port already has the hook point, because it separates HUD from scene for upscaling.
- Menu layout data: `menu/` (Scaleform .gfx in BNDs) plus `MenuPropertyLayoutParam` and `MenuPropertySpecParam`. Text scale
  patch is at `0x04D2913C`. **[V]** param list and XML.
- HUD toggle: I could not confirm an in-game HUD-off option for BB (DS3 and ER have one). The debug menu
  likely has draw toggles. **[S]** For VR it is easier to capture HUD passes at the GPU layer anyway.
- Lock-on reticle and enemy HP bars are screen-space and placed with game-projected coordinates. The resolution patches
  had to fix "lock-on/enemy/ally hp bar coordinates". **[V]** (patch notes) In stereo these will sit
  at the wrong depth. Options: render them on the HUD layer, or reproject them from the world position. **[S]**

## 6. Rendering facts useful for stereo (from the port docs) **[V]**

- G-buffer deferred renderer: 6 MRT plus D32S8, light volumes, RGBA16F scene colour, compute volumetric fog
  (reads linear depth), half-res effects at 960x540, bloom pyramid, tonemap, game AA, then UI. There is no velocity buffer; the port
  computes camera motion vectors itself. It already keeps the previous view and projection and injects jitter through the
  shader recompiler (`docs/upscaler.md`, `docs/motion_vectors.md`).
- Shadows use 4096² cascades with matrices at floats 112-175. They are view-dependent but could be shared by both eyes. **[S]**
- Resolutions are hard-coded at 1920x1080 guest. The port resizes host proxies. Per-eye sizes such as 2064x2208 (Quest 3)
  need either the "Resolution Patch" family (which also fixes HUD coordinates) or the port's proxy scaling. **[S]**

## 7. Cutscenes

- In-engine cutscenes in DS3-era games are "remo" files (`remo/*.remobnd.dcx`) and play in real time with the
  game camera. BB follows the same pattern. **[S]** I could not confirm this from a primary source in this
  pass. Logo and intro movies are FMV. The port identifies "movie shaders" for full-screen movie
  passes. **[V]** Skip Intro patch exists **[V]**.
- VR handling: show FMVs and probably remo cutscenes on a virtual screen (a quad layer), or render
  remo in stereo with head tracking added (the camera is scripted, so head rotation must be layered on top). Cutscene FOV
  differs per shot. **[S]**

## 8. Post FX summary

| Effect | Off switch | Status |
|---|---|---|
| Motion blur (+velomap) | `0x026C2549=00` | **[V]** patch exists, on by default in port |
| Game AA (post-tonemap) | `0x026C254A=00` | **[V]** |
| DoF | `0x025D7BBC` jmp | **[V]** |
| Chromatic aberration | `0x0269faa8` | **[V]** |
| SSAO | `0x026C2548=00` | **[V]** |
| Film grain / vignette / lens flare / bloom strength | GPARAM/drawparam (`param/drawparam`) via Smithbox GPARAM editor | **[S]** not checked |
| Screen-space effects (SSAO, SSR, volumetric fog, half-res FX) | screen-space and per-view | Need per-eye render, or they will mismatch between eyes **[S]** |

The flags at `0x026C2538..0x026C254A` look like one graphics-config constructor with byte flags for shadows,
SSAO, MB and AA. **[S]**

---

## VR-relevant hooks checklist

| VR need | Known BB mechanism | Confidence |
|---|---|---|
| Per-eye view matrix (head pose) | Patch view 3x4 (floats 8-19) and inverse view (180-191) in the 864-byte scene CB, found by signature in the shadPS4 layer | High: layout documented by port **[V]**; stereo use **[S]** |
| Per-eye asymmetric projection | Replace projection (floats 52-67) and inverse (36-51). Game FOV comes from LockCamParam.camFovY (43°) | High **[V]** |
| Camera position at eye (first person) | Garden of Eyes BB first-person patch (eboot, v1.09). Or LockCamParam camDistTarget≈0.1 + chrOrgOffset_Y. Or override in the player-camera code near `0x0183xxxx` | Medium: prior art exists **[R]**, code location **[S]** |
| Stop game turning the camera | "Disable Camera Auto Rotation via Movement" NOPs | High **[V]** |
| Decouple yaw: head vs stick | Override camera rotation in the camera update (same code cluster) | Low-medium **[S]** |
| Hide own head/hat | EquipParamProtector invisibleFlagNN (model mask), "Great One Hat" approach, or GPU-side draw skip | Medium-high **[V]** fields / **[R]** usage |
| Weapon and hands visible in first person | Garden of Eyes reworked animations (TAE) | Medium **[R]** |
| Sim rate = headset Hz (72-120) | Uncap FPS++ (variable dt) + port frame limiter ≤120 | High **[V]** |
| One sim step, two eyes | Renderer-level replay of the frame's command stream with per-eye CB patch. No community engine-side solution | Low (novel) **[S]** |
| Alternate-eye fallback | 120 Hz sim, eyes alternate. Needs reprojection (port already computes camera MVs) | Medium **[S]** |
| HUD on a quad layer | Scaleform UI passes identified by port (stencil-masked after tonemap). Redirect to separate RT | Medium-high **[V]** hook point |
| Hide HUD entirely | Debug menu (restore patch) or skip UI passes on GPU side. No confirmed in-game toggle | Medium **[S]** |
| Lock-on reticle and HP bars in stereo | Screen-space, game-projected. Reproject or put on HUD layer | Low-medium **[S]** |
| Kill motion blur, DoF, CA, AA | Kyo and illusion XML patches | High **[V]** |
| Bloom, grain, vignette, lens | GPARAM via Smithbox | Medium **[S]** |
| Cutscenes / FMV | FMVs detected as "movie shaders" → virtual screen. Skip Intro patch. Remo cutscenes probably real time | Medium **[V]**/**[S]** |
| Free cam for debugging the VR camera | Lance's "Restore Debug Camera" patch (X+L3) | High **[V]** |
| Data and asset modding | Port mod loader (`dvdroot_ps4` overrides). Smithbox/Paramdex BB defs | High **[V]** |

## Cross-links

- **01-existing-bloodborne-vr.md**: confirms no stereo BB VR exists. It points to AstroQuest / Farpoint PC VR (GPL
  shadPS4 forks with an OpenXR host) as the plumbing to reuse, and covers the same Garden of Eyes first-person mod and the
  community camera-patch addresses as section 2 and section 3 here.
- **04-flat2vr-tech.md**: UEVR / REFramework / R.E.A.L. VR techniques (alternate-eye, camera hook) and the
  emulator "replay draws twice" precedent. These map onto the "one sim step, two eyes" row of the checklist and onto the
  scene-constant patching in section 3 and section 6.
- Data handoffs from this file: the scene-constant layout and the Scaleform UI boundary come from
  `repos/supermedo/bloodborne_pc/docs/upscaler.md`. The >120 FPS slowdown and the 1/240 timestep constants come from section 4.
  The patch system is `scripts/patches.py` + `patches/Bloodborne.xml`. Third-party XML loads from the user
  patches dir, except patches with `mask` lines.
