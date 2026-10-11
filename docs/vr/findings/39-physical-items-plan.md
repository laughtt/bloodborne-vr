# 39 - Physical items in the game: plan (throw, buff, heal, weapon collision)

Date: 2026-10-10. Branch `vr`. **Plan only**: no code, no build, no game run. HTML copy:
`out/plans/physical_items_plan.html`. Tags as in 08/27: **[C]** confirmed in the disassembly or a game log,
**[L]** likely, **[S]** speculation, **[U]** unknown (must be reverse-engineered).

## 0. What exists today (evidence)

| Piece | State | Evidence |
|---|---|---|
| Sandbox throw (rack, grab, hand velocity, Bullet flight) | done, 408 checks, sandbox only | 35 §3; `sandbox3d/throwables.{h,cpp}` (`HandVelocity::Estimate`, throwables.h:60-66) |
| Sandbox mapping hand velocity -> Bullet | `dir = v/|v|`, `speed = initVellocity x clamp(|v|/7, 0.3, 1.5)`, cap `maxVellocity`, < 1.2 m/s = drop | 35:88-90, 35:122-125 |
| Sandbox consumables (wheel, rub on blade, bring to mouth, crush) | done; buffs instant by default, rub behind `buff_rub`; heals by mouth (12 cm, 3 cm below eyes, 0.3 s) | 36:32-35, 36:66-69 |
| In-game item use | wheel -> quick slot -> d-pad down until PGD+0x418 = slot, then square (closed loop) | 26:304-309, 26:333; `src/runtime_vr_ui.c` |
| Equip/quick-slot command | `0x1f63e00(slot, record)` on the game thread (step hook) [C] | 26:155-183 |
| Add item | `0x1f5c8c0(const u32 *id, int count)` [C, used by BB_DEBUG_GIVE] | runtime_vr_ui.c:55 |
| Pending use id | PlayerIns+0x394, read at 0x18d0822 [C] | runtime_vr_ui.c:56, :646 |
| Menu-use request | PGD+0x458 `m_useRequestItemFromMenu` [C], **not written** (reset owner unknown) | 26:186, 26:320-321 |
| Apply SpEffect directly | PlayerIns vtable slot **0x3F0** (as Co-op Quick-search 0x200d3b0 does); **ran in game** (speffect 9000/9900 applied, return value meaning unknown) | runtime_vr_ui.c:57, :375-395; out/coop_t1 log |
| Bullet launch hook | "VR Gun Aim (bullet spawn)" at **0x1df6ca0** (rdi BulletIns, rsi descriptor, rdx target, rcx launch 3x4); filter caller 0x1df060c, owner = player, ids 500-999; **ran in game** (bullets 600/601 redirected, child 601 goes through it) | 27:18-39, 27:58-63; patches/Bloodborne.xml:3501-3503; out/vr_all_alt*.log |
| Launch math | position = matrix column 4 (BulletIns+0x10), bullet flies along axis Z, velocity = speed x normalize(Z) written at 0x1df76df..0x1df77a7 to BulletIns+0x40/+0x50 [C] | 27:37-43 |
| Bullet request (manager entry) | `0x1e03620(SprjBulletManager [0x593E8D0], request)`; request +0x00 owner handle, +0x04 behaviour bullet id, +0x0c BulletParam id, +0x30 dummy-poly 3x4, +0x60 character 3x4 [C, partial] | 27:19-23 |
| Animation time scale | "VR Attack Speed (chr dt)" hook 0x1cbd711 scales the player's dt while newest TimeAct sub-id is 30000-49999 | runtime_vr_atkspeed.c:17-25; Bloodborne.xml:3511 |
| Arms IK during item use | in game the IK weight is reduced only for attack sub-ids 30000-49999, so the arm stays on the controller during item-use animations | runtime_vr_arms.c:683-686 (doc 13 §5.4 planned 0) |
| Weapon bounce on walls (attacks only) | world-hit handler 0x1cb8730; hook 0x1cb87ff sees the bounce flag (`al`) [C site, L meaning] | 20:664-676 |
| Attack capsules per frame | attack manager 0x1c63540, update 0x1c5ed50, test 0x1c5fbe0 | 32:70-81 |
| Weapon bones in headset space | the hitbox overlay already gets L_Weapon/R_Weapon matrices (BbXrGameDebug) | vr_game_hitbox.h; 32:101 |
| Camera collision (a world probe) | collision / near plane 0x18414f0 [C as a function, signature U] | 08:137 |

