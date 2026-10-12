# 19: Dodges, backsteps and jumps as a comfort dash (sandbox, filter, game)

Date: 2026-10-09. Request: "jumping/evading should be a dash, sandbox test this too". In first-person
VR the dodge (roll or quickstep, circle), the backstep (circle with no stick) and the jump (L3 while
sprinting) move the camera with the hunter's animation: the roll tumbles the eye to the floor, the
backstep bobs, the jump arcs and lands in a roll. This replaces that with a comfort dash.

Code:
- **The filter, shared by the sandbox and the game:** `gpu/xr_bridge/vr_dash.h`. Pure logic,
  static inline, C11 and C++, no OpenXR. Tests: `gpu/xr_bridge/vr_dash_test.cpp` (C++) and
  `tests/test_vr_dash.c` (C).
- **The sandbox:** `gpu/xr_bridge/sandbox3d/locomotion.*`, `turn_adapter.h`, `vignette.*`,
  `dash_preview.*` and `locomotion_test.cpp`, hooked into `main.cpp`. README section "Locomotion
  and the comfort dash".
- **The game:** `src/vr_dash_detect.h` (pure: move table, ring decode, input fallback,
  settings), `src/vr_dash_game.h` (API) and `src/runtime_vr_dash.c` (`BB_VR_DASH=1`). It is
  wired into first person (§6; not yet tried in the headset).
- **The moves:** `scripts/vr_assets/dodge_extract.py` writes `out/vr_assets/dash/dodges.json`
  from the user's dump. `out/` is git-ignored.

Legend: **[M]** measured from the user's dump or by the tests here. **[C]** confirmed in code or the
eboot (cross-references to 13 and 16). **[S]** design, not yet seen in the game.

## 0. Summary

1. **The dash.** While a move plays, the camera slides on one straight line from where it was to
   where the move ends.
   - Ease-out (quadratic by default, 0.4 s).
   - Level. A jump gets one smooth bump, 0.3 m over 0.9 s, or a flat dash.
   - No pitch, roll or bob from the animation. The head pose is composed on top, so the head stays
     the user's own.
   - Afterwards the camera joins the eye again with position and velocity continuous, and ends
     exactly on it (no drift).
   - A warp snaps. An optional vignette darkens the edge of the view with the dash's speed.
2. **The sandbox (where the user tests).**
   - Moves: smooth locomotion on the left stick relative to the head; snap or smooth turn on the
     right stick; dodge on A, backstep on A without the stick, jump on L3 while moving.
   - The moves are the game's own clips, every frame decoded from the dump, played at 30 fps.
   - `"mode": "raw" | "ab"` gives the A/B comparison.
   - All of it is live-tunable in `sandbox3d.json`.
3. **The game.**
   - Detection: the player's TimeAct ring names the clip. Forty dodge-family clips map to a
     distance and a direction in the character's frame **[M]**, and the facing heading turns that
     into the world.
   - Fallback: the controller presses the bridge generates (circle tap, L3 while sprinting),
     confirmed by the motion.
   - A move that stops short (a wall) ends the dash at once, read from the feet.
   - Status: the module compiles into bb-probe and is called from the first-person camera (§6).
     **It has not run in the game yet.**
4. **Tests:**
   - 81 filter checks, including no overshoot, no jitter at 72 Hz over a 30 fps game, cancel,
     chains, no drift and teleports;
   - 78 scripted sandbox locomotion checks;
   - 43 game-side C checks;
   - `--offline` plots, `preview/dash_*.png`.

## 1. What the game's camera does in these moves [M]

`dodge_extract.py` decodes the player's clips in `chr/c0000_a00_md.anibnd.dcx` (Havok 2014 spline
animations, all frames and blocks, plus the reference-frame root motion). It then follows the eye
(dummy 58 on the Head bone) through forward kinematics.

Validation:
- Frame 0 matches `hitbox_extract.py`'s decoder exactly.
- The bind-pose eye sits at 1.6544 m (the FLVER says 1.654).
- Every dodge ends with the eye at 1.60 m, standing.

Space: right-handed meters, +X right, −Z forward (FromSoftware X mirrored); the character starts at
the origin facing −Z.

