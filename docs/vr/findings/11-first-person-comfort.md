# 11: First person, comfort and performance (design + RE plan)

Date: 2026-10-09. Track: first-person camera for Bloodborne VR on Pimax (Dream Air now, Crystal-class wide FOV later).
PLAN §6 puts first person in v1, straight after Phase 3. This file is research and design only. No other repo file was
changed, and nothing was built or run.

Inputs:
- Prior art read in source: **erfps2** (Elden Ring, Dasaav, MIT/Apache-2.0) and **ds3fps** (DS3, Dasaav, MIT), from the
  earlier research clones (`bbvr/repos/dasaav-*`), plus Sekiro-VR (02 §3).
- The v1.09 eboot image that 08 prepared (`scratchpad/re/`, relocations applied at 0x400000). I ran read-only string, xref
  and disassembly queries on it with my own scripts in `scratchpad/fp11/`.
- The user's dump `C:/games/CUSA03173\dvdroot_ps4`: the player model `chr/c0000.chrbnd.dcx`, the behaviour data in
  `chr/c0000.anibnd.dcx`, `parts/*.partsbnd.dcx` and the param list. I parsed DCX/FLVER directly (`fp11/flver.py`, `mats.py`).
- The repo: `gpu/xr_bridge/xr_input.cpp`, `gpu/bbgpu.h`, `src/runtime_pad.c`, and the session's run logs (runtime
  "Pimax OpenXR 0.1.0", headset "Pimax Dream Air", controllers reported as `oculus/touch_controller`).
- Web pages, listed in §10.

Legend: **[C]** confirmed here from the eboot, the game data or source code. **[L]** likely (strong circumstantial
evidence; the same tag as 08). **[V]** verified on a primary page. **[R]** reported by press or secondary sources.
**[S]** speculation or inference.
Addresses are PS4 VAs (the patch-XML space), the same as 08.

---

## 0. Summary

1. **Every FromSoft first-person mod does the same five things.**
   - (a) Overwrite the camera matrix late in the frame with one anchored at the head.
   - (b) Hide the face, hair and helmet but keep their shadow.
   - (c) Switch off the character fade near the camera.
   - (d) Tame lock-on: no camera yanking, and strafing relative to the view.
   - (e) Decide per animation whether the camera follows the head.

   VR adds one rule that overrides all the flat mods: **the headset owns rotation; animation may only move the eye's
   position.** Luke Ross says the same: "the horizon will stay level".
2. **08's render-side hooks are also the right place for the first-person camera.**
   - H1 is at 0x22de113 and H2 at 0x22de386. They replace position and rotation in the draw copies.
   - The game keeps its own third-person camera, and culling follows the new camera automatically (08 §6).
   - The game camera gets one deliberate input: the follow-cam yaw, ChrExFollowCam+0x144 (08 [L]).
