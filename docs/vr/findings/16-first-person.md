# 16: First-person camera: the player, the eye anchor, hiding the own model, VR turning

Date: 2026-10-09. Code: `src/runtime_vr_fp.c` (player probe, anchor, heading, the own model, lock-on, pad
filter, the in-game step hook, the trace), `src/vr_turn_math.h` (the turn model, shared with the VR sandbox;
tests `tests/test_vr_turn.c`, also built as C++), `src/vr_fp_math.h` (eye and camera math, tested in
`tests/test_vr_head.c`), `src/vr_fp.h` (the H1/H2 interface), `src/runtime_vr_head.c` (H1/H2 call it),
`src/runtime_pad.c` (pad filter, `BB_PAD_KEYBOARD=0`), `patches/Bloodborne.xml` (hook "VR FP Camera Heading
(in-game step)"), `tools/vr_fp_test.py` (headset-free test), `tools/vr_turn_replay.c` (a game trace through the
turn model). Builds on 08 (camera chain, H1), 11 (the FP plan), 13 (pose chain), 15 (head tracking).

Legend: **[M]** measured in the running game (flat, fake head, scripted pad), **[C]** confirmed from the
disassembly, **[L]** likely, **[S]** design or not yet exercised.

## 1. Results

| Question | Answer |
|---|---|
| Player pointer | **`[[0x593E878]+0x60]` [M][C]**: the vtable is PlayerIns's (0x578F310) every frame; the in-game step hands exactly this pointer to the camera director (0x1d3bdfa, unless a debug override `[[0x593E880]+0x108]` is set). |
| Player position / facing | **physics module `[[P+0x3B0]+0x68]`: +0x1E0 position (vec4, the feet), +0x1D4 yaw [M]**. The yaw is the heading of model **+Z**: running straight along 152.1 deg the yaw reads -27.9 deg, i.e. **the hunter faces yaw + 180 deg**; model -> world is `pos + R(yaw)·p` (`vr_model_to_world`). |
| Live head bone | **`[[[P+0x3B0]+0x30]+0x28]+0x38` = hkaPose [M]**: 108 bones, Head 78, Neck 77 (looked up by name); Head bind pose at (0, 1.572, 0.013) as 13 predicted. |
| Eye position | **dummy 58 midpoint carried by the Head bone [M]**: offset in the Head frame (0.082, 0, 0.079). Standing: **1.60 m above the feet**, 0.12 m in front; running at 4 m/s: 1.40-1.52 m, 0.31-0.35 m in front (lean). |
| Camera at the eye | **Yes [M]**: H1's PersCam position = the smoothed eye, rotation = body heading psi x the headset (horizon level); H2 gets the same camera. |
| **Own model hidden** | **Yes, per model [M][C]**: every model of the hunter (the base body `[P+0x48]` and 24 ChrAsm parts at `[P+0x4D0]+0x28`) has a display entity `[model+0x18]` (SprjFD4ModelDispEntity, vtable 0x57A0820) that is drawn only while its flags `+0x210` have bit 0. The game sets that bit every frame (PlayerIns visibility update, 0x1cf4830); H1 clears it for the hidden parts after that and before the draw lists test it (IsVisible 0x2121c80). No flicker in 33 captures, everything back as soon as hiding stops (section 4). |
| What `hide=` hides | **head**: FaceGen head and parts, hat, hair, and the skin's head and neck (display-mask groups); **body** (recommended): all that, the coat, the legs and the base body: **only the arms, the hands and the weapons stay [M]**; **all**: everything. |
| Shadow | **Probably lost with the hidden parts [L]**: IsVisible's bit 0 gates the model, and the ground beside the boots got brighter by 9 levels (noise 3.5) with `hide=all`. The near plane (`near=`) keeps the shadow: the old way, still there. |
| **Turning** | **Decoupled [M]**: the view's yaw is psi + the headset's yaw; psi changes only by the right stick (snap 30 deg or **smooth 90 deg/s**, measured 90.0 deg/s) and recenters. The game's own camera, the character's rotation, root motion and lock-on never turn the view. One model for the game and the sandbox: `src/vr_turn_math.h`; a game trace replays through it to 1e-6 rad (section 9). |
| Movement direction | **Along the move heading (body: psi, head: psi + head yaw) [M]**: the hook turns the game camera; the game walks along the follow camera's own heading, which lagged 6 deg after a smooth turn, so the left stick now takes the rest: +0.1..+0.45 deg from the target (section 9.3). |
| Lock-on state | **`[[0x593E860]+0x2820]` (LockTgtMan "IsLockMode") [C]**, the locked target's point in its target list [C]; not exercised (no enemy at the test spot) [S]. |
| Cutscene / debug camera | Detected by comparing H1's camera with SprjCamera's field camera (+0x10) [C]. |
| Death | No flag found; heuristic: third person after the raw eye stays below 0.6 m for 1.5 s [S, not exercised]. |

