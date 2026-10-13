# 27: Gun aim: the player's bullets follow the left controller

Date: 2026-10-09. Branch `vr`. Status: **built and unit-tested (vr-gun-test, hooks-test). Not run in
the game yet** (the user was playing). Static analysis of `out/eboot.elf` with capstone and the
relocated image from 08 (`scratchpad/re/`). Tags as in 08: **[C]** confirmed in the disassembly,
**[L]** likely, **[S]** speculation. Image offset (logs) = VA − 0x400000.

## 1. The problem

In first person the left trigger (L2) fires the gun, but the shot goes along the character's facing
or toward the lock-on target. It ignores where the left controller points. 13 §6 #6(c) predicted
this: the shot direction comes from the character and the target, not from the gun barrel. The
direction has to be changed where the bullet is launched.

## 2. Where the shot direction is decided

```
TAE event 2/318 (bullet/gun behavior) -> RequestBehavior 0x1cc7530 -> behavior 0x1cb61c0
  -> 0x1cb6680 (bullet row): builds the request on its stack [rbp-0xc0]
       +0x00 owner handle ([chr+8]), +0x04 behavior bullet id, +0x0c BulletParam id,
       +0x30 dummy poly world 3x4 (0x1cc7800, dummy id [ev+0x20]), +0x60 character 3x4
       (transposed from [[chr+0x48]+0x20]+0x60..0x9f, or [chr+0x58]+0x90)        [C]
  -> 0x1cb7250 -> SprjBulletManager [0x593E8D0] 0x1e03620(mgr, request)          [C]
  -> 0x1dedc20 builds the emitter descriptor (0x440 bytes):                     [C]
       +0x04 BulletParam id, +0x0c owner handle ([obj+8] of 0x1946e70(request+0)),
       +0x3b0 / +0x3e0 the two 3x4 matrices,
       +0xe0..+0x113 the TARGET: lock-on (0x1df5be0, or the owner's lock target through
       [[chr+0x58]+0x280]->vf+0x88), else kind 4 = a point at muzzle + facing x BulletParam.dist
       (facing from the model quaternion [..+0x1d0], 0x1dee376..0x1dee43e)
  -> 0x1e03b50: slot (64 x 0x640, mgr+0x20), 0x1defcf0 copies the descriptor to emitter+0x1b0
  -> emitter update 0x1deffa0: per bullet (numShoot loop), picks the launch matrix (jump table on
     BulletParam +0x9a bits: identity, dummy poly 0x1df2440, character +0x590, ...), copies the
     target to [rbp-0x160] (adds the homing offset scatter, BulletParam +0x54), allocates the
     SprjBulletIns (0x1e03240) and calls
       0x1df6ca0(rdi = BulletIns, rsi = descriptor, rdx = target, rcx = launch matrix, r8d = index)
     at 0x1df0607                                                                [C]
  -> 0x1df6ca0: target -> BulletIns+0x7b0..+0x7e3, position = the matrix's 4th column
     (0x1df7067, BulletIns+0x10), orientation = the matrix (quaternion), velocity = speed x
     normalize(3rd column) (0x1df76df..0x1df77a7, BulletIns+0x40/+0x50)           [C]
```

The launch matrix is three float4 rows: row i = (axis X [i], axis Y [i], axis Z [i], position [i]).
**The bullet flies along axis Z** [C]. The target's kind-4 aim point (+0x20) is what the movement
code can steer to later: 0x1df4f00 (vtable 0x5797068) and 0x1dfe600 read BulletIns+0x7b0/+0x7d0 [C].
The direction is therefore final at the entry of 0x1df6ca0. Both the matrix and the target there are
copies on the caller's stack (`[rbp-0x120]`, `[rbp-0x160]`), so changing them affects only that bullet.