Item data that changes the plan (`out/vr_assets/items/items.json`, doc 17:25-31):

| Item | refCategory -> row | Kind |
|---|---|---|
| Blood Vial 1000 / Iosefka's 703 | 2 -> SpEffect 3010 / 3020 | heal (SpEffect) |
| Beast Blood Pellet 1110, Blue Elixir 1120, Madman's Knowledge 1500, Coldblood 1510+ | 2 -> SpEffect 3240 / 2014 / 2160 / 3600+ | body buff (SpEffect) |
| Fire Paper 1300, Bolt Paper 1320, Empty Phantasm Shell 1310, Bone Marrow Ash 1330 | 2 -> SpEffect 2200 / 2220 / 2210 / 2230 | weapon buff (Shell: not consumed, 3 bullets) |
| **Antidote 1100, Sedative 1101, Lead Elixir 2030** | **1 -> Bullet 200 / 230 / 320** | cure **through a Bullet** (spawned at the player) |
| Molotov 1200, Throwing Knife 1240, Oil Urn 1220, Pebble 1230, Numbing Mist 1270 | 1 -> Bullet 100 / 120 / 150 / 170 / 180 | thrown |
| Rope Molotov 1250 | 1 -> Bullet 110 | back throw, no flight (keep vanilla) |

Consequence: the throw redirect must use an **allow-list of flight bullets** (100, 106, 120..., from
`throwables.json` rows with `dist > 0` / `initVellocity > 0`), never "100-370": that range contains the cures
(200, 230, 320) and the rope molotov (110), which must keep spawning at the player.

## 1. Throwables

### 1A. Gesture -> the game's own throw, launch redirected (cheapest)

- **Trigger.** A throwable is the current quick item (picked on the wheel, or a "throw slot"). Right grip
  held + a throw motion (hand speed > 2.5 m/s forward of the head, release of the grip) -> the executor's
  `USE_QUICK` (square press, existing). Optionally start the press at the wind-up (hand behind the head
  with grip held) to hide the animation's delay.
- **Aim.** Extend the 0x1df6ca0 handler with a second mode: owner = player, caller 0x1df060c, BulletParam
  id in the thrown allow-list. Replace the launch matrix's column 4 (origin) with the right hand bone's
  world position (or the controller grip mapped through the H1 camera rows, as `vr_gun_view` does), and turn
  the Z axis (plus the kind-4 aim point about the origin) onto the buffered release velocity's direction.
  Same `vg_redirect` math (pitch then yaw, no roll), already unit-tested. No new hook site.
- **Speed.** At the entry the speed is not yet written; it comes from BulletParam `initVellocity`
  (27:39). Options: (i) keep the game's speed (direction only), phase 1; (ii) stash `rdi` (BulletIns) and
  rewrite BulletIns+0x40/+0x50 = dir x `initVellocity x clamp(|v|/7, 0.3, 1.5)` (cap `maxVellocity`) on the
  next game-thread step, after checking the slot still holds that bullet id and owner (one frame at the
  original speed, about 0.25 m of error); (iii) a second hook after the launch init returns (0x1df060c,
  BulletIns in a callee-saved register **[U]**) for an exact write.