| Clip | Move | Root motion | Root along the move after 0.1 / 0.2 / 0.3 / 0.5 s | Eye | Head | i-frames (TAE flag 8) |
|---|---|---|---|---|---|---|
| a000_007200 | forward roll | 4.00 m, ends 0.93 s (clip 1.67 s), peak 8.0 m/s | 20 / 39 / 59 / 83% | 1.60 → **0.17 m** at 0.43 s, surges up to 0.37 m ahead of the root | a full somersault: tilt up to 155°, pitch 84° | 0–0.37 s |
| a000_007100–7107 | quickstep, 8 directions (0, ±25, ±90, ±155, 180°) | 4.00 m, ends 0.83–1.27 s, peak 10.2 m/s | 2 / 24 / 48 / 80% (7100) | down to 0.82 m | pitch up to 76° | 0–0.37 s |
| a000_007000 | backstep | 2.48 m back, ends 0.83 s | 17 / 42 / 64 / 89% | a hop: 1.14 → 1.46 → 1.28 → 1.60 m | 17° | none in the TAE |
| a000_007800 | running jump (L3 while sprinting) | 6.84 m over 2.07 s: a leap (75% in 0.8 s), then a landing roll | 6 / 19 / 30 / 50% | up to **2.07 m** (0.47 above standing), then down to **0.05 m** (the landing roll) | tilt 155° | 0.77–1.17 s (the landing roll) |

- **A clip starts mid-lean.** Frame 0's eye is already low and ahead: 7200 starts at
  (0.02, 1.16, −0.45), but standing is (0, 1.60, −0.12). The game blends from standing into it.
  The sandbox blends over 0.15 s. Taking frame 0 as the start shifts the whole path up by about
  0.4 m.
- **Other clips in the family:**
  - 7150–7157: a second quickstep set, 3.7–4.0 m, i-frames 0–0.27 s.
  - 7250: a second forward roll.
  - 7300/7350: rolls back 2.5/2.0 m.
  - 7400–7457: 8-way rolls whose first frame is already low (probably getting up).
  - 8250: a 2.95 m roll.
  - 8000–8040: drops off ledges, falling 0.8–1.1 m. These are left out: the camera follows a fall.
- **Bank layout:** the `md` bank holds the 75 clips from a000_005000 to 008250; `lo` holds idle and
  walk; `hi` holds ≥ 9000.
- Of the 39 clips in the 7000s, 35 carry the invincibility flag, as 13 §7.4 says (missing on 7000,
  7050, 7300 and 7350).

## 2. The filter (`vr_dash.h`)

**Interface:** `vr_dash_update(&dash, &params, anchor, move_id, hint, t)` returns the camera
position.
- `anchor`: the eye, or the root plus an eye height (world meters, +Y up, either handedness).
- `move_id`: 0 = no move. Any other value is a move, and a new value starts a new dash, so
  back-to-back moves chain.
- `hint`: read once when a dash starts. It carries the kind (dodge/jump), the horizontal direction
  and distance, and optionally a duration and a jump height.
- `t`: any monotonic clock in seconds, at any update rate.

| Mode | Camera |
|---|---|
| IDLE | = the anchor (pass-through) |
| MOVE | `start + (end − start) · e(u)` horizontally, `u = (t − t0) / duration`. `end = anchor at the start + axis · distance`. Height: `start.y` + the jump bump `h · sin²(π τ / T_bump)` |
| SETTLE | `anchor + Hermite(offset, camera velocity → 0, 0)` over `settle`, or longer for a big gap (`|offset| / settle_speed`) |

The rules, and why:

| Rule | Why (comfort, robustness) |
|---|---|
| **Straight line**, start → end; the eye's sideways sway (up to 15 cm) is ignored | Lateral oscillation is optic flow nobody asked for |
| **Level**: the start height during the move, whatever the eye does (down to the floor in a roll) | Vertical bob is the strongest sickness cue in first person (findings/11 §3.7: the eye drops 1.4 m in 0.4 s) |
| **No rotation** from the animation (the filter outputs a position only) | findings/11's rule: the headset owns rotation, the horizon stays level |
| **Ease-out** `e(u) = 1 − (1 − u)^p`, default p = 2, 0.4 s | All the acceleration happens at once at the start, then the motion only slows down. Most of the distance is covered in the first frames: 44% after 0.1 s, 75% after 0.2 s. The time spent moving is short. No ease-in phase |
| **Trusts the hint's distance**, does not follow the eye past it | The eye surges 0.25–0.4 m past the end and comes back; following it would overshoot |
| **No hint distance**: follows the anchor's furthest progress, critically-damped smoothing at 5 Hz | Smooth between a 30 fps game's samples (no velocity scallops); never past the anchor's own furthest point |
| **No hint direction**: the anchor's smoothed travel sets it and keeps refining it | A direction taken from the first 5 cm is off by the head's sway |
| **Re-aim** (`vr_dash_retarget`) | The character may finish turning into a roll a frame or two late: the slide bends once and still lands on the end |
| **Settle**: cubic Hermite from (offset, the camera's own velocity) to (0, 0) | Position and velocity continuous. It ends exactly on the anchor: no drift, ever. A cancelled dash brakes instead of stopping dead. The anchor's past velocity is left out: carrying it pushed the camera past an eye that stopped |
| **`settle_speed` 4 m/s**: a bigger gap takes longer (1.4 m: 0.35 s) | A cancelled roll's camera comes back 1–1.4 m. In 0.2 s that is a jolt |
| **`max_lead` 2 m**: never further ahead of the anchor's progress | For a wrong distance or a blocked move. The guard uses the smoothed progress plus 2v/ω, the lag of the smoothing: without that it bent a normal roll by 0.54 m. It never decreases, so the guard never pulls the camera back mid-move. A 0.4 s quad slide leads the measured roll by up to 1.7 m, so 1.5 m would clip it |
| **`max_drop` 2 m**: the anchor that far below the start ends the dash | An eye cannot dip more than its own height (about 1.6 m): a real fall, followed. With 1.5 m, the jump's landing roll (1.55 m down) was mistaken for one |
| **Teleport** 2 m in one update, or a stall over 1 s: snap | A lamp, a respawn, a cut: no slide across the map |
| **Vignette** from the dash's analytic speed (full at `vignette_speed` 4 m/s), immediate rise, 0.15 s release | Dynamic field-of-view restriction lowers sickness unnoticed (Fernandes & Feiner 2016, findings/11 §4.3). It follows the dash's own speed, not the game's samples, so it does not flicker |

**72 Hz over a 30 fps game.** The dash is a function of time, so between the game's samples it
keeps moving.

| | Game 30 fps, camera 72 Hz |
|---|---|
| Anchor alone | 13 frames without motion in the first 0.3 s (the test); speed spikes up to 32 m/s (the recorded roll, `dash_roll.png`) |
| Dash, distance known | No stalls, the speed only falls, no reversals |
| Dash, distance estimated | No stalls, no reversals; it follows the anchor 64 ms late |

## 3. Parameters

| Sandbox (`sandbox3d.json` → `locomotion`) | Game (`BB_VR_DASH_SET` / `BB_VR_DASH_CONTROL` token) | Default | Meaning |
|---|---|---|---|
| `dash_duration` | `duration=` | 0.4 s | the slide of a dodge or backstep |
| `ease`, `ease_power` | `ease=`, `power=` | quad (2) | `linear`, `quad`, `cubic`, `quart`, `quint`, `expo` (ease-out); `smooth` (in-out, for comparison) |
| `settle_s`, `settle_speed` | `settle=`, `settle_speed=` | 0.2 s, 4 m/s | joining the eye afterwards |
| `jump_height`, `jump_flat` | `jump=` (0 = flat) | 0.3 m | the jump's bump |
| `jump_duration` | `jump_s=` | 0.9 s | the jump's slide and bump (the leap is about 0.85 s in the air) |
| `max_lead_m` | `lead=` | 0 (sandbox: nothing blocks) / 2 m | the lead guard |
| — | `drop=` | 2 m | a fall ends the dash |
| `vignette`, `vignette_inner_deg`, `vignette_outer_deg` | `vignette=` (strength only) | 0.6, 35°, 70° | the comfort vignette |
| `dodge_distance`, `backstep_distance`, `jump_distance` | — (the table) | the clip's | |
| `hint` | `detect=auto\|anim\|input` | exact / auto | what the dash knows |
| `mode` (`dash`, `raw`, `ab`), `raw_rotation`, `game_fps`, `raw_source`, `*_anim` | `dash=0\|1` | dash | sandbox only: A/B, the raw camera's rotation share, sample rate, which clips |
| `move` (`head`, `body`, `controller`), `turn` (`snap`, `smooth`, `off`), `snap`, `snap_repeat`, `turn_speed`, `expo`, `turn_accel`, `lock`, ... (the shared model's keys, `src/vr_turn_math.h`), `move_speed`, `sprint_speed`, `deadzone`, `accel_s`, `tap_s`, `vignette_walk`, `vignette_raw`, `show_paths`, `show_landmarks` | `tap=` (input fallback); turning is the first-person control file's (16) | head, snap 30° (repeat 0.3 s), 90°/s, 3 / 5.5 m/s | sandbox walking and turning |

## 4. The sandbox: test it here first

`scripts/vr_sandbox3d.sh` (the MSYS2 CLANG64 shell, the headset on). It builds, runs the tests,
extracts `dodges.json` when it is missing, and starts the sandbox.

| Input | Action |
|---|---|
| Left stick | Walk where you look; A held while walking sprints |
| Right stick | Snap turn 30° about your head (held: again every 0.3 s), or smooth turn |
| A tapped while walking | Dodge: the roll a000_007200, in the stick's direction |
| A tapped standing | Backstep a000_007000 |
| Left stick click while walking | Jump a000_007800 (the bridge passes L3 through at once while the stick moves) |
| A held 1.5 s standing still | The T-pose calibration, unchanged (A is kept from it while you walk or dodge) |

- **What you see.**
  - The hunter's eyes are yours, the head is hidden and the body is below you, as in the game's
    first person.
  - The world is a 60 m grid with posts.
  - After each move its paths stay drawn: orange is the animation's eye, green is the camera.
- **A/B.** `"mode": "raw"` rides the clip: its dip and sway, and the head's pitch through
  `raw_rotation` (shown as 2·asin(s·sin(pitch/2)); 1 = upside down in the roll, the default 0.5 =
  up to 60°). `"ab"` alternates on every move, and the log says `move 3: dodge RAW`.
- **Integration with the other sandbox work.**
  - While a menu has the controls, the menu kit strips its chords and zeroes the sticks before the
    locomotion sees them. The menus use grip chords, the triggers and the d-pad; "A, B, X or Y
    alone, R3 and the L3 click" stay free (README).
  - Turning and steering run the game's own first-person model, `src/vr_turn_math.h` (snap or
    smooth turn, psi + head, move = head/body/controller, recenter, lock-on).
    `turn_adapter.h` only converts between the game's heading (positive right) and the sandbox's
    yaw (positive left), and reads the model's own key names from the `locomotion` object.
- **Previews.** `scripts/vr_sandbox3d.sh --offline` plots, with the live settings, into
  `out/vr_assets/preview/`:
  - `dash_roll.png`, `dash_backstep.png`, `dash_jump.png`: top view, side view, progress over
    time (the game at 30 fps, the camera at 72 Hz), and speed with the vignette.
  - `dash_cases.png`: cancelled, three rolls chained, blocked by a wall, a teleport.
  - `dash_curves.png`: the ease curves and the vignette profile.

  With the defaults:
  - roll: the raw eye height changes by 1.43 m, the camera's by 0.00 m; peak 19.7 m/s;
  - backstep: 0.33 m against 0.00 m;
  - jump: 1.55 m against the 0.30 m bump.

## 5. The game: detecting the moves

**1. The TimeAct ring (primary, `detect=anim`).**
- The player is `P = [[0x593E878] + 0x60]`. The first-person code already validates it against the
  PlayerIns vtable 0x578F310 (16).
- `[[P + 0x3B0] + 0x18]` is the `SprjChrTimeActModule` **[C, 13 §3.1]**. Its ring holds 10
  entries `{i32 anim, f32 ×3}` at +0x20 with the write index at +0xC0, so the newest is
  `(head + 9) % 10` **[C, 13 §3.4]**.
- A new move: the index or the newest ID changes, and the ID (group × 1 000 000 + sub, groups a000
  to a009) is in the table (`vr_dash_moves[]`, 40 clips: distance, direction in the character's
  frame, root-motion end, clip length).
- The hint:
  - direction = the facing heading + the table's angle. The physics yaw (+0x1D4) is the heading
    of the character's **back** (16 §2), so the facing is `vr_facing_heading(yaw)`;
  - distance = the table's;
  - jumps slide over `jump_s`.
- Re-aim: for 0.25 s, if the facing turns more than 5° (the character still turning into the
  roll), the dash re-aims.
- End: when another clip starts.
- **Blocked:** the feet (`[[P + 0x3B0] + 0x68] + 0x1E0`, the physics position, 16) slower than
  0.5 m/s for 0.12 s, at least 0.2 s into the move, before the clip's root motion should end, and
  more than 0.4 m short. The dash then ends at once and the camera settles onto the eye: a roll
  into a wall comes back 0.4 s in instead of waiting out the clip. The eye cannot be used for this,
  because the head pauses about 0.23 s mid-roll as it comes over.
- Polling the ring costs about three `VirtualQuery`-guarded reads per frame on the H1 thread. No
  new hook, no patch.

**2. The input fallback (`detect=input`, or `auto` when the ring cannot be read).**
- We generate the presses ourselves (`xr_input.cpp`: A = circle; L3 passed through at once while
  the left stick moves).
- `bbgpu_vr_pad()` reads them. That is safe: the game path drops the gesture bits it would
  consume.
- The rules (`vr_dash_input_update`):
  - circle released within 0.3 s with the stick pushed: roll;
  - circle released within 0.3 s without the stick: backstep;
  - L3 pressed while circle is held and the stick pushed: jump.
- Confirmation: the eye must move faster than 2 m/s within 0.3 s; a press in a menu moves nothing.
- Distance by kind (4.0 / 2.48 / 6.84 m), direction from the motion. The move ends when the eye
  stands still for 0.2 s.
- Limits: VR controllers only (a gamepad is not seen); a roll and a quickstep look the same; the
  start lags the motion.

**3. Considered and not used.**
- **An edge-triggered hook on the ring push 0x1E27FB0.** 13 §3.4 gives the 15-byte prologue. The
  address is a PS4 VA, ELF vaddr + 0x400000 as in the patch XML. It needs a registry entry and a
  hook line in shared files, and the poll is enough at one read per frame.
- **The behaviour state names** ("Rolling State", "Step State").
- **TAE events** (flag 8 i-frames do not cover the backstep or the whole move).

## 6. Integration (applied in `src/runtime_vr_fp.c`)

Two small hooks into the rewritten first-person file, both inert without `BB_VR_DASH=1`:

- **Eye (H1, `vr_fp_frame`).** Right after `VrVec3 cam = world;`, before `forward`/`up` and
  `vr_cam_from_heading` (so before the head pose, which H1's `vr_cam_compose` adds later). The dash
  gets the player, the character's facing `vr_facing_heading(c.yaw)` (not the decoupled view heading
  `psi`: turning the view must not steer a dodge) and the eye, and returns the slid eye:

  ```c
  if (vr_dash_enabled()) { /* findings/19 */
      float p[3] = {cam.x, cam.y, cam.z};
      vr_dash_apply((uint64_t)c.player, vr_facing_heading(c.yaw), p);
      cam = vr_v3(p[0], p[1], p[2]);
  }
  ```
- **Pad (`fp_pad_filter`).** First thing, before the turn-stick and remap changes:
  `vr_dash_pad_sample(*buttons, *lx, *ly)`. The input fallback used to call `bbgpu_vr_pad`, whose
  `GetInput` clears the bridge's pending gestures, so it could steal a gesture press from the pad
  sampler. The dash now never reads the bridge: it runs the tap/sprint-jump detection on the
  sampler's own merged sample (at the game's poll rate, so no tap falls between two frames) and
  hands the result to H1 through one atomic.