Player firearms (BehaviorParam_PC refType 1 → Bullet; the game's params): pistols 500/510/520/530,
blunderbuss 600–631 (5 pellets, shootAngle −12), cannon/gatling 700–750, rifle spear 800, Reiterpallasch
900, flamesprayer 910–929, bowblade 940–944. Most fire a short "close range" bullet that spawns its child
(HitBulletID = id+1) when it expires (launchConditionType 254). Thrown items are bullets 100–370.

## 3. The hook

| | |
|---|---|
| Patch | `"VR Gun Aim (bullet spawn)"`, disabled by default |
| Site | **0x1df6ca0** (image 0x19f6ca0), the entry of the launch init. 20 bytes `554889e54157415641554154534881ec48010000` (push rbp … sub rsp,0x148), no RIP-relative operand, no branch into the window (rel32 scan: only the three calls to the entry). Absolute-jump form. Replay-after |
| Handler | `bb_hook_vr_gun_aim` → `src/runtime_vr_gun.c` |
| Filter | `[rsp]` = 0x1df060c (the emitter's launch call; the other callers 0x1df6c7d / 0x1df7a3d are not local shots) and descriptor +0x0c = `[player+8]` (player = `[[0x593E878]+0x60]`, vtable 0x578F310) and BulletParam id (descriptor +0x04) in `BB_VR_GUN_AIM_IDS` (500–999) |
| Lock-on | locked = target handle ≠ −1 or kind ≠ 4. Default: the game's aim is kept. `BB_VR_GUN_AIM_LOCK=aim`: the ray is used and the target becomes a kind-4 point 10 m along it |
| Edit | `vg_redirect` (src/vr_gun_math.h): the game's direction is the aim point's direction from the muzzle (`ref=auto`: when it is within 30° of the launch Z, otherwise Z). The turn onto the ray is a pitch then a yaw about world +Y, with no roll. It is applied to the matrix axes and to the aim point about the muzzle. The muzzle and the aim distance are kept. A blunderbuss keeps its spread around the ray |
| Ray | `bbgpu_vr_hands` now also carries the **aim** poses (valid bits 3/4; vk_xr Hands, bbgpu.cpp). Pointing = the aim pose's −Z, relative to the head of the same snapshot, mirrored into the camera frame, then through the first-person camera rows H1 just showed (`vr_gun_view`, called next to `vr_arms_view`). The grip is the fallback (`BB_VR_GUN_AIM_GRIP_PITCH`) |
| Log | every redirected shot (at most 20 lines/s): `VR gun aim: shot N bullet ID #i: yaw a pitch b -> yaw c pitch d (turn e deg from the aim point/launch Z, aim ray; launch Z vs aim point f deg) muzzle x y z`, plus the reason when a player shot is left alone (lock-on, no first-person camera, no controller) |

## 4. How to enable

```
BB_VR_GUN_AIM=1  BB_HOOKS="<the usual VR hooks>;VR Gun Aim (bullet spawn)"
optional: BB_VR_GUN_AIM_LOCK=aim  BB_VR_GUN_AIM_HAND=right  BB_VR_GUN_AIM_IDS=all
          BB_VR_GUN_AIM_REF=auto|target|forward  BB_VR_GUN_AIM_POSE=grip  BB_VR_GUN_AIM_LOG=1
```

It needs first person (`BB_VR_FP=1`, H1). Without first person the shots are left alone and logged.

## 5. What to test

1. Pistol without lock-on: point the left controller 45° left, then up and down, and fire. The log
   shows `yaw … -> yaw …`. The tracer and the hit go where the controller points.
2. The log's "launch Z vs aim point" angle should be about 0 for a pistol. If it is large, the game
   aims along the launch matrix and not at the point: try `BB_VR_GUN_AIM_REF=forward`.
3. Blunderbuss: the pellets spread around the ray. Cannon: the shell follows the ray.
4. Lock on and fire: the log says "lock-on … kept" and the shot goes to the target. Then try
   `BB_VR_GUN_AIM_LOCK=aim`.
5. A thrown Molotov or pebble is not touched (ids 100–370).
6. Child bullets: check that the long-range part of a pistol shot (bullet id+1) also follows the ray.
   It shows as a second log line when it goes through the same launch.

## 6. Risks and open points

- **[L] Child bullets.** Most guns launch a 0.03–0.16 s "close range" bullet whose child (id+1) is the
  real shot. If the child is launched through another path (not the emitter call at 0x1df0607), it may
  go back to the character's facing after a few meters. The log shows whether 501 / 601 … appear.
- **[L] The owner of a child bullet** may be the parent bullet, not the player. Then it is skipped as
  "not the player's" (visible with `BB_VR_GUN_AIM_LOG=1`).
- Gun parry and enemy hit reactions use the bullet's real path, so aiming away from the character's
  facing is a balance change (13 §6). Animations still face forward.
- Aim pose ≠ grip: older bridges without a valid aim pose fall back to the grip's −Z.
- The ray uses the head of the controller snapshot and the camera of the last frame. Latency is one
  frame.
- Online: other players see the shot through the game's own bullet sync. Not checked.

## 7. Files

`src/vr_gun_math.h` (pure math), `src/runtime_vr_gun.c` (handler), `src/vr_gun.h`,
`src/runtime_hooks.c` + `src/hooks.h` (`bb_hook_vr_gun_aim`, `bb_hook_set_gun_impl`),
`src/runtime_vr_head.c` (calls `vr_gun_view` / `vr_gun_register`), `gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`,
`gpu/shadps4/video_core/renderer_vulkan/vk_xr.{h,cpp}` (aim poses), `patches/Bloodborne.xml`,
`tests/test_vr_gun.c` (`vr-gun-test`, 115 checks), `tests/test_hooks.c` (the real window).

## 8. Gun-shot animation speed (2026-10-12)

Headset feedback: "the gun feels a little bit delayed, can we make the gun animation faster? double". The
delay is the shot clip's wind-up before the TAE bullet event plus its recovery after it, not the aim hook.

- **Which clips.** TimeAct ring sub-id (anim % 1000000) **51000-52999**. Evidence: 13 section 7.4 (from the TAE data)
  lists "gun groups 51000-52999, shots: bullet event at 0.33 s (51000), 0.37 s (52150), 0.17 s (52550)";
  13 section 5.4 uses the same range for the left-hand gun blend. The play logs (`out/vr_all_*.log`, `VR FP:
  face=attack` lines) show real shots as **a102_051000 (83 times) and a102_051003 (51 times)**; no other
  50000-52999 id appears in any log. Not confirmed from evidence: 52xxx ids in actual play (only the TAE table
  names them) and any separate quickshot / charged-shot ids (none seen; the left-hand shot is the same
  51000 family). If a 52xxx id turns out not to be a shot, narrow `VR_GUN_ANIM_TO` in `src/vr_gun_speed.h`.
- **Mechanism.** The same hook as the attack speed ("VR Attack Speed (chr dt)", ChrIns+0xE0). While the newest
  ring clip is a shot the dt is multiplied by `gun_speed`, with the attack guards: dt 0 (hit stop) and dt > 0.2
  (hitch) are left alone. The ring keeps the shot id through the recovery, so it is sped up too.
- **Bullet.** The bullet comes from the game's TAE event (a window the clip time crosses), so it is spawned once,
  just earlier in wall time. The gun aim hook fires at the bullet launch and is not touched.
- **Settings.** `BB_VR_GUN_SPEED=<k>` (default 2.0, 1 = off, max 4) or `gun_speed=<k>` in `out/fp.txt` (polled
  4 Hz; a missing key falls back to the env value / 2.0). Logged at start and on each change; with
  `BB_VR_FP_LOG=1` the measured clip rate (~k) twice a second during a shot.
- **Online.** Stays ON in co-op (user decision, 2026-10-12); `BB_VR_GUN_SPEED_ONLINE=0` opts out. At startup, when
  `BB_ONLINE=1` or `BB_ONLINE_ROLE` is set, one `VR online: gun_speed x.. and hurt_speed x.. are active` line
  is logged. What a peer sees: section 10.
- **Files.** `src/vr_gun_speed.h` (decision logic), `src/runtime_vr_atkspeed.c` (hook), `tests/test_vr_gun_speed.c`
  (`vr-gun-speed-test`, 24 checks). Not run in the game yet (range unconfirmed).

## 9. Hit-reaction animation speed (2026-10-12)

Request: "reduce stagger time to half from all hits too". The player's damage-reaction clips play `hurt_speed`
times faster (set `BB_VR_HURT_SPEED=2` and the stagger lasts half as long; **on by default at 2x**, per the user's request on 2026-10-13; `BB_VR_HURT_SPEED=1` turns it off; the anim range is still unconfirmed), through the same dt hook as sections 8 and the
attack speed, with the same guards (dt 0 = hit stop and dt > 0.2 hitches are untouched, so the freeze after a hit
keeps its length).

- **Which clips: NOT confirmed from Bloodborne data.** No hit-reaction id appears in any log or doc as such, and
  the game's animation files are not on disk here. Default range: sub-id **9000-12999**. Evidence and its limits:
  13 section 7.4 lists 9000-12999 as "likely damage" ([L], a guess from the TAE layout); 11 section 1 quotes the
  DS3 head-tracking rules (light hurt anims 10000-15000; its 5000-5500 is a plain loop in Bloodborne, so that part is
  not used; 13000+ is left out because deaths probably live there); the play logs show `a003_011100` and
  `a003_012000` as the clip that followed a dodge and `a003_008000` once (consistent, not proof). If the range
  is wrong the failure is harmless: a wrong id is simply not sped up, or a non-reaction clip in the range is.
- **Confirmed range (headset run 2026-10-13, run28all3): sub-id 20000-20999.** Each time an enemy attack behaviour (e.g. 226300160) ran just before, the player started a003_020100 / 020103 / 020104 / 020113 / 020123 / 020133 / 020134 / 020500 / 020900 (the eye dropped to 0.94 m in one: a knockdown); no clip in 9000-12999 played in two runs. The default is now 20000-20999 (`VR_HURT_ANIM_FROM/TO`); the 9000-12999 evidence below was a guess and is superseded.
- **Finding the real ids.** `BB_VR_FP_LOG=1` prints `VR hurt speed: new clip <id> (sub-id N) is not an attack...`
  for every clip with sub-id 8000 up that is not an attack, a shot or in the range (once per change). Get hit
  (flinch, big stagger, knockdown, get-up), read the ids, then set `BB_VR_HURT_RANGE=<from>-<to>` (inclusive, no
  rebuild) or change `VR_HURT_ANIM_FROM/TO` in `src/vr_hurt_speed.h`.
- **Excluded.** 100000-104999 (events, gestures, and where the victim half of grabs and visceral hits probably
  is: those clips are timing-locked to the enemy's clip and could desync when scaled), 13000+ (deaths),
  attacks 30000-49999, gun shots 51000-52999, dodges 7000s. Anything in 9000-12999 that is a synced grab
  victim would be scaled; none is known to be.
- **Settings.** `BB_VR_HURT_SPEED=<k>` (default 2.0 = on, 1 = off, max 4), `hurt_speed=<k>` in `out/fp.txt`
  (polled 4 Hz; a missing key falls back to the env value / 2.0), `BB_VR_HURT_RANGE`. Logged at start and on
  every change; with `BB_VR_FP_LOG=1` also the measured clip rate during a reaction.
- **Online.** Stays ON in co-op; `BB_VR_HURT_SPEED_ONLINE=0` opts out (section 10).
- **Files.** `src/vr_hurt_speed.h`, `src/runtime_vr_atkspeed.c`, `tests/test_vr_hurt_speed.c`
  (`vr-hurt-speed-test`, 25 checks). Not run in the game yet.

## 10. What a co-op peer sees when our clips run at 2x (read from the repo; mostly UNVERIFIED)

No packet capture of the game's own character sync exists (19 section 10: the game's P2P socket carried no
traffic in the traced runs; `src/runtime_vr_netstate.c` only watches connection flags; `docs/vr/coop-net-trace.md`
traces NP/HTTP calls, not the character state). So most answers are the Souls-series model, not measurements.

| Question | Answer | Evidence |
|---|---|---|
| What is sent (anim id, clip time, position)? | **Unverified.** The design (19 section 10, from the game): each client simulates its own hunter and sends position and animation state; the port's pose channel (34) states that "the root's own world position and heading come from the game's character sync" and that the base game syncs the partner's character animation. Which fields exactly (id, time, position) was never read from the wire. | 34 header and section 1; 19 section 10 |
| Does the peer replay at 1x or follow our clip time? | **Unverified.** Likely it plays the announced animation on its copy of our character with its own timing, or follows the sent state; in both cases it ends up short of or ahead of our 2x clip. Expect a visual mismatch (a quick shot or flinch), not a crash. The attack-speed header already assumes "co-op peers see the faster animation through the net state but their copy of the moveset times are unchanged". | `runtime_vr_atkspeed.c` header, 19 section 10 |
| Who decides damage and hits? | **Unverified.** The Souls-series model (19 section 10): the attacker's client decides hits on remote players, the host decides for enemies. Our gun bullet is spawned by the game's TAE event on our machine and is redirected by our gun aim hook; it is the game's own bullet sync (27 section 6, unchecked) that tells the peer. The shot is fired at the same TAE event, only earlier in wall time. A hit on us is decided by whichever machine the game uses; our stagger length is local to our machine. | 19 section 10, 27 section 6 |
| Does anything we change run on or write to the peer's game? | **No, from the code.** The hook only touches the character whose ChrIns is `[[WorldChrMan]+0x60]` with the PlayerIns vtable (`as_hook`: `player != chr` returns at once), i.e. the local player, and it only multiplies that character's dt (ChrIns+0xE0) in our process. We send no packets and write nothing to a remote character; the peer's PC runs its own unmodified game. | `src/runtime_vr_atkspeed.c` `as_hook` |

What this means in practice (reasoning, not tested): a shot at 2x means the peer sees our hunter finish the gun
animation earlier, or a mismatch between the animation they play and the position/state we send; a half-length
stagger means we recover earlier than the game's timings on the other machine expect (a hit that lands on the peer's
view of us during our get-up could look odd, or the peer could consider us still staggered). Both are a gameplay
advantage in co-op. To check: two instances with `BB_VR_FP_LOG=1` and the peer's view recorded (needs the P2P sync
working end to end, which it was not).

## 11. Shots too far left: the bullets followed the controller, not the visible barrel (2026-10-13)

Player report: "when I point the gun and shoot, it goes totally too much to the left".

**Cause.** The bullets followed the left controller's aim pose (its −Z), but the gun the player sees and points
with hangs off `L_Weapon`, which the arms IK places at grip · `arm_offset` · `arm_rot_l` (24, commit 863efde:
the trim is applied in the controller frame, yaw right = about −Y, pitch up = about +X, not mirrored for the
left hand). The player's `out/fp.txt` has `arm_rot_l=30,25` and `arm_offset=0,0.04,0.07,-105`: the visible gun is
turned 30° right and 25° up from the controller, about 38° in all (`tests/test_vr_gun.c`). Lining the visible
barrel up with a target therefore puts the controller ray, and the bullets, about 30° left and 25° below it.
No sign flip: the trim's signs are right (+30 = right, +25 = up in the world, tested), and so is the log's.

- **Log sign.** `vg_yaw_pitch` heading = atan2(x, z): 0 along world +Z, growing toward +X. The camera rows are a
  rotation (right × up = forward), so a camera looking along +Z has right = +X: **a growing heading is a turn
  right, a negative change is left** (tested: a controller turned 10° left reads heading −10).
- **Live log** (`out/vr_all_stereo.log`, run with that fp.txt, 31 redirected shots): every turn from the game's
  aim is to the left and down, e.g. shot 87 bullet 600 `yaw -148.1 pitch -3.2 -> yaw -158.3 pitch -7.2 (turn
  10.8 deg)`, shots 67-71 −13°/−6°, shots 92-96 (bullet 601) −10.4°/−3.8°. The game's aim is the body's facing,
  so these numbers are the controller ray against the facing, consistent with a ray that sits left of and
  below where the player pointed the visible gun.

**Fix.** A new ray source **`barrel`**, the default: the arms module publishes, after each IK write, the weapon
bone's model rotation relative to the H1 camera that placed it (`vr_arms_weapon_cam`, `vg_weapon_in_cam`), and
the hook takes the bone's barrel axis through the same camera rows as the old ray (`vg_barrel_in_world`). So
`arm_offset`, `arm_rot_l`, the bind and the animated hand → weapon local are all in it, and the body's pose is
not. The barrel axis is the bone's **−Z** (the weapon models' barrel: the pistol's muzzle dummy at z −0.44, 13
7.3; the sandbox aims guns along the model's −Z) [L, not seen in the headset yet]. Guards: without the arms (no
`BB_VR_ARMS=1`, no first person, weight 0) or when the barrel is more than 70° from the controller ray (a wrong
axis), the shot uses the aim ray and the log says why. Then the live trim: heading + `gun_yaw`, pitch +
`gun_pitch` (`vg_trim`).

