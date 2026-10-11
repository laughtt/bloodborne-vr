# sandbox3d: the 3D VR sandbox for body interaction

`bbport_xr_sandbox3d.exe` puts the hunter from the user's own Bloodborne dump into the headset. The
arms follow the controllers through IK, the fingers curl with the trigger and grip, and the
weapons sit in the hands with their hit capsules. A mirror copy shows the whole pose. Use it to
iterate on arms, hands, weapons and hitboxes in seconds, without the game.

The app renders both eyes itself and hands them to `bbport_xr.dll` through the ABI 4 stereo calls
(`AcquireStereo` / `ReleaseStereo`, documented in `../bbport_xr_bridge.h`). The game's stereo
renderer can use the same path later.

## Run

```sh
# MSYS2 CLANG64 shell, headset on (Pimax Play / SteamVR running)
scripts/vr_sandbox3d.sh                 # build, run the offline tests, run from out/sandbox3d/run-<time>/
scripts/vr_sandbox3d.sh --extract       # re-extract the assets from the dump first
scripts/vr_sandbox3d.sh --offline       # no headset: scripted poses -> out/vr_assets/preview/sandbox3d_*.png
BB_BUILD_LOCK=/path/to/lock scripts/vr_sandbox3d.sh   # serialize the build with flock
```

- **Assets.** `scripts/vr_assets/extract.py` writes them to `out/vr_assets/`. That folder is git-ignored and never committed. The dump at `BB_GAME_DIR` (default `C:/games/CUSA03173`) is only read.
- **Hit shapes.** They come from `scripts/vr_assets/hitbox_extract.py` (`out/vr_assets/hitbox/`, doc 14) when it has run: the weapon's attacks per form (bbhitbox `wp_<id>.json`, points on the weapon or on named body bones) and the player's hurtboxes (`chr_c0000.json`, capsules on named bones).
- **Bridge defaults.** The sandbox sets `BB_XR_STAGE=1` (floor at y = 0), `BB_XR_HANDS=0`, `BB_XR_LASER=0` and `BB_XR_INPUT_DIAG=1`, unless they are already set.

## Controls