## 2. The chain and the eye (F1)

```
WorldChrMan* W = [0x593E878]          +0x60 P (PlayerIns, vtable 0x578F310)
bag        = [P + 0x3B0]
physics    = [bag + 0x68]             +0x1E0 position (feet), +0x1D0 rotation vec4, +0x1D4 yaw (heading of model +Z)
pose       = [[bag + 0x30] + 0x28] + 0x38   hkaPose: skeleton +0x00, local +0x08, model +0x18, flags +0x28 (hkArray)
skeleton   = [pose]                   parents +0x18 (i16), bones +0x28 (16 B, hkStringPtr name), reference pose +0x38
eye(model) = model(Head) . inverse(bind(Head)) . (0, 1.654, -0.066)       dummy 58 midpoint, findings/11 2.2
eye(world) = position + R(yaw) . eye(model)
```
- Reads are guarded (`VirtualQuery`, cached per region, re-validated every second): the chain dangles during loads.
- Model-space bones come from the model pose when clean, else are composed up the parents from the local pose
  (Havok bone flags 1 local dirty, 2 model dirty).
- Evidence (run1 `trace.csv`, scripted pad `ly=0`): the player moved from (-11.57, -6.57, -15.71) to
  (-8.56, -7.04, -21.43) in 1.7 s at 3.9-4.2 m/s along 152.2 deg; the camera heading was 151.8-152.1 deg, the
  physics yaw -27.9 deg; the eye stayed 1.40-1.52 m above the feet.

## 3. The heading and the game camera (F2/F4)

- **The view** is `R(psi) · R_head` (horizon level, 11 §3.1) at the eye; psi is the turn model's (section 9).
- **The game must move along the move heading** (psi, or psi + the head's yaw with `move=head`). What did not work [M]:
  - writing the follow camera's yaw (ChrExFollowCam +0x144) at its update's entry (0x183ac60): the update rebuilds
    +0x144 from its own state every frame (it pulls the yaw toward a target yaw +0x270 with factor +0x240 = 0.3 at
    0x183c805, and other paths derive it from positions); writing +0x270 too still left +0x144 at the game's value;
  - turning only ChrCam's matrix after the director: the drawn camera turned exactly, but the walk followed the
    follow camera's own heading - the movement code reads the follow camera.
- **What works [M]**: hook **"VR FP Camera Heading (in-game step)"**, `bb_hook_vr_fp_cam` at **0x1d3be5a** (18 bytes
  `498b8528010000 c5fa104050 c4c17a114750`, replay-after, r13 = the step; right after the director call, before the
  step copies ChrCam to the render/field cameras). It turns, about the vertical through the player, to the move
  heading:
  - ChrCam's matrix ([r13+0x128]+0x10): what everything downstream sees this frame (H1's source included);
  - the follow camera's own state ([ChrCam+0x60]): its matrix +0x10, its output copy +0x2E0, its position +0x100,
    yaw +0x144 and target yaw +0x270, so its next update starts from that heading.
- **The residual** (new): the director's own output (`game_own` in the trace, the follow camera before the hook
  turns it) converges to the target only slowly after a turn: 6.3 deg off for seconds after a 90 deg smooth turn,
  9 deg after a smooth turn back, 7 deg after a snap [M, verify1]. The game walks along that heading (+0.1 deg from
  it). So the pad filter now rotates the left stick by `target - game_own` (`remap=1`, the default; without the hook
  it rotates by everything, as before; `remap=0` never).