**Settings** (env = start value; the `out/fp.txt` keys are read as tokens anywhere on its one line, 4 Hz, a
missing key falls back to the env value; logged at start and on every change):

| Env | fp.txt | Default | |
|---|---|---|---|
| `BB_VR_GUN_AIM_POSE=barrel\|aim\|grip` | `gun_src=` | barrel | aim / grip: the old controller rays (aim falls back to grip) |
| `BB_VR_GUN_YAW=<deg>` | `gun_yaw=` | 0 | + = shots further right (−90..90) |
| `BB_VR_GUN_PITCH=<deg>` | `gun_pitch=` | 0 | + = shots higher (−90..90) |
| `BB_VR_GUN_BARREL_AXIS=-z` | `gun_axis=` | −z | the bone axis that is the barrel (±x, ±y, ±z) |
| `BB_VR_GUN_BARREL_MAX_DEG` | | 70 | farther from the controller ray: the aim ray |

Log lines: `VR gun aim: source barrel axis -z, trim yaw +0.0 pitch +0.0 deg (...)` at start and on change;
per shot `... (turn ... from the aim point, barrel -z ray; ...)` and `VR gun aim: shot N: barrel 38.4 deg from
the controller ray, trim ...`, or `aim (barrel: <why>) ray` when it fell back.

