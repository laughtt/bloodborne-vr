# VR controls

What each controller input does in the game with the VR UI on. The full technical table, with the code
paths, is in [findings/26-game-ui.md](findings/26-game-ui.md#controls-in-the-game-one-button-one-action-v2).

## Buttons

| Control | In the world |
|---|---|
| **B** (right, upper) | **Roll / dodge**, hold to sprint |
| **A** (right, lower) | **Switch the right-hand weapon** (one press, one swap) |
| **Y** (left, upper) | **Interact**: doors, levers, pick up, talk |
| **X** (left, lower) | **Switch the left-hand weapon** (one press, one swap) |
| Left stick | Move |
| Left stick click | Moving: jump. Standing: open / close the **dashboard**. Held 1 s: recenter |
| Right stick | Turn / camera |
| Right stick click | Open the **item wheel** |
| Left trigger | Hold: aim the gun (laser); release: shoot. A left-hand thrust also shoots |
| Right trigger | Nothing in the world (it is the pointer click in the dashboard) |
| Left grip | Transform (L1) |
| Right grip | Heavy / charge modifier for swings; a quick tap swaps the right weapon |
| Left menu button | Options (pause); held 1 s: recenter |

There is no heal button: heal from the item wheel, or bring a vial to the mouth.

In the game's own menus: A confirms, B goes back, either stick moves.

## Fighting

| Gesture | Does |
|---|---|
| Swing the right hand | Light attack; swing with the right grip held: heavy attack |
| Thrust the left hand (trigger held) | Shoot |
| Shoot an attacking enemy at the right moment | Parry (stagger) |
| Jab the right hand at a staggered enemy, up to 2 m away | **Visceral attack** (automatic after a parry) |

## Items

1. Open the **item wheel** (right stick click) or the **dashboard** (left stick click, standing).
2. Pick the item by pointing at it and pulling the **right trigger**. It goes **straight into your hand**; no grip squeeze.
   The weapon of that hand hides and that hand cannot attack until the item is used or put back.
3. Use it with a gesture:

| Item | Gesture |
|---|---|
| Blood vial, pellets, drinks | Bring it to the **mouth** (24 cm zone, forgiving) |
| Bells | Bring it to the **head** |
| Papers (fire, bolt) | Touch it to the **blade**, or to the big zone (42 cm) in the air 25 cm in front of the blade toward you |
| Molotov, knives, pebble, oil urn | **Throw**: hold the **grip** to aim (an arc and a ring show where it lands), let go to throw; the **trigger** puts it back |
| Hunter's marks, shaman bone blade | **Squeeze hard** (crush) for 0.4 s |

   `BB_VR_THROW_MODE=flick` (or `throw_mode: "flick"` in the `held` settings) brings back the old forward-flick throw.

   **Aim pitch**: the launch angle is `bias + gain * (grip elevation - level)`, clamped to [-30, 60] deg. A neutral hand (the grip pose's -Z points about 20 deg above level in a relaxed fist) throws almost flat (3 deg); raise the hand and the angle grows 1.2 deg per degree (35 deg grip = 21 deg launch, lobbed), lower it and it aims down. The drawn arc, the ring and the game's launch all use this one direction. Tuning: `held` settings `throw_aim_pitch_bias_deg` (3), `throw_aim_level_deg` (20), `throw_aim_gain` (1.2), `throw_aim_min_pitch_deg` (-30), `throw_aim_max_pitch_deg` (60); env overrides `BB_VR_THROW_AIM_BIAS`, `BB_VR_THROW_AIM_LEVEL`, `BB_VR_THROW_AIM_GAIN`. If throws still go too high, raise `level`; too low, lower it.

4. **Put it back**: press a trigger, or open any menu.

**Drag and drop** (dashboard): squeeze the grip on an inventory card, drag it onto a quick slot and let go to
assign it; drop a quick slot's item on the grid to clear it; let go anywhere else to cancel.

## Co-op

In a join-code room the bells ring by themselves; nothing to press. The room code shows in the dashboard
title (`Room K7Q-M4X 2/3`).