## 4. Hiding the hunter's own model (F3)

The near plane alone (0.2-0.3 m) left the user seeing "small snaps of my body" while turning: the shoulders, the
coat collar and the hat cut by a plane that sweeps through them (`fp_none_left`, `fp_head_left`). The parts are now
not drawn at all.

### 4.1 The mechanism [C][M]

```
PlayerIns P
  +0x48          the base body model (c0000)            display entity [model + 0x18]
  +0x4D0 + 0x28  24 ChrAsm parts (8 B each)              display entity [part + 0x18]
display entity (SprjFD4ModelDispEntity, vtable 0x57A0820, constructor 0x2120bc4)
  +0x170  display mask, 128 bits: the FLVER #NN# mesh groups that are drawn (0x209cd20: ~OR of the protectors'
          invisibleFlags, bit 127 set; the same value in every part)
  +0x210  flags: bit 0 drawn, bit 1 alpha > 0 (both recomputed every frame); bits 2-5 set once, never restored
```
- **Who sets bit 0**: ChrIns DrawUpdate (vtable +0x118: 0x1cbff30, PlayerIns 0x1cf84f0) calls the visibility update
  (vtable +0x4D0: ChrIns 0x1cc9d30, PlayerIns 0x1cf4830) every frame: when the character is drawable (draw enable
  ChrIns+0x1E1 bit 12, `[ChrIns+0x18]+0x20` bits 0/1 and state 4) it ORs bit 0 into the base body (ChrAsm on: and
  clears its bit 2) and into each of the 24 parts; otherwise it clears them.
- **Who tests it**: `SprjFD4ModelDispEntity::IsVisible` (vtable +0x48, 0x2121c80): bit 0, then the draw group masks
  (+0x224/+0x234 against the context's). DrawUpdate runs in the in-game step; the draw lists are built in the draw
  step after H1, so **H1 clears bit 0 of the hidden parts every frame** (`fp_hide_apply`) and the game puts it back
  by itself the frame hiding stops.
- **Evidence**: hiding one slot at a time in third person (`hide_tp=1`, `tools/vr_fp_test.py --steps parts`) made
  exactly that part vanish in the same capture (26 captures); `hide=all` in 4 + 3 consecutive captures: the hunter
  never showed; `hide=none` afterwards: all back (`tp_none`, `tp_end`).
- **Never touch bits 2-5**: clearing bit 2 (`hide_bits=4`) also hid the hunter, but the game never set it again: the
  hunter stayed invisible for the rest of that session (verify1). `hide_bits=` is limited to 1-3.

### 4.2 The parts (slot table) [M]

The log lists them (`VR FP:   part N ... name`, BB_VR_FP_LOG=1); the third-person captures show what each draws.

| Slot | Part (test save) | Draws | Group |
|---|---|---|---|
| base | c0000 (`[P+0x48]`) | nothing visible with ChrAsm on | body, all |
| 0 | FG_A_0100 | FaceGen head | head |
| 1 | FC_M_0000 | the skin: bald head, neck, arms, hands (torso and legs masked by the armour) | head/body: its head and neck groups only (below); all: everything |
| 2 | HD_M_8000 | hat / hood | head |
| 3 | BD_M_8000 | body armour: coat, cape, collar | body |
| 4 | AM_M_8000 | arm armour (sleeves) | kept (all hides it) |
| 5 | LG_M_8000 | legs and boots | body |
| 6 | HR_A_0003 | hair | head |
| 7-10, 11-14 | WP_A_1000 (bare hands in the test save) | the two hands' weapon models (4 sub-models each, the trick forms) | kept (all) |
| 15, 16 | empty | more weapon models | kept (all) |
| 17-23 | FG_A_1600, 1500, 2001, 7002, 3000, 5000, 6000 | FaceGen parts (eyes, brows, beard, ...) | head |

The ChrAsm update (0x209cd20) fills the slots in this order with "kinds" 0, 14, 1, 2, 3, 4, 5, 6 x4, 7 x4, 8, 9,
15-21 [C]. The skin is one model with its head, neck, torso, arms and hands; its meshes carry display-mask tags
(#41 neck, #47/#48/#49 head, #43 arms, #44 hands, #34/#42/#46/#50/#61 torso, #45 feet; `out/vr_assets/hunter.json`).
`hide=head` clears groups 41, 47, 48, 49 in the skin's display mask (+0x170) and `hide=body` also the torso and feet
groups: the arms and hands stay. The game's mask is kept and put back when hiding stops (only if our value is still
there: an equipment change in between wins).

| `hide=` | Hidden | Visible in first person |
|---|---|---|
| `none` (default) | nothing (the near plane still cuts) | everything |
| `head` | slots 0, 2, 6, 17-23 + the skin's head and neck | coat, legs, arms, hands, weapons (`fp_head_down`) |
| `body` | head + slots 3, 5, base + the skin's torso and feet | **arms, hands, weapons** (`fp_body_down`: one bare arm) |
| `all` | every slot and the base | nothing |
| `weapons`, `skin`, `arms`, `base`, `skinhead`, `skinbody`, `sN`, numbers | one part or group, combined with `+` | for tests |

Edge: a bare-handed outfit draws its hands with the skin (kept). Other outfits differ only in which groups the
skin shows; hats with their own meshes are slot 2 either way.

### 4.3 The shadow, and what was not used

- **The shadow goes with the part [L]**: the shadow passes go through the same IsVisible; measured: the ground
  right of the boots brightened 57 -> 66 (noise between two captures 3.5) when everything was hidden. Not seen
  directly: the Hunter's Dream test spot has soft light. Keeping the shadow would need a shadow-only draw (a per-pass
  test is not exposed by the flags) [S]. The near-plane method (`hide=none near=0.2`) keeps it.
