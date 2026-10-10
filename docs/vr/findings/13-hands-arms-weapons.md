# 13: Hands, arms and weapons in VR (bones, arm IK, fingers, weapon recognition and logic)

Date: 2026-10-09. Track: VR body interaction for Bloodborne v1.09 on the native port. Arms and bones driven by the
controllers, hands with finger mapping, weapon recognition, weapon logic.

Scope: **11-first-person-comfort.md owns the first-person camera, the head/eye anchor, head hiding and comfort.** This
file covers arms, hands, fingers and weapons. Where both need the same thing (player pointer, world transform,
lock-on), it links to 11's RE table instead of repeating it.

Design only. No repo file other than this one was changed, nothing was built, and the game was not launched.

Inputs:
- **The user's dump, read-only.** `C:/games/CUSA03173\dvdroot_ps4`: `chr/c0000.chrbnd.dcx`, `chr/c0000.anibnd.dcx`,
  `parts/*.partsbnd.dcx`, `param/gameparam/gameparam.parambnd.dcx`, `paramdef/paramdef.paramdefbnd.dcx`,
  `msg/engus/item.msgbnd.dcx` and `action/*`. I parsed them with throwaway Python in the session scratchpad
  (`scratchpad/scripts/souls.py`, `hkx.py`, `param.py`, `tae.py`, `fmg.py`).
- **The prepared image `out/eboot.elf`** (VA = file offset + 0x3FC000, the patch-XML space of 08). I applied its
  231,478 `R_X86_64_RELATIVE` relocations in memory and ran capstone queries (`xref.py`, `rcmap.py`, `callers.py`).
- **The repo:** `gpu/xr_bridge/xr_input.cpp`, `bbport_xr_bridge.h`, `xr_sandbox.cpp`, `src/hooks.h`,
  `src/runtime_hooks.c`, `gpu/shadps4/video_core/renderer_vulkan/{vk_rasterizer.cpp,motion_history.h}`,
  `docs/motion_vectors.md`.
- **Work in progress by other tracks** (read, not changed): `src/runtime_vr_head.c`, `scripts/vr_assets/`,
  `gpu/xr_bridge/sandbox3d/`, and the input diagnostics in `xr_input.cpp`.
- **The session's run logs**, including a `BB_XR_INPUT_DIAG` sandbox run on the user's Dream Air.
- **Web research** by four helper agents (sources in §12).

Legend (as in 08/11): **[C]** confirmed here from the eboot, the game data or source code. **[L]** likely (strong
circumstantial evidence). **[V]** verified on a primary page or in published source. **[R]** reported by a secondary
source. **[S]** speculation, inference or estimate. Addresses are PS4 VAs, the same space as 08 and 11.

---

## 0. Summary

1. **The player's final animated pose is reachable, and the chain is mostly confirmed [C/L].**
   - `P = [[0x593E878] + 0x60]` is the main player (`PlayerIns`). The global is the WorldChrMan singleton; +0x60 is [L], as
     in 11.
   - `[P + 0x3B0]` is the module bag [C]. `[bag + 0x30]` is the `SprjChrBehaviorModule` [C].
   - `[module + 0x28] + 0x38` is a Havok 2014 **`hkaPose`** [C], with 108 bones in the order of `skeleton.HKX`.
   - The game's own `hkaPose` methods are at 0xAC4F00 (`getSyncedPoseModelSpace`), 0xAC4C90 (`getSyncedPoseLocalSpace`),
     0xAC4F20 (`syncModelSpace`) and 0xAC59A0 (`accessSyncedPoseModelSpace`) [C].
2. **The "AnimationToModel" copy is the override point [C code, L which copy the player uses].**
   - 0x1C375D0 and 0x1E19BC0 each take the synced model-space pose. They turn every Havok bone into a 3×4 row-major
     matrix (48 bytes) in **FLVER bone order** through a remap table, which is the same 3×4 / 48-byte format as the GPU
     bone palettes the renderer already classifies.
   - A 19-byte hook window right after the `getSyncedPoseModelSpace` call in each copy (0x1C37650, 0x1E19BFE) lets a
     host handler rewrite the arm chain in Havok model space **before** the copy. Rendering, weapon attachment and
     weapon dummy polys then all see the same arms.
3. **No arm IK to borrow [C].** The player behaviour graph has one `hkbHandIkControlsModifier`, but both of its "hands"
   target handles taken from the **foot-target** bones (`L_Foot`/`R_Foot` variables, `GetHandleOnBoneModifier` on
   L/R_Foot_Target). It is leg IK. No `hkbHandIkDriverInfo` object is in the player's files, so arm IK is our own
   analytic two-bone solver in host code. This corrects 11 §5.2 option (b).
4. **Weapon recognition is cheap [C code, L meaning].**
   - `[P+0x4B8]` (returned by PlayerIns virtual +0x2E8) holds the active slots: right at +0x10, left at +0x0C.
   - The six equipped weapon IDs are at +0x64 (`L0,R0,L1,R1,L2,R2`).
   - An arm/weapon style is at +0x08. BB's TAE `SetWeaponStyle` values add "transformed" styles to DS3's set, so the
     **trick form** is very likely that field: 1 = right normal, 3 = right transformed two-handed, 5 = right
     transformed one-handed.
   - The current animation is in the TimeAct module's 10-entry ring (`[bag+0x18]+0x20`). That is the same layout as
     Elden Ring's `CSChrTimeActModule`, independently confirmed by fromsoftware-rs.
5. **The data needed for per-weapon grips is in the user's params and models [C].**
   - EquipParamWeapon has, per weapon, the character dummy poly each sub-model attaches to: sheathed, form A (one-hand)
     and form B (two-hand), for up to four sub-models.
   - All hand-held attach dummies sit on `R_Weapon`/`L_Weapon` within 5 cm of the bone, except Logarius' Wheel (41 cm)
     and the Stake Driver (9 cm). Moving the hand bone moves the weapon, and a per-weapon grip offset can be generated
     from the dump.
6. **Attack timing is in the TAE files [C].**
   - Each weapon moveset (`a1xx`) has `InvokeAttackBehavior` (type 1) windows. Examples: a Saw Cleaver R1 hits at
     0.37–0.47 s, a Hunter Axe R1 at 0.53–0.67 s, a pistol shot spawns at 0.33 s.
   - Form-A attacks use sub-IDs 30000–39999, form-B attacks 40000–49999, and 33300/43300 are the transform attacks
     (they carry `SetWeaponStyle`).
   - The hit shapes are AtkParam_Pc capsules between **weapon-model dummies** (Saw Cleaver R1: 0.40 m between 10100
     and 10110), reached through BehaviorParam_PC row 100,000,000 + variation × 1000 + judge. They ride on the weapon
     model, so they follow an overridden hand.
7. **Fingers.**
   - The Pimax Dream Air's controllers come up as `oculus/touch_controller` on "Pimax OpenXR 0.1.0" [C, session logs].
   - A diagnostic sandbox run on them shows capacitive **touch on both thumbsticks, A/B/X/Y and the left trigger**.
     The right trigger's touch and `thumbrest/touch` were not seen [C].
   - The curl model: index from the trigger value and touch, middle/ring/pinky from squeeze, thumb from the touch
     sensors (§6).
   - Gameplay bindings don't read `/touch` yet. Hand tracking is optional and runtime-dependent.
8. **Ranking (value ÷ effort):**
   1. Weapon/form/anim readout plus better gestures, bridge only.
   2. Arm IK with weapon-follows-hand outside attacks.
   3. Fingers.
   4. Two-handed forms and gun aim.
   5. Hand-driven hits inside the game's attack windows (stretch).
   6. ERVR-style animation-free attacks (research only).
   7. GPU-only palette override: not recommended.

## 1. Prior art: how FromSoftware mods touch bones