**What to try.** Keep `arm_rot_l=30,25`, aim with the visible gun. If the shots still drift, A/B `gun_yaw=5` /
`gun_yaw=-5` (then 10 / −10) and `gun_pitch=±5` in fp.txt while shooting a wall. "barrel N deg from the
controller ray" near 38 confirms the axis; a fallback line "barrel too far" means −Z is not the barrel: try
`gun_axis=+y` / `-y` / `+x`. `gun_src=aim` gives the old behaviour.

**Files.** `src/vr_gun_math.h` (`vg_weapon_in_cam`, `vg_barrel_in_world`, `vg_trim`, `vg_hand_trim`,
`vg_key_float/word`, `vg_axis_parse`, `vg_src_parse`), `src/runtime_vr_gun.c` (sources, trims, control poll,
logs), `src/runtime_vr_arms.c` + `src/vr_arms.h` (`vr_arms_weapon_cam`), `tests/test_vr_gun.c` (`vr-gun-test`,
249 checks: the barrel equals the controller ray turned like the IK hand for the player's settings, a skewed
bind and any body pose; trim signs; fp.txt keys).

## Fire on release and the gun laser

The left trigger (gun role) now fires when it is let go, not while it is pulled. While it is held past
the threshold the game gets no L2 and an analog l2 of 0; on release (a tap too) xr_input sends one L2
pulse of 100 ms (digital L2, analog 255), logged as "left trigger released: L2 pulse 100 ms". A pull during
a pulse waits for it to end. `BB_XR_GUN_FIRE=release|press` (default `release`; `press` is the old
held-L2). Other meanings of the left trigger (menu pointer, held-item put back, switch mode) make no
pulse. The gun-thrust gesture (ShootGate) and parry paths still emit their own L2 pulses and are untouched.
State machine: `xrpad::GunRelease` in `gpu/xr_bridge/xr_input_logic.h`, tested in `xr_math_test.cpp`.

Laser: layout key `game.gun_laser` = `"hold"` (default: shown while the trigger is held aiming,
`BBXR_RAW_GUN_AIM` in `BbXrInput.reserved[1]`, and flashed on the shot), `"always"` or `"off"`;
`debug_gun_ray: true` still means always. Trim: `game.gun_laser_yaw_deg` / `game.gun_laser_pitch_deg`
(default 0) turn the drawn ray like the host's `gun_yaw` / `gun_pitch` (`vg_trim`): + = right / up, yaw
about the world vertical. Set them equal to the fp.txt keys so laser and shots agree.