- **The near plane stays**: `near=` still applies; with `hide=body` it can go down to 0.05-0.1 m (a hand at the face
  is no longer cut).
- Not used: the character fade (ChrIns +0x20C alpha, +0x210 rate: ESD ChrFadeIn/Out) and SetDrawEnable (ChrIns
  +0x1E1 bit 12) hide the whole character with its weapons; ESD SetDispMask is a no-op for the player (vtable +0x458
  is `ret`); an invisible protector needs data patching; a renderer filter needs draw classification.

### 4.4 Sandbox parity

`gpu/xr_bridge/sandbox3d`: `show.fp_hide` = `head` | `body` | `all` | `none` hides the same groups on your own body
(head: the head meshes; body: all but the `arm` slot and the skin's mask groups 43/44 (arms, hands), weapons kept;
all: everything), the mirror copy shows all. Empty keeps `show.head`.

## 5. Anchor, smoothing, state

- Anchor `head` (default): the raw eye in model space, exponentially smoothed (tau 0.12 s) and kept within 8 cm of
  the raw eye, then a floor at 0.75 x the standing eye height (1.24 m: rolls, quicksteps, knockdowns). `body`: a
  fixed eye 1.654 m above the feet, 6.6 cm forward, no bob. `raw`: unsmoothed. **`stable`** (new): the smoothed
  eye's height straight above the feet: no lean and no swing when the character turns (the head anchor's eye is
  0.12-0.35 m in front of the feet and orbits them when the hunter turns); meant for `hide=body`.
- First person only while H1's camera is the field camera: SprjCameraStep (0x1e08e00) copies the selected camera
  (flags at SprjCamera+0x30: 0x40/4 debug +0x20, 0x20/2 event/Remo +0x18, 0x10/1 field +0x10) into the active one
  (+8) and then clears the flags, so the flags are useless at H1; comparing H1's matrix with the field camera's
  (+0x10) is exact. Otherwise (cutscene, debug camera) the game's third person (with head tracking) is shown, and
  nothing is hidden.
- Downed: the raw eye below 0.6 m for `BB_VR_FP_DOWN` = 1.5 s -> third person until it rises (death, long
  knockdowns). The player's HP lives in another module (`SprjChrDataModule`, bag+0x20 [L]); not located.

## 6. Environment and live keys