| Project | What it does with arms/bones | Hook style | Relevance |
|---|---|---|---|
| **ERVR** (Elden Ring, IlyaMez, v0.4.0 2026-08-26) | Controllers move the in-game arms and held weapons. Collisions come from the visible weapon. No attack animation; damage scales with blade speed; some hit reactions/hit-pause skipped; rolls possible mid-swing. Two-hand by grabbing the hilt; shield guard from grip and guard angle **[R]** (Nexus page unreachable, Cloudflare) | Closed (claims GPL-3; repo not public) **[V]** | The "physical melee" end state (§8 P3), and its known costs |
| **erfps2** (Dasaav, ER 2.7.1.0) | No bone writes. `POSTURE_CONTROL_RIGHT` adds +15 to the right-hand posture value in first person; `CHR_ROOT_MOTION` rotates root motion in attacks; `GET_DMY_POS(ChrIns*, out, &id, i32)` reads dummy 907; a TAE `exec_one` hook zeroes `SetBulletAimAngle` deadzones **[V]** (`src/hooks.rs:113-144`, `player.rs:78-91`, `hooks/tae.rs:16-71`) | Mid-function hooks + vtable | TAE-event hooking is the model for §8 P2 window shifting; dummy query shape |
| **ERSkeletonMan** (Dasaav, MIT) | Runtime bone modifier: writes **local-space** translation/quaternion/scale per bone, from a hooked Havok-side virtual "for its good execution timing" (`SkeletonMan.h:122-132, 205-208`) **[V]** | PlayerIns/EnemyIns vtable hooks | Proof that post-animation pose writes work in this engine family |
| **fromsoftware-rs** (vswarte; Dasaav fork) | ER `CSChrTimeActModule`: `anim_queue[10]` at +0x20, write index +0xC0, read index +0xC4. `ChrAsmArmStyle` 0 empty, 1 one-handed, 2 left both-hands, 3 right both-hands. Module container: time_act +0x18 **[V]** (fork `crates/eldenring/src/cs/chr_ins/module/time_act.rs:7-30`, `player_game_data.rs:1151-1231`) | — | Same TimeAct layout as BB (§7.4); same ArmStyle idea |
| **The Grand Archives CE tables** | ER "Current Animation" = main player → module container → time_act → `anim_queue[0]`; DS3 "Left/Right Hand 1-3", "ArmStyle" **[V]** | — | Runtime cross-check recipe |
| **Sekiro-VR, R.E.A.L.** | Camera only, no arm code **[V/R]** | — | — |
| **Garden of Eyes BB first person** | Reworked weapon-holding animations; hands visible in first person **[V]** (Nexus #11). Permission needed for reuse | eboot patch + data | Reference only |

No public Cheat Engine "bone posing" tool for these games turned up **[R]**.

## 2. The player's skeleton and animation data (from the user's dump) [C]

### 2.1 Formats
- **Containers.** `.dcx` = `DCX\0`, version 0x10000, `DCP` **`DFLT`** level 9 (zlib, `78 DA` at +0x4C), not KRAK/Oodle.
  Every file I opened is DFLT. Inside are **BND4** archives (version `07D7R6`, format byte 0x2E: IDs, names,
  compression fields, 32-bit offsets, UTF-16 names).
- **Models.** FLVER2 version **0x20014**.
- **Havok.** Packfiles (magic `57E0E057 10C0C010`), file version 11, layout `8,1,1,1` (64-bit LE), contents
  **`hk_2014.1.0-r1`**. The eboot links `Havok_2014_1_0\hk2014_1_0_r1` (`hkaSkinningUtils.cpp` path).
- **Animation events.** TAE3 (`TAE `, version 0x1000C, 64-bit).
- **Params.** PARAMDEF format 201 (UTF-16), PARAM 64-bit with 0x18-byte row headers (EquipParamWeapon rows are 316
  bytes, 1,090 rows). **FMG** v2 for text.
- **Tools.** SoulsFormats, WitchyBND/Yabber, DSAnimStudio (TAE template `TAE.Template.BB.xml`) and Smithbox read all of
  these.

### 2.2 Two skeletons, two bone orders
- **`c0000.anibnd/skeleton.HKX`, the animated skeleton:** one `hkaSkeleton` "Master" with **108 bones**, no float
  slots. Its reference pose equals the FLVER bind pose (dmy 300's model position equals the computed `R_Weapon`
  position, below).
- **`c0000.chrbnd/c0000.flver`:** **310 nodes** (bones 0–107 hierarchical, `Dummy_*` 108–202 and `Ctrl_*` 203–309
  parentless rig leftovers), plus 358 dummies.
- **All 108 Havok bones exist by name among FLVER bones 0–107, with identical parents [C].** The A2M remap (§3.2) is a
  pure permutation, and the FLVER's `Dummy_*`/`Ctrl_*` nodes are never animated.
- **Ragdoll.** `c0000.chrbnd/c0000.HKX` is the ragdoll: 16 `hknpCapsuleShape`, hinge/ragdoll constraints, two
  skeletons and two `hkaSkeletonMapper`. BB uses **hknp** physics.
- **Parts reorder bones.** `parts/am_a_0009.flver` (arms armour) has 109 bones starting at `L_Forearm`, and its mesh
  has an empty bone table, so vertices index the part's own bone list. The game must remap by name per part [L]
  (§4).

| Bone | Havok idx | FLVER idx | Local translation (bind) | Notes |
|---|---|---|---|---|
| Spine2 | 48 | 12 | 0.109 | Parent of clavicles, Neck, Axilla supports |
| Neck → Head | 77 → 78 | 67 → 68 | 0.241, 0.058 | 11's anchors |
| R_Clavicle | 79 | 40 | (0.166, −0.010, 0.062) | |
| R_UpperArm | 80 | 41 | 0.179 | Shoulder joint |
| R_Forearm | 81 | 42 | **0.283** | Elbow. Bind bend 7.1° about local **Y** → hinge axis [L] |
| R_Hand | 82 | 43 | **0.273** | Wrist |
| R_Finger0/01/02 (thumb) | 83/84/85 | 44/45/46 | (0.020, 0.033, 0.021), 0.056, 0.032 | |
| R_Finger1/11/12 (index) | 86/87/88 | 47/48/49 | (0.090, 0.032, −0.011), 0.047, 0.027 | |
| R_Finger2/21/22 (middle) | 89/90/91 | 50/51/52 | (0.093, 0.010, −0.015), 0.050, 0.030 | |
| R_Finger3/31/32 (ring) | 92/93/94 | 53/54/55 | | |
| R_Finger4/41/42 (pinky) | 95/96/97 | 56/57/58 | | |
| **R_Weapon** | **98** | **59** | (0.076, 0.005, 0.017) | Child of R_Hand; all right-hand weapons attach here (§7.3) |
| Support_R_ForeArmTwist / ...1 | 99 / 100 | 61 / 60 | 0.273 / 0.135 along the forearm | Twist helpers, ~4.7° / 2.4° about X in bind |
| Support_R_Elbow, UpArmTwist, UpArmTwist1, UpArmTwist_NoRotX | 101–104 | 64, 63, 62, 65 | | Children of R_UpperArm |
| Support_R_Shoulder | 105 | 66 | 0.179 | Child of R_Clavicle |
| Left side | Clavicle 50, UpperArm 51, Forearm 52, Hand 53, fingers 54–68, **L_Weapon 69**, supports 70–76 | 13, 14, 15, 16, 17–31, **32**, 33–39 | mirrored | |

**Bind-pose geometry** (model space, metres, from composing the HKX reference pose):
- The skeleton is in an **A-pose**, arms 45° down.
- Shoulder joints at (∓0.171, 1.440, 0.033), **0.342 m apart**; elbow (−0.371, 1.240); wrist (−0.563, 1.049);
  `R_Weapon` (−0.604, 0.982, −0.005).
- Upper arm 0.283, forearm 0.273, wrist→`R_Weapon` 0.078. **Shoulder→grip reach 0.634 m.** Head bone at 1.572 m;
  eye dummy 58 at 1.654 m (11 §2.2).
- **Bone axes:** every limb bone points along its local **+X** (Max convention).
- **Handedness:**
  - The face looks toward **−Z** (eye dummy z = −0.066, 11).
  - The R_ bones are at **−X**.
  - The projection is D3D left-handed (08 §3.2).
  - So the game's model and world frames are **left-handed** [L]. OpenXR is right-handed: convert with one mirror, for
    example x → −x and quaternion (x, y, z, w) → (x, −y, −z, w).
  - Verify live: raise the right controller and the avatar's right hand must rise.

### 2.3 Behaviour graph facts (`c0000.anibnd/c0000.hkx`, 5 MB) [C]
- **Contents:**
  - 7,784 `hkbClipGenerator` and 1,917 FromSoft `CustomManualSelectorGenerator`.
  - One `hkbScriptGenerator` (the HKS script).
  - **One `hkbHandIkControlsModifier`** ("HandIKControlsModifier", two hands, `enable` = 1, `transformOnFraction` =
    1). Its `hands:0/1 controlData/targetHandle` are bound to variables 25/26, **`L_Foot`/`R_Foot`**.
  - **Two `hkbGetHandleOnBoneModifier`** (`_Left`/`_Right`) with animation bone indices **3 (`L_Foot_Target`) and 6
    (`R_Foot_Target`)**. The "hand" IK is foot placement.
  - One `hkbFootIkControlsModifier`.
  - Four FromSoft `CustomLookAtTwistModifier` (30°/60° limits), the lock-on upper-body twist.
  - Three `hkbTwistModifier`. `TwistModifier00` names bone 42 (`UpperPos`); the other two are variable-bound. None
    is on the arms [L].
- **No `hkbHandIkDriverInfo` / `hkbCharacterData`** is in `c0000.anibnd` or `c0000.chrbnd`. The class names exist in
  the eboot only as Havok reflection strings.
- **Behaviour variables of interest:** `WeaponRightType`, `WeaponLeftType`, `ChangeWeapon`, `ChangeHand`,
  `ChangeHandType`, `HandType`, `SelectArm(Type)`, `ModeChangeAttack`, `GunCondition`, `LeftWeaponAdd`,
  `EnchantGun`, `TwistUpperRootAngle`/`TwistLowerRootAngle`/`TwistMasterAngle`, `Guard*`.
- **HKS (`action/script/c0000.hks`).** HavokScript 5.1 bytecode. It names `Arm_R1/R2/L1/L2/...` request constants,
  `Act_ChangeMode`, and the trick-weapon states **`W_ChangeGimmick_RightSide_ModeA/ModeB`**,
  `W_Damage_Repelled_ModeA/B`. BB calls the two trick forms **ModeA/ModeB** and the transform **ChangeGimmick**.
- **`action/statenameid.txt`, `eventnameid.txt`, `variablenameid.txt`** (1012/1859/124 entries) list behaviour states,
  events and variables in plain text. Use them as the dictionary when walking the live graph.

## 3. Runtime: where the final bone transforms live (eboot RE)

### 3.1 The chain [C unless marked]
```
WorldChrMan* W = [0x593E878]                       ; FD4 singleton (306 of 400 assert sites, also 11)
PlayerIns*   P = [W + 0x60]                        ; [L] main player (GetRightCurrentWeaponId 0x1733E90 reads it)
bag            = [P + 0x3B0]                       ; module bag (module-creation code 0x1CB9AE0: mov r12,[r13+0x3b0])
beh            = [bag + 0x30]                      ; SprjChrBehaviorModule, 0x390 bytes, vtable 0x5735030, [beh+8] = P
holder         = [beh + 0x28]                      ; Havok character holder
pose           = holder + 0x38                     ; hkaPose (embedded), 108 bones
  +0x00 skeleton*  (hkaSkeleton: +0x10 name, +0x18 parentIndices hkArray<i16>, +0x28 bones hkArray<hkaBone 16 B>,
                    +0x30 bone count, +0x38 referencePose)
  +0x08 localPose  hkArray<hkQsTransform>   (48 B: t xyzw, q xyzw, s xyzw)
  +0x18 modelPose  hkArray<hkQsTransform>
  +0x28 boneFlags  hkArray<u32>  (1 = local dirty, 2 = model dirty)
  +0x38 modelInSync u8, +0x39 localInSync u8, +0x40 floatSlotValues
```
**Evidence.**
- **The debug skeleton drawer** 0x1E1A620 (menu "スケルトン描画 Draw Skeleton", flag `beh+0x382`; "Draw BoneName"
  `+0x383`; "Draw LookAt" `+0x386`):
  - It loads `r14 = [beh+0x28]`, calls 0xAC4F00 and 0xAC4C90 on `r14+0x38`, and reads bone names through
    `[[r14+0x38]]+0x28`.
- **`hkaPose::syncModelSpace`** 0xAC4F20 walks `[skel+0x30]` bones. It tests `boneFlags & 2`, reads the parent indices
  at `[skel+0x18]`, and composes `localPose`→`modelPose` with 48-byte stride.
- **`accessSyncedPoseModelSpace`** 0xAC59A0 syncs, sets every bone flag to 1, writes `+0x38 = 1, +0x39 = 0`, and returns
  `&modelPose`.
- The layout matches Havok 2014 headers (BotW stubs **[V]**, Skyrim SE for the shared parts **[V]**).

**Module bag map** (from the creation function 0x1CB9AE0; each slot's object identified through its vtable's FD4
runtime-class getter; 984 runtime classes mapped):

| bag+ | Module | Confidence |
|---|---|---|
| 0x18 | `SprjChrTimeActModule` (vtable 0x5735190) | **[C]** (0x1E27FB0 reads `[[chr+0x3B0]+0x18]` and pushes the anim ring; the same slot as ER in fromsoftware-rs **[V]**) |
| 0x20 | `SprjChrDataModule` (HP/stamina) | [L] |
| 0x28 | `SprjChrResistModule` | [L] |
| **0x30** | **`SprjChrBehaviorModule`** | **[C]** |
| 0x38 | `SprjChrBehaviorSyncModule` | [L] |
| 0x40 / 0x48 / 0x50 / 0x58 | Ai / SuperArmor / Talk / Event modules | [L] |
| **0x68** | **`SprjChrPhysicsModule`** (position, floor hit) | [L] |
| **0x80** | **`SprjChrActionRequestModule`** (input → action requests) | [L] |
| 0x88 | `SprjChrThrowModule` | [L] |
| 0x10 | `SprjChrBehaviorScriptModule` or `ActionFlagModule` (both vtables precede the store) | [S] |

Other vtables: `PlayerIns` 0x578F310 and `ChrIns` 0x578E2D0. Runtime-class globals: PlayerIns 0x5971538,
BehaviorModule 0x597A338, TimeActModule 0x597A5C8.

### 3.2 The AnimationToModel copy (the hook point)
- **0x1C375D0** `(rdi = anim owner, rsi = out, rdx = remap)`:
  1. Finishes a pending Havok animation job: if `[[rdi+0x20]+0xC] & 1`, a `lock cmpxchg [+0x10]` gate calls
     0x213C500.
  2. Takes `pose = [rdi+0x20] + 0x38` and `getSyncedPoseModelSpace`, and sets `[out+8] = 1`.
  3. For each Havok bone *i* with `j = remap[i] ≥ 0` (`int32` array at `[remap+0x10]`): converts `modelPose[i]` with
     0x83D4D0 (hkQsTransform → 4×4) and stores rows `(c0[r], c1[r], c2[r], t[r])`, *r* = 0..2, at `[out+0x10] + 48·j`.
  - Callers: 0x1AFABD3 (in 0x1AFA9C0, a virtual) and 0x2099194 (0x2099020, five callers).
- **0x1E19BC0** is the same loop with `pose = [rdi+0x28] + 0x38`, which is exactly `beh+0x28`. It has no static
  caller (it is reached indirectly).
- Which copy runs for the player is **not proven** [L]. The first runtime check logs both.
- **Hook windows** (all position-independent; `runtime_hooks.c`'s decoder accepts these encodings):

| Id | Site | Bytes (19) | State at the site | Use |
|---|---|---|---|---|
| **A1** | 0x1C37650 | `48 89 45 80 44 8b 68 08 48 8b 45 88 c7 40 08 01 00 00 00` | `rax` = `&modelPose` (synced), `r15` = pose, `rbx` = remap, `[rbp-0x78]` = out | Write the arm chain in Havok model space before the copy. Replay-after |
| **A2** | 0x1E19BFE | identical bytes | same | same |
| A3 | 0x1C37747 | `48 8b 05 92 c4 ba 03 48 8b 00 48 3b 45 d0` (14; RIP-relative load is relocated) | after the loop: `[rbp-0x78]` = out (FLVER-order 3×4), `[rbp-0x80]` = &modelPose | Alternative: patch the FLVER-order output directly |
| A0 | 0x1C375D0 / 0x1E19BC0 prologue | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58` (17) | entry | Logging only. Writing here races the pending-job sync |

- **Player filter:** `rax == &player_pose->modelPose`, where `player_pose = [[[P+0x3B0]+0x30]+0x28]+0x38`. One pointer
  compare per character per frame.
- **Thread:** the copy may run on an animation worker ("Animation SPU", `AnimThreadMan` names exist). The handler must
  be reentrant and must only read lock-free snapshots of the controller state.

### 3.3 Who reads the copy, and ordering [L/S]
- **Profiler categories** (UTF-16 table at 0x57BDAF0) name the per-character phases. In table order:
  `Animation SPU`, `Animation`, `Character Hit Raycast`, `ChrIns Update Pre/Post/Skip/Pose`, `Action(EzState)`,
  `Ragdoll`, **`AnimationToModel`**, **`Draw Update`**, **`ChrAttachSys`**, `AI`, **`IK`**, `SpEffect Update`.
- **Expected order [S]:** Animation (+ behaviour IK) → AnimationToModel → attachments and dummy polys → draw. An
  attack's hit capsules are built from weapon dummy polys (AtkParam `hit*_DmyPoly1/2`, §7.5), so pose writes at A1/A2
  reach them, at worst one frame late.
- Confirm by moving the hand and checking where (a) the weapon is drawn and (b) a hit lands (§9 W3/W6 exit tests).

### 3.4 Other confirmed engine anchors
- **TimeAct debug printer** 0x1E27B20 (`rdi` = TimeAct module):
  - Iterates a ring of 10 `{i32 anim, f32 start, f32 end, f32 duration}` at +0x20 from index `[+0xC4]` to `[+0xC0]`,
    and prints `"%sTimeAct ID:%03d_%06d"` (anim / 1,000,000, anim % 1,000,000).
  - `[+0xCC]`/`[+0xC8]` are an interpolation anim ID and time ("補間時間取得アニメID").
- **TimeAct ring push** 0x1E27FB0 `(edi = anim ID, rsi = ChrIns, xmm0–2 = three floats)` [C]:
  - It takes `bag = [chr+0x3B0]` and records only while `[[bag+0x30]+0x384] != 0` (the behaviour module's
    constructor sets it to 1).
  - It binary-searches the loaded TAE (`[[bag+0x18]+0x10]`: count +0x30, sorted anim pointers +0x38).
  - It writes `{anim, f0, f1, f2}` to ring slot `[+0xC0]`, then does `[+0xC0] = ([+0xC0]+1) % 10`. So the **newest
    entry is `(head+9) % 10`**.
  - It has no static caller; it is reached through a callback.
  - Its 15-byte prologue `55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 10` is a clean hook window for an
    **"animation started" event** (filter `rsi == P`), with no polling.
- **Weapon getters (ESD/Lua bindings):**
  - `GetRightCurrentWeaponId` 0x1733E90, `IsEquip` 0x1733E70, `IsInventoryEquip` 0x1733EF0.
  - AI-side `GetWepCateRight/Left` 0x165CC10/0x165CC40, `GetArmStyle` 0x165CC70, `IsBothHandMode` 0x165AB90.
  - `IsStartAttack`/`IsFinishAttack`/`IsHitAttack` (AI env flags +0x204/+0x205/+0x206/+0xA10).
- **Lua event API** (503 bindings registered around 0x1738000):
  - `PlayAnimation`, `ForcePlayAnimation` 0x172E290, `ChrResetAnimation`, `SetFootIKInterpolateType` (writes
    `[chr+0x58]+0x258/+0x25C`), `CreateSfx_DummyPoly`.
  - A last-resort way to play a specific animation.

## 4. The renderer side (alternative override point; diagnostics first)
- **Where palettes come from [C].**
  - `Rasterizer::BindBuffers` (`vk_rasterizer.cpp:2027`) reads each vertex-shader V# straight from guest memory
    (`vsharp.base_address`), then `buffer_cache.ObtainBuffer(...)` (:2128) uploads and binds it.
  - Palettes are stride-16 constant buffers of **3×4 matrices, 48 B per bone**. Character palettes are ≥ 640 B,
    weapon/prop palettes 96–384 B (`motion_history.h:39-51`, `docs/motion_vectors.md`).
  - The game writes them every frame into per-frame guest pages ("skinning output", `buffer_cache.cpp:113`).
- **Already capturable [C].** `FrameCapture::Buffer(...)` (`vk_rasterizer.cpp:2116`) dumps every VS buffer of a
  captured frame, so palettes can be analysed offline without new code.
- **Why not override here.**
  - It changes the picture only; hits, attachments and the camera-anchor bones keep the old pose.
  - Palette entries are in **per-part** bone order (§2.2) and probably pre-multiplied by inverse bind [S], so it needs
    per-part maps.
  - Identifying the player's draws needs a CPU hint anyway.
- **Use it for:**
  1. Verifying CPU overrides: a capture with IK on vs off shows the changed rows.
  2. **Finding the palette writer:**
     - The GPU page-fault path already sees the writer: probe.c:147-149 hands `ExceptionInformation[1]` and the context
       to `bbgpu_handle_fault`.
     - A debug option that logs `ContextRecord->Rip` for faults inside one watched palette page names the guest
       function that builds palettes.
     - That function is the last CPU point before skinning.

## 5. Arm IK design

### 5.1 Data flow (fits 12 §2.10's host/logic split)
```
guest A1/A2 site → trampoline → hook_arm_pose(ctx)            bb-probe.exe, fixed (runtime_hooks.c registry entry)
   filter: rax == &player_pose.modelPose ; read localInSync, style, anim (§7)
   copy needed model transforms → BbVrArmIo (plain data: 57 bones = 2 × 27 arm subtree + Spine2/Neck/Head)
   bbgpu_vr_arms(&io)  → Xr::ArmHook → host → logic ComposeArms(hands, tunables, &io)   (hot-reloadable math)
   write back modelPose[b] for the arm subtrees; boneFlags[b] |= 1; [pose+0x39] = 0
```
- **Build it like the head hook already in the working tree** (`src/runtime_vr_head.c`, math in `src/vr_head_math.h`,
  tests in `tests/test_vr_head.c`, installed through `bb_hook_set_camera_impl`):
  - Add `src/runtime_vr_arms.c`, `src/vr_arm_math.h` and `tests/test_vr_arms.c`, and an impl setter for the A1/A2
    registry entries.
  - Env flags, off by default: `BB_VR_ARMS=1` (write), `BB_VR_ARMS_LOG=1` (once per second), and
    **`BB_VR_FAKE_HANDS=...`** (scripted grip poses, "@file" re-read on change, like `BB_VR_FAKE_HEAD`), so IK runs in
    the game with no headset.
  - **Use the same "forward" as the head hook:** the cinema screen's yaw from `bbgpu_vr_views` (`BbVrViews.screen`).
    Head and hands then share one body frame.
  - **Spaces:** `BbXrInput` poses are LOCAL, while views can be STAGE (`BB_XR_STAGE=1`); convert one into the other
    before mixing them.
  - Move the math into 12's hot-reloadable logic module later.
- **`BbVrArmIo`** carries:
  - In: model-space QsTransforms of Spine2, Neck, Head, both clavicle → finger → weapon chains and the twist helpers,
    plus the local transforms of the children.
  - In: the frame tag, weapon style, IDs, current anim and time.
  - Out: new model transforms and a per-hand weight.
- **The logic never dereferences guest pointers**, the same rule as `BbVrGameCamera`.
- **Pose source:** the controller grip pose predicted for the display time of the frame being simulated, tagged like
  the head pose (08 §7.4, 02 §4). The grip pose must come from the same prediction as the camera, or hands swim against
  the view.

### 5.2 Frames and calibration
- **OpenXR → game:** apply the handedness mirror (§2.2), then go to avatar model space.
- **Third person ("puppet").**
  - **Body frame.** Estimate the user's body frame from the head: neck = HMD position + yaw-only offset
    (0, −0.20, +0.08)·k. Body yaw = HMD yaw low-passed (~0.5 s) with a ±40° dead zone.
  - **Map positions:** shoulder-relative into the avatar's shoulder frame, where shoulder = the current
    `UpperArm` joint from the live pose and orientation = Spine2 yaw.
  - **Map rotations:** unchanged after the mirror. No world transform is needed.
- **First person (registration):** hands must appear where the real hands are, relative to the eyes:
  `hand_model = worldFromModel⁻¹ · camera_world · (HMD⁻¹ · controller)`. This needs `worldFromModel`, which is 11's RE
  #2 (player position and facing; `SprjChrPhysicsModule` at bag+0x68 is the lead). Scale is 1.0 as in 11.
- **Calibration (once, stored in `vr_tune.ini [arms]`):**
  1. Stand or sit upright for 1 s, which gives the HMD height.
  2. Hold a T-pose for 1 s, which gives the grip-to-grip span D.
  3. Compute `s = 1.61 m / D`, clamped to 0.8–1.25. The avatar's own grip-to-grip span is 0.342 + 2 × 0.634 =
     1.61 m, so this assumes the user's shoulders scale with their arms. The avatar's proportions are those of a
     ~1.78 m adult [S].
  - **Seated:** shoulders = HMD − (0, 0.20, −0.08) in the yaw frame, ±0.171·k sideways.

### 5.3 Solver
1. **Wrist target.** The controller grip pose gives the target pose of `R_Weapon`, as `grip ∘ C ∘ D_w⁻¹`, where:
   - *C* is one global grip-axes→R_Weapon-axes rotation, fitted once. OpenXR grip pose: −Z runs through the tube of
     the closed fist (little finger → thumb), +X is normal to the palm [L, re-check against the spec text].
   - *D_w* is the weapon's attach-dummy offset on R_Weapon (§7.3).
   - The `R_Hand` target follows as `R_Weapon_target ∘ local(R_Weapon)⁻¹`.
2. **Two-bone IK** with a = 0.283 and b = 0.273:
   - Set d = min(|T−S|, 0.995(a+b)).
   - Elbow interior angle from the law of cosines; shoulder offset `acos((a²+d²−b²)/2ad)`.
   - Elbow plane from a **pole**: down (−0.85), back (+0.35, toward +Z in model space) and outward (0.4), all in the
     body frame. Rotate the pole about the S→T axis by 0.5 × the wrist roll beyond neutral, so turning the palm down
     raises the elbow. Low-pass the pole at 8–12 Hz.
   - Build the UpperArm rotation so local +X points to the elbow and local **Y** is the hinge normal [L]. Build the
     Forearm likewise toward T.
3. **Out of reach.**
   - Lift the clavicle (≤ 15° up/forward) before clamping.
   - Beyond that, let the hand lead along the S→T ray (soft clamp) and fade haptics to signal it.
4. **Wrist.**
   - The hand takes the controller's orientation.
   - Wrist limits: flexion ±75°, deviation ±30°.
   - Decompose the hand-vs-forearm rotation into swing and twist (about forearm +X). Give the twist to
     `Support_*_ForeArmTwist` (100%) and `ForeArmTwist1` (50%), and the upper-arm twist to `UpArmTwist`/`UpArmTwist1`
     (0% and 50%). That avoids the candy-wrapper look. The game bakes these helpers into its animations [L], so we must
     set them.
5. **Write.**
   - Descendants (fingers, `*_Weapon`, helpers) are recomputed as `model = parentModel ∘ local`. The Havok bone order is
     parent-before-child, so one forward pass suffices.
   - The locals come from `localPose` when `[pose+0x39] = 1`; otherwise derive them from the pre-edit model pose.
   - Then set `boneFlags |= 1` on the written bones and `[pose+0x39] = 0`, which is exactly what 0xAC59A0 does for the
     whole pose.

### 5.4 Blend policy (per hand, from §7.4's state)

| State (source) | Weight | Blend |
|---|---|---|
| Idle, walk, run, guard idle (anim group a00 low IDs, no attack/dodge flags) | 1 | 150 ms in |
| **Attacks**: sub-ID 30000–49999 in the weapon group, or any type-1 window active | 0 by default (P1); 1 in "hand-driven" mode (§8 P2) | 80 ms out at anim start, 200 ms back in after the `Cancel-RH Attack` flag (ChrActionFlag 4) |
| Gun shot (left; 51000–52999 in gun groups) | 0.6 left (keep the aim), 1 right | — |
| Dodge/quickstep (a00 7000s, 35/39 carry `Invincible`) | 0.3 | fast |
| Item use, gestures, visceral/throws, ladders, doors, damage, death, cutscenes | 0 | — |
| Weapon style change (a00 19000s carry `SetWeaponStyle`) | 0 | until the anim ends |

The anim ranges are [C] from the TAE files; their meanings are [L/S] until the runtime readout confirms them (§9 W0).

### 5.5 Tunables (`vr_tune.ini [arms]`, Tier 0 of 12)
`enabled`, `mode = puppet|fp`, `scale`, `pole_down/back/out`, `pole_roll_gain`, `clavicle_max_deg`, `reach_soft`,
`wrist_flex_max`, `twist_share_fore/up`, `blend_in_ms/out_ms`, `attack_weight`, `grip_rot_offset_deg[3]`,
`grip_pos_offset_m[3]`, `debug_log`.

## 6. Hands and fingers

### 6.1 What the controllers and runtimes offer
- **Today's state [C]:**
  - Runtime "Pimax OpenXR 0.1.0", headset "Pimax Dream Air", both controllers `/interaction_profiles/oculus/touch_controller`
    (sandbox and game logs).
  - The left stick's Y axis reaches only ~0.3 (the bridge rescales it).
  - `xr_input.cpp`'s gameplay actions bind `trigger/value`, `squeeze/value`, buttons, sticks, grip and aim poses, and
    haptics, but **no `/touch` path**. It reads linear velocity only.
  - The working tree's `BB_XR_INPUT_DIAG=1` (default on in the sandboxes) binds every Touch-profile path in a second
    action set and logs each path's first report.
- **Measured on the user's Dream Air controllers** (session `sandbox_tune.log`, Pimax OpenXR 0.1.0) **[C]:**

| Path (Touch profile) | Left | Right |
|---|---|---|
| `thumbstick/touch` | reported | reported |
| `x/touch`, `y/touch` / `a/touch`, `b/touch` | reported | reported |
| `trigger/touch` | reported | **not seen**, although `trigger/value` reached 1.00 |
| `thumbrest/touch` | not seen | not seen |
| `squeeze/value`, `trigger/value`, clicks, stick axes | reported | reported |
| `menu/click` (left) | not seen (Pimax keeps it for its system menu) | — |

  So the thumb has full capacitive coverage, and the left index has it. Whether the right index touch is missing,
  or simply was not exercised alone, needs a 10 s targeted test: rest the finger on the trigger without pressing.
- **Touch profile paths** (spec source `semantic_paths.adoc` **[V]**; the bridge's `kTouchPaths` uses the same list,
  and Pimax's runtime accepted it in the diagnostic run [C]):
  - Both hands: `trigger/value`, **`trigger/touch`**, `squeeze/value`, `thumbstick` (+x/y), `thumbstick/click`,
    **`thumbstick/touch`**, **`thumbrest/touch`**, `grip/pose`, `aim/pose`, `output/haptic`.
  - Right: `a/click`, **`a/touch`**, `b/click`, **`b/touch`**, `system/click`.
  - Left: `x/click`, **`x/touch`**, `y/click`, **`y/touch`**, `menu/click`.
  - OpenXR 1.1 core adds `trigger/proximity` and `thumb_resting_surfaces/proximity`. It obsoletes
    `XR_FB_touch_controller_proximity` (`trigger/proximity_fb`, `thumb_fb/proximity_fb`) **[V]**.
  - `/interaction_profiles/meta/touch_controller_plus` adds **`trigger/curl_meta`** (0 fully pointed … 1 flat on the
    trigger), `trigger/slide_meta`, `trigger/force` and proximity paths **[V]**. That is a ready-made index curl where
    a runtime offers it.
- **Hand joints [V].**
  - `XR_EXT_hand_tracking` gives 26 joints: PALM, WRIST, THUMB (METACARPAL, PROXIMAL, DISTAL, TIP), then INDEX,
    MIDDLE, RING, LITTLE, each METACARPAL, PROXIMAL, INTERMEDIATE, DISTAL, TIP.
  - `XR_EXT_hand_joints_motion_range` (UNOBSTRUCTED / CONFORMING_TO_CONTROLLER; a runtime must support both for every
    controller profile that supplies joints) and `XR_EXT_hand_tracking_data_source` (UNOBSTRUCTED / CONTROLLER;
    CONTROLLER only while the controller is held) are how runtimes expose controller-driven curls. The spec does not
    say how controller joints are derived.
- **What we don't know yet:** which instance extensions "Pimax OpenXR 0.1.0" lists (hand tracking or not). **Fix:**
  log `xrEnumerateInstanceExtensionProperties` at `Init` (11 §6.3 asks for the same). See §6.5 for the web results.

### 6.2 Curl model (no hand tracking)

| Finger (bones) | Input | Curl c (0 open … 1 fist) |
|---|---|---|
| Index (`*_Finger1/11/12`) | trigger value v, trigger touch t (where the hand reports it; else t = v > 0.02) | `t ? 0.25 + 0.75·v : 0` (pointing when not touching) |
| Middle/ring/pinky (`Finger2x/3x/4x`) | squeeze value g | `0.15 + 0.85·g`; ring/pinky +0.05 lag (cosmetic) |
| Thumb (`Finger0/01/02`) | touch of A/B (X/Y), thumbstick, thumbrest; button clicks | no touch: 0 (thumbs-up); any touch: 0.55 (resting); click: 0.75 |

Angles per joint at c = 1: index/middle/ring/pinky MCP 70°, PIP 95°, DIP 60°; thumb CMC 25° in, MCP 40°, IP 50°.
Bend about local **Y** [S]. The bind pose's hinge bends are about local Y: the elbow by 7.1° and the thumb tip
(`Finger02`) by 1.5°. The other finger joints have no bind bend, so confirm the axis and sign in the sandbox (§9 W2).
The thumb's frame is rotated, so treat it separately. Spread is 0.

### 6.3 Combining with the game's hand animations
- **The right hand always holds a weapon,** and the game animates a closed grip around it. Default: fingers keep the
  animation (weight 0).
  - When the weapon is sheathed (style 0) or in menus: weight 1.
  - Option `fingers.right = trigger`: the index finger alone follows the trigger.
- **Left hand.**
  - **Pistols/blunderbuss/cannon:** the index finger follows the trigger value, so it squeezes the in-game trigger.
    The gun model `WP_A_2500` even has a `B_Trigger` bone; drive it cosmetically by rotating that bone in the weapon's
    own skeleton, a later step.
  - **Torch/shield:** the animation grip.
  - **Empty** (gun unequipped): the full curl model.
- **Fist pose for punches/visceral:** squeeze > 0.8 and trigger > 0.8 → c = 1 on all fingers.

### 6.4 Bridge additions (the next bridge ABI, 5, since 4 is now app-rendered stereo; or 12's `BbXrRawHand`)
- **Per hand:**
  - `trigger`, `squeeze` (floats); touch bits {trigger, thumbstick, thumbrest, A/X, B/Y}.
  - `angular_velocity[3]` (`XrSpaceVelocity` already requested; add `XR_SPACE_VELOCITY_ANGULAR_VALID_BIT` handling)
    and `linear_velocity[3]`.
  - Optional `joints[26]` when `XR_EXT_hand_tracking` exists.
- **The curl math** lives in the logic module, so it hot-reloads.
- **Action bindings** need a new session (12 §4.3), so add all `/touch` actions at once. A single unknown path makes
  the runtime reject the whole suggestion for that profile; the bridge's `Suggest` already retries without the
  optional paths and logs it.

### 6.5 Web results (Pimax, other runtimes, conventions)
- **Dream Air controllers.**
  - Pimax's FAQ lists A(X), B(Y), Home, Pimax key, trigger, grip, joystick and "Capsense", without naming zones
    **[V]**. MIXED's copy of the spec sheet adds "Five Finger Tracking" **[V]**.
  - Pimax says its Crystal controllers have finger-tracking hardware, in beta **[V]**.
  - No public source names the capacitive zones or says whether grip is analog. §6.1's measurement is the best data
    there is: grip reads as an analog value (0.42/0.71 seen).
- **Hand tracking.**
  - Pimax Play EVO (Jun 2026) describes "gesture-based input" from the Dream Air's onboard tracking cameras **[V]**.
  - Whether it reaches OpenXR apps as `XR_EXT_hand_tracking`: **not found**. Neither is a published extension list
    for the Pimax Play runtime, nor any statement that the Dream Air maps to the Touch profile (our logs show it does).
  - PimaxXR (unofficial, abandoned for Pimax's own runtime) once had Index finger sensing and an Ultraleap toggle
    **[V]**.
- **Other runtimes.**
  - SteamVR: a 2021 community list marks `XR_EXT_hand_tracking` and `XR_EXT_hand_joints_motion_range` as supported
    **[R]**. Controller-driven joints on current builds are unconfirmed.
  - Valve's `openvr.h` warns that the "WithController" range "might not allow a closed fist" **[V]**.
  - Meta: Capsense feeds `XR_EXT_hand_tracking_data_source` with the CONTROLLER source, and camera poses win when both
    are present **[V]**. On PC/Link, hand tracking is reportedly behind developer mode **[R]**.
  - VDXR: not found.
- **Conventions.**
  - Meta's controller-driven hand sample: index curl = trigger axis **[V]**.
  - VRChat on Touch: trigger = index, grip = middle (others follow), thumb up/down from touch **[V]**.
  - "Touched but not pressed" curl values are not documented anywhere; §6.2's 0.25/0.55 are starting points to tune
    **[S]**.
- **Conclusion.** Build the curl model on the inputs §6.1 measured. Probe `XR_EXT_hand_tracking` (with its data-source
  and motion-range companions) and `meta/touch_controller_plus` at startup. Use joints when a runtime supplies them,
  and keep the curl model as the fallback.

## 7. Weapon recognition

### 7.1 Equipped weapons [C code, L meaning]
```
equip = [P + 0x4B8]            ; PlayerIns vtable 0x578F310 slot +0x2E8 = 0x1D077D0 "mov rax,[rdi+0x4b8]; ret"
style = [equip + 0x08]         ; compared with 3 at 0x1C95F47/0x1E51208, then reads the right weapon (→ two-handed)
slotL = [equip + 0x0C]         ; 0..2
slotR = [equip + 0x10]         ; 0..2
ids   = int32[6] at equip+0x64 : L0, R0, L1, R1, L2, R2
right = ids[2*slotR + 1], left = ids[2*slotL]
```
- **Evidence.** `GetRightCurrentWeaponId` (0x1733E90) does exactly
  `[ [P]->vfunc_0x2E8() + 0x64 + 4·(2·[+0x10]+1) ]`. About 20 other sites use the same `+0xC`/`+0x10` → `+0x64` pattern
  (the sequence ending at 0x1CF55C0 loads `[r13+0x4B8]` directly).
- **Weapon family:** `id / 100000`. Upgrades are +100 per level, Uncanny +10000, Lost +20000 (**[V]** Paramdex names
  and a save-editor table, matching the param rows).

### 7.2 Trick form [L]
- **The values.** The DSAnimStudio BB template defines TAE event **32 `SetWeaponStyle`**: 0 none, 1 right one-handed,
  2 left two-handed, 3 right two-handed, **4 one-hand (left weapon transformed)**, **5 one-hand (right weapon
  transformed)** **[V]**.
- **The TAE data agrees [C]:**
  - The transform attack 33300/33310 sets **5** in a112 (Saw Cleaver) and a127 (Threaded Cane), and **3** in a103
    (Hunter Axe). The group-to-weapon mapping is confirmed by each weapon's anibnd (§7.3).
  - 43300/43310 in a127 sets **1** (whip back to cane).
- **Read the form as `style`** (equip+0x08): right normal = 1, transformed = 3 or 5. Verify with three weapons (§9 W0).
- **The sub-models swap with the form** (EquipParamWeapon `dmypolyId_Slot{0-3}{Right,Left}{Hang,FormA,FormB}`).
  Saw Cleaver: sub-model 0 at dummy 300 in form A, sub-model 2 at 300 in form B. Hunter Axe: form B sub-model 2 at 320.
- **Backups:**
  - The behaviour variable `ModeChangeAttack` and the state names `W_ChangeGimmick_*_ModeA/B`.
  - The anim sub-ID range (30000s = form A, 40000s = form B).

### 7.3 Per-weapon table (from the user's params and models) [C]
Columns:
- *Anim group*: **[C]** for the Saw Cleaver, Hunter Axe, Threaded Cane and Hunter Pistol. Each weapon's own `.anibnd`
  names the player animations it accompanies: `WP_A_0800.anibnd` holds `a112_033300.hkx` … and `a000_019000.hkx`,
  `WP_A_1020` holds a103, `WP_A_1070_2` holds a127, `WP_A_2500` holds a117. The other rows are **[R]** (a Nexus
  BB HKX dump); derive them all from the weapon anibnds, as `scripts/vr_assets/hitbox_extract.py` does.
- *Attach* is the character dummy for form A / form B. **Sub** is the sub-model index.
- Every right-hand dummy is on `R_Weapon` and every left one on `L_Weapon`, except 344 on `L_Hand` and 350 on
  `L_Forearm`.
- The offsets are the dummy position in the bone's local frame (bind pose).

| Weapon | Row | wepmotion | Model `WP_A_` | Form A attach | Form B attach | Offset of dummy | Anim group |
|---|---|---|---|---|---|---|---|
| Saw Cleaver | 7000000 | 28 | 0800 | S0 → 300 | S2 → 300 | 0 | a112 |
| Saw Spear | 7100000 | 38 | 1011 | S0 → 300 | S2 → 300 | 0 | a122 |
| Hunter Axe | 5000000 | 30 | 1020 | S0 → 300 | S2 → **320** | 320: (0, 0.047, −0.007) | a103 |
| Threaded Cane | 22000000 | 127 | 1070 | S0 → 300 | S2 → 300 | 0 | a127 |
| Kirkhammer | 8000000 | 48 | 1031 | S0 → 300, S1 on back 416 | S2 → **322** | 322: (0.014, −0.002, −0.007) | a105 |
| Ludwig's Holy Blade | 8100000 | 34 | 1030 | S0 → 300, S1 on back 2401 | S2 → 322 | | a120 |
| Burial Blade | 5100000 | 37 | 1021 | S0 → 300 | S2 → 320 | | a121 |
| Blade of Mercy | 4000000 | 27 | 1060 | S0 → 300 | S2 → 300, S3 → **340 (left hand)** | | a113 |
| Rakuyo | 27000000 | 144 | 1703 | S0 → 300 | S2 → 300, S3 → 340 | | a144 |
| Chikage | 2000000 | 29 | 1063 | S0 → 300 | S2 → 320 | | a114 |
| Rifle Spear | 10000000 | 36 | 1040 | S0 → 300 | S2 → 300 | | a104 |
| Reiterpallasch | 10100000 | 40 | 1041 | S0 → 300 | S2 → 300 | | a124 |
| Beast Claw | 9000000 | 47 | 1001 | S0 → **304** | S1 → 304, S2 → 344 | 304: (0.006, 0.006, −0.026) | a106 |
| Stake Driver | 11000000 | 22 | 1061 | S0 → **309** | S1 → 309 | 309: (0.090, 0.032, 0.008) | a116 |
| Logarius' Wheel | 12000000 | 35 | 1052 | S0 → **306** | S2 → **326** | 306/326: (**0.415**, 0, 0.010) | a110 |
| Tonitrus | 13000000 | 33 | 1051 | S0 → 300 | S0 → 320 | | a109 |
| Whirligig Saw | 31000000 | 148 | 1707 | S0 → 300 | S2 → 320 | | a148 |
| Hunter Pistol | 14000000 | 24 | 2500 | L: 347 | L: 327 | 347: (0.009, −0.021, −0.004) | a117 |
| Hunter Blunderbuss | 6000000 | 41 | 2600 | L: 345 | L: 325 | 345: (−0.013, −0.003, 0) | a102 |
| Cannon | 15000000 | 31 | 2700 | L: 348 | L: 328 | | a118 |
| Torch | 20100000 | 25 | 1800 | L: 340 | — | 0 | a107 |
| Wooden Shield | 19000000 | 20 | 1501 | L: 341 | — | | — |

- Sheathed dummies are 400–417 on the spine/pelvis case bones. 2401–2461 are special back/hip slots.
- **Generate the full table from the user's files; don't hand-maintain it.**
  - Another track is already writing `scripts/vr_assets/`. `catalog.py` maps weapon → model and names.
    `hitbox_extract.py` produces, per weapon, the attach dummies per form, the TAE attack windows (event 1), blade
    trails (event 118) and the AtkParam capsules, as "bbhitbox" JSON under `out/vr_assets/hitbox/`.
  - Add what this file needs: each attach dummy's full local frame on `R/L_Weapon` (position + fwd/up), the
    `SetWeaponStyle` value per animation, and the cancel/parry/invincibility flags (§7.4).
  - The output stays under `out/`; game data is never committed (PLAN §5.7).
- **Weapon-model dummies** (`WP_A_*.flver`, model space):
  - **100** tip, **110** mid-blade/shaft, **120** grip (≈ origin), **122** the pommel end of the handle (Hunter Axe
    (0.03, 0.23, −0.01), Threaded Cane (0, 0.12, 0)). The axe's AtkParam closes its shaft capsule at 10122.
  - **130/131** blade back/edge, **206** blood/SFX points.
  - Pistol `WP_A_2500`: 100, 131, 206 on `B_Barrel`, 208, 215 at (0, 0.05, −0.44) (muzzle candidate [S]).
  - Each weapon also has its own skeleton (the Saw Cleaver's `handle`/`edge`/`stopper`; the axe's `B_Shaft_00..02`) and
    an `.anibnd` for the trick mechanism.

### 7.4 Current animation and attack state
- **Current animation [C].** `tam = [bag+0x18]`; latest = `(([tam+0xC0] + 9) % 10)` (the push function 0x1E27FB0
  writes at the head and then advances it, §3.4); `anim = [tam + 0x20 + 16·latest]`. The entry's `+0x24` float is
  labelled "srt" by BB's debug print and `play_time` by fromsoftware-rs (ER); `+0x2C` is the duration. For
  edge-triggered state, hook the push function instead of polling.
  - TGA's ER table reads slot 0 instead **[V]**; log both.
  - Full ID = group × 1,000,000 + sub ([C], from the debug format). The group is the TAE number: `a112_030000`.
- **Ranges [C from the TAE data, L/S meaning]:**

| Group | Sub-ID | Content |
|---|---|---|
| a1xx weapon groups | 30000–39999 | form-A attacks (`InvokeAttackBehavior`, judge IDs 0, 10, 20, … per combo step) |
| | 33300/33310 | transform attack (A→B) |
| | 40000–49999 | form-B attacks |
| | 43300/43310 | B→A |
| gun groups | 51000–52999 | shots: bullet event at 0.33 s (51000), 0.37 s (52150), 0.17 s (52550) |
| a00 | 7000s | dodges (35/39 have `Invincible`) |
| | 19000s | trick transforms outside attacks, plus style/weapon change (138 `SetWeaponStyle`). [C] per weapon: Saw Cleaver 19000/19010, Hunter Axe 19040/19250, Threaded Cane 19150/19300 (named in each weapon's anibnd) |
| | 9000–12999 | likely damage |
| | 100000–104999 | events/gestures [S] |

- **Hit windows (type 1)** for the first R1:

| Weapon | Hit window | Anim length | Next-combo cancel |
|---|---|---|---|
| Saw Cleaver (a112) | 0.37–0.47 s | — | — |
| Hunter Axe (a103) | 0.53–0.67 s | 2.77 s | ~0.77 s (`Cancel-RH Attack`, ChrActionFlag 4) |
| Threaded Cane (a127) | 0.43–0.53 s | — | — |

- **Precompute per group and anim** (extend `hitbox_extract.py`'s TAE pass rather than adding a second reader): the
  type-1 windows (already there), type-0 cancel flags 4/16/26, parry flag 5, invincibility 8, and `SetWeaponStyle`.
- **Event types** (DSAnimStudio BB template **[V]**, consistent with the data **[C]**):
  - 0 `ChrActionFlag`, 1 `InvokeAttackBehavior` {AttackType, AttackIndex, BehaviorJudgeID, DirectionType, StateInfo},
    2 `InvokeBulletBehavior` {DummyPolyID, BehaviorJudgeID, …}, 5 `InvokeCommonBehavior`, 32 `SetWeaponStyle`,
    33 `SwitchWeapon`, 118 `SpawnFFX_Blade`, 224 `SetTurnSpeed`, 233/237 `ChangeChrDrawMask`, 300
    `ActivateJumpTableEarly`, 304 `InvokeThrowAttackBehavior`, 307 `InvokePCBehavior`, 315 `EnableTwistModifier`,
    316 `InvokeFixedRotationDirection`, 318 `InvokeGunBehavior`, 320 `PlayerInputCheck`.
  - `ChrActionFlag` IDs: 1/4 RH-attack input/cancel, 9/16 LH, 25/26 dodge, 5 parry possible, 8 invincible, 24 hyper
    armour, 7 disable turning, 32 generic cancel.

### 7.5 How hits are computed (what "physics hits" must feed) [C data, L runtime flow]
- **The chain [C, traced in the user's params]:**
  - EquipParamWeapon `behaviorVariationId` (Saw Cleaver 700, Hunter Axe 500, Threaded Cane 2200) and the TAE
    `BehaviorJudgeID` select the BehaviorParam_PC row **100,000,000 + variation × 1000 + judge**.
  - That row's `refType` 0 / `refId` select an **AtkParam_Pc** row: up to four spheres/capsules (`hit{0..3}_Radius`,
    `hit*_DmyPoly1/2`), `hitSourceType`, damage, and the row's `stamina` cost.
- **Examples:**

| Attack | BehaviorParam_PC → AtkParam_Pc | Stamina | Capsules (radius, dummy pair) |
|---|---|---|---|
| Saw Cleaver R1 (judge 0) | 100700000 → 700000 | 20 | 0.40 m, **10100–10110** |
| Saw Cleaver form B (judge 400) | 100700400 → 700400 | 24 | 0.40 m, **12100–12120** |
| Hunter Axe R1 | 100500000 → 500000 | 25 | sphere 0.50 m at 10100; 0.20 m, 10130–10122 |
| Threaded Cane whip (judge 400) | 102200400 → 2200400 | 17 | four capsules along 12100–12130–12110–12120 |

- **The dummy IDs are weapon-model dummies with a sub-model prefix**: 10xxx = sub-model 0 (form A), 12xxx = sub-model 2
  (form B), the same sub-models as EquipParamWeapon's `Slot0`/`Slot2` [L]. The capsules therefore move with the
  attached weapon model, which follows `R_Weapon`, so a pose written at A1/A2 moves the hit capsules too [L].
- **Bullets.** BulletParam has `shootAngle`, `shootAngleXZ`, `lockShootLimitAng`, `homingAngle` and `FollowType`
  [C]. Bullets launch relative to the character facing and lock target, not along the gun barrel [S]. Changing that
  needs a bullet-spawn hook (§10 #7).

## 8. Weapon logic in VR, ranked by value ÷ effort

| # | Feature | Needs | Value | Effort [S] | Risk |
|---|---|---|---|---|---|
| 1 | **Readout:** right/left weapon, form, current anim + phase (log + debug HUD); per-weapon gesture sets | §7 reads in a cheap hook (in-game step or A1) | High: gates everything below | 2–4 days | Low |
| 2 | **Gestures v2 (bridge only):** onset-timed swings (fire when the swing *starts*: speed > 1.2 m/s and accelerating, dominant direction stable for 40 ms); direction classes (horizontal/vertical → R1, overhead or two-hand → R2, forward thrust → R1 = visceral when an enemy is staggered); **wrist-flick → L1** (angular speed about the forearm > 9 rad/s, < 150 ms, hand not translating); **gun: thrust or trigger → L2**; **two-hand grab** (left grip closed within 12 cm of the right hand on a two-handable weapon → L1, transform to the two-handed form); vial at the mouth (existing) | Angular velocity, the readout (#1) for weapon-specific rules | High | 1 week | Low; tune with 12's replay |
| 3 | **Arm IK + weapon follows hand outside attacks** (§5) | A1/A2 hook, IK, blend policy | Very high (presence; third and first person) | 2–3 weeks incl. RE confirm | Medium (pose timing, thread) |
| 4 | **Fingers** (§6) | Touch actions, finger write in the same hook | Medium | 3–5 days | Low |
| 5 | **Two-handed forms (style 3):** when the left controller is within 15 cm of the shaft, the left hand IKs to the second grip; otherwise it is free. **Learn the second grip from the game itself:** in the form-B idle, sample the animated `L_Hand` relative to `R_Weapon` from the pose *before* our write, and cache it per weapon and form. Fallback: a point on the shaft line through the model's 120/122/100 dummies | The A1/A2 hook (already reading the pose) | Medium | 3–5 days | Low |
| 6 | **Gun aim**, three steps: (a) 11 §5.1's aim-to-face stick pulse; (b) lock-on by hand ray (11 RE #8); (c) bullet direction = gun barrel (hook the bullet spawn, override direction for the player's bullets) | (a) bridge; (b) lock manager; (c) bullet RE | High | (a) 1 day, (b) 3–5 days, (c) 1–2 weeks | (c) medium |
| 7 | **Parry** = shoot the attacker during its windup: works with #2/#6. Add haptic confirmation on a successful parry (watch the enemy's stagger anim or the visceral prompt) | — | Medium | 2 days | Low |
| 8 | **Hand-driven hits inside the game's window ("P2")**: keep IK weight 1 during attacks, so the VR weapon sweeps the game's capsules. Align timing either (i) by pressing R1 at swing onset (#2) and raising anim speed during VR swings, or (ii) with a TAE event hook (erfps2-style `exec_one`) that opens the attack behaviour when the swing peaks and suppresses the late one | §10 #6, #8 | High if it lands | 3–6 weeks | High: desync, AI reads the animation |
| 9 | **ERVR-style animation-free attacks ("P3")**: invoke the attack behaviour (BehaviorParam→AtkParam) directly from swing speed, no player animation; damage ∝ speed | Attack-behaviour invocation RE, stamina/poise rules | Highest immersion | 6+ weeks | Very high: AI reacts to an animation that never plays, rolls mid-swing, balance (ERVR's own limits **[R]**) |
| 10 | GPU-only palette override | §4 | Low | 1–2 weeks | Hits disagree with the picture; not recommended |

**P1 (default):** game-driven hits; IK fades out for attack animations and back in at the cancel window. Without
hooks, gestures alone (#2) already make combat physical. Arms (#3) make it look right everywhere except mid-swing.

## 9. Phased plan, exit tests and fast testing

| Phase | Content | Exit test | Fast test loop |
|---|---|---|---|
| **W0 Readout** (2–4 days) | Hooks `bb_hook_log_once` on A0/A1/A2 + the in-game step: log P, bag, beh, pose (bone count 108, bone 0 "Master"), which copy fires for the player, equip block, TimeAct ring. Extend `scripts/vr_assets` with the §7.3/§7.4 fields | Ten scripted actions (equip each slot, transform, R1, R2, L2, dodge, vial) each produce the expected log line (`R=Saw Cleaver 7000000 style 1→5; anim a112_033300 t=0.21`) | Tier 3 boot (12 §5) once, then CE on `bb-probe.exe` against the logged pointers; `bbctl` scripted pad (12 §3.3) for repeatable actions |
| **W1 Gestures v2** (1 week, bridge only) | #2, with per-weapon rules from W0 | Replayed recordings: ≥ 90% of 50 swings give the intended button; < 1 false fire/min while idle or looking at the hands; flick → L1 in 9/10; two-hand grab 9/10 | 12's `.bbxrrec` record/replay through the logic DLL, golden JSONL (`tests/test_xr_logic.py`); sandbox shows the fired button on the hand markers |
| **W2 Pose read + sandbox avatar** (1 week) | Read-only hook copies the player's 108 model transforms into a ring the bridge can read. In the **3D sandbox** being built (`gpu/xr_bridge/sandbox3d/`, which loads the hunter body and weapons that `scripts/vr_assets/extract.py` writes to `out/vr_assets/`, right-handed with X mirrored, the same convention as §2.2), drive the real skinned hunter with the IK and show the `bbhitbox` capsules on the weapon | Bone lengths read live match §2.2 within 1 mm; in the sandbox the IK wrist error is < 5 mm inside reach; no NaN over a 10 min replay | Sandbox Tier 0/1 (12 §2): edit `vr_tune.ini [arms]`, rebuild, see it in seconds; mirror placement (the avatar faces you) for elbow and pole tuning; offline IK unit tests (`tests/test_vr_arms.c`: reach, elbow plane, continuity, wrist limits, handedness) |
| **W3 Arm override** (1–2 weeks) | Write the arm chains at A1/A2 with the blend policy (§5.4) | Hunter's Dream, third person: hands track the controllers (wrist error < 2 cm at rest, no pop > 1 frame); weapon stays in the hand; attack anims take over and hand back without snaps; 30 min soak, no crash; IK-off ⇄ IK-on toggle live | `BB_TOGGLE_FILE` bit for arms on/off (live A/B); `BB_XR_ARM_LOG=1` one line per second (target vs achieved, weight, anim); `BB_CAPTURE_TRIGGER` captures with IK on vs off; palettes in the capture differ only in arm rows (§4); debug "Draw Skeleton" (`beh+0x382`) if its draw backend works in retail [S]; 11's "Draw Dummy Polygon" shows dmy 300 at the controller |
| **W4 Fingers** (3–5 days) | §6 | Each finger group follows its input; the gun index finger follows the trigger; right-hand weapon grip unchanged | sandbox3d hunter hands (fingers visible up close); `BB_XR_INPUT_DIAG` path log |
| **W5 Weapon logic** (2–3 weeks) | Two-handed forms (#5), gun aim (a)(b), transform/parry integration | Lock-by-ray picks the pointed enemy 8/10; two-handed axe shows the left hand on the shaft; gun points where the controller points | Same as W3 plus a fixed test spot (golden save in front of an enemy, 12 §3.5) |
| **W6 Stretch: hand-driven hits** (3–6 weeks) | #8, then evaluate #9 | Test arena: hits register when the VR weapon crosses the enemy during a swing ≥ threshold and never when idle; damage equals the game's damage for the same attack | Frame-stepped captures; TAE event log; enemy HP readout via `SprjChrDataModule` (bag+0x20) |

## 10. RE tasks (ranked)

| # | Task | Leads (this file) | Strategy | Effort [S] |
|---|---|---|---|---|
| 1 | Confirm the pose chain and which copy (0x1C375D0 / 0x1E19BC0) runs for the player, on which thread | §3.1–3.2 | `bb_hook_log_once` at A1, A2, A0 with `rax`, `r15`, thread ID; compare with the chain | hours |
| 2 | Confirm the equip block and the style values; watch the TimeAct ring live | §7.1–7.4 | Log on change; CE "find what writes" on `equip+0x08` while transforming | hours |
| 3 | `worldFromModel` / facing (shared with 11 RE #2) | `SprjChrPhysicsModule` bag+0x68; ChrCtrl; WorldChrMan +0x70/+0x80 | CE unknown-value scan while walking; check `world·model(Head)` against the camera | 1 day |
| 4 | Finger and elbow bend axes | §2.2 bind hints | Rotate one joint by +30° about Y then Z in the W3 hook; see which curls | hours |
| 5 | Palette writer (optional, for §4 diagnostics) | probe.c:147-149 fault path | Log `Rip` for faults in one watched palette page | 1 day |
| 6 | TAE event dispatch for the player (type 1 open/close) | `SprjChrTimeActModule` vtable 0x5735190; erfps2's `CSChrTaeAnimEvent::exec_one` pattern | Break on the module's update, find the switch on event type; log type-1 events with judge IDs | 2–4 days |
| 7 | Bullet spawn and direction | `SprjBulletIns` runtime class; BulletParam fields | Find the BulletIns constructor through its vtable/RC; hook, log origin/direction | 3–5 days |
| 8 | Action requests (inject R1/L1/L2 without pad pulses) | `SprjChrActionRequestModule` bag+0x80; HKS `Arm_*` constants | CE: watch the module while pressing R1; find the request bitfield | 2–3 days |
| 9 | Lock-on manager (shared with 11 RE #8) | `IsLockon`, `LockTgtListNum:` | as 11 | 2–3 days |
| 10 | Weapon-model instance and its dummy query (for two-handing and aim) | `ChrAsmModel`, `SprjAsmModel`; 11 RE #3's dmy query | Follow the A1 output's consumers; or compute from R_Weapon ∘ dummies offline (no RE) | 2–4 days (optional) |

## 11. Risks and open questions
- **Pose timing.**
  - If the pending-job gate (0x213C500) runs *after* A1/A2 on some frames, our write is lost. The mid-function sites
    are after the gate, so this should not happen.
  - Hit detection may run before A2M (one frame old): acceptable.
- **Threads.** A2M on worker threads means concurrent hook calls (player + NPCs); the filter is a single compare. No
  locks in the handler.
- **Animation fights IK.** Third-person locomotion swings the arms; at weight 1 the avatar's arms ignore the walk
  cycle. That is intended in VR, but expose `weight_locomotion` for players who prefer the animation.
- **Trick-weapon models switch mid-animation** (form B sub-model visibility). Our hand drives `R_Weapon` only; the
  game still decides which sub-model is visible. Fine.
- **Shields/torch light** follow the hand automatically (`L_Weapon` dummies).
- **Online:** off (PLAN §3 Phase 6).
- **Handedness and axis conventions** (§2.2) are the most likely first-day bug; test with the mirror avatar.
- **Pimax capacitive paths:** on Pimax OpenXR 0.1.0 the right `trigger/touch` and both `thumbrest/touch` were not
  seen (§6.1). Fallback: curls from value inputs only (index =
  trigger value, others = squeeze, thumb = button clicks).
- **Licences:** ERSkeletonMan (MIT), erfps2 (MIT/Apache-2.0) and ds3fps (MIT) for techniques and attribution;
  fromsoftware-rs: check its licence before copying struct layouts (we only cite offsets); Garden of Eyes: reference
  only; ERVR: closed.

## 12. Links
- **erfps2:** https://github.com/Dasaav-dsv/erfps2 (`src/hooks.rs`, `src/rva.rs`, `src/player.rs`,
  `src/hooks/tae.rs`)
- **ERSkeletonMan:** https://github.com/Dasaav-dsv/ERSkeletonMan (`SkeletonMan.h`, `HkSkeleton.h`)
- **fromsoftware-rs:** https://github.com/vswarte/fromsoftware-rs and the fork pinned by erfps2,
  https://github.com/Dasaav-dsv/fromsoftware-rs (`crates/eldenring/src/cs/chr_ins/module/{time_act,behavior}.rs`,
  `cs/player_game_data.rs`)
- **The Grand Archives tables:** https://github.com/The-Grand-Archives (Elden-Ring-CT-TGA, Dark-Souls-III-CT-TGA)
- **ERVR:** https://www.nexusmods.com/eldenring/mods/10711 (unreachable to the agents; details **[R]**)
- **Paramdex BB:** https://github.com/soulsmods/Paramdex/tree/master/BB (`Defs/EquipParamWeapon.xml`,
  `Names/EquipParamWeapon.txt`)
- **DSAnimStudio BB TAE template:** https://github.com/Meowmaritus/DSAnimStudio (`Res/TAE.Template.BB.xml`)
- **SoulsAssetPipeline / Havoc:** https://github.com/Meowmaritus/SoulsAssetPipeline
- **Havok layouts:** BotW decomp stubs https://github.com/zeldaret/botw/tree/master/lib/hkStubs/Havok/Animation/Animation/Rig
  (reference only); CommonLibSSE-NG https://github.com/CharmedBaryon/CommonLibSSE-NG; HKX2Library
  https://github.com/ret2end/HKX2Library
- **Weapon IDs (community):** https://github.com/Noxde/Bloodborne-save-editor (`src-tauri/resources/weapons.json`)
- **BB HKX animation groups:** https://www.nexusmods.com/darksouls3/mods/522
- **OpenXR 1.1 spec** (interaction profiles, `XR_EXT_hand_tracking`, `XR_EXT_hand_joints_motion_range`,
  `XR_EXT_hand_tracking_data_source`, `XR_FB_touch_controller_proximity`):
  https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html; sources
  https://github.com/KhronosGroup/OpenXR-Docs/tree/main/specification/sources/chapters (`semantic_paths.adoc`,
  `extensions/ext/ext_hand_joints_motion_range.adoc`, `ext_hand_tracking_data_source.adoc`,
  `extensions/meta/meta_touch_controller_plus.adoc`); runtime inventory
  https://github.khronos.org/OpenXR-Inventory/runtime_extension_support.html
- **Pimax:** Dream Air FAQ https://jp.pimax.com/en/blogs/news/faq-about-the-pimax-dream-air; MIXED Q&A
  https://mixed-news.com/en/high-end-vr-headset-dream-air-pimax-answers-the-most-important-questions/; Crystal AMA
  https://support.pimax.com/en/support/solutions/articles/60000948997-ama-july-7-questions-answers-about-the-pimax-crystal;
  Pimax Play EVO https://new.pimax.com/explore/blogs/introducing-pimax-play-evo; PimaxXR
  https://github.com/mbucchia/Pimax-OpenXR (releases)
- **Finger conventions:** Meta Capsense
  https://developers.meta.com/horizon/documentation/native/android/native-capsense; Meta controller-driven hands
  https://developers.meta.com/horizon/reference/unity/v81/class_o_v_r_controller_driven_hand_poses_sample; VRChat
  https://docs.vrchat.com/docs/touch and https://docs.vrchat.com/docs/valve-index; OpenVR
  https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h

## Cross-links
- **02:** ERVR's motion-controlled combat model and its limits (the P3 end state); the Sekiro/erfps2 hook styles.
- **05:** EquipParamWeapon/Paramdex, the mod loader (`dvdroot_ps4` overrides) for any future data patch.
- **08:** VA space, the host-call hook design (A1/A2 follow its rules: 14+ bytes, position-independent,
  replay-before/after), pose bookkeeping §7.4.
- **10:** HUD layer; the readout of §8 #1 can show on the debug panel.
- **11:** First-person anchor, head hiding and comfort. This file shares RE #2 (world transform), #3 (dummy query) and
  #8 (lock-on) with it, and corrects §5.2 (b): the player's Havok "hand IK" is foot placement, so arm IK is ours.
- **12:** Hot-reloadable logic (IK and curls live there), `vr_tune.ini` `[arms]`/`[fingers]`, `.bbxrrec` replay,
  `bbctl` boot and capture.
- **14 (hitboxes, being written in parallel), `scripts/vr_assets/`, `gpu/xr_bridge/sandbox3d/`:**
  - The same BehaviorParam → AtkParam chain as §7.5, extracted to JSON.
  - The hunter and weapon models in a right-handed, X-mirrored space.
  - The 3D sandbox that W2–W4 test in.
- **Working tree `src/runtime_vr_head.c`:** the H1/H2 head hook that §5.1's arm hook copies in structure, env flags
  and fake-input testing.
