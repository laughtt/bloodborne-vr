# 24: Arms in first person: the interim fix, the shared arm IK, and the game's bone hook

Date: 2026-10-09. Code: `src/vr_arm_math.h` (the arm IK, shared with the sandbox; tests
`tests/test_vr_arms.c`, also built as C++), `src/runtime_vr_arms.c` (the game hook), `src/vr_arms.h`,
`gpu/xr_bridge/sandbox3d/rig.cpp` (now uses the shared IK), `patches/Bloodborne.xml` (hook "VR Arms IK
(AnimationToModel)"), `tools/vr_fp_test.py --steps arms` (headset-free test). Builds on 13 (bones, A1/A2,
§5 IK design), 16 (first person, the eye, `hide=body`), 14 option C and 20 §6.2 G5 (hits where the hand
swung need these arms first).

Legend: **[M]** measured in the running game, **[C]** confirmed from the disassembly, **[L]** likely,
**[S]** design or not yet exercised.

## 0. The feedback and the answer

"The arms are too low, not like in the sandbox; weapons are not implemented yet." In the sandbox the
hands follow the controllers (`rig.cpp`). In the game, `hide=body` leaves the hunter's own arms, posed
by the game's animation: idle hands hang at the hips, about 0.6 m below the eye and close to the body,
so in first person they sit at the bottom edge of the view or below it.

| Step | What | Status |
|---|---|---|
| 1. Interim (today) | **Camera offset**, existing keys `up=` / `forward=` in the control file (`fp.txt`) | ships now, no rebuild |
| 1b. Interim, bones | `arm_src=rest`: the arms posed in a "ready" pose by the new hook | built (`out/gpu/bb-probe-ik.exe`); not verified in the game yet (section 5) |
| 2. Shared IK | `src/vr_arm_math.h`: two-bone IK, wrist and hand from a grip pose, twist bones, rest pose, the game's frames | done; 48 checks (C and C++); the sandbox uses it, its 99 + 147 checks pass |
| 3. Game hook | `bb_hook_vr_arms_a1/_a2` after AnimationToModel's synced pose, player only, `BB_VR_ARMS=1` | built, off by default; installs in the game, handler not yet exercised (section 5) |
| 4. Controllers in the game | `bbgpu_vr_hands` (the bridge already has the grip poses) | **not done**: needs `vk_xr.*`, owned by the stereo work (section 6) |

### 0.1 The interim fix (the honest one): move the camera, not the arms

The simplest change that works today is a camera offset relative to the body: lower the eye a little
and move it back, so the hands sit higher and further into the view. The keys exist already (16 §6) and
the running game re-reads the control file. Add to `out/fp.txt`:

```
fp=1 hide=body near=0.08 anchor=head turn=smooth turn_speed=90 move=body up=-0.08 forward=-0.10
```

`up=-0.08` lowers the camera 8 cm (the arms rise by that much in the view; you will feel 8 cm
shorter), `forward=-0.10` moves it 10 cm back along the body heading (the arms come forward in the view;
with `hide=body` there is no head to clip). Range: `up` -0.3..0.3, `forward` -0.5..0.5. Try -0.05..-0.12
for both. It does not change where the game's arms are: it only changes where you look at them from.

## 1. The hook point [C]

Re-checked in `out/eboot.elf` (the ELF maps VA 0x400000 at address 0; the patch addresses are VAs):

```
0x1C375D0  AnimationToModel copy 1   (rdi = anim owner, rsi = out, rdx = remap)
  0x1C37644  r15 = [rdi+0x20] + 0x38 = the pose; call 0xAC4F00 (hkaPose::getSyncedPoseModelSpace)
  0x1C37650  mov [rbp-0x80],rax ; mov r13d,[rax+8] ; mov rax,[rbp-0x78] ; mov dword [rax+8],1   <- A1
  0x1C37680  loop: modelPose[i] -> 3x4 rows at out[remap[i]] (FLVER order, 48 B)
0x1E19BC0  AnimationToModel copy 2   (pose = [rdi+0x28] + 0x38: the behaviour module's holder)
  0x1E19BFE  the same 19 bytes                                                                  <- A2
```

- At A1/A2 `rax` = `&pose.modelPose` (an `hkArray<hkQsTransform>`: data, size): the next instruction reads
  the size at `[rax+8]`, the loop reads `[rax]` + 48 i.