| Input | Action |
|---|---|
| Controllers | Arms (two-bone IK to the grip pose), hand orientation |
| Trigger | Index curl |
| Grip | Middle, ring and little finger curl |
| Both trigger and grip | Thumb curl |
| A held 1.5 s, standing in a T-pose | Calibration: eye height sets the body scale, controller span sets the arm scale. Saved to `out/vr_assets/sandbox3d_calibration.json`, with a haptic tick |
| B | Next right-hand weapon (from `assets.json`) |
| Y | Next attack's hit shapes. Cycles through the attacks of the configured `form`, then "all" |
| X | Mirror on/off; switching it on places it in front of you |
| Right stick click | Skeleton drawn on top of everything, or depth tested |
| Left stick click held 1 s | The bridge's recenter. It does nothing in stereo mode |
| Left stick | Walk where you look (`move`), A held while walking: sprint ([locomotion](#locomotion-and-the-comfort-dash-the-locomotion-object-of-sandbox3djson)) |
| Right stick left / right | Snap turn 30° about your head (held: again every 0.3 s), or smooth turn (`turn`; the game's turning model) |
| A tapped: while walking / standing | Dodge (a roll) in the stick's direction / backstep, as a comfort dash or the raw animation (`mode`) |
| Left stick click while walking | Jump (the game's L3 while sprinting; the bridge passes the click through at once while the stick moves) |
| Right controller swung | A slower swing is LIGHT (R1), a fast one HEAVY (R2); the wind-up held still overhead charges R2 and the swing out of it is CHARGED. Stamina is paid only when the swing connects; a dodge, backstep or jump pays at once; at 0 nothing starts. A stamina bar head-locked below the view and above the left wrist ([swings and stamina](#swings-and-stamina-the-swing-object-of-sandbox3djson)) |
| Right grip + B, left grip + X, right grip + A, grip at a body slot, left palm up; left stick click + right stick flick | The VR menu prototypes ([below](#vr-menu-prototypes-outvr_assetsui_layoutsjson)). B, X and A alone keep their actions above |

## Live tuning: `out/vr_assets/sandbox3d.json`

- The file is written with every default on the first run. It is polled twice a second, so saving it applies the change at once.
- Keys you leave out keep their defaults.
- Every reload prints the full config.

| Key | Default | Meaning |
|---|---|---|
| `render_scale` | 0.6 | Of the runtime's recommended eye size (the Dream Air recommends 3832×3076) |
| `near_z`, `far_z` | 0.03, 200 | Clip planes; also given to the compositor with the depth |
| `want_depth` | true | Submit depth (XR_KHR_composition_layer_depth) when the runtime takes it |
| `eye_height_m`, `arm_scale` | 1.65, 1 | Body size; the calibration overrides them |
| `floor_y` | 0 | Floor height in LOCAL space (only used without STAGE) |
| `follow_head_height` | true | Crouching lowers the body. False keeps the feet on the floor |
| `eye_offset` | [0, 0.10, −0.09] | The eyes relative to the Head joint, in model space |
| `body_yaw_deadzone_deg` | 30 | How far the head turns before the body follows |
| `body_back_m` | 0 | Pushes the body back from the eyes |
| `elbow_pole` | [0.4, −0.85, 0.35] | Right elbow direction in body space (+X out, +Y up, +Z back); mirrored for the left arm |
| `forearm_twist` | 1 | Share of the wrist roll that `ForeArmTwist` takes; `ForeArmTwist1` takes half of it |
| `hand_offset` | pos [0, −0.01, 0.07], rot [−15, 0, 0] | Grip pose → wrist, for the right hand (mirrored left) |
| `trigger_curl_deg`, `grip_curl_deg`, `thumb_curl_deg` | 95, 95, 40 | Middle-joint angle at full curl |
| `rest_curl` | 0.15 | Curl with nothing pressed |
| `weapon`, `left_weapon` | "Saw Cleaver", "Hunter Pistol" | Name, `wp_a_XXXX`, or "" for none |
| `weapon_grip`, `left_weapon_grip` | 0 | Offset of the weapon relative to `R_Weapon` / `L_Weapon` |
| `weapon_align` | true | Turns a blade's long axis into the grip's up/forward plane and points a gun's barrel along the grip's −Z. False uses the game's raw bone frame |
| `form`, `attack` | "A", 0 | Which form's attacks, and which one, show hit shapes (−1: all, weapon shapes only) |
| `placeholder_radius` | 0.06 | Capsule radius over dummies 100..1xx when there is no hitbox JSON |
| `mirror_distance_m` | 2 | Distance to the mirror plane |
| `show` | | `body`, `head` (off: first person), `masked`, `skeleton`, `dummies`, `weapon`, `hitboxes`, `grid`, `controllers`, `laser`, `mirror`, `skeleton_on_top`, `hurtboxes` (the player's, on the mirror copy), `hurtboxes_self`, `fp_hide` (`head`, `body`, `all`, `none`: your own body hidden as the game's first person hides it, docs/vr/findings/16 4; empty: `head` decides) |

### Hit shape scale: `physics.hit` (docs/vr/findings/32-vr-hitbox-scale.md)

The weapon's hit capsules, thicker and longer for VR (you stand closer to enemies than the flat game assumes). `"physics": {"hit": {"enabled": true, "radius_scale": 1.5, "reach_m": 0.15, "short"|"medium"|"long": {"radius_scale": x, "reach_m": y}}}`. `null`/missing = the global value. The wireframe draws the scaled shapes and the weapon physics hits with them. The maths is `src/vr_hit_scale.h`, shared with the game (`hit=1 hit_scale=1.5 hit_reach=0.15` in out/fp.txt).

## VR menu prototypes: `out/vr_assets/ui_layouts.json`

Pop-up menus for Bloodborne VR, drawn with the game's own item pictures and menu font: try a design in
the headset in seconds, then compare designs in the interaction log. Five presets come built in
(A to C follow doc 18's ranking, E is doc 17's fuller inventory); the layout file defines them and
is reloaded when saved.

**Art.** `scripts/vr_assets/ui_extract.py` writes `out/vr_assets/ui/` (git-ignored, never commit it;
`scripts/vr_sandbox3d.sh` runs it when `ui/icons.json` is missing):
- `icons.png` + `icons.json`: 237 item pictures at 128 px. Each is `menu/menu_knowledge_<iconId:05d>.tpf.dcx`, one 400×400 BC7 texture in PS4 tiling (`tpf.py` decodes TPF, BC1–BC5, BC7 and the tiling). The iconId comes from EquipParamGoods (1..345) and EquipParamWeapon (1000+); names come from `msg/engus/item.msgbnd.dcx` (FMG 10 goods, 11 weapons). The JSON also has stack sizes, categories, and each weapon's hand and model. When the items catalog of doc 17 exists (`out/vr_assets/items/items.json`), each goods item also gets its `use` (heal, ammo, throw, buff, tool, other) and its `effect` ("fire attack +80, 60 s").
- `font.png` + `font.json`: the menus' font "TT-Reimin Y10 B" from `menu/engus/font.gfx`. Its DefineFont3 outlines (`gfxfont.py`) are rendered as a signed distance field, so text stays sharp at any distance.

### Presets

| Preset | Menus | In the headset |
|---|---|---|
| **A body_slots** (default) | `body_slots`, `wrist_counters` | Items sit on the body as markers: right hip Saw Cleaver, over the right shoulder Hunter Axe, chest Blood Vial, left hip Molotov, left hip front Pebble, behind the left hip Blunderbuss. Reach a marker (it lights up, with a tick) and squeeze the grip: weapons go into their hand, consumables count down (a stronger pulse). Sitting down switches to the seated offsets (`posture` auto). Turn the left palm up to see the vial and bullet counts |
| **B wrist_strip** | `wrist_strip` | Turn the left palm up: vials, bullets and Molotov appear above the inner wrist. Touch one with the right index fingertip, or point the right hand at it and pull the trigger |
| **C radial** | `quick_radial`, `weapon_wheel` | Hold **right grip + B**: a ring of 8 quick items opens in front of the right hand. Pick with the right stick, by moving the hand toward an item, or by pointing the left hand (left trigger also picks). Release B to use, or release with nothing picked to cancel. Hold **left grip + X**: the weapon wheel at the left hand (left stick, left hand movement, or the right pointer); release to equip. Trick weapons go to the right hand, firearms and the torch to the left |
| **D grid** | `inventory_grid`, `wrist_counters`, `weapon_wheel` | **Right grip + A** opens or closes a 5×3 inventory panel within reach (0.6 m). Point the right hand and pull the trigger, or poke a card with an index finger. Tabs (All, Heal, Throw, Buff, Tools, Ammo) filter by use: point at one, or flick the right stick up/down. Flick left/right or use the arrows for pages, and **x** closes it. The footer shows the item's effect and description. Held weapons hide while it is open |
| **E grid_far** | `inventory_far`, `wrist_counters`, `weapon_wheel` | Doc 17's option 3. **Right grip + A** opens a large 6×3 panel 1.5 m ahead. It is lazy head-locked: it catches up when you turn more than 25° away. Laser only, same tabs |

| Input (all from the layout file) | Action |
|---|---|
| Left stick click + right stick flick right / left | Next / previous preset. The bridge's L3 layer turns the flick into d-pad right/left. A toast shows the preset's notes, with a tick on both hands |
| Left stick click + flick up / down | Help panel on/off / refill every count |

- **Keys are taken from the scene.** When a binding's key is pressed with its modifiers held, it never reaches the scene or the locomotion until released. So B does not switch the weapon while the right grip is held, and A does not dodge. While a radial or the grid is open, the sticks belong to it and locomotion pauses.
- **Keys the menus use:** right grip + B, left grip + X, right grip + A, the triggers while pointing at an open menu, the grip near a body slot, and the d-pad. The rest stays free for the other features: A, B, X or Y alone, R3, and the L3 click.

**Selection methods** (a menu's `select` list):
- `stick`: the menu hand's stick past `deadzone` picks the item in that direction. The pick stays when the stick recenters.
- `move`: the menu hand moved more than `move_threshold` from where it opened.
- `point`: the other hand's aim ray (or `pointer`'s), drawn as a laser with a dot. On a radial the center cancels. The `click` binding selects.
- `touch`: the index fingertip of the posed hunter (bones `X_Finger11/12`). A poke is the tip crossing 6 mm above the panel.

Body slots use reach (the grip pose within the slot radius) and the grip press.

### Layout file

The file is written with the defaults on the first run. Delete it to get newer defaults. Colors are
sRGB plus alpha, lengths are meters, angles are degrees. Top-level keys:
- `active_preset`
- `cycle_next`, `cycle_prev`, `help`, `reset`: bindings
- `log`: write the interaction log
- `infinite`: counts never run out
- `style`: colors `panel`, `panel_border`, `slot`, `slot_border`, `highlight`, `highlight_border`, `text`, `text_dim`, `accent`, `laser`; `open_seconds` and `close_seconds` (scale and fade); `text_outline`; haptic pulses `haptic_open`, `haptic_hover`, `haptic_select`, `haptic_grab`, `haptic_empty` and `haptic_preset` as `[amplitude, seconds]`
- `inventory`: `[{"item": name, "count": n}]`. Names resolve against `icons.json` by item name, `goods:<id>` / `weapon:<id>`, a weapon model, or a name prefix
- `presets`: `[{"name", "label", "notes", "menus": [menu names]}]`
- `menus`: `[{"name", "type", ...}]`

A binding is tokens joined by `+`. The last token is the key that acts; the others must already be
held. Tokens: `left_grip`, `right_grip`, `left_trigger`, `right_trigger`, `a`, `b`, `x`, `y`, `l3`,
`r3`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`, `options`, `touchpad`, `none`.

| Menu keys | Types | Meaning |
|---|---|---|
| `label`, `items`, `action` | all | Title; the item names (the grid defaults to `inventory`); `use` (count down) or `equip` (weapons) |
| `hand`, `open`, `mode` | radial, grid, wrist | The menu's hand (`left`/`right`); its binding; `hold` (open while held, release acts) or `toggle` |
| `select`, `pointer`, `click` | all | Methods (`stick`, `move`, `point`, `touch`); the pointing hand (`other`, `left`, `right`); the pointer's select binding |
| `on_top`, `hide_weapons` | all | Draw over the scene (no depth test); hide the held weapons while open |
| `radius`, `inner_radius`, `item_size`, `gap`, `forward`, `up`, `deadzone`, `move_threshold`, `use_on_release` | radial | Ring size; where it opens (from the hand, away from the head, and up); selection thresholds |
| `columns`, `rows`, `cell`, `distance`, `below`, `tilt`, `page_stick` | grid | Cards per page and their size (all of the panel scales with `cell`); where it opens in front of the head; the stick for pages (x) and tabs (y) |
| `anchor`, `tabs` | grid | `world` (stays where it opened), `head` (follows) or `lazy` (catches up past 25°); the tab filters: `all`, `heal`, `ammo`, `throw`, `buff`, `tool`, `other` |
| `stick_nav`, `block_move` | dashboard | Default true. `stick_nav`: either stick moves a focus (hover style) to the nearest entry in the pushed direction (0.35 s, then every 0.14 s), A or a trigger pulls the focused entry, B closes it; pointing moves the focus to what the ray hits. `block_move`: while it is open the game gets no stick, swing or weapon swap |
| `show`, `glance_deg`, `view_deg`, `offset`, `normal`, `width`, `height` | wrist | `glance` / `hold` (the `open` binding) / `glance+hold` / `always`. Glance: the panel faces the eyes within `glance_deg` and is within `view_deg` of the view. Panel center in grip space (left hand +X = the palm side), the grip axis it faces, its size |
| `posture`, `seated_drop`, `markers`, `grab_radius`, `marker_size`, `neck`, `slots` | body_slots | `auto` / `standing` / `seated`. Auto means seated when the head is `seated_drop` below the calibrated eye height. `markers`: `always` / `near` / `never`. `neck`: the pivot in head space. `slots`: `[{"name", "item", "action", "hands" (any/left/right), "standing": [x, y, z], "seated": [x, y, z], "radius"}]`, offsets from the neck pivot with +X right, +Y up and +Z back, turning with the body yaw |

### Interaction log

`out/vr_assets/ui_log/ui_<time>.jsonl` gets one JSON object per event. Every event has `t`
(seconds), `event`, `preset`, `menu` and `type`. The events:
- `open`, with `binding`
- `hover`: `item`, `method`, `hand`, `since_open_ms`
- `select`: `item`, `method`, `hand`, `action`, `since_open_ms`, `hover_changes`, `count_left`
- `cancel` and `close`: `since_open_ms`, `hover_changes`
- `grab`: `slot`, `item`, `hand`, `posture`, `since_hover_ms`
- `glance` and `glance_end`: `shown_ms`
- `empty`, `page`, `tab`, `preset`, `refill`, `layout`
- `summary`: per preset and menu: opens, selects, cancels, grabs, and the mean time from open to select

Each select and grab also prints one line to the console. The summary prints on each preset change
and at exit.

### Testing it in the headset

1. Run `scripts/vr_sandbox3d.sh`. It builds, runs the tests (`bbport_xr_sandbox3d_ui_test.exe` too) and the extractor. Preset A is active; the console lists every preset and binding.
2. **A:** look down, reach for the right hip, chest and left hip, squeeze, and watch for the popups and pulses. Sit down: the markers move to the seated places. Glance at the left wrist for the counts.
3. **Left stick click + right stick flick right → B:** palm up, touch a slot with the right index finger, then point and pull the trigger.
4. **→ C:** hold right grip + B and try the stick, then moving the hand, then pointing with the left hand; release each time. Hold left grip + X and pick a weapon: it appears in its hand.
5. **→ D:** right grip + A. Point and click, poke a card, flick the right stick for page 2 and up/down for the tabs, and close with x.
6. **→ E:** right grip + A. Turn your head away and watch the panel catch up, then use the laser.
7. Tune `out/vr_assets/ui_layouts.json` while it runs (sizes, offsets, bindings, colors). Saving applies the change at once. Afterwards compare the presets in `ui_log/*.jsonl`: times to select, hovers before a select, cancels.

## Solid weapons: collision and haptics

The held weapons are solid. A weapon follows the hand until it would pass through something. Then it stops at the surface, slides along it or turns about the contact, and the avatar's hand stays on it. A light-cyan wire hand shows where the real controller is once any joint is 3 cm off. The two weapons collide with each other too (blade on blade, blade on the pistol).

- **Shapes** (`BuildWeaponShape`). Each bone's vertices are sliced along their long axis into 10 cm slices. A slice becomes one capsule, or up to four side by side when it is flat (a blade), so the radius follows the thickness, not the width. The Saw Cleaver gets 53 capsules of 1.5–4.4 cm (median 2.7) with 99.4 % of its vertices within 5 mm, against the game's 0.40 m attack capsule.
- **World.** The floor, a wooden training post (pole and crossbar) 1.1 m to your front right, a wall 1.75 m to your right, and an enemy 1.9 m to your front left, facing you.
  - The enemy is **c2632, the Central Yharnam Huntsman** ("群衆A（大）", torch and axe, NpcParam row 263203) at its bind pose. Its 19 hurtbox capsules from `hitbox/chr_c2632.json` are colliders.
  - `python scripts/vr_assets/extract_enemy.py` writes `enemy_c2632.json`; `scripts/vr_sandbox3d.sh` runs it when the file is missing.
  - The props are placed relative to where you stand and look at the first frame. The T-pose calibration (A held) places them around you again.
- **Solver.** Position-based rigid body, every frame:
  - the weapon moves toward the pose the hand gives it in substeps, no point more than 0.8 × the thinnest capsule radius per substep (up to 96), so fast swings cannot tunnel;
  - each substep pushes it out of what it overlaps (capsule–capsule, capsule–box, capsule–plane; 4 passes);
  - the hand's mass sits at the grip and resists turning like a forearm (`hand_gyration_m`); the weapon's mass is spread over its capsules. A hit near the tip turns the weapon about the grip; a hit near the hand pushes the hand back;
  - the rig then runs again with the hands moved onto their weapons;
  - a hand 0.45 m or 100° away from its blocked weapon lets go: the weapon snaps back into it and passes through until free.
- **Haptics** (`Haptic(hand, amplitude, seconds)`):
  - a pulse when a contact starts, scaled by how fast the hands drive the two surfaces together along the contact normal: 0.12 + 0.3 per m/s up to 1, 30–80 ms; both hands for weapon on weapon;
  - a light buzz every 50 ms while a weapon grinds along a surface faster than 8 cm/s: 0.06 + 0.15 per m/s up to 0.35;
  - a "hit", 1.0 × 100 ms, when the shown attack's hit capsules (the game's, radius × 0.6) enter the enemy's hurtboxes faster than 2 m/s. One per swing; a swing ends below 1 m/s. A fading marker shows the hit point and the hurtbox flashes red.
- **Logs.** Every contact start (pair, speed along the normal, depth, haptic amplitude × seconds), contact end (duration, peak slide speed and buzz, peak depth), hit and snap is printed. It is also appended to `sandbox3d_contacts.jsonl` in the working directory (the run folder), one JSON object per line after a `session` line with the date. Every 5 s a `physics:` line gives the solver's cost (average and worst ms), substeps, contacts and hits.
- **Toggles** are config keys only; no buttons are taken.
- Files: `collision.*` (contact math, 3×3 and pose helpers), `physics_config.*` (the settings), `physics_weapon.*` (shapes, world, solver, contacts, haptics, hits, logs), `physics_view.*` (props, enemy, wireframes, ghost hands, markers), `collision_offline.*` (the previews), `collision_test.cpp`.

### Settings: the `physics` object of `sandbox3d.json`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | true | Weapons are solid. False: they pass through everything (hits still register) |
| `weapons_collide`, `floor`, `enemy_solid` | true | Left vs right weapon; the floor; the enemy's hurtboxes stop weapons |
| `show_colliders`, `show_ghost`, `ghost_min_m` | true, true, 0.03 | Wireframes (weapon capsules green, orange where touching; hurtboxes cyan); the controller's ghost hand and how far off it must be |
| `stiffness` | 1 | Share of the weapon-to-hand gap closed per 1/90 s in free space (1: rigid, no lag) |
| `iterations`, `max_substeps`, `max_step_m` | 4, 96, 0 | Push-out passes; substep cap; per-substep travel (0: 0.8 × thinnest radius) |
| `max_separation_m`, `max_separation_arc_m` | 0.45, 1.5 | How far the hand drags a blocked weapon before it lets go: meters, or the turn as an arc (angle × reach; long blades let go at a smaller angle) |
| `swing_through_enemy` | true | A swing in progress (`hit_speed` met) passes through the enemy's hurtboxes (hits still register); the post and wall stay solid |
| `teleport_m`, `teleport_deg` | 0.3, 90 | The raw controller jumping or turning this much in one frame (tracking glitch, same as `swing_logic.h` `glitch_m`) resets its weapon and swing |
| `hand_mass_kg`, `weapon_mass_kg`, `hand_gyration_m` | 2.5, 1.5, 0.25 | More hand mass or gyration: the weapon turns less and pushes the hand back more |
| `shape` | `slice_m` 0.1, `radius_min_m` 0.012, `radius_max_m` 0.05, `radius_scale` 1, `max_lanes` 4 | Collision capsules from the mesh |
| `haptics` | see above | `impact_min_speed`, `impact_amplitude_min/_per_mps/_max`, `impact_seconds/_per_mps/_max`, `slide_min_speed`, `slide_amplitude_min/_per_mps/_max`, `buzz_interval_s`, `buzz_seconds`, `release_s` (a contact ends after 0.08 s apart) |
| `melee` | `hits` true, `hit_speed` 2, `rearm_speed` 1, `rearm_reverse_deg` 120 (a stroke reversing by more: rearm at once), `attack_radius_scale` 0.6, `hit_amplitude` 1, `hit_seconds` 0.1, `marker_s` 0.8 | The hit rule (doc 14 §4: 0.4 m radii are too big for a tracked blade) |
| `props` | `show` true, `post` [0.8, −0.8], `post_radius` 0.07, `post_height` 1.8, `crossbar_height` 1.3, `crossbar_length` 0.8, `crossbar_radius` 0.035, `wall` [1.75, −0.7], `wall_size` [0.25, 2.4, 2.6], `wall_yaw_deg` 0, `enemy` "c2632", `enemy_pos` [−1.1, −1.5], `enemy_face_user` true, `enemy_yaw_deg` 0 | Positions are [x, z] in meters from where you stood, x to the right, −z ahead. `enemy` "" removes it |
| `log_contacts`, `contact_log` | true, "sandbox3d_contacts.jsonl" | Print the contact lines; the JSON-lines file ("" off) |

### Testing it in the headset

1. The post is to your front right, the wall to your right, the Huntsman to your front left. If they are badly placed, stand where you want them around you and hold A (the T-pose calibration).
2. Swing the Saw Cleaver into the post. It stops at the pole while your hand goes on; the cyan ghost shows the controller. A faster swing pulses harder.
3. Drag the blade along the wall: a light continuous buzz. Rest it on the floor, then lift it.
4. Clash the blade on the pistol in the left hand. For blade on blade set `"left_weapon": "Kirkhammer"` in the config.
5. Step up to the Huntsman and slash fast: a strong pulse, a red flash and marker, a `HIT` line. A slow push gives only the contact pulse.
6. Tune live: the `haptics` amplitudes; `hand_gyration_m` and `hand_mass_kg` (more turning or more push-back); `max_separation_m`. `"show_colliders": false` hides the wireframes.
7. Afterwards: `sandbox3d_contacts.jsonl` in the run folder.

### Known gaps (solid weapons)

- Pose-level physics: the weapon keeps no momentum (it stops dead, no bounce). Nothing pushes back on the arm beyond moving the hand.
- The enemy stands still at its bind pose (A-pose). Its attacks, the player's own body and the mirror copy are not colliders.
- Shapes come from the bind mesh: trick weapons in form A only.
- Capsule lanes are rounder than a flat blade: a flat-on hit stops 1–2 cm early.
- The folded Saw Cleaver has a gap between handle and blade that thin things (the pistol barrel, the crossbar) can enter, as in the game's mesh.

## Locomotion and the comfort dash: the `locomotion` object of `sandbox3d.json`

In first person Bloodborne's dodge, backstep and jump move the camera with the hunter's animation:
the roll takes the eye down to the floor and turns it over, the jump arcs and lands in a roll. The
comfort dash (`../vr_dash.h`, the same code the game runs, `docs/vr/findings/19-dash-locomotion.md`)
slides the camera instead: straight from where it was to where the move ends, ease-out, level (a
jump: one gentle bump), then onto the character again with no jump or drift. Here you walk, turn,
dodge, backstep and jump, and compare the dash with the raw animation.

- **The moves are the game's own.** `scripts/vr_assets/dodge_extract.py` decodes every frame of
  the player's dodge clips from the dump into `out/vr_assets/dash/dodges.json`: the roll
  `a000_007200` (4.0 m, the eye down to 0.17 m), the backstep `a000_007000` (2.48 m) and the
  running jump `a000_007800` (6.84 m, a leap and a landing roll). They play like the game, sampled
  at `game_fps`. Without the file, synthetic look-alikes play.
- **First person as in the game:** the hunter's eyes at yours, the head hidden (`show.head`), the
  body below you; the left stick steers like the game's first person (`move`) and the right stick
  turns about your head. Turning and steering are the game's own model, `src/vr_turn_math.h`
  (through `turn_adapter.h`, which only converts the yaw conventions).
- **A/B:** `"mode": "raw"` rides the animation (its dip, its sway, the head's pitch:
  `raw_rotation` 1 = all of it, upside down in the roll); `"ab"` alternates dash and raw on every
  move, so you can feel both without taking the headset off. The log names each move (`move 3:
  dodge RAW ...`). After a move, its two paths stay drawn: the animation's eye (orange) and the
  camera's (green), at their heights and as floor shadows.
- **Vignette:** during the dash the edge of the view darkens with the camera's speed (`vignette`,
  clear inside `vignette_inner_deg` of where your head points, darkest from `vignette_outer_deg`).
  A pass of its own after the scene (`vignette.*`).
- **Grid and posts** out to 30 m, so the motion shows beyond the scene's floor.

| Key | Default | Meaning |
|---|---|---|
| `enabled` | true | Locomotion on |
| `mode` | "dash" | `dash`, `raw` (the animation's camera), `ab` (alternate) |
| `move_speed`, `sprint_speed` | 3, 5.5 | m/s at full deflection; A held while walking sprints |
| `deadzone`, `accel_s` | 0.15, 0.08 | Stick dead zone; seconds to full speed and back to rest |
| `move` | "head" | The left stick's forward: `head` (where you look), `body` (the view's own heading, psi), `controller` (the left controller) |
| `turn`, `snap`, `snap_repeat`, `turn_speed`, `expo`, `turn_accel`, `snap_on`, `snap_off`, `recenter`, `lock`, `lock_auto` | "snap", 30, 0.3, 90, 1, 0.08, 0.6, 0.3 | The game's first-person turning model (`src/vr_turn_math.h`, the same keys as its control file): `snap`, `smooth` or `off`; the step (deg), its repeat while held (s), the smooth rate (deg/s), its curve and ramp (s); the stick thresholds; recenter and lock-on policy |
| `dash_duration` | 0.4 | Seconds the camera slides in a dodge or backstep (the animation itself moves for about 0.9 s) |
| `ease`, `ease_power` | "quad", 0 | `linear`, `quad`, `cubic`, `quart`, `quint`, `expo` (ease-out), `smooth` (in-out, for comparison); `ease_power` > 0 sets the exponent |
| `settle_s`, `settle_speed` | 0.2, 4 | After the move the camera joins the eye over this long, longer for a bigger gap (gap / `settle_speed`) |
| `jump_height`, `jump_duration`, `jump_flat` | 0.3, 0.9, false | The jump's one bump (m) and its slide (s); flat: no bump |
| `dodge_distance`, `backstep_distance`, `jump_distance` | 0 | Meters; 0 = the animation's (4.0, 2.48, 6.84) |
| `max_lead_m` | 0 | The camera never more than this ahead of the character's progress (the game uses 2 m, for rolls into walls); 0 = no limit |
| `hint` | "exact" | What the dash knows of the move: `exact`, `direction` (the distance follows the motion), `none` |
| `vignette`, `vignette_inner_deg`, `vignette_outer_deg`, `vignette_walk`, `vignette_raw` | 0.6, 35, 70, 0, false | The dash vignette's strength and its angles; strength while walking or smooth-turning; also in raw mode |
| `raw_source`, `roll_anim`, `backstep_anim`, `jump_anim` | "auto", a000_007200, a000_007000, a000_007800 | Recorded (`dodges.json`) or `synthetic` moves; which clips |
| `raw_rotation` | 0.5 | Raw mode: how much of the head's own pitch the view takes (shown as 2 asin(s sin(p/2)): 1 = all, 0.5 = up to 60°, 0 = position only) |
| `game_fps` | 30 | The animation's sample rate (the game's frame rate); 0 = every frame |
| `tap_s` | 0.3 | A released sooner is a dodge or backstep; held longer, a sprint |
| `show_paths`, `show_landmarks` | true, true | The last move's paths; the grid and posts |

The section is added to `sandbox3d.json` with these defaults when it is missing. A still A held
1.5 s is still the T-pose calibration; A is kept from it while you walk or dodge. While a menu has
the controls (`ui.*`), locomotion gets no sticks or keys.

### Testing it in the headset

1. `scripts/vr_sandbox3d.sh` (MSYS2 CLANG64 shell). Walk with the left stick, turn with the right.
2. Push the stick and tap A: the dodge, as a dash. Tap A standing still: the backstep. Click the
   left stick while walking: the jump.
3. Set `"mode": "ab"` in `out/vr_assets/sandbox3d.json` and save: from now on every other move is
   raw (the log says which). Compare.
4. Tune `dash_duration`, `ease`, `jump_height`, `vignette` while it runs (saving applies it).
5. Without a headset: `scripts/vr_sandbox3d.sh --offline` writes `preview/dash_roll.png`,
   `dash_backstep.png`, `dash_jump.png`, `dash_cases.png` (cancelled, chained, blocked, teleport)
   and `dash_curves.png` with the current settings.

### Known gaps (locomotion)

- The hunter does not animate: during a raw move the body sinks with the eye.
- No collision: you walk through the posts and the mirror; the dash cannot be blocked here (the
  game's `max_lead` is tested offline).
- `move: "body"` uses the play space's heading (the game's psi), not the rig's lagging body yaw.

## Swings and stamina: the `swing` object of `sandbox3d.json`

Phases S1 and S2 of `docs/vr/findings/20-swing-and-stamina.md`, on the logic the game will share
(`../swing_logic.h`): a real swing of the right controller is an attack, and Bloodborne's stamina
rules apply, except that a swing pays only when it connects.

- **Light or heavy.** The classifier follows a point 0.25 m along the controller (where a held blade
  points), relative to your head, in the tracking space: walking with the stick, turning and
  dashing do not move it. A swing whose speed peaks above `light_speed` is LIGHT (R1). One that
  reaches `heavy_speed` is HEAVY (R2), decided the moment it gets there. A rising stroke is a
  WINDUP, never an attack. Hold the wind-up still overhead (not in front of your face) for 0.35 s:
  CHARGING (R2 held). The swing out of it is CHARGED after `charge_full_s` of hold, else HEAVY.
  The thresholds follow the right weapon's class (doc 20 §4.3): the Saw Cleaver is quick, light
  2.0 m/s and heavy 4.3 m/s.
- **Paid on contact.** Each attack holds its cost for `hit_window_s` (0.6 s). The cost is paid,
  once per swing, when in that time the shown attack's hit capsules touch the enemy's hurtboxes
  (the shapes of the physics' HIT), or the weapon touches the post or the wall (`charge_world`).
  A swing that touches nothing costs nothing (MISS). A contact the blade makes while the swing
  is still being measured belongs to that swing. The costs are the game's own, read from
  `hitbox/wp_<id>.json` (BehaviorParam_PC): the Saw Cleaver's R1 20, R2 35, charged R2 50
  (form B: 24, 40, 55).
- **Moves.** A dodge, backstep or jump (locomotion) pays its cost at once: 20 each, placeholders
  until the probe measures the game's.
- **At 0.** Nothing starts at 0 or below. The swing does not register (NO STAMINA, two short
  buzzes) and a move is refused (nothing moves). A cost may take you below 0, never below the floor
  (−50). Regen is 45/s after 0.75 s without an action. A missed swing also pauses regen, like the
  game's attack animation.
- **HUD.** Head-locked below the view (`hud.head`):
  - the stamina bar over the game's whole range, −50 to max: green above 0, red below, grey while
    regen waits, a white ghost for what the open swings hold, an orange flash where a cost was
    taken, a red blink on a refusal; "SP 71 / 91" beside it;
  - the last swing's label and result: `LIGHT  HIT -20`, `HEAVY  -35 if it hits`,
    `CHARGED  MISS  free`, `WINDUP  not an attack`, `TOO SLOW  1.8 m/s`,
    `NO STAMINA  swing ignored`, `DODGE  -20`; under it the button, the peak speed and how long
    after its onset the swing was decided;
  - the live speed on a scale marked at the onset, light and heavy speeds.

  Above the left wrist, facing you (`hud.wrist`): "SP", the bar and the label. Where a swing
  connected, its cost floats up. Text uses the menu font (`out/vr_assets/ui/font.png`); without
  it only the bars show.
- **Haptics** (right hand):
  - a tick when a swing registers: light 0.2 × 25 ms, heavy 0.4 × 35 ms, charge 0.3 × 60 ms;
  - a hit pulse by attack kind: light 0.6 × 60 ms, heavy 1.0 × 120 ms, charged 1.0 × 200 ms. It
    replaces the physics' hit pulse of that frame;
  - two short buzzes when tired, 0.5 × 50 ms, 0.1 s apart;
  - a soft bump instead of the physics' hit pulse when that hit was no attack
    (`replace_physics_hit`).
- **Logs.** Printed, and appended to `sandbox3d_swing.jsonl` in the run folder (a `session` line,
  then one JSON object per event):
  - every swing: kind, peak, arc, latency, cost;
  - every hit or clang (what it touched, how long before or after the decision), miss, refusal,
    dash, and dropped stroke (wind-up, too slow);
  - the stamina's changes: exhausted, regen, above 0 again, full.

  Swing and stroke lines carry the hand samples from 0.3 s before the onset (`samples`: t, grip
  position and rotation, head position and rotation, tracking space) for offline replay.
- A menu that uses the sticks (radial, grid) blocks swings (BLOCKED).

| Key | Default | Meaning |
|---|---|---|
| `enabled` | true | Swings and stamina; off: moves are free |
| `class` | "auto" | `auto` (by the right weapon and `form`), `quick`, `medium` or `heavy`: the class thresholds below |
| `light_speed`, `heavy_speed`, `min_arc`, `decide_s`, `max_swing_s` | 0 | 0 = the class's: quick 2.0 / 4.3 m/s, 0.18 m, 80 ms, 0.45 s; medium 2.2 / 4.6, 0.20, 90 ms, 0.45 s; heavy 2.0 / 4.2, 0.28, 110 ms, 0.55 s |
| `onset_speed`, `heavy_accel`, `up_limit` | 1.3, 0, 0.7 | A stroke starts; heavy by acceleration as well (0: off); rising steeper than this is a wind-up |
| `rearm_speed`, `rearm_s`, `reverse_rearm`, `reverse_dot`, `reverse_follow` | 0.9, 0.04, true, −0.3, true | The next swing: after the hand slows, or a stroke back the other way (forehand → backhand) against the follow-through's direction |
| `probe`, `head_relative`, `turn_comp` | [0, 0, −0.25], true, 1 | The measured point (grip frame); walking and body-turn compensation |
| `glitch_m`, `max_speed`, `max_gap_s`, `smooth` | 0.3, 15, 0.1, 0.6 | Tracking glitches and gaps; speed smoothing |
| `charge`, `charge_above_head`, `charge_behind`, `charge_still_speed`, `charge_hold_s` | true, −0.1, 0.05, 0.8, 0.35 | The charge: the hand at head height − 0.1 m or above, not in front of the face, slower than 0.8 m/s for 0.35 s |
| `charge_max_s`, `charge_drop_s`, `charge_needs_grip`, `charge_full_s` | 2.5, 0.25, false, 0.8 | Released after 2.5 s or 0.25 s lowered; the grip required; R2 held this long makes the release CHARGED (a placeholder) |
| `stamina` | `max` 91, `floor` −50, `regen_per_s` 45, `regen_delay_s` 0.75, `hit_window_s` 0.6, `gate_at_zero` true, `infinite` false | The game's numbers (doc 20 §1); `infinite`: nothing is paid or refused |
| `stamina` costs | `light_cost`, `heavy_cost`, `charged_cost` 0, `dodge_cost`, `backstep_cost`, `jump_cost` 20 | 0: the held weapon's |
| `stamina` contacts | `charge_world` true, `charge_floor` false, `contact_margin_m` 0.01, `pre_contact_s` 0.15 | The post and wall pay, the floor does not; how close counts as touching; contacts this long before the swing's onset count |
| `haptics` | see above | `light`, `heavy`, `charge`, `hit_light`, `hit_heavy`, `hit_charged`, `tired`, `soft_contact` as [amplitude, seconds]; `tired_gap_s`; `replace_physics_hit` |
| `hud` | `head` true, `wrist` true, `distance` 0.9, `down_deg` 24, `scale` 1, `wrist_scale` 0.7, `wrist_offset` [0, 0.08, 0], `label_s` 1.6, `speed_max` 8, `popups` true | The head-locked panel's distance and angle below the view; the wrist panel's size and offset (world axes) from the left grip; how long a label stays; the speed scale's end; the floating costs |
| `log`, `log_samples`, `log_file` | true, true, "sandbox3d_swing.jsonl" | Print; the hand samples in the JSON lines; the file ("" off) |

The section is added to `sandbox3d.json` with these defaults when it is missing. Saving applies a
change at once, the class and the costs included. Files: `combat.*` (the logic, pure CPU) and
`combat_view.*` (the HUD and the previews).

### Testing it in the headset (doc 20 S1, S2)

1. `scripts/vr_sandbox3d.sh`. The console prints the right weapon's class, thresholds and costs.
2. Swing the Saw Cleaver through the air, slowly and then fast: LIGHT, then HEAVY, each with a tick,
   and `MISS  free` 0.6 s later. The bar stays where it was.
3. Step up to the Huntsman and swing: `HIT -20` or `HIT -35`. The bar drops once per swing with an
   orange flash, the cost floats up from where you hit, and the pulse is stronger for a heavy.
4. Hit the post or the wall: `CLANG -20`. `"charge_world": false` makes those free.
5. Raise the blade overhead and hold it still: CHARGING fills up. Swing down into the Huntsman:
   `CHARGED  HIT -50` (HEAVY if released before 0.8 s).
6. Dodge and backstep (A): −20 each. Empty the bar: at 0 or below swings show NO STAMINA with two
   buzzes and A does nothing; regen starts 0.75 s after your last action.
7. S1: 50 deliberate slow and 50 fast swings, then a minute standing, walking in place, looking
   around: count the labels and the false events. Tune `light_speed`, `heavy_speed`, `min_arc`,
   `decide_s` (or `class`) live. Afterwards: `sandbox3d_swing.jsonl` in the run folder.

### Known gaps (swings and stamina)

- No attack animation: the attack happens at the decision and the blade's own path decides the
  contact. In the game the attack's TAE capsules do (doc 20 §4.7). The combo timing (the
  scheduler's queued presses) is not simulated.
- The game's attack capsules are thick (the Saw Cleaver's R1: 0.24 m): standing close to the
  enemy, almost every swing connects, as in the game.
- Sprinting costs nothing (the game's sprint cost is not measured); the move costs (20) and the
  full charge (0.8 s) are placeholders.
- Body-slot grabs and fast reaches are not blocked: one can register as a swing (a free MISS).
- The recorded samples are not replayed yet (doc 20 S4).

## Trick-weapon transform: the `swing.transform` object

Doc `docs/vr/findings/33-trick-transform.md`, on `../trick_transform.h`. A **wrist snap** of the weapon
hand (the grip turns 90 degrees or more within 150 ms while it travels less than 0.2 m) or **L1** (the
left grip) switches the held trick weapon between form A and form B (`config.form`; the hit shapes of that
form are shown and used, the swing class and costs follow). A weapon with no form B (the Hunter Pistol)
answers with one soft tick. While the wrist snaps (turn rate above 9 rad/s, grip slower than 0.8 m/s) the
swing classifier is not fed, so a snap is never an attack. The form shows as FORM A / FORM B beside the
stamina bar (large for a moment after a switch). Haptics: a click, then a lock, on the weapon hand.
Keys (`enabled`, `gesture`, `button`, `min_angle_deg`, `window_s`, `max_travel_m`, `cooldown_s`,
`rest_rate`, `veto_rate`, `veto_grip_speed`, `veto_hold_s`, `lock_s`, `haptic`, `haptic_lock`,
`haptic_gap_s`, `haptic_none`) are added with their defaults the first time `swing` is written. Both
weapon models are the same mesh in the dump (the model slots are not extracted yet), so the visible
change is the HUD form and the hitbox set. Editing `form` in the file overrides a transform.

## Co-op pose sync: the `net` object of `sandbox3d.json` (docs/vr/findings/34-coop-pose-sync.md)

The partner's head, both hands and weapon over plain UDP (49 bytes at 40 Hz), interpolated ~100 ms
behind (`../vr_pose_net.h`) and drawn as wireframes at `net.remote_position` / `remote_yaw_deg`. A single
headset cannot run two XR sessions, so the second instance is the no-XR, no-GPU scripted partner:

```
bbport_xr_sandbox3d.exe --net-port 7001 --net-peer 127.0.0.1:7002                       # headset
bbport_xr_sandbox3d.exe --offline-peer --net-port 7002 --net-peer 127.0.0.1:7001 --seconds 60
```

A side without `--net-peer` learns the partner from its first packet. `net` keys: `show_remote`,
`show_overlay` (bars above the partner's head: RTT, buffer depth, loss; a log line every 2 s),
`send_hz` (30-45), `render_delay_ms` (100), `max_extrapolate_ms` (150), `remote_position`,
`remote_yaw_deg`, and a simulated link on the packets this side receives: `sim_latency_ms`,
`sim_jitter_ms`, `sim_loss_pct`, `sim_dup_pct`. Tests: `bbport_xr_pose_net_test.exe` (codec, loss,
reorder, duplicates, jitter, wraparound, silence, RTT) and `bbport_xr_sandbox3d_coop_test.exe` (two
peers on real UDP under 100 ms latency and 5 % loss, interpolated poses against the scripted truth).

## Hand-thrown goods: the `throwables` object of `sandbox3d.json` (docs/vr/findings/35-throwables.md)

A table in front of where you stood (and looked) at the start (and at each T-pose calibration) holds the 15
hand-thrown goods: Molotov Cocktail, Delayed Molotov, Poison Knife, Throwing Knife, Oil Urn, Pebble, Rope
Molotov, Delayed Rope Molotov, Pungent Blood Cocktail, Numbing Mist, Shining Coins, Tiny Tonitrus,
Executioner's Gloves, Blacksky Eye, Accursed Brew. They look like the flight-effect meshes of the game
(`python scripts/vr_assets/extract.py --throwables` writes `out/vr_assets/throwables/th_<goods>.json`; 14
of 15 found, the Blacksky Eye and anything missing is a procedural bottle, urn, knife, stone, disc or
sphere).

| Input | Action |
|---|---|
| Grip near an item (`grab_radius_m`, 16 cm of the hand's grab point) | Grab the nearest one; a tick when one comes within reach, a pulse on the grab. It rides the hand with the per-item `grip` offset; that hand shows no weapon meanwhile |
| Open the grip | Throw (mode `hand`): the hand's velocity over the last `smooth_ms` (80), mean blended with the fastest step by `peak_weight`, gives the direction and, scaled, the speed: `initial_velocity x clamp(hand speed / ref_speed_mps (7), min_scale, max_scale)`, capped by the bullet's `max_velocity`. Below `min_throw_speed_mps` it is just dropped with the hand's velocity |
| Trigger of the holding hand (mode `game`) | The game's throw: along the view (+`game_pitch_deg`) at the exact `initial_velocity`; opening the grip only drops it |
| `dpad_up` (`mode_button`) | Switch hand velocity / game throw |
| `dpad_down` (`respawn_button`) | Everything back on the table (spent items return by themselves after `respawn_s`, dropped ones after `rest_return_s`) |

Flight is the game's Bullet rules (`param/gameparam` Bullet, read from the dump by the extractor): gravity
`gravity_in` for the first `range_m` metres of travel and `gravity_out` after, `accel_in` while in range,
speed capped at `max_velocity` (a capped bullet turns downward instead of arcing: the Delayed Molotov's
steep drop), ended by `life_s` or by the floor, the table, the post and wall and the enemy's hurtboxes
(penetrating ones, `penetrate`, fly through the enemy). `num_shoot` 3 with `shoot_angle_deg` 20 is the
Executioner's Gloves' fan. Back throws (Rope Molotovs) fall straight from the hand and burst where they
land; delayed items burst `fuse_s` after landing. Impacts show a wireframe marker per type (fire, poison,
oil, mist, blood, coin, lightning, slash, eye, dust).

Every throw and impact prints a line and appends JSON to `sandbox3d_throws.jsonl` (`event`: `throw`, `end`,
`land`, `pierce`, `drop`, `drop_burst`; item, hand and release speed, direction, `hit` what, range, path,
flight time). Keys: `enabled`, `mode` (`hand`/`game`), `show`, `show_markers`, `grab_radius_m`,
`grab_point`, `grab_squeeze`, `release_squeeze`, `smooth_ms`, `peak_weight`, `release_lag_ms`,
`ref_speed_mps`, `min_scale`, `max_scale`, `min_throw_speed_mps`, `game_pitch_deg`, `game_trigger`,
`respawn_s`, `rest_return_s`, `effect_scale`, `gravity`, `rack_pos` `[x, z]`, `rack_height_m`,
`rack_spacing_m`, `rack_columns`, `respawn_button`, `mode_button`, `log`, `log_file`, and `items`: per item
(by goods id or name) any of `initial_velocity`, `gravity_in_range`, `gravity_out_range`,
`accel_in_range`, `max_velocity`, `life_s`, `range_m`, `num_shoot`, `shoot_angle_deg`, `penetrate`,
`back_throw`, `fuse_s`, `effect`, `color`, `scale`, `radius_m`, `spin_dps`, `align`, `grip`
(`position`, `rotation_deg`), e.g. `"items": {"Pebble": {"scale": 0.7, "grip": {"position": [0, 0.03, -0.04]}}}`.
`mesh`/`shape` changes need a restart. Not from the game: the fuse delay (1.5 s, the Bullet row has none),
the grip offsets, the model scales and the markers; Tiny Tonitrus' in-range gravity of 500 is read as 0.

## Consumables as objects: the `consumables` object of `sandbox3d.json` (docs/vr/findings/36-vr-consumables.md)

Press **R3** (right stick click; `wheel_mode` `toggle`, the default): the wheel opens and stays open until you take an item (`close_on_grab`) or press R3 again; `"wheel_mode": "hold"` keeps it open only while R3 is held (+ `linger_s`). Every item you have appears as a real 3D object in front of you, a little
below eye level (`layout` `arc` = rows on a circle around you, `shelf` = flat rows; `distance_m` 0.45,
`down_deg` 30, `spacing_m`, `row_spacing_m`, `per_row` 10), with a small count under each (`x12`, `~` for
reusable ones). Order: weapon buffs, healing, cures, body buffs, tools. Empty items are not listed.
**The wheel holds consumables only**: the throwables live on the table / rack (as before), unless
`"throwables_on_wheel": true` (then `hide_rack` can hide the rack). **Weapon buffs apply at once**
(`"buff_rub": false`, the default): take a Fire Paper / Bolt Paper / Phantasm Shell / Bone Marrow Ash and the
weapon in your other hand (the right weapon when that hand has none) glows immediately, the item is used up,
toast + haptics, the wheel closes; `"buff_rub": true` brings the rub along the blade back. Heals, cures and body
buffs are still brought to the mouth. While the wheel is open the weapons are put away
(`stow_weapons`, default on) and come back when the wheel closes with nothing held, or when the item is
used / thrown / dropped; the weapon a held paper is meant for stays out. `"wheel": "cards"` brings back the old card wheel (R3 opens `quick_wheel` again
and toggles the skeleton, as before).

Take an item by closing the grip (`grab_squeeze` 0.6) with the hand's grab point within `grab_radius_m`
(6.5 cm); it stays in that hand (open the hand to put it back). By group:

| Group | Gesture | Rule |
|---|---|---|
| Throwables | throw | Handed to the throwables (`mode` hand / game as in the section above); the throw uses one up |
| Weapon buffs: Fire Paper, Bolt Paper, Empty Phantasm Shell (arcane), Bone Marrow Ash (gun buff) | rub | Take with either hand; it is rubbed onto the weapon the **other** hand holds: the paper (or the hand) stays within `contact_radius_m` (10 cm) of the blade and covers `coverage` (50 %) of the blade (the weapon minus its first `blade_start` 25 %), in any direction, within `window_s` (2.5 s); haptics, a glowing covered range and a progress gauge along the blade while rubbing; the log (`rub ...` lines, every `diag_interval_s`) says why a rub is not counted (no weapon, too far, past the tip). The weapon then glows and sparks in the element's colour (fire orange, bolt blue, arcane purple) for the SpEffect's duration (60 s; `buff_default_s`) with a bar in the corner HUD |
| Healing: Blood Vial, Iosefka's Blood Vial, Blood of Adeline | head | Bring within `mouth_radius_m` (24 cm) of the mouth (`mouth` offset from the head), below the eyes, for `head_hold_s` (0.3 s; the log prints `head ... cm from the mouth` near the head): toast and HP bar (HUD, flashes), the dump's heal % |
| Cures: Antidote, Sedative | head | Remove the ailment (poison, frenzy) |
| Body buffs: Beast Blood Pellet (60 s), Blue Elixir (30 s), Lead Elixir, Madman's Knowledge (+1 insight), Old Hunter Bone (16 s) | head | Timed status bar in the HUD |
| Bells (Beckoning, Small / Sinister Resonant, Choir) | head | Rung next to the ear; not used up |
| Hunter's Mark, Bold Hunter's Mark, Shaman Bone Blade | crush | Squeeze to `crush_squeeze` (0.97) for `crush_hold_s` (0.5 s); a haptic ramps up |

The tools that cost quicksilver bullets in the game (Old Hunter Bone 4, Choir Bell 7, Empty Phantasm Shell
3, Tiny Tonitrus 6, Executioner's Gloves 3, Blacksky Eye 1, Accursed Brew 2) draw from the `900` count and
are not used up; with too few bullets the pick is refused. `inventory` is `{goods id: count}`; missing: the
sample inventory. Durations, heal % and costs come from `out/vr_assets/items/items.json` when present.
Meshes: `python scripts/vr_assets/extract.py --consumables` writes `out/vr_assets/consumables/cs_<goods>`
(vials, flask, bells; the rest are procedural paper, pellet, packet, bone, tablet, flask, syringe). Every
wheel open, pick, put back, use, throw, buff and expiry prints a line and appends JSON to
`sandbox3d_consumables.jsonl`. Keys: see `ConsumeConfig` (`consumables_config.h`).

## Gun parry and custom hands: the `parry` and `hands` objects of `sandbox3d.json` (docs/vr/findings/41-parry.md, 40-custom-hands.md)

The left hand's gun is shot like in the game: `ShootGate` (`../xr_input_logic.h`) fires on a jab of the left controller along its barrel (thrust >= 1.5 m/s relative to the head, alignment >= 0.8, rising, 250 ms debounce, re-arm below 0.5 m/s). A bullet leaves the gun's muzzle (flash, tracer, recoil pulse), pulled toward the dummy's chest by the game's aim assist (`vg_assist_dir`: cone clamp(atan(0.6/d), 4, 18) deg, strength 0.8, range 12 m). Every jab is logged once: `parry: SHOT #n ...` or `parry: jab rejected: speed|alignment|falling|debounce|disarmed (best frame ...)`. The defaults come from the same `BB_XR_SHOOT_*` and `BB_VR_PARRY_*` environment variables as the game.

The parry dummy is the sandbox enemy (Central Yharnam Huntsman) straight ahead (`dummy.distance_m`, default 2.2 m; moved at the T-pose calibration like the other props). Loop: idle (`idle_s` 1.2) -> wind-up (`windup_s` 0.7: amber, the weapon rises, a ring closes over its head, a soft beep and a tick in the left hand) -> PARRY WINDOW (`window_s` 0.15: green, a high beep) -> swing (`swing_s` 0.35, red) -> recover (`recover_s` 0.4). A bullet that lands in the window is `PARRY!` (toast, console, big haptic): the dummy is staggered (blue) for `stagger_s` 2.0, and a right-hand swing that connects then is a `VISCERAL!` (it goes down for `down_s`). A bullet in the wind-up is `TOO EARLY by N ms`, after the window `TOO LATE by N ms`, one that hits nothing `MISS` (with its closest approach), one at the idle dummy is not graded. The score (parries green, early yellow, late orange, misses grey, visceral blue) is drawn as digits beside the dummy and printed with every event.

```json
"parry": {"enabled": true, "trigger_fire": false, "sound": true,
  "shoot":  {"speed_mps": 1.5, "align": 0.8, "rearm_mps": 0.5, "debounce_ms": 250},
  "assist": {"enabled": true, "strength": 0.8, "radius_m": 0.6, "cone_min_deg": 4, "cone_max_deg": 18, "range_m": 12},
  "bullet": {"speed_mps": 60, "range_m": 40},
  "dummy":  {"enabled": true, "attack_loop": true, "distance_m": 2.2, "x_m": 0, "idle_s": 1.2, "windup_s": 0.7,
             "window_s": 0.15, "swing_s": 0.35, "recover_s": 0.4, "stagger_s": 2.0, "down_s": 1.0},
  "visceral": {"mode": "thrust", "speed_mps": 1.5, "align": 0.7, "reach_m": 1.2, "debounce_ms": 300}}
```

`trigger_fire` also lets the left trigger shoot (aim practice); `dummy.attack_loop: false` makes it stand still.

The visceral (`parry.visceral`, live-polled) finishes the staggered dummy. `mode`: `thrust` (default) = a forward thrust of the right hand at the dummy (hand velocity relative to the head along the hand's aim >= `speed_mps`, the stroke within `align` (cosine) of the direction to the chest, thrust still rising, `debounce_ms` since the last one, re-armed below the shoot `rearm_mps`, and the hand within `reach_m` of the chest) or the right-hand melee hit; `swing` = only the melee hit (thrusts are ignored); `auto` = after a parry it fires by itself once the right hand or the head is within `reach_m` of the chest (the melee hit works too). Outside the stagger nothing special happens. Log: `parry: VISCERAL! (thrust|swing|auto) ...`, and during the stagger `parry: visceral thrust rejected: speed|alignment|falling|debounce|disarmed|reach (best frame ...)` once per thrust.

Hands: `"hands": {"model": "custom" | "current", "scale", "position", "rotation_deg", "blend_rate"}`. `custom` (default) draws the WebXR generic hand (`out/vr_assets/hands/{left,right}.glb`, `../vr_hand_model.h`) at each controller, GPU skinned, fingers blended from the analog grip and trigger (open / index-curled / point / fist), the hunter's own hands hidden; also in the mirror. While a hand holds an item the pose is that item's (the sandbox's tuned grips of the throwables and consumables first, then the category defaults: bottle, small, paper, bell, tool); a held weapon closes the hand, a gun keeps the index off the frame until the trigger. `current` draws the hunter's hands. `position` / `rotation_deg` shift the hand in the grip frame (the left mirrors it) to tune the fit.

### Testing it in the headset

1. Launch (below), press A for 1.5 s in a T-pose: the dummy appears 2.2 m ahead, props around you. The left weapon is the Hunter Pistol, the right one the Saw Cleaver.
2. Hands: open, fist (grip), point (grip + trigger off), trigger only; grab a vial or knife from the rack (grip) and see the item's pose; drink, throw: still work.
3. Thrust the left gun at the dummy along the barrel, fast (2-3 m/s): `SHOT`, a flash and a tracer. A slow poke, a sideways swing and a quick second jab are logged as rejected with the reason.
4. Watch the amber wind-up and the ring; shoot as it closes: `PARRY!` and a long buzz; `TOO EARLY` / `TOO LATE` show the error in ms. Widen `window_s` while it is hard.
5. After a parry thrust the right hand at the blue dummy (or swing it into it): `VISCERAL!`; try `visceral.mode` `auto` and `swing`. Compare `assist.enabled` on and off with the gun a few degrees off.

Tests: `bbport_xr_sandbox3d_parry_test.exe` (window timing, visceral by swing / thrust / auto and its rejections, the gate's rejections, the assist, the config).

## How it works

| File | Role |
|---|---|
| `model.*` | Reads the bbmodel files (JSON + .bin), `assets.json`, and the bbhitbox weapon files (doc 14) |
| `rig.*` | Pure CPU, unit tested. Places the body under the head (eye point, yaw deadzone, scale), runs two-bone arm IK (swing toward the elbow pole, then twist), drives the forearm twist bones, the hand orientation (grip→hand basis from the bind hand's finger and thumb directions), finger curl about geometric palm axes, and the head bone |
| `scene.*` | Assets, controls and calibration; builds the skinning palette, draw list and debug lines |
| `renderer.*` | Vulkan 1.3 dynamic rendering, GPU skinning (palette SSBO), flat lighting, depth-tested and on-top line pipelines, offscreen mode; a draw hook for extra passes |
| `ui.*` | The VR menu prototypes: bindings and key filtering, the radial / grid / wrist / body-slot logic, pointing, touch, stick and hand-movement selection, animation, haptics, the interaction log. No GPU and no scene needed (unit tested) |
| `ui_layout.*` | `ui_layouts.json`: the built-in defaults, parsing, hot reload |
| `ui_render.*`, `png_read.h` | Icon atlas, SDF font and text layout, `UiCanvas` (world-space panels, rounded boxes, ring sectors, icons, text built on the CPU), and the UI pipeline drawn through the renderer's hook. Premultiplied alpha, no depth writes, a depth-tested layer and an on-top layer. `png_read.h` is a small inflate plus PNG reader |
| `ui_scene.cpp` | The menus with the scene: their input and the fingertips from the rig, and the `--offline` previews `ui_*.png` |
| `collision.*`, `physics_*`, `collision_offline.*` | Solid weapons (see above): contact math; capsules from the weapon mesh; the props and the enemy; the substepped push-out solver; haptics, hits and the contact log (pure CPU, unit tested); drawing; the `--offline` collision previews |
| `locomotion.*`, `turn_adapter.h`, `../vr_dash.h` | Walking, turning, the moves (recorded or synthetic) and the comfort dash: moves the tracking space in the world (`WorldFromTracking`), the scene works in the world. The dash filter and the turning model (`src/vr_turn_math.h`) are the game's (pure C/C++) |
| `vignette.*`, `dash_preview.*` | The vignette pass (the line shaders, blended by multiplication); the `--offline` dash plots |
| `combat.*`, `../swing_logic.h` | Swings and stamina (see above): the shared classifier and stamina model on the real right hand, the weapon's contacts from the physics, tickets paid on contact, the moves' gate, labels, haptics and the JSON-lines log (pure CPU, unit tested) |
| `combat_view.*` | The swing and stamina HUD (its own `UiCanvas` and UI pipeline, chained after the menus' draw hook) and the `--offline` previews `combat_*.png` |
| `shaders/` | GLSL, compiled to SPIR-V headers at build time by glslangValidator |

### Asset format

`scripts/vr_assets/bbmodel.py` describes it.
- Coordinates are right-handed meters with FromSoftware's X mirrored, so the hunter faces −Z with his left hand at −X.
- Bone matrices are bind-pose world matrices in column-major order.
- A vertex is 56 bytes: position, normal, uv, 4 bone indices, 4 weights.
- Each mesh keeps its model-mask id and a "hidden by the outfit" flag (EquipParamProtector `invisibleFlag`).

### Tests and headset-free checks

- `bbport_xr_sandbox3d_test.exe out/vr_assets` covers the math (projection, reflection), two-bone IK, JSON, and the rig on the real hunter: eyes on the HMD, hand on the wrist target, bone lengths kept, finger curl, weapon alignment.
- The same binary runs the collision tests (`collision_test.cpp`): segment, capsule, box and plane contacts; the weapon shapes (also on the real Saw Cleaver); push-out convergence; no tunnelling of a 20 m/s swing at 72 and 90 Hz through a 3.5 cm pole or the other hand's blade; letting go; one hit per fast swing and none for a slow one; harder pulses for faster impacts and the grinding buzz.
- `--offline` also scripts swings at 72 Hz into `collision_post`, `collision_blades`, `collision_gun`, `collision_enemy` and `collision_wall`.png (first person beside an outside view), with their log in `collision_contacts.jsonl`.
- `--offline` renders four scripted poses (hold, T-pose, fist up, guard), first person beside third person.
- `--offline` also scripts the menus through their presets: `ui_radial`, `ui_radial_select`, `ui_weapon_wheel`, `ui_grid`, `ui_grid_far`, `ui_wrist`, `ui_body_slots` and `ui_preset_toast`.png. Each shows the first-person view (the head pose, as in the headset) beside an outside view. Their log goes to `ui_log/offline_*.jsonl`.
- `bbport_xr_sandbox3d_ui_test.exe out/vr_assets` checks the PNG reader (fixed and dynamic Huffman, every row filter), the layout file and bindings, and the menus on scripted input. That covers radial by stick and by hand movement, the weapon wheel, the grid by pointing, touch, tabs and paging, the wrist glance, body slots standing and seated, key filtering, preset cycling, hot reload and the log.
- `out/xr-bridge/bbport_xr_dash_test.exe` tests the dash filter: no overshoot, no jitter at 72 Hz over a 30 fps game, cancel, chains, no drift, teleports, jumps, blocked moves, falls.
- `bbport_xr_sandbox3d_dash_test.exe out/vr_assets` drives the locomotion with scripted controllers: walking and turning, the steering modes, each move as a dash and raw (synthetic and recorded), the A/B alternation and A for the calibration.
- `--offline` also plots the dash against the raw moves: `dash_roll`, `dash_backstep`, `dash_jump`, `dash_cases` and `dash_curves`.png.
- `out/xr-bridge/bbport_xr_swing_test.exe` tests the shared swing logic: light, heavy and charged swings, wind-ups and weak strokes, one attack per long arc, walking, head and body turns, glitches, the scheduler, the stamina model.
- `bbport_xr_sandbox3d_combat_test.exe out/vr_assets` drives the sandbox's use of it with scripted swings: light and heavy hits paid once, misses free, contacts before the decision, the post and wall, exhaustion and the floor, regen, menus, wind-ups, charges, the JSON-lines log and the config section, the weapon costs (the dump's Saw Cleaver: 20, 35, 50). Then the same through the weapon physics (a torso and a wall) and the locomotion's move gate.
- `--offline` also renders the swing HUD after scripted swings through the scene and its physics: `combat_hit` (a heavy into the Huntsman), `combat_tired` (no stamina, the wrist bar) and `combat_charge`.png, with their log in `combat_swing.jsonl`.
- `bbport_xr_sandbox3d_throw_test.exe out/vr_assets` drives the throwables with a scripted hand at 90 Hz: the built-in rows against the dump's Bullet values and the extracted table, the hand velocity estimator (constant, ramp-and-stop, noise), grab (radius, squeeze, nearest, riding the hand), flat throws of every item against an independent fine integration of the Bullet rules (range within 3 %), speed scaling and the max-velocity cap, straight knives, the fan of three, the Delayed Molotov and its fuse, back throws and weak flicks (drops), the enemy (burst, or pierced), the post and wall, Tiny Tonitrus' life, the game-style throw, the buttons, the log, the extracted meshes and the procedural stand-ins.
- `bbport_xr_sandbox3d_consume_test.exe out/vr_assets` drives the item wheel with scripted hands and R3: layout below eye level, empty items hidden, the card-wheel switch, picking and putting back, the vial at the mouth (a fly-by or the eyes do not count), cures, timed buffs and their expiry, insight, bells, bullet costs, the paper rub (full, back-and-forth, too short, too slow, beside or across the blade, wrong hand, no weapon, gun buff), a throwable from the wheel thrown by hand velocity and used up when thrown, crush, the wheel reopening after every outcome, weapons stowed and returned, the jsonl log.
- `bbport_xr_sandbox3d_parry_test.exe` drives the parry trainer with a scripted player (no headset): the window timing (parry, early and late by N ms, idle, miss, a tunable window), the visceral, the thrust gate (fire, speed, alignment and debounce rejections, walking), the aim assist and the config.
- `python scripts/vr_assets/test_ui_assets.py` tests the TPF/BC7/PS4-tiling decoder and the font reader.
- `python scripts/vr_assets/test_vr_assets.py` tests the asset pipeline.

## Findings (assets)

- **Skeleton.** `chr/c0000.chrbnd.dcx` → `c0000.flver` has 310 bones, of which 0–107 deform. The rest are helpers: `Dummy_*`, `Ctrl_*`, `Point*`. It also holds 358 dummy points. The arm chain is `X_Clavicle > X_UpperArm (0.283 m) > X_Forearm (0.273 m) > X_Hand`, with `X_Finger0..4` (three bones each: `Finger1`, `Finger11`, `Finger12`), `X_Weapon` under the hand, and `Support_X_ForeArmTwist(1)` / `UpArmTwist(1)` helpers. Bones run along their local +X.
- **Parts.** Part FLVERs carry their own bone order; vertex indices point into it, so they are mapped by name. `fc_m_0000` is the bare body: face, hands and arm skin. Armor hides its pieces by mask.
- **Outfits.**
  - Hunter set = `hd/bd/am/lg_m_2200`.
  - Starting set (Foreign Garb, Sullied Bandage, Foreign Trousers) = `*_8000`.
  - `0000` = nothing.
- **Weapons.** Saw Cleaver `wp_a_0800`, Hunter Axe `1020`, Threaded Cane `1070`, Kirkhammer `1031`, Saw Spear `1011`, Hunter Pistol `2500`, Blunderbuss `2600`, Torch `1800` (`catalog.json` has every row). The grip is at the origin; blades run along −Y and gun barrels along −Z. Blade dummies: 100 tip, 110 mid, 120 grip, 130/131 edge.

## Known gaps / TODO

- **Headset path untested.** It is built but has not run in a headset (the offline renders are the proof so far). Check:
  - the views and the depth layer;
  - the hand orientation from the grip pose. The basis follows the OpenXR spec text (−Z = the extended index finger's direction, +Y up). Doc 13 §5.3 guesses −Z runs through the fist; the gloves should overlap the real hands if the spec reading is right.
- **IK refinements** (13 §5.3):
  - clavicle lift before clamping;
  - pole roll with the wrist;
  - wrist limits;
  - upper-arm twist helpers;
  - low-pass on the pole.
- **Fingers.**
  - Capacitive touch (pointing index, resting thumb) needs touch states from the bridge: a later ABI with the `/touch` paths of 13 §6.4.
  - No spread, and no per-weapon grip poses.
- **Weapons.**
  - Trick forms are not posed. The bind mesh is form A or a rest pose, and the weapon skeletons and anibnd are unused.
  - Attachment uses `R_Weapon`, not the attach dummy frames (300/320/322/347…, 13 §7.3).
  - No two-handed form B.
- **Hitboxes.**
  - No TAE timing animation (doc 14 §5.3.2).
  - Enemy attacks are not drawn; one enemy (c2632) stands at its bind pose with its hurtboxes (solid weapons, above).
  - Hit sweeps run in the solid weapons' substeps; there is no separate per-attack sweep against the TAE windows.
  - Body-sourced hits use bind-pose bones only.
- **Body.**
  - No spine bend or leg IK; crouching moves the whole body.
  - No textures (`*.tpf`): flat colors per part.
  - No MSAA.
- **Hot reload.** Code changes still need a restart; only the config reloads live (12 §2 has the plan for logic hot reload).

## VR UI in the desktop window (bridge only)

The bridge (not this sandbox) can draw the game's VR UI into the desktop mirror window for screen recordings: `BB_XR_MIRROR_UI=1` (default with `BB_XR_MODE=mono`; live file `bb_xr_mirror_ui.txt`). The UI code here (`ui_render`, `Draw(cmd, view_proj, format)`) is drawn a third time from the mono camera into an image the host blends over the window. Math and its tests: `../vr_mirror_ui.h`, `../vr_game_ui_test.cpp` (`TestMirrorProjection`). See `docs/vr/findings/26-game-ui.md`.