| Variable | Effect |
|---|---|
| `BB_HOOKS=...;VR FP Camera Heading (in-game step)` | the movement hook (with H1, H2, the LOD fix of findings/15) |
| `BB_VR_FP=1` | first person (needs H1; works flat without `BB_VR_HEAD`: the game's rotation at the eye) |
| `BB_VR_FP_CONTROL=<file>` | **live tuning**, re-read when it changes (keys below) |
| `BB_VR_FP_HIDE=none\|head\|body\|all\|...` | the own model (section 4; default none) |
| `BB_VR_FP_NEAR=<m>` | near plane (0.2) |
| `BB_VR_FP_UP=<m>`, `BB_VR_FP_FORWARD=<m>` | camera offset from the eye (0, 0) |
| `BB_VR_FP_ANCHOR=head\|body\|raw\|stable`, `BB_VR_FP_SMOOTH`, `BB_VR_FP_FLOOR` | anchor (head), smoothing (0.12 s), floor (0.75) |
| `BB_VR_TURN=snap\|smooth\|off\|game`, `BB_VR_SNAP_DEG`, `BB_VR_TURN_SPEED` | the right stick (snap 30 with `BB_VR_HEAD=1`, else game), snap angle, smooth deg/s (90) |
| `BB_VR_FP_MOVE=body\|head\|controller` | what the left stick walks along (body; controller falls back to head: no controller pose in the game yet) |
| `BB_VR_FP_SYNC=0`, `BB_VR_FP_REMAP=0` | no game-camera turning / no left-stick correction |
| `BB_VR_FP_DOWN=<s>` | downed -> third person after s seconds (1.5, 0 = never) |
| `BB_VR_FP_LOG=1` | 1 log line group per second, the parts list, lock-on changes, and the per-frame trace to `out/vr_fp_trace.csv` |
| `BB_VR_FP_TRACE=<csv>` | the trace elsewhere (section 9.4) |
| `BB_PAD_KEYBOARD=0` | ignore the keyboard (automated runs) |

Control file keys (the turn keys are `vrt_config_set`'s, the same in the sandbox's JSON): `fp=0|1 near= up= forward=
anchor= smooth= floor= hide= hide_tp=0|1 turn=snap|smooth|off|game snap=<deg> turn_speed=<deg/s> turn_accel=<s>
deadzone= expo= snap_on= snap_off= snap_repeat= move=body|head|controller recenter=view|reset lock=view|game|turn
lock_auto=<deg> sync= remap= deep=` (and `hide_bits=1..3` for tests).

Headset: `BB_VR=1 BB_XR_MODE=mono BB_VR_HEAD=1 BB_VR_HEAD_LEVEL=1 BB_VR_FP=1` plus `BB_HOOKS="VR Camera Hook H1 (draw
PersCam);VR Camera Hook H2 (draw main context);VR LOD Fix (SetCamera);VR FP Camera Heading (in-game step)"`.
Suggested control file: `fp=1 hide=body near=0.08 anchor=head turn=smooth turn_speed=90 move=body` (or `turn=snap
snap=30`, `move=head`, `anchor=stable` for no sway when the hunter turns); `hide=none near=0.2` is the old look.

## 7. Headset-free test (`tools/vr_fp_test.py`)

Flat, on a copy of `user/` and with `BB_DATA_DIR` in the output folder (a game already running from `out/` is not
disturbed), fake head file, control file, scripted pad, `BB_PAD_KEYBOARD=0`; the game ends itself.
`--steps full` captures the views of section 1; `--steps move` walks after snap turns; **`--steps parts`** hides one
ChrAsm slot at a time in third person; **`--steps verify`** checks the hide groups (third person, first person all
around at the game's 0.05 m near plane, repeated captures, everything back), then smooth turns, a snap and walks
after them. `python tools/vr_fp_test.py --steps verify --probe out/gpu/bb-probe-fp2.exe --game-dir C:/games/CUSA03173
--out DIR`. The report adds where each capture changed (`*_diff.png`) and the parts list.
This session's evidence (session scratch directory, not in the repository): `fp2/parts1` (slots), `fp2/verify1`
(turn residual found; its hide views are void: bits 2-5), `fp2/verify2` (groups, flicker, turns with the residual).

## 8. Open issues

1. **Lock-on is unverified** (no enemy at the test spot). With `lock=view` the right stick goes to the game while
   locked (target switching) and the game camera keeps following the view; target choice uses that camera. The lock
   state and target point reads (section 9.5) are logged (`VR FP: lock-on on/off`) for the next headset session.
2. The shadow of hidden parts is probably gone (section 4.3).
3. `move=controller` needs the controller's pose in the game (`bbgpu_vr_pad` has none; the bridge has `aim`): it
   falls back to the head.
4. The lock-on reticle and HUD markers are placed by the game's projection; not checked in first person.
5. Death/grab/ladder/door/visceral animations are untested; the eye follows the Head bone (smoothed, floored).
6. In menus the right stick's X is taken by turning while first person is active, and the left stick gets the
   (small) residual rotation.
7. Leaving first person (cutscene) is a cut to the game's third person; coming back after more than 0.5 s restarts
   psi from the game camera (a shorter drop-out keeps it).

## 9. VR turning: the decoupled yaw model (`src/vr_turn_math.h`)

### 9.1 The model

```
view yaw = psi + head_yaw                      the headset turns the view freely
move yaw = psi | psi + head_yaw | psi + controller_yaw      move=body | head | controller
psi      += smooth (rate x dt) or snap steps from the right stick; += the reference's move on a recenter
```
- Where the view still followed the game before: with `snap=0` psi was the game camera's own heading (its
  right-stick acceleration, auto-rotation and lock-on turned the view; now only `turn=game`); any frame out of first
  person restarted psi from the game camera (now only after 0.5 s); and the head anchor's eye sits 0.12-0.35 m in
  front of the feet, so it swings around them when the hunter turns (a sway, not a rotation: `anchor=stable`).
- psi never takes anything from the game: not from the character's rotation (it turns toward its movement or its
  lock target by itself), not from root motion, the game camera, or lock-on. Measured [M]: standing still, the view
  stays exactly on psi whatever the game camera does; walking, the hunter's facing turns to the move heading and the
  view does not follow it.
