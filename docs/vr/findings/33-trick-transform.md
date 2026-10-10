# 33 - Trick-weapon transform in VR (wrist snap, L1 fallback)

Date: 2026-10-10. Track: VR melee (docs 13, 14, 20). Built and tested in the sandbox; nothing run against
the game. Flat play transforms with L1; the game's own attack path (judges 390/395 from A, 790/795 from B,
doc 14 1.7) does the animation, hit shapes and form change.

## 1. Design: one button, one action

| | Action | Maps to |
|---|---|---|
| **Wrist snap** (chosen) | The weapon hand's grip turns >= 90 degrees (any axis) within 150 ms while the grip travels <= 0.2 m | L1 pulse |
| **Fallback button** | Left grip (already L1 in `xr_input.cpp`) | L1 |
| Two-hand pull apart (not built) | Left hand grabs the weapon hand, then both pull apart | Reserved for the two-handed forms; see 4 |

Why the snap and not the pull apart: it works for every trick weapon (a pull apart needs a free off hand,
and the left hand holds the gun or the lantern), it is one hand and one motion, and its signature (rotation
without translation) is the opposite of a swing's, so it can be told apart numerically. The pull apart
depends on the two-hand grab (doc 13 W1), which is not built.

## 2. The detector: `gpu/xr_bridge/trick_transform.h`

Header only, like `swing_logic.h` (same `HandSample`, no OpenXR, no heap). `TrickTransformDetector::Update`
returns `fired` once per snap and exposes `Veto()`.

- Fire: largest orientation change inside `window_s` (0.15) >= `min_angle_deg` (90) and the largest grip
  travel in that window <= `max_travel_m` (0.20); armed again only after the wrist rests (< 3 rad/s) and
  `cooldown_s` (0.8). So the snap back is not a second transform.
- A swing moves the hand 0.4-0.7 m in 150 ms, so it never fires; a casual wrist turn (100 degrees in 0.4 s)
  is too slow.
- **Keeping the swing classifier from misfiring.** The classifier's probe sits 0.25 m along the blade, so a
  wrist snap moves it at 2-7 m/s (a light or heavy attack: the sandbox test shows a sideways snap does read
  as an attack when ungated). `Veto()` is true while the turn rate is >= `veto_rate` (9 rad/s) and the grip
  speed is <= `veto_grip_speed` (0.8 m/s), from the first fast frames (well before the classifier decides,
  about 90 ms after its onset), for `veto_hold_s` (0.25) after, and `lock_s` (0.6) after a fired transform.
  The caller skips `SwingClassifier::Update` while it holds (not while R2 is charging); the classifier restarts
  by itself when the samples resume (`max_gap_s`). Real slashes (the scripted light, heavy and 180 ms ones)
  never raise it.
- Trade-off: a pure wrist slash with no arm travel is treated as a snap/veto. That is the intended price.

## 3. Sandbox (done)

- `combat.cpp/.h`: the detector runs on the weapon hand in `Combat::Update`; L1 edge in `Combat::Frame` (the
  left grip, as the game). `CombatEvents::set_form` carries "A"/"B"; `main.cpp` and `combat_view.cpp` set
  `config.form`. `scene.cpp` resets the attack picker when the form changes (hit shapes swap through
  `FormAttacks`/`AttackShapesFor`; the swing class and costs follow through `LookUpSwingWeapon`, form B uses
  judges 400/550/570). A weapon counts as two-form when its hitbox JSON has a form B attack (else a known
  list); others get a soft tick and a log line.
- Models: the dump has one mesh per weapon (no `_N` slots extracted), so the visual change is the HUD
  (FORM A / FORM B beside the stamina bar, large for 1.6 s) and the hitbox wireframes.
- Haptics: click (0.8, 40 ms) then lock (0.5, 80 ms, 90 ms later) on the weapon hand; they survive the form
  change's swing reset. No transform cost (stamina) is charged in the sandbox.
- Config: `swing.transform` in `sandbox3d.json` (README section). Log: `transform` events in
  `sandbox3d_swing.jsonl`.
- Tests: `swing_logic_test` (149 checks: snap on three axes, rejects, cooldown, slash unaffected, glitch,
  off) and `sandbox3d_combat_test` (203: snap, button, menus, one-form weapon, open swing dropped, config).

## 4. Game side plan

1. **Trigger.** Reuse the bridge input path: pulse `BBXR_BTN_L1` for about 80 ms (`Pulse(BBXR_BTN_L1, now)`
   in `xr_input.cpp`, the way `Fire` pulses R1/R2). Caution: today the left grip holds L1 level
   (`grip[0] > 0.6`); a snap pulse and a held grip must not fight (the pulse adds nothing when L1 is
   already down, which is fine). No game command is needed for the base case. If a press is dropped during
   an attack, hold it until `ChrActionFlag 87` (the input window of doc 20) like the `AttackScheduler`
   does for R1/R2: add a `Transform` kind there (queue, drop after about 0.4 s).
2. **Wiring.** `xr_input.cpp` already has the right hand's grip pose and velocity. Feed
   `TrickTransformDetector::Update` with the same `HandSample` the classifier gets (doc 20 6.3); skip
   `SwingClassifier::Update` while `Veto()`. Fire = L1 pulse + haptic (0.8/40 ms then 0.5/80 ms at +90 ms).
3. **Gating.** Only while a transformable right weapon is held (the equip table at `+0x64`, doc 13; the
   form-B attack set from `hitbox/wp_<id>.json`, same as the sandbox) and no menu has the controls.
   Otherwise do nothing (soft tick).
4. **Two-handed forms (later).** Two-hand grab on a two-handable weapon -> L1, per doc 13 W1; when built,
   run it into the same pulse and veto the classifier the same way.
5. **Open points (to measure in the game).** Whether the game accepts L1 during the recovery frames of an
   attack (queue it if not); whether transform costs stamina (BehaviorParam 390/395/790/795 `stamina`, read
   the same way as doc 20 1.4); that a held-L1 from the grip does not already transform on every
   grip squeeze (it does today, which is the button fallback; the bridge must then not also hold L1 from
   the gesture). Tune `min_angle_deg`, `veto_rate` with recorded user snaps (`.bbxrrec`, doc 13 W1: flick -> L1
   in 9/10 and < 1 false fire per minute are the acceptance numbers).