- The 19 bytes (`48 89 45 80 44 8b 68 08 48 8b 45 88 c7 40 08 01 00 00 00`) are position independent;
  no branch lands inside them (the closest targets are 0x1C37644 and 0x1C37680). `tests/test_hooks.c`
  installs them through the trampoline (absolute jump) [C].
- **Player filter**: `rax == player_pose + 0x18`, `player_pose = [[[P+0x3B0]+0x30]+0x28]+0x38`,
  `P = [[0x593E878]+0x60]` with PlayerIns's vtable 0x578F310 (the same chain 16 reads the eye from).
- **The write** (13 §5.3 step 5): new model transforms for both arm subtrees, then `boneFlags |= 1`
  (local dirty) for the written bones and `[pose+0x39] = 0` (localInSync), what
  `accessSyncedPoseModelSpace` (0xAC59A0) does for a whole pose: a later local-space read is rebuilt from
  the new model pose; the next frame's animation overwrites everything again (no drift).
- Which copy runs for the player was not known (13 §3.2); the log says (section 5).

## 2. The shared IK (`src/vr_arm_math.h`)

Static inline C11, also compiled as C++ (the sandbox's language), like `vr_turn_math.h`. Types are
`vr_head_math.h`'s (`VrVec3`, `VrQuat`).

| Function | Does |
|---|---|
| `vam_two_bone` | elbow for lengths a, b from the shoulder toward the target, bending toward a pole; the target clamped to |a-b|..0.9995 (a+b) |
| `vam_aim_with_twist` | a bone's rotation: bind segment onto the new one, its roll set by a reference (the pole) |
| `vam_solve_arm` | one arm: new model rotations of UpperArm, Forearm, Hand and the two forearm twist helpers (Twist takes `twist_share` of the wrist roll, Twist1 half) |
| `vam_hand_basis`, `vam_hand_rotation` | the bind hand frame (wrist to knuckles, thumb side) onto a grip's finger and thumb axes |
| `vam_side_offset`, `vam_grip_wrist` | the grip -> wrist offset (right hand's, mirrored for the left) |
| `vam_rest_target` | a "ready" pose without controllers: wrist relative to the live shoulder along the body axes, fingers pitched down |
| `vam_xr_in_head_game` | an OpenXR grip relative to the head into the game camera's frame (the Z mirror of 08/15) |
| `vam_camera_in_model` | the H1 camera (rows) into the character's model space (position, physics yaw: 16 §2) |
| `vam_weight_step` | the IK weight's fade in / out |

- **One code for both handednesses.** Every function is built from from-to rotations, cross products and
  signed angles, which are equivariant under a mirror; the game's left-handed model space (face -Z, R
  bones at -X) and the sandbox's mirrored right-handed one run the same calls. `test_mirror` solves an arm,
  mirrors every input (x -> -x, quaternion (x, -y, -z, w)) and gets the mirrored output to 1e-5.
- **The sandbox** (`rig.cpp`): `SolveTwoBone`/`AimWithTwist` are now thin wrappers, the per-arm solve and the
  hand basis call the header; the grip offset mirror is `vam_side_offset` (the same as the old Euler
  mirror). `bbport_xr_sandbox3d_test` (99 checks) and `_combat_test` (147) pass unchanged.
- **Tests** (`tests/test_vr_arms.c`, 48 checks, `ninja -C out/gpu vr-arms-test vr-arms-test-cpp`): the
  game's arm geometry (upper 0.283 m, forearm 0.273 m, A-pose); the wrist reached; the bind locals
  rebuilding the solved chain (what the game's forward pass does); elbow toward the pole, down and out;
  no elbow flips over a 600-step path (worst step < 2 cm); clamping; twist shares (60 deg wrist roll:
  Twist 60, Twist1 30); mirror equivariance; hand axes; the game mapping (a grip ahead/right of the
  headset lands ahead of the face on the R side; a head turned 30 deg left keeps the grip ahead-left);
  the weight fades.

## 3. The game side (`src/runtime_vr_arms.c`)

```
H1 (runtime_vr_head.c, first person only)   final camera rows + the head pose  -> vr_arms_view
    vr_fp_body: the player's position and yaw this H1 read       -> camera in model space (2-slot seq)
A1/A2 (animation; any thread; a busy flag)   rax == player pose + 0x18 ?
    read the skeleton once per skeleton: bones by name, parents, reference pose -> VamArmBind per side,
         subtree map (UpperArm and below: Forearm, Hand, fingers, Weapon, twist and elbow helpers)
    weight: arm=1 and (first person or arm_tp=1); attack (TimeAct ring's newest sub-id 30000-49999,
         13 §5.4 [L]) -> arm_attack (0); faded in arm_in (0.15 s) / out arm_out (0.08 s)
    per side: shoulder = the live UpperArm; target from arm_src; vam_solve_arm
    write: rotations nlerp(anim, IK, w) for UpperArm/Forearm/Hand/twists; every subtree bone's position
         = new parent . its animated local (bone lengths never change); boneFlags |= 1; +0x39 = 0
```

- **Guards**: every guest read and write goes through its own readable/writable check
  (`VirtualQuery`, per-thread region cache); a skeleton that is not parent-first or lacks the
  UpperArm/Forearm/Hand chain is refused (logged); torn transforms (not finite, not unit) are skipped.
- **Sources** (`arm_src=`): `rest` (default: wrists 0.30 m below and 0.32 m ahead of each shoulder,
  fingers 25 deg down), `fake` (grips relative to the head, OpenXR head frame: the whole controller path
  without a headset), `xr` (falls back to rest until section 6 is done).
- **Grip frame in the game**: OpenXR's grip -Z (fingers) becomes +Z after the Z mirror; the hand rotation
  is `vam_hand_rotation(cam . grip, +Z, +Y, ...)`. The grip -> wrist offset is the sandbox's
  `hand_offset` (0, -0.01, 0.07 m, -15 deg pitch), applied in OpenXR space before the mirror.

### 3.1 Settings

| Variable / key | Effect |
|---|---|
| `BB_VR_ARMS=1` | enable the handler (else it returns at once) |
| `BB_HOOKS=...;VR Arms IK (AnimationToModel)` | installs A1 and A2 |
| `BB_VR_ARMS_LOG=1` (or key `arm_log=1`) | once per second: A1/A2 calls and player calls, weight, source, anim id (attack), reach per side, writes |
| `BB_VR_ARMS_SRC=rest\|fake\|xr` | start value of `arm_src` |
| `BB_VR_ARMS_CONTROL=<file>` | live keys; default: the first-person control file (`BB_VR_FP_CONTROL`, `fp.txt`); only `arm*` keys are read |
| `arm=0\|1` | on/off live (1) |
| `arm_src=rest\|fake\|xr` | the target source |
| `arm_weight=<0..1>` | how much of the IK pose (1); 0.5 = halfway between the game's arms and the target |
| `arm_attack=<0..1>` | weight during attacks (0: the game's swing) |
| `arm_in=<s> arm_out=<s>` | fades (0.15, 0.08) |
| `arm_tp=0\|1` | also in third person (rest only) |
| `arm_rest=<out,up,forward[,pitch_deg]>` | rest pose relative to the shoulder (0.02, -0.30, 0.32, 25) |
| `arm_pole=<out,down,back>` | elbow pole (0.4, 0.85, 0.35; the sandbox's `elbow_pole`) |
| `arm_twist=<0..1>` | forearm twist share (1) |
| `arm_soft=<0.5..0.99>` | reach fraction where a far target is eased toward the straight arm instead of cut (0.92; 0 = hard clamp) |
| `arm_give=<m>` | metres the shoulder may slide toward a target past the soft limit (0.04; the chain is only 0.556 m) |
| `arm_fore_roll=<0..1>` | share of the wrist roll the Forearm bone takes before the twist bones (0.35) |
| `arm_fake_r= / arm_fake_l=<x,y,z[,yaw,pitch,roll]>` | fake grips relative to the head (m, deg; right default 0.18, -0.38, -0.32, pitch -20) |
| `arm_offset=<x,y,z[,pitch]>` | grip -> wrist offset, right hand |

## 4. Weapons

Nothing extra is needed for the weapon to follow: every hand-held weapon hangs off `R_Weapon`/`L_Weapon`
(13 §7.3), children of the Hand bones, inside the rewritten subtree; the AnimationToModel copy then gives
the weapon model and its dummy polys the new pose. The hit capsules come from those dummies (14 §1.3), so
once attacks are allowed through (`arm_attack` > 0, later the option C hook, 14 §3.5) the capsules follow
the hand at worst a frame late [L, 13 §3.3]. The per-weapon grip alignment (`Rig::WeaponAlignment` in
the sandbox: a blade's long axis into the grip's plane) is not ported yet; the game's weapon keeps its
bind angle in the hand.

## 5. Headset-free test

`python tools/vr_fp_test.py --steps arms --probe out/gpu/bb-probe-ik.exe --game-dir C:/games/CUSA03173
--out DIR` runs flat (fake head, scripted pad, its own `user/` copy and data folder), adds the arms hook and
`BB_VR_ARMS=1 BB_VR_ARMS_LOG=1`, and captures: third person with and without `arm_tp=1`, first person 30
deg down with the game's arms / the rest pose / the fake grips (low, raised, one hand above the eyes),
walking with the rest pose, `arm=0` back, and the interim camera offset.

**Run 1 (2026-10-09 21:22, `bb-probe-ik.exe`, flat, BB_VR unset): inconclusive.**
- Both hooks installed: `bb_hook_vr_arms_a1 at image offset 0x1837650 (19 bytes, replay-after)`, `_a2 at
  0x1a19bfe` [M]: the expected bytes matched in the user's eboot.
- The game reached the title, took both cross presses, sat on the loading screen (16 draws/frame) for
  about 80 s and then ended **without any error line** (no exception, no Windows error report for
  bb-probe-ik; the script reports "the game ended while waiting for the world").
- **The arms handler never ran**: it prints its first log line on its first call (`VR arms: A1 ...`), and
  no such line exists. No character was animated yet. So the handler's code did not cause the exit; the
  trampolines themselves are not ruled out by this run (they are the same mechanism as the stamina and
  camera hooks and install in `hooks-test`).
- Not re-run: the user's headset session started on the GPU. **Next step: re-run the same command**
  when no game is running. Check that the log shows `VR arms: right arm: UpperArm 80 Forearm 81 Hand 82`
  (13 §2.2's indices), that one of A1/A2 shows `player 1` per frame, and compare `fp_game` with
  `fp_rest` / `fp_fake*` in `png/`. If the game dies again on the load with the hook installed and
  `BB_VR_ARMS` unset, the trampolines are at fault, not the handler.


### 5.1 Headset run 1 (bb-probe-all4, HEAD 5c811c5): the arms did not move [M]

The log had only `VR arms: on (src xr; ...)`: no `control:` line after `arm_log=1` was added, no per-second line, no
skeleton lines. From the code: the control file and the skeleton were read only after a call matched the player's
pose, and the per-second line needed `BB_VR_ARMS_LOG=1` or that control key. So **neither A1 nor A2 ever saw the
player's pose** (or neither ran): "arms down, touching, following the body" was the game's own idle pose. Nothing was
written.

Static analysis found more readers: `hkaPose::getSyncedPoseModelSpace` (0xAC4F00) has 25 call sites. Among them is a
third copy right next to A1, **0x1C37770** (an i16 bone map, count at `[out+0xC]`: probably the per-part copy for the
ChrAsm parts, the skin and the arm armour that `hide=body` leaves visible), and 0x1C3AEA0, 0x1C0F4B5, 0x1C0FE1B,
0x1C11B19, 0x1C141E1, 0x1E1A681 (the debug skeleton drawer), 0x20E58D5 and Havok internals (0x2134xxx, 0x2138xxx).

Changes (`out/gpu/bb-probe-arms2.exe`):
- **A3** at 0x1C377C6 in 0x1C37770 (15 bytes `48 89 45 80 48 8b 45 88 44 8b 60 0c 45 85 e4`, rax = &modelPose),
  added to "VR Arms IK (AnimationToModel)".
- **Sync probe** "VR Arms Sync (getSyncedPoseModelSpace)" at 0xAC4F0E (11 bytes `48 8d 43 18 48 83 c4 08 5b 5d c3`,
  rel32 jump, the window ends in `ret`): rbx = the pose for **every** caller. It logs each distinct caller that syncs
  the player's pose (`the player's pose is synced from 0x...`) and per-second hit counts. With **`arm_at=sync`** (key, or
  `BB_VR_ARMS_AT=sync`) the arms are written there, so every reader sees them. A side that already carries the last
  written rotation is skipped, so repeated syncs in one frame do not compound.
- **Unconditional diagnostics**: the first call per site (its pose and the player's), the first player match per site,
  the player pose when it changes (or why it is not found), the first edit (source, view, first person, target
  weight), and the per-second counter line for the first 20 seconds. The control file is now polled on every call, so
  `arm_log=1` and the other keys work before any match.

Next headset run: `BB_HOOKS=...;VR Arms IK (AnimationToModel);VR Arms Sync (getSyncedPoseModelSpace)`, then read the
`VR arms:` lines. If only the sync probe matches, put `arm_at=sync` in fp.txt (live) and/or hook the named caller.

### 5.2 Headset run 2 (all5, HEAD 2efc677) [M]

- The sync window (11 bytes, `E9 rel32`) failed: "trampoline out of rel32 range" (the image is more than 2 GiB from the
  trampoline slab) and the game stopped at start. **Fix**: `runtime_hooks.c` now places a 14-byte absolute jump
  ("jump island") in a 64 KiB page allocated within reach of the site when the slab is too far.
- With A1/A2/A3 installed, **not one diagnostic line** in ~4 min: every handler prints on its first call before any
  check, so **none of the three copies ever runs** in this game (they are dead code or debug paths for us).
- **The real path** (static analysis): the PlayerIns pose update **0x1CF83F0** takes `[[[chr+0x3B0]+0x30]+0x28]`,
  finishes the animation job (0x213C500), calls `hkaPose::syncModelSpace` (0x1CF845A) and hands the model to
  0x209AB00 (`[rbx+0x4C8]`, the draw/attachment update). ChrIns has the same sequence for enemies (0x1CBD009). New
  hook **"player"** at **0x1CF845F** (10 bytes `4c 8b b3 c8 04 00 00 4d 85 f6`, replay-after, through a jump island):
  r14 = the pose, the model pose final for the frame. No branch lands inside the window; the two skips to the site
  leave a stale r14, which is only compared with the player's pose. It is now the default write site
  (`arm_at=player`; `copy` = A1-A3, `sync` = inside getSyncedPoseModelSpace).
- A3 was removed from "VR Arms IK (AnimationToModel)" (it never ran, so it cannot explain the crash at death, but it
  is useless); the patch now installs A1, A2 and the player site.
- **The crash at death** ("write access to 0x10 at guest offset 0x24A435E", a linked-list unlink with a null node in
  0x24A4xxx; host frame only the thread entry): no arms handler had run (no first-call line), and the frames hold no
  arms site. Unpatched code that never runs cannot fault, so the A3 patch is not the likely cause. With the player
  site active the handler will run during death and respawn: it reads and writes only through the guards, compares
  the pose pointer before anything, re-reads the arrays from the live pose and checks the bone count.

### 5.3 Doubled arms (all7) [M]

The player site ran ~120 times a second, half of them for another character's pose on another thread; while that call
held the handler's lock the player's call was skipped ("busy" ~14/s), so ~1 frame in 5 drew the game's own arms: in
alternate-eye stereo one eye showed the old arms. Fixed (arms4): calls for other poses leave on a lock-free check of
the cached player pose, and the write site waits for the lock instead of skipping (`waits` in the log). A grip more
than 1.5 m from the head is ignored (both arms once read "reach 168": the grips were ~93 m away).

### 5.4 The virtual upper body (arms5)

Feedback (all9): the arms follow the hands but seem rooted in a body that turns separately from the head. The arm
chains start at the character's shoulders, which face the character's own heading, while the view is psi + the
head's yaw (16 §9). Now the **shoulders turn toward the head's yaw** (option b of the coordinator's two): the yaw
between the character's facing (model -Z) and the H1 camera's forward, both in model space, is followed past a
deadzone with a time constant (`vam_body_follow`, `src/vr_arm_math.h`); both UpperArm joints turn about the vertical
through their midpoint (`vam_turn_about`), and the pole and the rest pose turn with them. Only the arm subtrees are
written: the character's heading, movement and the rest of its pose are unchanged (the hidden body does not turn).
Keys: `arm_body=0|1` (1), `arm_body_dz=<deg>` (25), `arm_body_tau=<s>` (0.2, 0 = at once). The hands themselves are
placed from the grips and the head of one GetInput snapshot, relative to the H1 camera, so head and hands do not lag
each other. Tests: `test_body_follow` (59 checks in all).

### 5.5 Hands jumping ahead during a dash (all12, arms6)

The comfort dash's follow mode (findings/19, `runtime_vr_dash.c`) draws the camera behind the real eye for ~0.2-0.4 s.
The hands are placed from that drawn camera, but the shoulders were the character's real ones, ahead of the view:
the arms reached from a body in front of the user, and the hands showed ahead. Now H1 records the dash's camera
offset (`vr_fp_dash_offset`: the dashed camera minus the eye, before the `up`/`forward` keys), `vr_arms_view` turns it
into model space, and the shoulders move by it: the whole arm moves with the view, so the hands stay where the
controllers are relative to the eyes and the reach is the same as standing. The character, its hit shapes'
timing and its position are unchanged. Chosen over fading the arms (they would vanish on every dash) and over
letting them stretch (clamped reach, hands short of the controllers).

### 5.6 Walking backwards (arms7)

Without lock-on the game turns the character to face its movement: walking backwards spins the hidden body 180 deg
while the view stays forward. The shoulder follow of 5.4 was kept relative to the character, so every body turn
became a head offset to catch up with (0.2 s, the long way at 180 deg): the arms pointed backwards. Now the shoulders'
heading is kept **in the world** and follows only the head (`vam_shoulder_yaw`: head world yaw = character yaw + the
head's model yaw; deadzone and smoothing as before); the turn written into the model is that heading minus the
character's yaw, so a body turn of any size is cancelled in the same frame, with the wrap at +-180 handled.
The pole and the rest pose turn with the shoulders, so the elbows stay sane at 180. Tests: 90, 180, the +-pi wrap and
the head deadzone/follow (66 checks).

## 6. Controllers in the game (built, untested in the headset)

`Vulkan::Xr::Input()` (`vk_xr.cpp`) now also stores the head and `grip[2]` of the same `GetInput` call
(one snapshot, bridge LOCAL space, valid bits, steady-clock stamp; `Xr::LatestHands()`), exposed as
`int bbgpu_vr_hands(BbVrHands*)` (`bbgpu.h`). It updates whenever the pad is polled (`bbgpu_vr_pad`),
so `GetInput` is still called once. `runtime_vr_arms.c`, `arm_src=xr` (now the default): the hand is
taken relative to the head of that same snapshot and placed through the H1 camera
(`vam_grip_to_model` = `vam_grip_wrist` + `vam_xr_in_head_game` + the camera in model space), i.e. with the
same recenter/heading as the head, so it lands where the controller markers are. Per side it falls back to
the rest pose while the grip is invalid, the snapshot stale (no new stamp for 250 ms) or there is no
first-person camera. Not done: prediction to the views' display time (the grips are one input poll old),
finger curls, `WeaponAlignment`, 14 option C.

## 7. Risks and open questions

1. **The hook is new in the game.** Only a flat run tested it (section 5); keep it off in the headset
   until that run's captures look right. A wrong write is visual only for one frame (the next animation
   rebuilds the pose), but a crash in the handler ends the game: the handler reads and writes only
   through the guards.
2. **Order** [L]: if the renderer, the attachments or the hit system read the pose before
   AnimationToModel or from another copy, they see the game's arms (13 §3.3). A hand that renders right
   but whose weapon stays behind would show it.
3. **The arm armour** (`AM_*`) and the skin are skinned per part through their own bone maps (13 §2.2),
   from the same FLVER-order output: they should follow [L].
4. **Attack detection** is the TimeAct ring's newest sub-id 30000-49999 [L]: other actions (items,
   gestures, ladders, visceral) still get the IK until the state readout of 13 W0 exists.
5. **Registration**: the hands are relative to the H1 camera of the previous frame and its body
   position; at 4 m/s that is ~6 cm of lag in the world, none relative to the body.
6. `rest` is a static pose: the arms do not swing when walking. It answers "too low", not "not like
   the sandbox"; that needs section 6.