- **Must reverse-engineer.** [U] whether thrown goods reach 0x1df6ca0 through the emitter call 0x1df0607
  (27 §5 test 5 assumes they do; no log line yet - run once with `BB_VR_GUN_AIM_LOG=1 BB_VR_GUN_AIM_IDS=all`
  and throw a pebble). [U] the release delay of each use animation (`goodsUseAnim` 2/3/13/34, 35:37-38) =
  time from square to the launch. [U] whether BulletIns+0x40/+0x50 is the live velocity the mover integrates
  (27:39 says written there; the mover 0x1df4f00 reads +0x7b0/+0x7d0 for homing).
- **Risks.** The body still plays the throw animation along its facing (the IK keeps the arm on the
  controller, so the hand looks right; the torso may twist). Latency 0.3-0.6 s between the real release and
  the launch: the projectile appears from where the hand is *then*. Item counts and stamina stay the
  game's (no desync). Co-op: the bullet is synced by the game. Crash risk low (the handler only edits the
  caller's stack copies; the velocity rewrite touches a live object: validate).
- **Comfort.** Good; no camera motion. Mis-timed release feels "laggy".
- **Effort.** 1.5-2 days (direction/origin), +1 day for the speed rewrite.
- **Tests.** Offline: `vr_gun_math.h` cases for a thrown row (origin swap, aim point turn, allow-list
  excludes 110/200/230/320); a gesture detector test fed with the sandbox's recorded throws
  (`throwables_test.cpp` fixtures); hooks-test unchanged window. Headset: `BB_DEBUG_GIVE_FILE` with pebbles,
  knives, molotovs; throw left/right/up/down/soft/hard; log line per throw (bullet id, game dir -> hand dir,
  hand speed, delay since release); check a cure (Antidote) still lands on the player.

### 1B. Direct spawn at release (no animation, zero latency)

- **What.** On grip release, call the bullet manager `0x1e03620(mgr, request)` on the game thread (step hook)
  with a request built like 0x1cb6680 does: owner handle `[player+8]`, BulletParam id = the goods' `refId`,
  the launch 3x4 at the hand with Z = release direction. Then fix the speed as in 1A(ii), consume one item,
  and skip the square press entirely.
- **Must reverse-engineer.** [U] the full request layout (only +0x00/+0x04/+0x0c/+0x30/+0x60 known) and its
  required fields (behaviour id, hand, attack power/scaling: the bullet's damage may need the
  BehaviorParam_PC 3000-variation row, 14:305); [U] the **consume** routine (the game's own decrement after
  a use; add-item 0x1f5c8c0 / GetItemNum 0x18cf450 are known, a remove is not) - writing entry+8 (quantity)
  directly risks a stale quick slot at 0; [U] whether online sync happens inside the manager or in the TAE
  path.
- **Risks.** Highest crash risk (a wrong request = guest fault in the bullet code); damage scaling may be
  wrong (Arcane scaling of molotovs); balance (throw spam with no animation: add a cooldown = the vanilla
  animation length); count desync if the consume is wrong.
- **Comfort / immersion.** Best: the item leaves the hand at the hand's speed.
- **Effort.** 4-6 days RE + 2 days integration.
- **Tests.** Offline: request-builder unit test against a request captured from a real throw (dump the
  0x1e03620 argument once with a logging hook). Headset: count before/after, damage numbers vs a vanilla
  throw on the same enemy, 50 throws soak.

### 1C. Our own physics proxy, game effect on impact (most work)

- **What.** Fly our own item (sandbox `throwables.cpp` Bullet integration, mesh drawn on the UI layer) and
  on impact spawn the burst bullet (`HitBulletID`, e.g. 101 for the Molotov) at the hit point via 1B's request.
- **Must reverse-engineer.** Everything in 1B plus a **world ray/sphere cast** for the flight [U] (the same
  query as 4B), and enemy hit tests (or let the burst bullet do the damage).
- **Verdict.** Not worth it: the game's bullet already flies with the right gravity, range and hit tests
  once its origin, direction and speed are set (1A/1B). Only the item mesh would differ.
- **Effort.** 2-3 weeks.

### Shortening the throw animation (applies to 1A)