3. **Everything the eye anchor needs is in the user's dump [C].**
   - The Head bone is index 68 of `c0000.flver` (parent: Neck, 67).
   - Dummy poly **58 "Dummy_eye"** has two instances at ±2.5 cm, on the Head bone, at eye height 1.654 m.
   - Dummy 255 "Dummy_voice" marks the mouth.
   - The hair, face and FaceGen meshes carry model-mask tags (#19#…#61#). Hats carry none.
4. **Eboot leads [C].**
   - The WorldChrMan singleton is the global at **0x593E878**.
   - Head and neck bone indices are set up in **0x01933210** (Neck index at +0x10, Head index at +0x14, block stored at
     owner+0x1c8).
   - The step functions for camera, remo, HUD and sound are known (§2.1).
   - Game classes have no RTTI; only FMOD's do.
5. **A playable first person needs only two small RE tasks beyond 08:** the player pointer and position, and confirming the
   yaw field. That gives a body-anchored camera, with a GPU near-eye clip to hide the head. A head-anchored camera with
   shadow-keeping head hiding comes second.
6. **Comfort defaults for seated play on Pimax.**
   - Snap turn of 30°.
   - Movement relative to the head.
   - A vignette driven by speed.
   - A stabilized head anchor with a floor during dodges.
   - Rotation never driven by animation.
   - Automatic switch to the cinema screen or third person for cutscenes, grabs and death.
7. **Motion gestures still work but feel late in first person [S].** The game's attack animation starts after the physical
   swing ends. "Weapon follows hand" is a 4–8 week stretch goal; the gun is the best first target.
8. **Performance.** The Dream Air needs 27 MP per frame at 90 Hz, 13× a 1080p frame. The levers, in order:
   1. Fix the game's LOD and culling (08 H3, ctx+0x150).
   2. Run the upscaler at about 0.6–0.7 scale per axis.
   3. Add quad views (`XR_VARJO_quad_views`) with eye tracking through Pimax OpenXR, as 4-layer multiview. This needs
      Phase 4 D-mv.

## 1. What other FromSoftware first-person and VR mods hook, and the Bloodborne equivalent

| Concern | Elden Ring: erfps2 **[C src]** | DS3: ds3fps **[C src]** / Zullie CE script **[R]** | VR mods | Bloodborne lead |
|---|---|---|---|---|
| Camera write point | Replaces `CAMERA_STEP_UPDATE`, writing `CSCam.pers_cam_1` + `ChrCam.pers_cam`. Also writes in the MoveMapStep `update_chr_cam` | ~30 mid-function patches on the instructions that store camera rows (`CamCodeAOB01..30`). Writes the Debug, Base, Follow and Aim cam matrices. Zullie repurposed the **AimCam** | SKVR: after the director's copy, restored at entry. R.E.A.L. and ERVR are closed | 08 H1/H2 (draw copies, no restore needed). Game-side alternative G1 0x1d3bec5. `SprjCameraStep::STEP_Update` 0x1e09cd0 picks the active PersCam |
| Eye anchor | `GET_DMY_POS(chr, &m, dmy 907, 1)`, the head dummy poly's world matrix (`player.rs` `head_matrix`) | Ragdoll body 14 (head) read from the Havok world (`FrpgHavokMan→hknpWorld`, `Pointer.cpp`) | R.E.A.L.: position follows the head, orientation follows the real head **[R]** | **dmy 58** on bone 68 (§2.2). The dmy-poly query function is not located yet (§7 #3) |
| Smoothing | Stabilizer: averages head position in model space over **0.3 s** and clamps (factor 0.8 → max 8 cm). Rotation eased only while tracking (`stabilizer.rs`, `head_tracker.rs`) | None. Head tracking per TAE event or animation ID | — | The same, time-based (§3.2) |
| When the camera follows head *rotation* | Only in throws, plus optional damage and dodges (`track_dodges=false` default) | TAE rules: rolls optional (event 307, arg 504); light hurt anims 5000–5500 and 10000–15000 off; ladders yaw-locked; interact/throw anims 68000–80000 on | Never, in R.E.A.L. | **Never in VR** (§3.7) |
| Head hiding | Per part, `model_disp_entity.disp_flags1` bit 0 off for the face, helmet and hair parts. `0x100000A1→0x100000A0` keeps the shadow. Ear and hood mask bits at `chr_asm_model_res+0x50..0x60` | `HeadSizeCode` hook sets the FaceGen head size to −20. The shadow loses its head (the README says so) | ERVR: "head hiding" option **[R]** | `SprjFD4ModelDispEntity`, `ChrAsmModel`, `DbgMenuChrAsm` and the ESD commands `SetDispMask`/`SwitchDispMask`/`SubDispMaskByBit` **[C names]**. Mask tags in the parts and `invisibleFlag00..62` (§2.4). GPU fallback in §3.4 |
| Fade near the camera | `USES_DITHERING` hook returns false. The near-camera VFX fade (GX FFX draw pass) is switched too | `AimFadeCode` forces alpha 1.0 | — | Not located. It may not trigger at all with render-side injection (§3.5) |
| Lock-on | Soft lock: a 0.2 m sphere cast 20 m along the view picks the target. Forces the "locked" strafing state when not sprinting; `locked_on_cam_offset=0`; ramps `lock_chase_rate` | Lock-on removed in first person; strafing relative to the screen centre | SKVR: lock markers trail by one frame | Strings `IsLockon`, `LockTgtListNum:`, `optLockOnAuto`; classes `SprjTargetingSystem`, `SprjTargetBankManager`, `FrpgMenuDlgTargettingSite` **[C]** |
| Follow-cam side effects | Auto-rotation off, wall recovery off, `reset_camera` to kill attach-point lag, pitch limits | Own camera | — | Auto-rotate NOPs (verified by 08). Collision/near plane at 0x18414f0 (08) |
| Cutscenes | Leaves first person if `CSRemo.remo_man.state != 1` | `[[SprjRemo]+8]+0x48 > 3` | ERVR and RPCS3: cinema screen | `SprjCamera+8 == +0x18` (Remo slot, 08). `SprjRemoMan::STEP_Update` 0x20fa670, `RemoPlayerImpl::STEP_Play` 0x1421400 **[C]** |
| Audio listener | Wwise listener moved to the player (+1.5 m) | — | — | **FMOD Designer** (`N4FMOD12EventSystemIE` RTTI) **[C]**. `SprjSoundStep::STEP_Update` 0x2391550 |
| HUD world markers | `CSFeManImp` lock-on and tag screen positions re-projected | — | SKVR: clamped to the middle 80% | `SprjScaleformPubFe::STEP_Update` 0x2342010, `FrpgMenuDlgTargettingSite` **[C]** |
| Behaviour state | Active hkb node names `Attack_SM`, `Damage_SM`, `Evasion_SM`, `Gesture_SM` (`core.rs`, `behavior.rs`) | Animation ID ranges + TAE events | — | c0000 has "Rolling State StateMachine", "Step State StateMachine", "Attack StateMachine", "Damage StateMachine", "Throw StateMachine", "Dead StateMachine" and "Event … StateMachine" **[C]** |
| FOV | Flat: 90° horizontal plus a barrel shader | Settings FOV | Headset frusta | 08: FOV and aspect at H1/H2, LOD fix |

**Bloodborne first-person precedents**
- **Garden of Eyes** (PS4, Nexus #11, v1.0, 2021-10-29) **[V]**.
  - It ships an eboot DiffPatch `.txt` and `dvdroot_ps4` files, needs a decrypted eboot, and is compatible with Lance's
    60 fps patch.
  - It gives you the Great One Hat and Hunter Gloves on spawn, and reworks the weapon-holding animations. It builds on
    Zullie's DS3 work.
  - Its permissions forbid modification and asset reuse without the author's consent, so treat it as a reference only.
- **shadPS4 first person** (Nexus #287, Mar 2025) **[R]**. The details are behind Cloudflare.
- **Cheapest RE shortcut:** ask the user to download both mods. Then diff their addresses against the v1.09 image with
  08's tools to see where they write the camera.
- I found no GoldHEN first-person cheat.
- **R.E.A.L. Elden Ring:** the camera follows the head through rolls and attacks "but … the horizon will stay level"
  **[R, GamesRadar]**. UploadVR still found fighting in first person "majorly disorienting" **[R]**. That supports a
  stabilized anchor, a vignette and third person as the default.
- **ERVR:** first person "where the weapon follows your hand", with physical melee that plays no attack animation, so AI
  reacts oddly (02 §2) **[R]**.

## 2. Bloodborne facts gathered for first person

### 2.1 Names and globals in the eboot [C]
- **No RTTI for game classes.** The typeinfo slot of the vtables checked is 0. Only FMOD carries Itanium type names
  (`N4FMOD…E`). Use the FD4 debug names instead:
  - `FD4Singleton.h` asserts name every singleton.
  - UTF-16 `X::STEP_*` strings name the step functions (08 `steps.py`).
  - Debug-menu labels and ESD/Lua command names give the rest.
- **WorldChrMan = global 0x593E878.**
  - The assert at 0x1622ef2 names `WorldChrMan`.
  - **+0x60** is by far its most-read field (235 sites), likely the main-player pointer **[L]**.
  - The ChrCam director reads the vec4s at **+0x70/+0x80** (0x1836a20) **[C]**.
- **Camera classes by name:**
  - `ChrCam`, `ChrExFollowCam`, `ChrFollowCam`, `ChrAimCam`, `BallistaAimCam`, `BehindCam`, `ChrOverCam`, `FallDieCam`,
    `DbgCamera`/`DbgFreeCam`, `DepthCamera`, `ReflectCamera`, `FrpgOrthoCam`, `SprjPersCam`, `SprjCamera`.
  - There is also a **`FirstCam`**, with no code reference found in this pass.
  - The UTF-16 class names are used by debug and registration code in the camera cluster, at 0x0183ab10 (ChrExFollowCam)
    and 0x01833700 (ChrAimCam).
  - An aim camera has debug fields `NearPlane`, `ZoomInFovY`, `ZoomOutFovY`, `ZoomRate`… (UTF-16 at 0x4d523e6, right
    after the `BallistaAimCam`/`BehindCam` names). The names are confirmed; which class owns them is **[S]**, probably
    the aim camera behind the Monocular zoom.
- **Character control classes:** `ChrCtrl`, `PlayerCtrl`, `PadManipulator`, `ChrManipulator`, `ActionCtrl`,
  `BehaviorTurn`, `FieldInsBase`.
- **ESD commands of interest:** `CamReset`, `ResetCamAngle`, `SetCamModeParamTargetId(ForBossLock)`, `SetDispMask`,
  `SwitchDispMask`, `SubDispMaskByBit`, `ChrFadeIn/Out`, `RequestRemo`, `PlayerChrResetAnimation_RemoOnly`,
  `CreateSfx_DummyPoly`, `AddObserveChrDmySphere`.
- **Debug-menu items useful for validation** (built in 0x01ccf620, shown by the "Restore Debug Menu" patch):
  **"Draw Dummy Polygon :"** and **"Force Enable Head Spin : %d"**.
- **Step functions:**
  - `SprjRemoMan::STEP_Update` 0x20fa670, `SprjRemoStep::STEP_Update` 0x2108d10, `RemoPlayerImpl::STEP_Play` 0x1421400.
  - `SprjScaleformPubFe::STEP_Update` 0x2342010, `SprjSoundStep::STEP_Update` 0x2391550, `SprjDrawStep::STEP_Update`
    0x21924c0.
  - `CSMovieIns::*` 0x23cd910..
- **Singleton candidates from the assert scan** (noisy heuristic, verify each before use) **[S]**:
  - `SprjHkBehManager` 0x59401F8, `SprjHkBehLookAtTwistAdapter` 0x59401C0, `SoloParamRepository` 0x5940340,
    `SprjSound` 0x5940388, `FaceGenMan` 0x593D730.
  - `SprjCamera` 0x593E8F8 is confirmed by 08.

### 2.2 The player's skeleton and dummy polys [C] (`chr/c0000.chrbnd.dcx` → `c0000.flver`, version 0x20014, 310 bones, 358 dummies)

| Item | Index / ID | Bind-pose model space (m, −Z forward) | Use |
|---|---|---|---|
| Bone `Master` / `Root` / `Spine2` | 0 / 7 / 12 | Master at y 1.019 | Body anchor (A0) |
| Bone `Neck` → `Head` | 67 → 68 | — | Head anchor; parent chain Spine2 12 |
| **dmy 58 "Dummy_eye"** ×2, attached to Head | 58 | (±0.025, **1.654**, −0.066) | **Eye anchor = midpoint** |
| dmy 7, attached to Head | 7 | (0, 1.594, −0.055) | Alternative head-centre anchor |
| dmy 255 "Dummy_voice", attached to Head | 255 | (0, 1.594, −0.095) | Mouth (vial gesture check) |
| dmy 200/205, attached to Head | | (0, 1.698, −0.022) | SFX |
| Arm chain R | `R_Clavicle` 40, `R_UpperArm` 41, `R_Forearm` 42, `R_Hand` 43, `R_Weapon` 59 (+ `Support_R_*Twist`) | | Weapon-follows-hand IK (§5.3) |
| Arm chain L | 13, 14, 15, `L_Hand` 16, `L_Weapon` 32 | | Gun hand |
| Weapon hit dummies on `R_Weapon` | 100/101/110/111/120/121 "tip-root decision", 45/55 bullet | e.g. dmy 101 at z −1.147 | Hit sweeps (§5.3) |
| Gun dummies on `L_Weapon` | 46/56 "Dummy_bullet", 325/327 gun | | Shot origin |

Elden Ring uses dummy 907 for the head; Bloodborne's model has no 9xx head dummy, so **use dmy 58**. Check it live with the
debug menu's "Draw Dummy Polygon".

### 2.3 Head and neck bone setup in code [C] / [L]
- **0x01933210 builds the head/neck control blocks [C].**
  - It allocates 0x80-byte blocks through an allocator vtable call (`[rax+0x58]`, size 0x80, align 0x10).
  - It finds bones by name in the **hkaSkeleton**: bones array at +0x28, count at +0x30, 0x10-byte `hkaBone` entries,
    `hkStringPtr` names with the low bit masked, compared with strcmp 0x2fbe568.
  - Names it looks up: **"Upper_Root", "Spine1", "Master", "Neck", "Head"**.
  - Block layout for the neck/head block: Neck index at **+0x10**, Head index at **+0x14**, `+0x20 = 180.0f`,
    `+0x28 = 1.0f`.
  - It stores the block at **owner+0x1c8**. The skeleton is reached through `P = [block+8]`, then
    `skeleton = [[P+0x18]+0x30]`.
- **This is very likely the neck look-at controller [S].** `SkeletonParam` has `neckTurnGain`, `neckTurnMaxAngle` and
  `neckTurnPriority`, and the debug menu has "Force Enable Head Spin".
- **0x01921320** looks up "HeadNub". Both functions are called from the virtual 0x0191dcc0 (slot 22 of the vtable at
  0x576FE80, 38 slots). The owner is probably a ChrCtrl-type object **[S]**.
- **Havok version:** 2014.1 (`Havok_2014_1_0` paths) **[C]**.
- **Consequence for first person:** a head that turns toward enemies (look-at) moves the eye dummy a few cm sideways.
  The stabilizer absorbs it, or set the player's `neckTurnGain` to 0 while in first person.

### 2.4 Masks and params [C]
- Model-mask tags (`#NN#` prefixes on FLVER material names):
  - hair `hr_a_0001`: **#19, #20, #26, #27, #51, #54**
  - face `fc_m_0000`: **#34, #41–#50, #61**
  - FaceGen parts `fg_a_0100`: **#41, #47–#49**
  - hat `hd_m_1100`: **untagged** ("hat", "Material #382"). A hat can only be hidden per part, not by a mask.
- `EquipParamProtector` (Paramdex BB) has `invisibleFlag00..62`, `headEquip`, `useFaceScale` and `faceScaleM/F_*`.
- `LockCamParam`: `camDistTarget` 4, `chrOrgOffset_Y` 1.42, `camFovY` 43.
- `ThrowParam` has **no camera fields**.
- `MoveParam` lists animation IDs and turn angles.
- The gameparam bundle holds 70 params, among them `BehaviorParam(_PC)`, `RagdollParam`, `SkeletonParam`,
  `CharaInitParam`, `FaceGenParam`.

### 2.5 Behaviour [C] (`c0000.anibnd`)
- **State machines:**
  - Movement and defence: "Move", "Guard", "Defense".
  - Dodges: "Step State" (**quickstep**, used when locked on) and "Rolling State" (**roll**, when unlocked).
  - Combat: "Attack" ("Atk ModeA/ModeB/LeftHand/Fall"), "Damage", "Throw" (`W_ThrowAtk`, `W_ThrowDef`, `W_DamageThrow`).
  - Other: "Fall", "Dead", "Gesture", "Item", "Bow", and "Event Lever/Treasure/Door/Messenger/Load".
- **IK data:** `hkbHandIkDriverInfo` and `hkbFootIkControlsModifier` are present, so the character has hand IK
  configured (§5.3).

### 2.6 How the port reads and writes game memory
- **The game is native code in `bb-probe.exe`.**
  - Guest pointers are host pointers, and globals live at `image + (VA − 0x400000)`.
  - Host code runs inside the game frame through the **host-call patch type** that 08 §7 specifies (H1/H2/H3, G1).
  - Read player data **inside a hook on the game thread**: the in-game step 0x1d3ac10 or H1 in `SprjDrawStep`. That
    gives a consistent snapshot.
  - Headset data already flows into the port: `bbgpu_vr_views` (`gpu/bbgpu.h:52-64`) exposes the head pose and each
    eye's pose and FOV.
  - Data going back to the bridge, such as the anchor speed for the vignette, needs a small frame-tagged mailbox.
- **Dynamic RE needs no PS4 tooling.**
  - Cheat Engine attaches to `bb-probe.exe` (07 §3). Log the image base first.
  - Lance's "Restore Debug Camera" puts a free camera into the hunter's head.
  - "Restore Debug Menu" (needs the debug fonts from Nexus #253) shows dummy polys and the ChrAsm toggles.

## 3. First-person camera design

### 3.1 Composition (H1/H2 handler, once per rendered frame)

| Input | Meaning |
|---|---|
| `G` | The pristine game camera at H1 (rows right/up/forward, position); `θg` = yaw of its forward row |
| `A` | The eye anchor in world space (§3.2) |
| `ψ` | The reference ("body") yaw: in first person our own accumulator (§4.2); in third person the filtered `θg` |
| `Δp`, `Rh` | Headset pose relative to the recentre pose (bridge, predicted display time); `Δp` clamped (§4.6) |

```
R_cam = R_y(ψ) · R_h                 rotation: yaw from ψ, everything else from the headset
p_cam = A + R_y(ψ) · Δp
fov/aspect = union of the per-eye frusta from xrLocateViews (Pimax: canted, never assume parallel)
```

- Write `R_cam` and `p_cam` into both draw copies (H1 PersCam +0x10..+0x4f, H2 format B), and the FOV and aspect
  (+0x50/+0x54).
- The stereo `M_eye` (PLAN §1) is built from this camera, so the mono view `V_mono` is the first-person view.
- **Never** take rotation from the head bone, and never take pitch or roll from the game. That is the horizon lock and
  needs no setting.

### 3.2 Anchor modes
- **A0 "body"** (no bone RE).
  - `A = root position + up·1.65 m + facing·0.07 m`. Needs RE #2 only.
  - No head bob. During rolls the body passes under the camera, and the near-eye clip (§3.4) hides it.
  - Good for bring-up, and some players will prefer it.
- **A1 "stabilized head" (default).**
  1. Take the eye point: the midpoint of the two dmy 58, or the Head bone × the bind offset.
  2. Transform it into the character's root frame.
  3. Average it over **0.3 s** and clamp the deviation from the raw value to **8 cm** (erfps2's numbers).
  4. While "Rolling State" or "Step State" is active, floor the height at **75%** of standing eye height, then blend back
     over 0.2 s.

  Rolls then become mostly horizontal motion.
- **A2 "raw head".** No filtering, the R.E.A.L. behaviour. For testing.
- **Switch out of first person in these cases** (with a 150 ms fade):
  - Remo is playing (`SprjCamera+8 == +0x18`): cinema screen (PLAN Phase 5), or the remo camera with headset rotation
    only.
  - "Dead" state or FallDieCam: fade, then the cinema screen.
  - **Being grabbed** (`W_DamageThrow`): third person after 0.3 s (option).

  Performing a visceral attack and using doors, levers and lamps stay in first person with A1.

### 3.3 Slaving the game camera
- **Why it matters.** The game camera (ChrCam/ChrExFollowCam, still behind the hunter) chooses the lock-on target, defines
  the stick axes, carries the audio listener and places the HUD markers.
- **Slaving.** Each frame while not locked on, write `θg := ψ + yaw(Rh)` (movement relative to the head) or `θg := ψ`
  (relative to the body) to **ChrExFollowCam+0x144**, before the follow-cam update 0x183ac60 (08 [L]; ChrCam+0x60 =
  follow cam).
  - The auto-rotate NOPs keep the game from fighting the write.
  - Pitch (+0x140) := the clamped headset pitch, optionally, for vertical target choice.
- **Fallback without the write.**
  - Rotate the left stick in the bridge or in `sample_vr()` (`src/runtime_pad.c:278`): `s' = R(θref − θg)·s`.
  - Drag the game camera along with a "servo" on the right stick: right-stick X ∝ `wrap(θref − θg)`, saturating.
- **Option "near-head game camera".**
  - Set `LockCamParam` `camDistTarget` ≈0.3 and `chrOrgOffset_Y` ≈1.6, in memory, for every row.
  - That puts the listener, markers and target choice at the head.
  - Risks: camera collision and the near fade. **Default off**; test after FP-2.

### 3.4 Hiding the head (three tiers)
- **T1. GPU near-eye clip** (no RE; for bring-up).
  - Where: the vertex-shader epilogue, the same place as `M_eye`.
  - Which draws: scene-camera draws (the TAA jitter predicate, `vk_rasterizer.cpp:1100-1107`) that are **skinned** (a
    bone palette is bound; the object-motion code already detects palettes).
  - What: `gl_ClipDistance[0] = |p_view| − r`, with `p_view = P_mono⁻¹·clip` and r ≈ 0.12–0.15 m (tune it against hat
    brims). It needs the `shaderClipDistance` feature.
  - Shadow and reflection passes don't match the predicate, so the head keeps its shadow and reflection.
  - The depth pre-pass and the G-buffer pass both match it, so their depth stays consistent.
  - Side effect: anything skinned within r of the eye is cut, such as your hand at the mouth or an enemy's head during a
    grab. That is acceptable.
- **T2. Per-part display flag** (the target; erfps2 parity).
  - Hide the head, hair, face and FaceGen parts, keeping their shadow.
  - Leads: `SprjFD4ModelDispEntity`, `DbgMenuChrAsm` "Enable ChrAsm %s", and the ESD `SetDispMask` family (mask bits at
    runtime).
  - This is the only way to hide hats.
- **T3. Data only** (Great One Hat style **[S]**).
  - Use a protector row with `invisibleFlag` 19/20/26/27/51/54 (hair) and 34/41–50/61 (face), plus no hat mesh.
  - The head shadow is lost.
  - As files, build it from the user's own params at install time. No game data ships.

### 3.5 Near fade, VFX fade, near plane
- **First test:** run H1 in first person and look down at the body and the weapon. With render-side injection the game
  camera stays in third person, so a fade computed game-side won't fire. A fade computed from the draw camera would.
- **If you see dithering, find the fade:**
  - Put the debug free camera into the hunter's head.
  - Cheat Engine: scan for a per-character float that drops as the camera approaches, find its writer, and disable it
    for the main player.
  - Dithered skin materials exist (`P_ChrCustom_Skin_Dither.mtd`) **[C]**.
- Check the VFX near-camera fade the same way; erfps2 had to patch its GX FFX draw pass.
- The near plane is 0.0501 (08). That is fine for hands at about 10 cm.

### 3.6 Lock-on in first person
- **Keep the game's lock-on (R3).** It gives quicksteps, auto-facing and attack tracking.
- **The view never rotates with it.**
  - ψ is unchanged, and we stop writing `θg` while locked. The game keeps `θg` on the target, so stick-forward means
    toward the target.
  - While locked, **give the right stick back to the game**, so a flick switches targets as in flat play, and suspend
    snap turn.
- **Options:**
  - Auto-snap (blinked, 30°) toward the target when it leaves ±70° of the view.
  - Later, gaze lock-on: look at an enemy and press R3. Needs RE #8 and eye tracking.
- **The reticle:** hide it in first person until marker re-projection exists (Phase 5). The game projects it with its own
  third-person camera, so it lands in the wrong place.

### 3.7 Animations and states that move the camera

| Situation | Flat game | First person in VR |
|---|---|---|
| Roll (unlocked) / quickstep (locked) | Camera smooth | A1 position with the height floor, no rotation, vignette (§4.3) |
| Sprint (hold Circle) | — | Vignette above a set speed |
| Hit stagger, knockdown | Camera shake | Shake dropped from the view and sent to haptics. A1 position. Knockdown: the floor applies |
| Visceral or riposte (player attacks) | Scripted animation | Stay in first person (A1) |
| Grabbed (`W_DamageThrow`), boss grabs | Scripted | Detach to third person, or freeze + vignette |
| Ladder | Camera follows | A1, no change |
| Fall death, death | FallDieCam | Fade to cinema |
| Doors, levers, lamps, messengers (Event SMs) | Animated | A1 |
| Monocular (AimCam zoom, `ZoomInFovY`) | FOV zoom | A zoomed inset quad, or ignore the zoom |
| Remo cutscene, boss intro | Remo camera | Cinema screen by default |
| `CamReset` / `ResetCamAngle` (ESD) | Camera snaps | Ignored by the view. Note it in the yaw log |

### 3.8 Audio listener
- It follows the game camera (third person, ~4 m behind).
- With yaw slaving, directions are right but distance is wrong. That is acceptable for v1.
- **Fixes:** the near-head camera option (§3.3), or a hook on FMOD `EventSystem::set3DListenerAttributes`. Find it
  through the `N4FMOD12EventSystemIE` vtable slot and its callers under `SprjSoundStep::STEP_Update` (RE #9).

## 4. Comfort and locomotion

### 4.1 One yaw model for third and first person

| Mode | View yaw ψ | Game yaw θg | Left stick |
|---|---|---|---|
| Third person (Phase 2) | `F(θg)`: passthrough (smooth), **quantised to 30° steps with a blink (snap)**, or rate-limited | The game's own (right stick) | Remapped by `θref − θg` |
| First person | Our own accumulator (snap/smooth from the right stick) | Slaved to `ψ + head yaw` (or `ψ`) when not locked; the game's own when locked | No remap needed while slaved |

Body yaw (the model's facing) stays the game's. Looking down shows the body facing its movement or target direction.

**Known limit:** the hunter can't turn in place without stepping. Test whether a 1–2 frame stick tap only rotates it
**[S]**.

### 4.2 Snap and smooth turn (right stick X)
- **Snap.** 30° when |x| > 0.6, re-armed when |x| < 0.3, repeating every 300 ms while held. Optional 80 ms blink.
  - The Dream Air's sticks reach full range on X but only about 0.35 on Y (`xr_input.cpp:263-264`), so X is fine.
- **Smooth (opt-in).** 90°/s with the vignette. Acceleration ≤ 0.1 s (Meta: keep accelerations short and infrequent).
- **In third person, snapping writes +0x144 directly.**
  - Without the write, the fallback is a timed full-deflection burst on the right stick, hidden behind a blink.
  - Imprecise: the game camera speed depends on the user's in-game setting.
- **While locked on**, the right stick goes to the game (§3.6).

### 4.3 Vignette
- **Inputs, with no RE needed:**
  - The anchor's linear speed, `|ΔA|/dt` (in third person, the game camera's).
  - The rate of ψ (smooth turn only).
  - The vertical acceleration (falls).
- **Strength:** `smoothstep(v; 3.5 → 7 m/s)`, combined with `smoothstep(ψ̇; 30 → 120 °/s)`. Rises over 0.1 s and
  releases over 0.3 s.
- **Rendering:** a radial alpha mask per eye in angle space, drawn into the eye images we already own before submit.
  At "medium", keep the central **60–70°** clear; that matters with Pimax's 105–140° FOV.
- **Evidence:** dynamic FOV restriction cut sickness without users noticing (Fernandes & Feiner 2016) **[R]**. Meta
  recommends vignettes during motion and a short fade after stopping **[V]**.
- **Speeds are guesses [S]:** run about 4–5 m/s, sprint 6–7, quickstep peak 8–10. **Measure them in FP-0** from the
  anchor deltas and tune the thresholds.

### 4.4 The dodge/roll camera
- Rolls are the worst case.
- **Ranking of fixes:**
  1. Stabilized position with a height floor (A1).
  2. Vignette (§4.3).
  3. Optional "lock height during evasion": A0 behaviour only while dodging.
- **Never** rotate with the roll. A blink during every dodge would be too frequent for Bloodborne's combat.

### 4.5 Frame rate correctness
- **The "1/120 constant" is not a timestep.** 08 refuted it: 0x04D25844 holds **1/30** and only converts frame counts to
  seconds.
- **The real slowdown above about 120 FPS is still unexplained** (08 open #5). The eboot also carries a baked-in
  frame-rate mod (08 §2).
- **Run the game at the headset rate.**
  - Dream Air at 90 Hz: the game runs at 90, below the limit.
  - Crystal Light at 120 Hz: only after the limit is found. Otherwise run at 90.
- **First person punishes dropped frames more than third person.** The eye moves with the character (≈7 m/s **[S]**).
  The runtime's reprojection cannot fix game-driven translation, so a dropped frame shakes the whole world.
  - Lower the render scale before falling back to 45 Hz + Smart Smoothing. That fallback is only acceptable in third
    person.
- **Filters use seconds, not frames:** stabilizer, vignette, snap repeat, servo. erfps2 is frame-time based **[C]**.
- **`A` must come from the same simulation frame as the drawn body.** H1 runs in `SprjDrawStep` after the in-game step
  (08). A one-frame-old anchor makes the arms slide about 8 cm per frame at sprint speed.

### 4.6 Defaults for seated play on Pimax (Dream Air, 90 Hz)

| Setting | Default | Note |
|---|---|---|
| View | Third person (PLAN); first person on a toggle | — |
| First-person anchor | A1 (0.3 s, 8 cm, 75% floor while dodging) | A0 selectable |
| Rotation from animation | Never | Horizon lock |
| Turn | Snap 30° (threshold 0.6 / re-arm 0.3 / repeat 300 ms), blink off | Smooth 90°/s opt-in |
| Movement reference | Head | Options: body (ψ), left controller |
| Vignette | Medium: speed > 3.5 m/s or smooth turn; central 60–70° clear | Off / low / high |
| Lock-on | View never rotates; right stick switches targets while locked | Auto-snap when the target is > 70° off-view (opt-in) |
| Cutscenes, death, grabs | Cinema screen / fade / switch to third person | — |
| 6DoF position | Headset deltas 1:1, clamped ±0.25 m horizontal and ±0.15 m vertical; recentre on load and on the button | Seated |
| Height | The character's eye (~1.65 m); the user's real height is ignored | — |
| IPD and world scale | True IPD, scale 1.0 | FromSoft units are metres |
| Effects | Motion blur, DoF and CA off; camera shake → haptics | Existing patches |
| Refresh | 90 Hz with the game locked to it | 120 Hz waits for 08 #5 |

## 5. Motion controls with first person

### 5.1 The existing gestures (`gpu/xr_bridge/xr_input.cpp`) in first person

| Gesture | Mapping | In first person | Proposed change |
|---|---|---|---|
| Right-hand swing > 2 m/s | R1 (100 ms pulse); R2 with the grip held or > 3.5 m/s | Works, but the visible weapon follows the animation, which starts only after your swing is over **[S]**. Looking at your hands also invites false swings | In first person, raise the threshold to 2.3–2.5 m/s, and require the hand to be in front of the headset and moving away from it |
| Left thrust > 1.8 m/s along the aim | L2 (gun) | Players will point the gun, but the hunter fires along its facing or the lock target **[S]** | **Aim-to-face:** with the thrust, inject a 1–2 frame left-stick vector toward the hand's aim, rotated into `θg`, so the hunter turns before firing. Later, lock onto the enemy under the hand ray (RE #8) |
| Left hand at the mouth for 0.3 s | Triangle (vial) | Natural, and relative to the headset | Keep. It matches dmy 255 "Dummy_voice" |
| Left grip | L1 (transform) | Fine | Optional wrist-flick transform |
| Hand markers | — | They duplicate the game's own hands | Off by default in first person |
| Rumble | Mirrors `pad_vibration` | Fine | Add camera shake → haptics |

### 5.2 Weapon follows hand (stretch goal, feasibility only)
- **What it needs:**
  1. **A pose override after animation and IK, and before both the skinning palette and the dummy-poly queries.**
     Hit detection sweeps the weapon dummies (100–121) during the TAE attack windows. Overriding only the render would
     make hits disagree with what you see.
  2. **Two-bone IK** for R_UpperArm/R_Forearm to reach the controller grip pose in character space. The `Support_*Twist`
     bones need updating too, or the skin candy-wraps.
  3. **A trigger for the attack window.** The gesture fires the attack event, the arm animation is suppressed, and your
     motion then drives the sweep. That is ERVR's model, and AI that reads animations reacts oddly **[R]**.
  4. **Trick weapons** change model in the middle of animations, and two-handed grips need both arms.
- **Options:**
  - **(a)** Hook the bone pose in the animation pipeline. Leads: `AnimThreadMan`, `SprjHkBehManager`, and
    `SprjHkBehLookAtTwistAdapter` (which already modifies the pose after animation).
  - **(b)** Drive Havok's own **hand IK**. `hkbHandIkDriverInfo` is in the c0000 behaviour **[C]**, so feed IK targets
    through the behaviour controls.
  - **(c)** GPU-only: move the weapon draw in the vertex shader. It is cosmetic and the hits don't match. **Not
    recommended.**
- **Verdict:** medium-low feasibility for v1, about **4–8 weeks** for RE plus development.
- **Best first target:** the **left-hand gun.** It is one arm with no hit sweep; the shot origin is the dummy 46/56 on
  `L_Weapon`. It is worth it only if the bullet direction follows that dummy's forward vector, so check the bullet launch
  code first.

## 6. Performance for first person and wide FOV

### 6.1 Pixel budget per frame (two eyes, 1.0 scale)

| Headset | Per eye | Horizontal FOV | Hz | Eye tracking | MP/frame | vs 1080p |
|---|---|---|---|---|---|---|
| Dream Air | 3840×3552 micro-OLED | ~102–110° (sources disagree) | 72/90 | Yes (Tobii) | 27.3 | 13× |
| Dream Air SE | 2560×2560 | — | 90 | Yes (2.0.3 calibration fix) | 13.1 | 6× |
| Crystal Super (QLED) | 3840×3840 | 106–140° (by lens module) | 72/90 | Yes, 120 Hz | 29.5 | 14× |
| Crystal Light | 2880×2880 | 105–120° | 60–120 | No | 16.6 | 8× |

- First person adds no pixels over third person. It does select higher-detail LODs, because geometry is closer to the
  camera.
- **The real limit is still the port's GPU command thread** (06 §2d). Only multiview (D-mv) adds views without adding
  recording cost.

### 6.2 Culling and LOD
- **Culling follows the injected camera automatically** (08 §6).
- **Widen it only when the pose is updated after culling** (late latching): add a 5–10° margin through H3 (0x26923b4).
- **LOD:** the context's LOD scale (ctx+0x150) is proportional to `tan(fov/2)`. A 100–120° view therefore drops LODs
  2–3× (08). In first person nearby props are seen close up, so rewrite +0x150 back to the 43° value, scaled by pixel
  density.
- **Visibility mask:** PimaxXR exposed `XR_KHR_visibility_mask` **[C src]**. Pre-filling depth under the mask
  (D-dup/D-mv) skips the pixels outside the lens.

### 6.3 Foveation options
1. **Quad views (best on Dream Air and Crystal Super).**
   - **Runtime support:** Pimax Play has its own OpenXR runtime with a "Pimax OpenXR Quadviews" switch. Quad views work
     only "in games that support Quad Views" **[V, Pimax Play tips page]**. DCS enables them together with eye tracking
     **[R, Pimax DCS guide via search]**. Pimax Play 2.0.3 (Aug 2026) added "advanced Quad View adjustment" **[R]**.
   - **What we implement:** `XR_VARJO_quad_views` (4 views: 2 wide context views, 2 inner focus views), plus
     `XR_VARJO_foveated_rendering` so the inner views follow the gaze.
   - **In our renderer:** **4-layer multiview** (D-mv with `viewMask=0b1111`). Every layer can have the same size: the
     inner layers cover a narrow FOV at native density, the outer ones a wide FOV at low density.
   - **Why it fits:** the `M_eye` clip-space remap works for any per-view P/V, asymmetric inner frusta included.
   - **Example:** outer 2× 1536×1420 + inner 2× 1536×1536 (~42° at ~36 px/°) = **9.1 MP**, a third of full resolution.
   - **Known artefacts:** shimmer at the inner/outer seams **[R]**, and SSAO/fog radius changes between layers.
2. **Variable rate shading (`VK_KHR_fragment_shading_rate`).**
   - Fixed, or driven by the gaze through `XR_EXT_eye_gaze_interaction`, which PimaxXR exposed **[C src]**.
   - Applies to the G-buffer, light volumes and full-screen pixel shaders, not to compute passes.
   - Saves about 15–30% **[S]**. It is the Crystal Light path (no eye tracking).
3. **Runtime-side foveation for Vulkan apps:** don't count on it **[S]**. OpenXR Toolkit-style injection targets
   D3D11/12.
- **Check at startup:** log `xrEnumerateInstanceExtensionProperties` (the bridge already enumerates the list,
  `bbport_xr_bridge.cpp:143-155`), the view configurations (`PRIMARY_QUAD_VARJO`) and
  `XrSystemEyeGazeInteractionPropertiesEXT`.

### 6.4 Resolution targets (starting points, measure) [S]
- **Dream Air at 90 Hz, RTX 4080 class.**
  - Output per eye: the runtime's recommended size, which depends on Pimax Play's "Image Quality".
  - **Internal per eye: 2304×2131 (0.6 per axis)**, upscaled by DLSS or FSR4 (between their Quality and Balanced
    presets), 9.8 MP per frame. Drop to 0.5 if GPU-bound.
  - With quad views + eye tracking: the §6.3 example, upscaling only the outer views.
- **Crystal Light:** 0.7 per axis with fixed VRS.
- **Never** trade 90 to 45 fps in first person (§4.5).

## 7. Ranked RE tasks (first person and comfort)

| # | Task | Why | Leads | Strategy | Effort |
|---|---|---|---|---|---|
| 1 | Confirm the **follow-cam yaw/pitch** (+0x144/+0x140) and a stable path to ChrCam | Snap turn, slaving, third-person yaw filter | 08: ChrCam = `[r13+0x128]` in the in-game step 0x1d3ac10; follow cam = ChrCam+0x60; update 0x183ac60 | CE: turn with the stick, watch +0x144; write it and check that the camera turns and movement follows. Find a global path to ChrCam (WorldChrMan or a field of the step object) | hours |
| 2 | **Main player** pointer, world position and facing | Anchor A0, vignette speed, sprint/roll speed measurement | WorldChrMan 0x593E878, +0x60 most read **[L]**; +0x70/+0x80 vec4s read by the ChrCam director | CE: unknown float, walk, changed/unchanged → writer → pointer back to +0x60. Measure the movement speeds | 1 day |
| 3 | **Eye anchor:** a dmy-poly world query (dmy 58), or the Head bone world matrix | Anchor A1 | Head index at owner+0x1c8 → +0x14 (0x01933210); skeleton via `P=[blk+8]`, `[[P+0x18]+0x30]`; dmy users: ESD `CreateSfx_DummyPoly`, `AddObserveChrDmySphere` (0x01620a10), "DmyPlyID" (0x016ff500), "dmy_poly_id" (0x023e9160, 0x02407bd0), "dmypolyId" (0x020ea910…) | Follow the common callee taking (chr, id, out matrix) and call it from the hook (guest thread, SysV), or find the pose buffer that the look-at code reads through +0x1c8. Validate against the bind data (§2.2) and "Draw Dummy Polygon" | 2–4 days |
| 4 | **State flags:** Remo, death/fall cam, ChrCam mode | Switching out of first person | 08: SprjCamera slots (+8 vs +0x18), ChrCam+0x84 mode 0..4, `RemoPlayerImpl::STEP_Play` | Log the slot and mode while triggering a cutscene, a death and a ladder | hours |
| 5 | **Head part visibility** (keep shadow) | Hide hats too (tier T2) | `SprjFD4ModelDispEntity`, `DbgMenuChrAsm` "Enable ChrAsm %s", ESD `SetDispMask` family | Toggle the debug menu entries, find the byte they flip, map part slots (hd/hr/fc/fg) | 2–3 days |
| 6 | **Near-camera fade** (if it fires at all) | Body and weapon visible | Dithered mtds exist; erfps2 `USES_DITHERING` analogue | First see whether H1 first person dithers; then CE on a per-character alpha | 0.5–3 days |
| 7 | **Behaviour state** (rolling/step/attack/damage/throw/dead/event) | Height floor, switch out on grabs, gesture gating | State-machine names (§2.5); `SprjHkBehManager` **[S]** | Walk the player's hkbBehaviorGraph active nodes (erfps2 `update_behavior_states`), or use CE on the current animation ID | 2–3 days |
| 8 | **Lock-on manager:** flag, target handle and position | Reticle, auto-snap, gaze/hand-ray lock, gun aim | "IsLockon", "LockTgtListNum:" (debug display), `SprjTargetingSystem`, `FrpgMenuDlgTargettingSite` | Xref the debug string to the list owner; CE on a bool that flips with R3 | 2–3 days |
| 9 | **Audio listener** | Correct listener in first person | FMOD `EventSystemI` RTTI → vtable; `SprjSoundStep::STEP_Update` 0x2391550 | Find the `set3DListenerAttributes` slot and its game caller | 1–2 days |
| 10 | **HUD world markers** (FeMan analogue) | Lock reticle and HP bars in VR | `SprjScaleformPubFe::STEP_Update` 0x2342010 | As erfps2 `update_fe_man`; shared with PLAN Phase 5 | 3–5 days |
| 11 | The **real >120 FPS limit** | 120 Hz headsets | 08 §2 and open #5 (0x2418e3d dt clamp, 0x24348xx) | 08's follow-up | 2–4 days |
| 12 | Free check of **`FirstCam`** and `Game.IsUseChrFollowCam` | Might be a dev first-person camera | Strings only, no xrefs this pass | Search registration tables and system properties | hours |
| 13 | **Post-animation bone override / hand IK** | Weapon follows hand (stretch) | `hkbHandIkDriverInfo` **[C]**, LookAtTwist adapter | §5.2 | 2–4 weeks |

**Data tasks (no eboot RE):**
- **D1.** Dump all head-part mask tags across `hd_*`, `hr_*`, `fc_*` and `fg_*` (`fp11/mats.py`) → the exact `invisibleFlag`
  set for tier T3.
- **D2.** Diff the Garden of Eyes and Nexus #287 patch bytes against the v1.09 image. The user downloads them.
- **D3.** Measure walk, run, sprint, roll and quickstep speeds (RE #2) for the vignette thresholds.

## 8. Phased plan (fits PLAN §3 and §6)

| Phase | When | Content | Exit |
|---|---|---|---|
| **FP-0 RE** | Alongside PLAN Phase 2 (1–2 weeks) | RE #1–#4, D1, D3; log OpenXR extensions, view configurations and eye-gaze support | Addresses confirmed; speed table |
| **C-1 Comfort core** | With Phase 2 (1 week) | Yaw model (§4.1): third-person snap via +0x144 or blink burst, left-stick remap, recentre; vignette pass; seated defaults (§4.6) | 20 min seated in third person with head tracking, no comfort complaints |
| **FP-1 Body-anchored first person** | Right after Phase 3, per the user decision (1–2 weeks) | Anchor A0; GPU near-eye clip (T1); switch out for Remo and death; yaw slaving; lock-on rules (§3.6) | Central Yharnam in first person at 90 Hz on the Dream Air: own head never visible, shadow intact, no view rotation from the game |
| **FP-2 Head-anchored first person** | +2–3 weeks | Anchor A1 + height floor (RE #3, #7); part hiding T2 (RE #5); fade fix (RE #6); switch out on grabs; optional near-head game camera; reticle hidden | Rolls, quicksteps, visceral attacks, ladders and grabs pass a comfort checklist with 3 testers |
| **FP-3 Controls polish** | +1–2 weeks | Gesture gating, gun aim-to-face, shake → haptics, hand markers off; gaze/hand-ray lock (RE #8); listener (RE #9) | Pistol and melee usable with gestures in first person |
| **FP-4 Stretch** | After v1 | Gun-follows-hand, then weapon-follows-hand (RE #13) | Prototype |
| **P-1 Culling/LOD** | With Phase 2 | FOV/aspect at H1/H2; H3 margin; LOD scale fix | No pop-in at the view edge; LOD as flat |
| **P-2 Resolution** | Phases 3–4 | Upscaler presets per headset (§6.4) | 90 Hz on the Dream Air in average scenes |
| **P-3 Quad views** | Right after Phase 4 (D-mv), per the Pimax decision | 4-layer multiview + `XR_VARJO_quad_views/foveated_rendering` + eye gaze | ≥30% GPU saving at equal centre sharpness |
| **P-4 VRS** | Optional | Fixed or gaze VRS for the Crystal Light | Measured gain |

## 9. Risks and open questions
- **Anchor timing.** Check that the player's bones are final by H1. `AnimThreadMan` may run animation on workers. Read
  the anchor in the in-game step if needed (G1).
- **Writing +0x144 is gameplay input.** Lock-on, `CamReset` and boss camera modes (`SetCamModeParamTargetIdForBossLock`)
  may fight the write. Stop writing whenever ChrCam+0x84 isn't the follow mode.
- **The near-eye clip** cuts your own hands near the face, and skinned enemies at kissing distance. That is acceptable
  in v1 and replaced by T2 later.
- **Turning in place.** Without it, the body can face away from the view while you stand still.
- **Gesture lag** in first person can't be fixed without the stretch goal. It is a UX expectation risk; say so in the
  release notes.
- **Pimax runtime capabilities** (quad views, eye gaze) are unverified on "Pimax OpenXR 0.1.0" until the startup log
  shows them.
- **Online:** none of this should run in online sessions (PLAN §3 Phase 6).
- **Licences.**
  - erfps2 (MIT/Apache) and ds3fps (MIT): code reuse is allowed with attribution.
  - Sekiro-VR (no licence): techniques only.
  - Garden of Eyes: permission needed.
  - ERVR (GPL-3 claimed, unreachable) and R.E.A.L.: closed.

## 10. Links
- **erfps2:** https://github.com/Dasaav-dsv/erfps2 (`src/core.rs`, `src/hooks.rs`, `src/player.rs`,
  `src/core/head_tracker.rs`, `src/core/stabilizer.rs`, `src/shaders.rs`, `dist/erfps2.toml`)
- **ds3fps:** https://github.com/Dasaav-dsv/ds3fps (`src/AOB.cpp`, `src/Pointer.cpp`, `src/camera/CameraCtrl.cpp`,
  `src/camera/TAE.cpp`, `src/camera/Viewmodel.cpp`, `src/asm.cpp`)
- **Zullie's DS3 first person:** https://nexusmods.com/darksouls3/mods/20 and
  https://www.pcgamesn.com/dark-souls-3/mod-first-person
- **Sekiro first person (Zullie, GameStar):** https://www.gamestar.de/artikel/sekiro-mod-ego-perspektive,3349558.html
- **Sekiro-VR:** https://github.com/junglejuz/Sekiro-VR
- **ERVR:** https://www.nexusmods.com/eldenring/mods/10711
- **R.E.A.L. Elden Ring:** https://www.gamesradar.com/elden-ring-vr-mod and
  https://uploadvr.com/elden-ring-vr-mod-gameplay-hand-on/
- **Garden of Eyes (Bloodborne):** https://www.nexusmods.com/bloodborne/mods/11 and
  https://uploadvr.com/bloodborne-first-person-mod
- **shadPS4 first person:** https://www.nexusmods.com/bloodborne/mods/287 and
  https://www.dsogaming.com/mods/you-can-now-play-bloodborne-in-first-person-on-pc-via-shadps4/
- **Paramdex BB definitions:** https://github.com/soulsmods/Paramdex/tree/master/BB/Defs (`EquipParamProtector`,
  `SkeletonParam`, `LockCamParam`, `ThrowParam`, `MoveParam`)
- **Meta locomotion:**
  - https://developers.meta.com/horizon/design/locomotion-best-practices/
  - https://developers.meta.com/horizon/design/locomotion-comfort-usability/
  - https://developers.meta.com/horizon/documentation/iwsdk/concepts/locomotion/slide/
- **Fernandes & Feiner 2016 (dynamic FOV restriction):**
  https://spectrum.ieee.org/dynamic-field-of-view-restriction-makes-virtual-reality-less-barfy
- **Pimax:**
  - Dream Air: https://business.pimax.com/headsets/dream-air and
    https://skarredghost.com/2025/05/19/pimax-dream-air-preview-impressions/
  - Dream Air eye tracking: https://eu.pimax.com/blogs/highlights/how-does-dream-airs-eye-tracking-change-your-vr-experience
  - Pimax Play: https://store.pimax.com/blogs/crystal-light-tutorials/pimax-play-tips-and-features-overview
  - Quad views in DCS: https://store.pimax.com/pages/pimax-crystal-super-the-ultimate-dcs-setup-guide
  - Crystal Super: https://eu.pimax.com/pages/pimax-crystal-super
  - Crystal Light: https://pimax.com/pages/crystal-light
  - Downloads and release notes: https://eu.pimax.com/pages/downloads-manuals
  - Integrated OpenXR announcement: https://forum.dcs.world/topic/357527-integrated-openxr-quadviews-coming-to-pimax-play
- **mbucchia:** https://github.com/mbucchia/Pimax-OpenXR (retired; extension list in `instance.cpp`),
  https://github.com/mbucchia/Quad-Views-Foveated, https://github.com/mbucchia/Meta-Foveated
- **Specs:**
  - OpenXR `XR_VARJO_quad_views`, `XR_VARJO_foveated_rendering`, `XR_EXT_eye_gaze_interaction`:
    https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
  - Vulkan `VK_KHR_fragment_shading_rate`:
    https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_fragment_shading_rate.html

## Cross-links
- **08** supplies the camera chain, the H1/H2/H3 hooks, the yaw field and LOD/culling. Everything in §3 assumes its
  render-side injection. It also refutes PLAN's 1/120 premise (§4.5 here).
- **02 §4/§6** has the erfps2 and ds3fps summaries this file deepens from source.
- **05 §3** covers EquipParamProtector, LockCamParam and the Garden of Eyes mechanics, refined in §2.4/§3.4.
- **06 §2d:** the GPU command-thread bottleneck. That is why quad views wait for D-mv.
- **07:** Cheat Engine on `bb-probe.exe` and the debug-menu patch, reused as RE tools here.