- The same header (static inline, no OpenXR, C and C++) runs in the game (`runtime_vr_fp.c`) and in the sandbox
  (`gpu/xr_bridge/sandbox3d`, the dash agent's `locomotion.cpp`); `tests/test_vr_turn.c` (229 checks) is built as C
  (`vr-turn-test`) and as C++ (`vr-turn-test-cpp`).
- Angles are headings: 0 = the world's forward, positive = a right turn (the game's `vr_cam_heading`); OpenXR and the
  sandbox's yaw are left positive (heading = -yaw). `vrt_move_vector` turns the left stick into world components
  (game: x = right, z = forward; sandbox: x = right, z = -forward).

### 9.2 Turning

- **Snap** (default in the headset): 30 deg when |x| crosses 0.6 from below 0.3, repeating every 0.3 s while held;
  a reversal snaps at once; after the stick was given to the game (lock-on) it must return to the centre first.
- **Smooth**: rate = `turn_speed` x m^`expo`, m from 0 at the dead zone (0.15) to 1 at full deflection; the rate
  ramps over `turn_accel` (0.08 s) in and out; frames longer than 0.1 s count as 0.1 s (no jump after a hitch).
  Measured [M]: 90.0 deg/s at full deflection, -21.5 -> 68.5 deg for a 1 s hold, the drawn camera one frame behind.
- No drift: inside the dead zone psi does not change by one bit (100 000 frames tested); wrap-around at +-180 is
  continuous (a full circle at 1 deg/frame steps exactly 1 deg each frame across the seam).
- `turn=off`: the stick does nothing (the game gets it); `turn=game`: the old flat behaviour, psi = the game
  camera's own heading (its right-stick turning, auto-rotation and lock-on included).

### 9.3 Movement

`move=body` walks along psi, `move=head` along where the head looks (`fp_head_walk`: the hunter turned to psi + 40
and the game camera converged to within 0.1 deg of it), `move=controller` along the controller (falls back to the
head in the game, section 8). The hook turns the game camera to the move yaw (section 3) and the left stick takes
the residual. Measured [M, verify2, with the residual]: walking after a 90 deg smooth turn +0.3 deg from psi
(before: -4.8 deg, along the director's own heading), after the smooth turn back +0.1 deg; after a snap the hunter
turned to psi + 0.45 deg and with `move=head` (head 40 deg right) to psi + 40.4 deg, while the director's own
heading stayed 2-5 deg off (the walks then ran into the workshop door: the facing is the evidence).

### 9.4 Recenter

The reference's own heading comes with the head (`ref_yaw`, the cinema screen's yaw, findings/15 §5). A change is a
recenter: with `recenter=view` (default) psi takes the change, so the view does not jump and "body forward" becomes
where the user faces; `recenter=reset` leaves psi, so the view returns to the body heading.