Extend the chr-dt hook (0x1cbd711) with a second sub-id range for the item-use animations (x1.5-2 while
one plays). [U] the sub-ids of the use animations: read the TimeAct ring's newest animation (already read by
`runtime_vr_atkspeed.c` / `runtime_vr_arms.c`) and log it during each use (a log-only change). Risk: the
whole character's dt is scaled (stamina regen, movement) for that time; OK up to ~2x (runtime_vr_atkspeed.c:34).

Done (headset check pending): `BB_VR_THROW_ANIM_SPEED` (default 3.6, 1 = off, max 4; was 1.8, doubled after the headset test). The throw gesture's `USE_QUICK` (c > 0) calls
`vr_atkspeed_throw_begin`; the "VR Attack Speed (chr dt)" hook then waits (4 s) for a NEW TimeAct clip (id change or clip restart,
not a weapon move 30000-49999 or a dodge 7000s) and scales the player's dt by the factor until that clip ends (`src/vr_throw_anim.h`,
test `vr-throw-anim-test`). Drinking and other uses never arm it. The release point inside the clip is not moved (the sub-ids are
still unknown): the projectile leaves at the same clip time, so the delay shrinks by the factor. `BB_VR_FP_LOG=1` logs `VR throw anim: clip <id> latched`.

## 2. Weapon buffs (Fire Paper, Bolt Paper, Empty Phantasm Shell, Bone Marrow Ash)

### 2A. Gesture -> the game's own use (cheapest)

- **What.** Picking a buff on the wheel no longer uses it at once: the item is "held" in the free hand
  (icon or mesh drawn at the controller on the UI layer). Touching it to the blade (the sandbox rub test:
  10 cm, 50 % of the blade, 2.5 s, `consumables.cpp`) or a simple tap of item to blade -> `USE_QUICK`. The
  blade segment = the R_Weapon/L_Weapon bone matrices the hitbox overlay already receives (headset space).
- **Must reverse-engineer.** Nothing new. [L] the weapon's length per weapon: from the hitbox JSON capsules
  (out/vr_assets/hitbox), already loaded by the overlay.
- **Risks.** The buff animation (about 1.5-2 s) still plays after the rub; the item count and SpEffect are
  the game's. Bone Marrow Ash buffs the gun (left), so the rub target is the left hand's gun.
- **Effort.** 1-1.5 days (bridge-side only: the gesture code exists in the sandbox).
- **Tests.** Offline: move the rub detector into a header shared with the bridge, replay the sandbox test
  cases with game-like bone poses; `vr_game_ui_test.cpp` for the held state (wheel closes, R3 again cancels).
  Headset: rub, partial rub (no use), rub on the wrong hand, buff glow appears, count -1.

### 2B. Direct SpEffect, no animation (instant)

- **What.** On the rub, apply the item's SpEffect (2200/2210/2220/2230) through PlayerIns vtable **0x3F0**
  (already run in game for 9000/9900) and consume one item (or 3 quicksilver bullets for the Shell).
- **Must reverse-engineer.** [U] the consume routine (as 1B); [U] the Shell's bullet cost write (PGD bullet
  count or the same consume with the bullet item id); [L] that a weapon-buff SpEffect applied this way
  attaches to the held weapon (wepParamChange, the blade glow SFX) - test first with the debug use file
  `speffect:2200`, no code needed; [U] the meaning of slot 0x3F0's arguments (a=0, b=1, c=0, floats 1.0)
  and whether co-op peers see the buff.
- **Risks.** Item count desync if the consume is wrong; no animation = a balance change (buffing mid-combo).
- **Effort.** 0.5 day test (debug file), 2-3 days with the consume RE.
- **Tests.** Headset: `BB_DEBUG_USE_FILE` line `speffect:2200` with the Saw Cleaver: glow, +fire damage
  numbers on an enemy, 60 s duration, then the same through the rub.

### 2C. Rub plus faster animation

