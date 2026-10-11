# 41: Gun parry feel: thrust onset gate and aim assist

Date: 2026-10-11. Branch `vr`. Status: built and unit-tested (bbport_xr_math_test, vr-gun-test). Not run
in the game (the user was in the headset). The lock-on candidate list read is **[L]** (offsets from 16 and
the first-person code, never checked against several live enemies).

## 1. Facts that set the budget

- A gun parry is a bullet hit on an enemy inside its "parry possible" window (TAE event 0 flag 5/73).
  Gascoigne has exactly one 30 Hz tick (33 ms) at the start of each attack window (14 1.6). Common
  enemies carry no flag 5 in their attack animations, their vulnerability comes from elsewhere (open).
- The window is short, so latency and aim error are the two things a VR player cannot control: detection
  delay, the 100 ms L2 press, the game frame, the bullet's travel time (short-range bullets 0.03-0.16 s).
- Enemy attack state and positions of all enemies are not read by us. What is readable: LockTgtMan
  (0x593E860, +0x2820 locked byte, list at +8, next +0x80, point +0x00, flags +0x90) and the launch hook's
  target (handle, kind 4 point).

## 2. Before

Thrust fired when the left hand moved faster than 1.8 m/s along its pointing axis (velocity relative to
the head, projection only), re-armed under 0.5 m/s, no alignment test, no debounce. A sideways swing at
45 degrees with 2.6 m/s fired; the threshold sat well up the jab's acceleration ramp. The bullet went
along the controller ray with no assist; with a lock-on the game's aim was kept.

## 3. The equation

Thrust (xrpad::ShootGate): rel = hand velocity - head velocity, t = rel . fwd, a = t / |rel|. Fire when
armed and t >= 1.5 m/s and a >= 0.8 (stroke within 37 degrees of the barrel) and t rising and 250 ms since
the last shot; re-arm at t < 0.5; a snap turn or recenter disarms.

Aim assist (vg_assist_dir): for each lock-on candidate within 12 m, angle to the ray, cone(d) =
clamp(atan(0.6/d), 4, 18) degrees. The smallest angle inside its cone pulls: w = 0.8 * (1 - smoothstep(0.4
cone, cone, angle)), dir = normalize(lerp(ray, to_target, w)). Muzzle and bullet speed are untouched.

## 4. Tunables (environment, read at start)

| Key | Default | Effect |
|---|---|---|
| BB_XR_SHOOT_SPEED | 1.5 | onset speed m/s (0.5..6) |
| BB_XR_SHOOT_ALIGN | 0.8 | cos of the allowed angle off the barrel (-1..0.99) |
| BB_XR_SHOOT_REARM | 0.5 | re-arm speed m/s |
| BB_XR_SHOOT_DEBOUNCE_MS | 250 | minimum time between thrust shots |
| BB_VR_PARRY_ASSIST | 0.8 | pull strength 0..1, 0 off |
| BB_VR_PARRY_RADIUS_M | 0.6 | body radius for the cone |
| BB_VR_PARRY_CONE_MIN / MAX | 4 / 18 | degrees |
| BB_VR_PARRY_RANGE_M | 12 | candidate range |

Not live-reloadable (no control file for the gun or the thrust yet): restart the game after changing.

## 5. What is not done

- The attack wind-up of the enemy is not known, so the assist cannot prefer the attacker: it picks the
  candidate closest to the ray. Reading enemy TAE state needs RE.
- Haptic confirmation of a successful parry needs a stagger signal we do not have.
- A wider parry window is a balance change and is not done.

## 6. Test

Log lines "VR gun aim: assist pulled/no target in the cone", "assist target #i, ray x deg off it" (needs
BB_VR_GUN_AIM=1 and the gun hook). Without a lock-on, point the left controller at an enemy 5 degrees
off and fire: the shot should land. If "0 candidates" always appears, the candidate list is empty
without a lock-on: then lock on first (the game aim is kept) or the list must be read elsewhere.

## 7. Visceral

In Bloodborne a visceral is R1 pressed in front of an enemy in the riposte (parried) state; the game decides.
The runtime cannot read the enemy's state (no enemy ChrIns / stagger read exists), so the visceral is a fallback:

1. A **parry candidate** is a player firearm shot (the launch hook, BulletParam id in `BB_VR_GUN_AIM_IDS`, also
   with `BB_VR_GUN_AIM=0`: then only the notification happens, nothing is redirected) that has a target: the
   lock-on target (the LockTgtMan node with flags & 0x20) when the shot is locked on, else the lock-on candidate the
   controller ray points at (the parry assist's cone, strength irrelevant). No target, no window.
2. The shot opens a **window** (`src/vr_visceral_math.h`). Nothing is pressed for `delay_ms` (the stagger has to
   start) and nothing after `window_ms`. While it is live the pad thread presses R1 (100 ms) once, then closes it.
   The target follows the nearest lock-on candidate within `track_m` (enemies move and fall), read at most every 50 ms.
3. Optional confirm (`BB_VR_VISCERAL_CONFIRM=hit`): a player character hit (`vs_hit` in `src/runtime_vr_stamina.c`)
   within 600 ms of the shot must also have been seen; its point moves the target if it is near.

Modes, `BB_VR_VISCERAL_MODE`:

- `auto` (default): fires when the head or the right hand is within `reach_m` of the target (horizontal; the lock point's
  height is not trusted).
- `thrust`: the right hand's speed relative to the head, projected on its aim, passes the gate of the gun parry
  (speed, alignment of the stroke to the target, rising, debounce, armed/re-arm, reach). Outside a live window a thrust
  does nothing and does not use up the arming. Snap turn, recenter or a view jump (head yaw > 25 deg or camera > 0.25 m
  between two steps) disarms.
- `swing`: never; the swing gesture of the bridge already sends R1 by itself.

**False positive:** if the shot did not actually parry, the pressed R1 is a normal R1 attack at the target. The window
(2.5 s) and the reach keep it rare; `confirm=hit` removes the shots that did not touch anything.

### Tunables

| Key | Default | Range | Effect |
|---|---|---|---|
| BB_VR_VISCERAL_MODE | auto | auto/thrust/swing | see above |
| BB_VR_VISCERAL_CONFIRM | aim | aim/hit | hit: wait for a player character hit within 600 ms of the shot |
| BB_VR_VISCERAL_SPEED | 1.5 | 0.1..10 | thrust speed m/s |
| BB_VR_VISCERAL_ALIGN | 0.7 | -1..0.999 | cos of the stroke to the target |
| BB_VR_VISCERAL_REARM | 0.5 | 0..10 | m/s below which the thrust re-arms |
| BB_VR_VISCERAL_REACH_M | 1.2 | 0.1..5 | horizontal reach to the target |
| BB_VR_VISCERAL_DEBOUNCE_MS | 300 | 0..3000 | between two thrust presses |
| BB_VR_VISCERAL_WINDOW_MS | 2500 | 200..10000 | the stagger window; after it nothing is tried |
| BB_VR_VISCERAL_DELAY_MS | 400 | 0..2000 | no R1 earlier than this after the shot |
| BB_VR_VISCERAL_LOG | 1 | 0/1 | the lines below |

### Test lines (`VR visceral:` in the log)

- `mode auto, confirm aim, window 400-2500 ms ...` at start.
- `window opened (locked on | aimed at a lock-on candidate), target 3.2 m away, confirm: ...`; `shot without a target ...: no window`.
- `confirmed by a player hit (atk N) X ms after the shot` (any confirm mode, while a window is open).
- `R1 sent (auto: the hand|head ... m from the target <= reach ...)` or `R1 sent (thrust x m/s >= ..., align ..., hand ... m ...)`.
- `thrust rejected: speed,align,... (best frame: ...; peak thrust ...)`, one line per thrust while live.
- `window closed (expired | replaced by a new shot)`; a shot without a target leaves an open window alone.

Headset check: parry an enemy with the left gun (aimed or locked on), step in or thrust; the visceral animation should start.
If R1 is sent but the enemy was not staggered, the player attacks normally (the false positive).

### What is left

A real riposte read: the enemy ChrIns from the hit hook's rdx/rsi (the parry connects as a hit of the gun bullet), its
state / the ThrowParam / TAE "throw possible" window, so the window opens only when the game really allows the visceral
and the target comes from the enemy itself instead of the lock-on list.