- **Notes on the call.**
  - `src/runtime_vr_dash.c` is already compiled into bb-probe by the `src/runtime*.c` glob, and
    stays inert until this call exists.
  - Whatever first person does to the eye still applies: its smoothing, and its 75% floor
    (`BB_VR_FP_FLOOR`; redundant during a dash, the dash keeps the start height).
  - When first person drops out (a cutscene, downed), the dash state is forgotten after 0.25 s
    without a call.
- **Running it:** `BB_VR_FP=1 BB_VR_DASH=1` with the first-person hooks (16).
  - `BB_VR_DASH_LOG=1` prints a line per move: the clip, the table's distance and angle, and at
    its end the eye's real displacement, the angle off the table's direction, the lead and the
    re-aims. That validates the table and the facing sign in the game.
  - `BB_VR_DASH_CONTROL=<file>` re-reads the tokens of §3 while the game runs; `dash=0` turns it
    off live.
- **Vignette in the game:** `vr_dash_vignette()` (0..1, any thread) is ready. The bridge has no
  vignette layer yet: it would darken the eye images before submit, as the sandbox's pass does.
- **Third person** is not touched: the follow camera neither tumbles nor bobs.

## 7. Tests (no headset, no game)

| Command (MSYS2 CLANG64 shell, under `BB_BUILD_LOCK`) | Checks |
|---|---|
| `ninja -C out/xr-bridge bbport_xr_dash_test && out/xr-bridge/bbport_xr_dash_test.exe` | 81: ease curves; reach without overshoot, straight, level; an eye surging 0.4 m past the end; no jitter at 72 Hz over 30 fps (known and estimated distance); cancel at 40%; three chained moves; 50 random moves then walking: the camera is the anchor in every idle frame (no drift); teleport idle and mid-dash; jump bump and flat; blocked (`max_lead`); a fall; a roll to the floor not taken as a fall; estimated direction; translate/rotate; the game's settings leave the measured roll untouched; re-aim; vignette |
| `out/xr-bridge/sandbox3d/bbport_xr_sandbox3d_dash_test.exe out/vr_assets` | 78: the config section inserted, the rest of the file kept; walking speed and head-relative direction; snap turn about the head; the `body` steering mode, snap repeat, smooth turn (all through `vr_turn_math.h`); dodge, backstep and jump as a dash (level, straight, no pitch, landing on the clip's end) and raw (the dip, the pitch), synthetic and recorded; A kept from the scene while dodging, a still A passed; `ab` alternation |
| `ninja -C out/gpu vr-dash-test && out/gpu/vr-dash-test.exe` (or `cd tests && python -m unittest test_vr_dash`) | 43: the filter built as C11 on a roll; the clip table; directions; the ring decode; the stall check; the input fallback; the settings tokens |
| `scripts/vr_sandbox3d.sh --offline` | `preview/dash_*.png` |

## 8. Open questions and next steps

1. **In the game.** Run with §6 applied and log a roll, a quickstep, a backstep and a jump with
   `BB_VR_DASH_LOG=1`. Check that:
   - the ring's newest ID is the dodge clip in the frame the move starts;
   - the eye's displacement matches the table (4.0 m, angle within 5–10°), which also confirms
     the facing sign;
   - weapon-specific groups (a002–a005 also carry 7000s) are covered by the sub-ID match.
2. **Feel.** Try `dash_duration` 0.3–0.5 s, `quad` against `cubic`, and the jump bump against flat,
   in the sandbox first. The defaults come from the measured root motion (the lead stays under 2 m)
   and the comfort rules above, not from a headset session.
3. **Blocked rolls in the game.** Check the stall check against a wall, and that `lead=2` is never
   hit in open ground (the log reports the lead).
4. **The vignette in the game** needs a bridge layer (§6). The sandbox's pass shows the look.
5. **Shared turning (done):** the sandbox turns and steers with `src/vr_turn_math.h`, the game's
   first-person model, through `turn_adapter.h`. The sandbox's default is `move=head`, as asked;
   the shared default is `body`.
6. **Optional:** move the dash into the bridge's hot-reloadable logic (12 §2.10) with the rest of
   the camera math.

## 9. FOLLOW mode (fix: the camera never leads the character)

Headset report: the predicted slide (the move table's straight line, up to 2 m ahead) went under
the terrain and through walls, then jumped back. The game now runs `follow=1` (default;
`VrDashParams.follow`, `follow_omega` 14 rad/s): the filter ignores the table, direction and
distance and trails the character's REAL position through a critically damped spring, so it is
never ahead, stops when the feet stop (walls) and its height is the anchor's (hills, stairs) plus
a jump's bump (always above the feet). `runtime_vr_dash.c` feeds it feet (physics +0x1E0) plus
the eye offset captured while idle; a 0.15 s decaying correction hides the hand-back to the eye.
`max_lead` now defaults to 0 (off, `lead=`); `follow=0` restores the predicted slide (the
sandbox keeps it: nothing blocks the character there). Tests: `TestFollow` (wall, slope, jump) in
`vr_dash_test.cpp`, `test_follow` in `tests/test_vr_dash.c`.

## 10. Evade distance scale (1.5x in VR by default; first 2x) and co-op

Request: in VR the roll and quickstep should travel about twice as far ("double is good").

**How the distance is decided.** It is the clip's root motion: the table of `src/vr_dash_detect.h` (4.0 m for the quickstep and roll, 3.7 m for the 7150 set, 2.5 m for the backstep, over 0.8-1.5 s). The game applies it as a per-frame change of the physics position (`[[P + 0x3B0] + 0x68] + 0x1E0`). Speed multipliers do not help: the "VR Attack Speed (chr dt)" hook scales the character's time step, which plays the clip faster (same distance, shorter) and would also shrink the i-frame window. The root-motion application itself is not hooked in this port.

**What was added.** `src/vr_evade_scale.h` (pure) and `as_evade` in `src/runtime_vr_atkspeed.c`, in the same hook as the attack speed. Once per player update, while the newest TimeAct ring clip is a roll, quickstep or backstep, the previous frame's real horizontal step is measured from the feet and `(k - 1) * step` is added to the feet along it. Clip time, i-frames and stamina are not touched (they follow the clip and the behaviour, not the position), and walking, running, jumps and attacks are not in the table. Guards: at most `(k - 1) * table distance` per clip; only until the clip's root motion ends; the extra stops for the rest of the clip when the step speed halves (a wall, the end of the motion); none for a step over 1.5 m (a teleport); a restarted clip (its time drops) starts afresh. The extra is not collision-checked: a very thin wall inside the extra metre can be crossed (the next physics step pushes out).

| Setting | Meaning |
|---|---|
| `BB_VR_EVADE_SCALE=<k>` | start value, default 1.5 (was 2.0; the player settled on 1.5 live) (1 or less = off, at most 4) |
| `evade_scale=<k>` in `out/fp.txt` | live (polled 4 Hz with `atk_speed=`; a missing key = the env value, else 1.5; `evade_scale=1` = off; every change of the value is logged as `VR evade: scale <k> (control file)`) |
| `BB_VR_EVADE_LOG=1` | a line when an evade starts and ends |
| `BB_VR_EVADE_ONLINE=1` | keep the scale on while online (testing only) |

Needs the hook "VR Attack Speed (chr dt)" in `BB_HOOKS` (the launchers already have it). The comfort dash follows the real feet, so its camera simply covers the longer move.

**Tests.** `tests/test_vr_dash.c` `test_evade_scale` (`ninja -C out/gpu vr-dash-test`): clamp, parse, online flag, table membership (the jump is excluded), a 4 m roll gains up to 4 m, off/walk/jump give nothing, a wall stops it, a teleport and a restart, the end of the motion. **Not verified in game**: whether a write to `+0x1E0` sticks (the position may be re-derived from another copy), and how it feels. Run once with `BB_VR_EVADE_LOG=1` and `BB_VR_DASH_LOG=1`; the dash log prints the table distance against the distance the eye really moved.

**Co-op.**

- The base game replicates the character through its own P2P state, not through anything this port adds. The port's pose channel (findings/34) says explicitly that "the root's own world position and heading come from the game's character sync".
- In this port that sync has never been seen working: the game's P2P socket carried no traffic in the traced runs, and bell co-op is not end to end yet (private notes, not copied here).
- So the answer to "does the other player see the longer evade" cannot be measured yet. From the game's design (each client simulates its own hunter and sends position and animation state), a longer step is sent as a faster, longer move: the peer most likely sees a quick slide, and a peer with a position sanity check could snap it back (rubber-band). Unverified.
- Hit decisions are likewise not verifiable here. The Souls-series model is that the attacker's client decides hits on remote players and the host decides for enemies; i-frames stay the same length, but the dodged attack's hit test happens where the roller is on the roller's own machine, so a 2x roll keeps the i-frame behaviour on the roller's side and may disagree with the peer's view of where the roller was.
- **Recommendation: auto-disable online.** The scale is a client-side position change the base game's sync and its moveset timing do not expect, and there is no way yet to test it. Implemented: the scale is forced off when `BB_ONLINE=1` or `BB_ONLINE_ROLE` is set (the same flags the port uses elsewhere); `BB_VR_EVADE_ONLINE=1` overrides it for a test session with two instances. Revisit once the P2P sync works end to end and two instances can be compared.

## 11. Backward evade = the backstep

Request: an evade while moving backwards should be the game's backstep (what dodge does with no stick), not a backward roll or quickstep.

**How it is triggered.** In the game the dodge is decided when circle is released after a tap: stick pushed = roll or quickstep along the stick, stick neutral = backstep. The pad thread (`fp_pad_filter` in `src/runtime_vr_fp.c`, first person only) therefore checks each press of circle: if the left stick's world direction is more than `backstep_angle` (default 120 deg) from the character's facing, it sends the game a neutral left stick (128,128) from the press until 0.10 s after the release. The game sees a plain tap with no stick and does its own backstep (same clip, i-frames, stamina, replication). Nothing else is touched: no position write, no animation write. Walking is unchanged; only the stick is held neutral for the length of the tap.
- "World direction" = the move heading (`fp_target_bits`, the head's yaw with `move=head`; the stick is head-relative in VR) + atan2(stick x, stick y). "Facing" = the character's heading published by H1 each frame (`vr_facing_heading(yaw)`, after the face write, so with `face=` it is the view). The stick may arrive up to 0.08 s after the button and still count; a deadzone of 0.3 applies.
- A circle held longer than 0.30 s is a sprint, not a dodge: the stick is given back at once, so a backward sprint is untouched.
- The pure part (classification, hold window, off switch) is `src/vr_backstep.h`, tested by `test_backstep` in `tests/test_vr_dash.c`.

**Lock-on.** In the base game a dodge with the stick back while locked on is a back quickstep (the stick is relative to the target). Decision: the backstep applies there too (default `backstep_lock=1`), because the player asked for backward evades to be the backstep and a lock-on quickstep backwards is the same gesture; `backstep_lock=0` keeps the base game's behaviour while locked on.

| Setting | Meaning |
|---|---|
| `BB_VR_BACKSTEP=1\|0` | default on in VR first person |
| `backstep=1\|0` in `out/fp.txt` | live |
| `BB_VR_BACKSTEP_ANGLE=<deg>` / `backstep_angle=<deg>` | threshold, default 120 (30..180) |
| `BB_VR_BACKSTEP_LOCK=1\|0` / `backstep_lock=` | also while locked on (default 1) |
| `BB_VR_EVADE_LOG=1` | a line `VR backstep: dodge with the stick behind (...)` each time it triggers |

**Online.** Safe, so it is not gated by `BB_ONLINE`: it only changes the input the local game reads, which then performs a normal backstep (the same animation and position an unmodified client produces when the player presses dodge with no stick). Unlike the evade scale (section 10) it adds no position change the peer's sync does not expect.

**The evade scale fix.** A report said `evade_scale=1.5` in `out/fp.txt` was ignored. In the log (`out/vr_all_stereo_run12b_plain.log`) the file was read at start without the key (so 2.00), and `VR evade: scale 1.50 (control file)` appears at line 2543 once the key was added: the live key worked, the evades of that session simply ran before it. Two real weaknesses were fixed: a missing key used to reset to the built-in default, overriding `BB_VR_EVADE_SCALE`; now it falls back to the env value. The change line is now always logged (it needed `BB_VR_EVADE_LOG`/`BB_VR_FP_LOG`), and the startup line names the control file in use. `test_evade_control` covers the parse (last token with LF, CRLF, no newline).

**Fix after the first headset test (run12c never triggered).** The first version compared the stick's world direction with the CHARACTER's facing. Out of lock-on a Souls character turns to its walk direction, so with the stick pulled back it already faces the movement and the angle was ~0. Now "back" is judged against the player's VIEW heading (`fp.turn_out.view_yaw`, published by H1; with `move=head` this is just the stick pulled back from stick-forward); the character's facing is only a fallback when no view is known, and with no heading at all the raw stick angle is used. With `BB_VR_EVADE_LOG=1` every circle press is logged (`VR backstep: circle pressed (buttons, stick, degrees from the view, back, locked, on)`), which shows that the filter runs and the button mask matches.

**Backstep distance (own scale).** The run12d log showed backsteps at x1.50 gaining only 0.34-1.12 m (mean ~0.8 m, the limit 1.25 m): the backstep's root-motion speed decays early, so the roll rule "a step under half the previous speed = a wall" stopped it. Backsteps (clips of the table with kind backstep: 7000, 7050) now use their own multiplier, `BB_VR_BACKSTEP_SCALE` / `backstep_scale=` in `out/fp.txt` (live, default 2.0, 1 = off; a missing key falls back to the env value; changes logged as `VR evade: backstep scale <k> (...)`), and their stop guard only stops on a step under 15% of the previous one (a wall gives ~0) besides the existing end-of-root-motion (clip's motion time + 0.05 s), teleport (> 1.5 m) and total (<= (k - 1) x the table distance, 2.48 m) limits. Rolls and quicksteps keep `evade_scale` and the 50% rule. Same online rule as the evade scale (forced off online). Tests: `test_backstep_scale`.

## Cross-links
- **11 §3.2/§3.7/§4.3–4.4:** the first-person comfort design this realises: no rotation from
  animations, the height floor during dodges (superseded by the dash), the vignette.
- **13 §3.1/§3.4/§7.4:** the module bag, the TimeAct ring, the a00 7000s as dodges.
- **16:** the first-person camera, the player chain, the physics position and the yaw convention
  (the back's heading).
- **12:** fast iteration: the sandbox and its live config, the logic module this could move into.