2A + the chr-dt scale for the buff animation (same [U] sub-ids as the throw). Middle ground that keeps the
game's own consume and sync. 0.5 day after the sub-ids are logged.

## 3. Heals and other consumables (Blood Vial, Antidote, Sedative, Pellet...)

### 3A. Bring to the mouth -> the game's own use (cheapest)

- **What.** As 2A: the wheel puts the item "in the hand"; within 12 cm of the mouth (3 cm below the eyes,
  0.3 s; 36:32-33) -> `USE_QUICK`. Bells to the ear, marks crushed (sandbox rules). Covers every kind
  (SpEffect heals, Bullet cures 200/230/320) because the game's path does the work.
- **Must reverse-engineer.** Nothing. Head and controllers are both in tracking space.
- **Risks.** The injection animation still plays (the arm stays on the controller because the IK keeps
  weight 1 during item use, runtime_vr_arms.c:683-686: the body plays the stab at the thigh while the hand
  is at the mouth - visible only in a mirror/shadow).
- **Effort.** 1 day (shares 2A's held-item state).
- **Tests.** Offline: mouth detector with recorded head/hand poses (sandbox `consumables_test.cpp`);
  Headset: vial at the mouth heals, near the chest does not, hold 0.2 s does not, the wheel cancel.

### 3B. Direct effect, no animation

- **What.** For refCategory 2 (vials, pellets, coldblood): apply the SpEffect (3010, 3240...) via slot
  0x3F0 and consume. For the Bullet cures (200/230/320): spawn the bullet at the player through 1B's request.
- **Must reverse-engineer.** Consume [U]; vial rules [U] (the heal amount of 3010 uses
  `transfusionHpRate` and the rally window - does the SpEffect alone give the same heal? compare numbers);
  the use limit `useHpCureMaxNum` (20) is enforced by the menu path, not by the SpEffect.
- **Risks.** Large balance change (instant heals without the stab commitment). Offer a "lockout" (no
  attack for 0.5 s) or keep 3C as default.
- **Effort.** 2-3 days after the consume RE (shared with 1B/2B).

### 3C. Mouth + faster animation

3A + item-use animations at x1.5-2 via chr dt. Keeps the game's rules, shortens the commitment.

## 4. Weapon vs world collision (optional)

### 4A. Feedback only (cheapest)

Haptic pulse + short stop flash when the game bounces an attack off a wall: the bounce flag at 0x1cb87ff
is already read by the stamina hits hook (20:670-676). No new RE. 0.5 day. Does not stop the weapon outside
attacks.

### 4B. World probe + IK clamp (solid weapon)

- **What.** Each frame cast the weapon segment (grip -> tip) against the map; when it hits, clamp the IK
  hand target so the blade stops at the surface (sandbox `physics_weapon.cpp` does the solver: ghost
  controller, weapon stays at the contact), with haptics.
- **Must reverse-engineer.** [U] a callable world ray/sphere cast: candidates are the camera collision
  0x18414f0 (08:137, it pushes the follow camera out of walls, so it queries the map) and the Havok
  "Character Hit Raycast" task (13:265). Signature, thread safety (step hook = game thread) and the
  collision filter (map only, not characters) are unknown.
- **Risks.** Hand/controller mismatch (comfort: show a ghost controller); the game's own hit test is
  unchanged, so a clamped blade still hits through the wall during an attack unless 4C is added; cost of a
  cast per frame per hand.
- **Effort.** 4-7 days.

### 4C. Attack stop on contact

Use the 4B contact to end the attack early (the game's own bounce path 0x1c61420 -> 0x1c614b0, 20:671) so
the swing recoils as in vanilla. 3-5 days on top of 4B; risk of stuck animation states.

## 5. Recommended roadmap

| Phase | Deliverable | Days |
|---|---|---|
| **1** | "Held item" state from the wheel (bridge): mouth -> use (heals/cures/body buffs), touch to blade -> use (weapon buffs), throw gesture -> use; thrown bullets redirected to the hand (origin + direction) via the existing 0x1df6ca0 hook with a thrown allow-list; log lines; held item drawn as its icon/wire mesh at the hand; 4A wall-bounce haptics | 2.5-3 |
| 1.5 (log only, 0.5 day, can run inside phase 1) | Discovery run: thrown bullets at 0x1df6ca0 (`IDS=all`), use-animation sub-ids from the TimeAct ring, release delay per `goodsUseAnim`, `speffect:2200` via the debug file | 0.5 |
| **2** | Throw speed from hand velocity (velocity rewrite next step, then exact post-hook); item-use animations x1.5-2 via chr dt; extracted item meshes at the hand; per-item haptics | 3-4 |
| **3** | Instant mode (option): consume RE; SpEffect apply for buffs/heals; direct bullet spawn at release (1B) with a vanilla-length cooldown | 6-9 |
| **4** (optional) | Solid weapons: world cast RE, IK clamp with ghost controller, then optional attack stop | 5-12 |

## 6. Open questions for the user

1. Balance: keep the game's animations (faster) or go for instant (no animation) use? Instant heals change the game a lot.
2. Throw input: grip-and-throw only, or also keep a "throw along the view" button for players who cannot throw well?
3. Buff gesture: full rub (50 % of the blade) or a simple touch of item to blade?
4. Which hand throws: right only (the weapon hand), or either hand (the left hand holds the gun)?
5. Is a held-item icon/wire mesh good enough for phase 1, or are extracted meshes needed from the start?
6. Weapon-world collision: wanted at all (comfort cost of a ghost hand), or haptics only?
7. Co-op: must instant effects be visible to peers (needs the sync check), or is solo fine first?

## 7. Decisions (the user's answers) and what phase 1 implements

1. Keep the game's use animations (sped up later, phase 2); not instant.
2. Throw: a real throw only (grab, throw with the arm); no "throw along the view" button.
3. Weapon buff: a simple touch of the item to the blade, not a rub.
4. (no answer) Right hand only throws, as recommended.
5. The held item is drawn simply (a coloured disc and a hint at the hand); no extracted meshes yet.
6. Weapon vs world: haptics only, no solid weapons.
7. Single player first; co-op ignored.
Extra request: the weapon hand vibrates when the in-game weapon hits an enemy or an attack bounces off a wall or object.

Phase 1 as built (branch `physical-items`):

- `gpu/xr_bridge/vr_held_items.h` (pure logic, tested in `vr_game_ui_test.cpp`): `Classify` (which goods are
  drinks, weapon buffs, throwables, and which throwable bullet is in the redirect list), `HeldItem` (state machine
  Idle / Pending / Held; mouth sphere dwell, blade-segment touch dwell, release-velocity throw, cancel), `CollisionPulse`.
- `vr_game_ui.cpp`: a wheel/dashboard/body-slot pick of a physical item goes into a hand (`held_items`, default true)
  instead of the use action; the gesture sends the same `USE_QUICK` the dashboard did (the host's executor). A thrown item
  first sends `BBXR_UI_THROW_ORIGIN` / `_DIR` (head-frame mm and unit vector). Selecting the item IS the grip (no squeeze; `held.pickup_grace_s` is
  unused); a fresh trigger press, opening a menu, the game's own menu or lost tracking puts the item back.
  While an item is in a hand (and `held.lockout_s` after) the filter hides R1/R2/d-pad right (and L1 for the left hand)
  from the game (`InputFilter::SetHoldMask`), so the throw is not an attack and the grip tap does not swap weapons.
- Hands: drinks and throws the right hand (a drink grabbed from a body slot stays in the grabbing hand); a weapon paper in
  the left hand (it touches the right weapon's blade), Bone Marrow Ash in the right hand (the left weapon, the gun).
- Blade segment: `GameHitboxes::BladeSegment` = the weapon bone origin to the farthest hit-shape point of the weapon's
  hitbox data (the first `blade_start` share is the handle); without hitbox data the bone with `held.blade_fallback_m`.
- Host: `src/runtime_vr_ui.c` forwards the throw actions to `vr_gun_throw_origin/_dir` (`src/runtime_vr_gun.c`), which turn them
  into the world with the latest first-person camera and arm the launch hook (0x1df6ca0, "VR Gun Aim (bullet spawn)") for 4 s.
  The hook's throw mode (`vg_throw_launch`, `src/vr_gun_math.h`) moves the launch origin to the hand (unless it is more than
  `BB_VR_THROW_SHIFT_M` = 2 m from the game's muzzle) and turns the launch Z axis and aim point onto the release direction.
  Only ids in `BBXR_THROW_BULLET_IDS` = 100,105,110,114,120,121,150,160,170,180,300,350,410,460,470 (the bullet of every goods the
  held-item code classifies as Throw; NOT the cures 200/230/320 nor the impact children). The rope molotov (110) was left to the game at
  first and flew backwards (the game aims along the character's facing), so it is aimed too. A throw with several bullets (the rope
  molotov's chain 110/111/112, the gloves' fan) redirects them together: later bullets with the first's id or in
  `BBXR_THROW_FOLLOW_IDS` (111,112,115,116; `BB_VR_THROW_FOLLOW_IDS`) within 1 s get the first bullet's turn (`vg_throw_group_*`), so their
  spread is kept; the origin shifts only for a bullet born within 0.5 m of the first's muzzle.
  Every player projectile id that reaches the hook is logged (first sight, then rate limited). A throwable that never reaches
  the hook is still thrown with the game's own direction.
- Haptics: `collision_haptics` (`on`, `hit`, `hit_strong`, `wall`, `object`, `strength_ref`, `scale_by_strength`) on
  `BBXR_EV_SWING_HIT` (stamina hook; contact 0 character / 1 wall / 2 object, strength = the stamina the attack paid).
- Config: the "game" object of `ui_layouts.json`: `held_items`, `held{...}`, `collision_haptics{...}` (missing keys keep the defaults).
- Not done (phase 2): throw speed from the hand velocity, faster item-use animations, item meshes, per-item haptics.
- Gated on the headset: the head-frame to world conversion of the hand position (a wrong assumption keeps the game's origin
  and logs it), that thrown goods reach 0x1df6ca0 at all, the launch delay of each use animation, and the blade segment per weapon.

Phase 2, speed from the hand (option ii of 1A; `BB_VR_THROW_SPEED=1` default, **[L] unverified live**):

- The headset's `BBXR_UI_USE_QUICK` of a throw carries the hand's speed relative to the head in `c` (cm/s). `src/runtime_vr_ui.c`
  passes it to `vr_gun_throw_speed`, which stores it in the armed throw (the loop handles `THROW_DIR` before the use).
- `thr_redirect` (the launch hook, now given `rdi` = BulletIns) stashes `{bullet, release direction, factor}` with
  `factor = vg_throw_speed_factor(speed, BB_VR_THROW_SPEED_REF=7, _MIN=0.3, _MAX=1.5)` (a factor within 2 % of 1 is skipped). The launch
  init writes the velocity vec4 at BulletIns+0x40 (copied to +0x50) after the hook returns (static RE, 0x1df77a7/0x1df77b3), so the
  rewrite waits for the next camera frame (`vr_gun_view`) or the next launch hook entry, within 300 ms: the vec4 must be finite,
  |v| 0.5..100 m/s and within 15 deg of the release direction, and writable; then xyz of +0x40 and +0x50 are scaled (w kept).
- Log lines: `VR throw: speed rewrite bullet N: hand x m/s (ref 7.0) -> factor f: |v| a -> b m/s ...`, then `one frame later |v| = X
  (expected about Y)`. If X equals the old speed the mover does not integrate +0x40 (the rewrite has no effect: drop it or find the
  field the mover reads, 27:39 / mover 0x1df4f00); if it equals Y it works. `dropped: <why>` when the velocity never validated.