### 9.5 Lock-on

- **Detection [C]**: LockTgtMan, singleton `[0x593E860]` (constructor 0x1c8d8f0 builds the debug menu
  "LOCK_TGT_MAN" with "IsLockMode:" bound to `+0x2820`). PlayerIns code (0x1cf682f) reads `+0x2820` and walks the
  target list at `+8` (next `+0x80`, flags `+0x90`, 0x20 = the locked one) and takes the target point at `+0x00`;
  the in-game step copies `!+0x2820` to `+0x2821` before the camera director (0x1d3bd79). `fp_lock_state` reads both;
  the target heading is `atan2(dx, dz)` from the player to that point.
- **Policy** (`lock=`): the view stays free in every policy, the game turns the body to the target.
  - `view` (default): the right stick goes to the game while locked (target switching), stick turning pauses; the
    game camera keeps following the move yaw, so walking stays relative to the view and the next lock picks what
    the view faces.
  - `game`: as `view`, and the game camera is left to the lock-on (walking relative to the target, as flat).
  - `turn`: the right stick keeps turning psi (no target switching).
  - `lock_auto=<deg>` (0 = off): psi turns toward the target (smooth at turn_speed, or snaps) once it is that far
    out of the view, until it is within a quarter of it; never past it.
- Not exercised in the game [S]: no enemy at the test spot. The log prints `VR FP: lock-on on/off` and, once per
  second, `lock 1 target <deg>`.

### 9.6 The trace (`BB_VR_FP_TRACE`, or `out/vr_fp_trace.csv` with `BB_VR_FP_LOG=1`) and its replay

One CSV line per H1 frame (capped at 100 MB). Positions in metres (game world), angles in radians as headings
(right positive, 0 = world +Z), sticks -1..1 (right, up), times in seconds:

| Columns | Meaning |
|---|---|
| `t, active` | seconds since start; 1 when first person was applied |
| `px, py, pz, yaw` | player position (feet) and physics yaw (model +Z; the face looks along yaw + pi) |
| `eye_rx, eye_ry, eye_rz` / `eye_x, eye_y, eye_z` | the raw / the used eye in model space |
| `wx, wy, wz` | the eye in the world (the camera before `up`/`forward`) |
| `cam_x, cam_y, cam_z, heading` | the game camera at H1 (position, heading) |
| `game_own` | the director's own heading (before the hook turned it) |
| `psi` | the body heading |
| `dt, stick_x, lx, ly` | the frame time; the right stick X and the left stick as read (before the filter) |
| `head_yaw, ref_yaw` | the head relative to the reference; the reference's heading (a change = recenter) |
| `view_yaw, move_yaw, turn_delta, turn_rate` | the turn model's outputs (rad, rad, rad this frame, rad/s) |
| `snapped, recentered, sync, stick_to_game` | -1/0/1; 0/1; the game camera turned; the right stick went to the game |
| `locked, target_valid, target_yaw` | the lock-on state and target heading |
| `turn, move, hidden` | the turn and move settings (names); display entities hidden this frame |

`out/gpu/vr-turn-replay.exe <trace.csv> [turn keys]` feeds every first-person frame's inputs (`dt, stick_x,
head_yaw, ref_yaw, locked, target_*`, the frame's `turn`/`move`) through `vr_turn_math.h` with the given settings,
restarting as the game does, and compares psi, view and move yaw. verify2 (4650 first-person frames: smooth turns
both ways, a snap, head-relative walking): worst per-frame difference 8.9e-7 rad, the free replay ends 1.8e-7 rad
from the game's psi (verify1's coarser trace: 1.5e-5 / 7.6e-5 rad). The sandbox
can play a real session through the same code this way (its yaw = -psi, its head yaw = -head_yaw).
