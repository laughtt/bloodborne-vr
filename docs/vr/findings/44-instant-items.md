# 44 - Instant items: no use animation, our own throw, the game's effect

Date: 2026-10-13. Branch `instant-items` (from `vr-next`). Static RE of `out/eboot.elf` (the relocated image and the
capstone helpers of 08, `scratchpad/re/bb.py`, `xrefs.pkl`, `ptrs.pkl`) and the game's own paramdefs read through
`scripts/vr_assets/souls.py` (the dump, read only). **The game was not run.** Tags as in 08/27/39: **[C]** confirmed in
the disassembly or the params, **[L]** likely, **[S]** speculation, **[U]** unknown.

The user changed decision 1 of 39 section 7 ("keep the game's animations"): items are now used with **no use
animation**. Throws are 39's option 1C (our own projectile, the game's burst where it lands), heals and buffs are 3B
(the effect applied directly, one item consumed). Our hands do the action; the game's body does nothing.

## 1. Phase A: what the game does (RE results)

### 1.1 The goods use chain (the key finding) [C]

```
use animation's TAE event (dispatcher 0x1e249d0)
  0x1e25b0c: PlayerIns vtable 0x408 (goods = ChrIns+0x1c0 & 0x0fffffff, dummy poly = event param)   [C call, L meaning]
  0x1e25706: the same slot with goods 1000 (the vial event; precheck 0x197f200 first)                [C]
debug menu handler 0x1ccef90 (item 0x204):
  0x1ccf180: PlayerIns vtable 0x408 (goods = [PlayerIns+0x394] via 0x231e4f0, dummy poly -1)          [C call, L "debug menu"]

PlayerIns::UseGoods 0x1cf9720 (PlayerIns vtable 0x578F310 + 0x408 = 0x578F718; also 0x578FE88)       [C]
  team-type table check ([chr+0x78]); PGD = vf+0x1c8
  ChrIns::UseGoods 0x1cc9160(chr, goods, dummy)   (ChrIns vtable slot 0x408 at 0x578E6D8)             [C]
     EquipParamGoods row (0x231e3d0); bullet/SpEffect id = refId + 0x1cc9390(chr, row+0x41)
     builds the behavior request BR (rbp-0xc0), table in 1.2, then 0x1cb61c0(BR, chr+0x178, chr)
  0x1cb61c0 dispatches on the row types:
     Magic (0x23210e0, +0x23), BehaviorParam (0x23133c0, +0x09), Goods (0x231e3d0, +0x3b refCategory):
     refCategory 1 -> 0x1cb6680 (the bullet request, 1.2)                                             [C]
     refCategory 2 -> chr vtable 0x3F0(chr, SpEffect = refId + replace, chr, 0, 0, 0, [rsp] 0,
                       xmm0..3 = BR+0x60..+0x6c, xmm4 = const)  (0x1cb63cb)                            [C]
     then BR+0x70 HP, +0x78 stamina, +0x7c Insight, +0x80 Quicksilver (0x1cf9f00), 0x398, 0x18e08d0    [C]
  back in 0x1cf9720, when vf+0x1b8 or vf+0x1c0 (the local player):                                    [C]
     idx = 0x18deff0(PGD+0x3d0, goods)                 the quick slots (6) holding it
     if idx < 0: idx = 0x18d9f60(PGD+0x328, &(0x40000000|goods))   the inventory hash
     if idx != -1 and !(word [[[chr+0x3b0]+0x20]+0x200] & 0x200)  (a debug "infinite items" flag):
        n = 0x18d0be0(PGD+0x1d0, idx, -1, 0, 0, 0)      THE CONSUME: the count change (14 callers, the add path too)
     PGD+0x458 = -1 (m_useRequestItemFromMenu: its reset owner, 26's open question), [MenuMan 0x5962878]+0x1c0 = 0,
     byte 0x596020a = (n <= 0) (ran out), bytes 0x5960209 = 1, 0x5980c50 = 1; goods 0x66 / 0x6f: PGD+0x680;
     PlayerIns+0x490 (the use state) = 0
```

So the whole effect of a use (SpEffect or bullet, the count, the menu flags) is one virtual call the animation only
*times*. Calling slot 0x408 with dummy poly -1 is exactly what the game's own debug menu does [C call site].

**Q2 answer: the consume routine** is `0x18d0be0(PGD+0x1d0, inventoryIndex, -1, 0, 0, 0)` inside UseGoods [C]; the
index comes from `0x18deff0` (quick slots) or `0x18d9f60` (inventory hash) [C]. The quick slot's reaction at 0 is
the game's own, because the same code runs: the "ran out" byte 0x596020a and the two "used" bytes are set for the HUD
[C writes, L meaning]. PGD = vf+0x1c8 of PlayerIns; the host checks it equals `[[GameDataMan 0x593B130]+8]` before use.

### 1.2 Q1: the bullet request [C]

Two structures. **BR** (the behavior request, built by RequestBehavior 0x1cc7530 / UseGoods 0x1cc9160 /
the magic path 0x1e402b0) is what `0x1cb6680(BR, chr+0x178, chr)` reads; **REQ** (0x90 bytes at rbp-0xc0 of 0x1cb6680)
is what `0x1cb7250(chr+0x178, REQ)` -> `0x1e03620(SprjBulletManager, REQ)` reads. 0x1e03620 has exactly one caller
(0x1cb7314); 0x1cb7250 one (0x1cb6abe) [C]. Building the BR and calling **0x1cb6680** is the game's own goods path.

| BR | UseGoods writes | REQ (from 0x1cb6680) | read by |
|---|---|---|---|
| +0x00 s32 | -1 | +0x08 | 0x1dee4b0: Magic row (0x23210e0) when +0x10 = -1; 0x1cb7250 compares the emit slot |
| +0x04 s32 | **goods id** | +0x10 | 0x1dee4b0 / 0x1dee5f0: EquipParamGoods row: `spEffectCategory` (+0x3c), **`refVirtualWepId` (+0x5c) for the attack power**, `isApplySpecialEffect` |
| +0x08 s32 | -1 | +0x04 | BehaviorParam id (0x23133c0): the bullet id when REQ+0x0c = -1 (refType 1 rows) |
| +0x0c s32 | **bullet id** | +0x0c | BulletParam id (desc+4) |
| +0x10 s32 | -1 | +0x18 | the lock target handle (0x1df5be0); -1: kind-4 aim point (27) |
| +0x14 s32 | -1 | +0x1c | hand -> desc+0xa0; vf+0x388 weapon when no goods |
| +0x18 s32 | -1 | +0x20 | weapon slot -> desc+0xa8; vf+0x388 |
| +0x1c u8 | **1** | +0x2c bit 1 (and bit 4 = vf+0x1a8, the local player) | bit 1: the emit slot keeps no handle (0x1cb72d2/0x1cb7322) |
| +0x1d u8 | 0 | +0x2c bit 2 | desc+0x122 |
| +0x20 s32 | dummy poly | +0x14 | dummy id -> desc+8; **< 0: the BR's own matrix +0x30 is the launch matrix** (0x1cb681d `js`) |
| +0x24 u8 | 0 | +0x2c bit 0 | desc+0x120 |
| +0x30..+0x5f | identity | +0x30..+0x5f | the launch 3x4 (rows X[i], Y[i], Z[i], pos[i]; flies along Z); EmittePosType 0 uses it (0x1df0490 -> 0x1df2440 reads emitter+0x560 = desc+0x3b0) |
| (chr) | | +0x60..+0x8f | the character matrix (transposed from [[chr+0x48]+0x20]+0x60, or [chr+0x58]+0x90) |
| +0x60..+0x6c f32 | vf+0x298/0x2a0/0x2a8/0x2b0(chr, refVirtualWepId) | +0x24/+0x28 = BR+0x68/+0x6c | desc+0xb4/+0xb8: the damage scaling |
| +0x70..+0x80 | 0, 0, 0, -consumeHeroPoint, bulletConsumeNum | - | 0x1cb61c0 only (not called by our path) |
| REQ +0x00 | | [chr+8] | the owner handle (0x1946e70 -> desc+0x0c) |

Required: owner (from chr), bullet id, goods id (or a weapon: the attack power), dummy -1 + matrix, the +0x1c flag.
0x1cb7250 refuses (returns 0) while the emit slot `[chr+0x180]` holds a weapon bullet's handle [C]; the host retries.
REQ +0x2c bit 4 makes 0x1e03620 record the request in mgr+0x78 (0xd0-byte entries) [C], most likely the online
send list [L]; the feature is off online anyway.

### 1.3 Q3: a world ray cast [C signature, L filter]

`0x18414f0` (08's "camera collision") is a **debug draw** (it starts with `cmp byte [r12+0x340],0` and calls the
debug primitive emitters 0x135d810/0x135d870 that ChrIns::DbgDraw 0x1cd41c0 uses) [C]: not a query.

The hknp world cast is `0xf212a0` (timer string "TtWorldCastRay", 11 callers) [C]. Its cleanest wrapper is
**`0x1bfe5a0`**, a virtual (vtable 0x578d168, with a twin 0x1bfe7d0 at 0x578d190) of the havok behavior **raycast
interface** (the foot IK's) [L]: `bool f(unused rdi, const vec4 *from, const vec4 *to, vec4 *hitPos, vec4 *hitNormal)`;
it never reads rdi, takes the world from `[[[0x593D700]+0x28]+8]`, filter info 0x2f (from `[[world+0x150]+0x5e8]`),
a closest-hit collector, writes position/normal only on a hit (aligned vec4: vmovaps) [C]. What layer 0x2f collides
with is **[U]** (made for the feet, so the ground is [L]; walls [L]; characters [S]). The host validates the pointer
chain first, trusts a hit only when it lies on the segment, and keeps two fallbacks: the **lock-on candidates' points**
as character spheres (LockTgtMan list, already read for the parry assist, 0.6 m), and a **floor plane** at the feet
(30 m below once the cast has proved it hits). The renderer-side fallback (a ray march in the bridge against each eye's
depth buffer) is not built: it would need the depth of the game's frame in the bridge and a reprojection to the world;
keep it for the case where layer 0x2f turns out not to hit walls.

### 1.4 Q4: the burst bullet alone [L]

Molotov: Bullet 100 (atk 16, flight sfx 20100, `HitBulletID` 101, `isInheritEffectToChild` 0, `sfxId_Hit` -1) [C]. The
burst **101** carries everything itself: atkId 1052, `hitRadius` 1.4 m, life 0.4 s, `sfxId_Bullet` 20200 (the fire
burst effect; FromSoft puts the sound in the effect [L]), `spEffectId0` 421 (burning), its own child 75 -> 76 [C].
Nothing comes from the parent (no inherited effect, no hit sfx), so 101 requested alone at a point with zero speed
(its `initVellocity` is 0) should give the full burst, with the damage scaled like the thrown one because the request
carries goods 1200 -> `refVirtualWepId` 12000 [L]. Same shape: oil urn 150 -> 151, numbing mist 180 -> 181, delayed
molotov 105 -> 106. Knives 120/121 and the pebble 170 have no burst (`HitBulletID` -1): the bullet itself hits.
**Gated on a game run** (section 5).

### 1.5 Q5: SpEffect 3010 alone vs drinking [C path, L amount]

SpEffect 3010: `transfusionHpRate` -40 (40 % of max HP), instant, every target flag [C]. Drinking reaches it through
UseGoods -> 0x1cb61c0 -> slot 0x3F0 with the scaling floats of `refVirtualWepId` 16000 [C]. The instant path calls
**the same UseGoods**, so the call is byte for byte the game's: same heal [C path]. Applying 3010 with slot 0x3F0 and
1.0 floats (the Co-op Quick-search way, 39 phase 0) might differ if the floats scale the heal [U]; not used.
The rally (regain) is a damage-time mechanic, unrelated to the vial [L].
`useHpCureMaxNum` is a goods **flag** (1 on the vial 1000, 0 on Iosefka's 703) [C]: it marks the items that count
toward the 20-vial carry limit, not a use limit [L]. What the menu path checks before a use is the predicate
**`0x197f200(goods, chr, [chr+0x1c8], [chr+0x78], vf+0x388(-1), vf+0x388(-2) or -1, MenuMan+0x1c0 > 0)`** (8 callers: the
quick-item key 0x1a5d241, the item menu 0x1ae50b4, ChrIns::CanUseCurrentItem 0x1cc3840 ...) [C]; the host calls it
too, plus a held count > 0 (the TAE loop checks the count the same way, 0x1e25a41).

## 2. Design

| Item | Path | Game code |
|---|---|---|
| refCategory 2 (vials, pellets, papers, Shell, coldblood...) consumed or costing Quicksilver | **USE** | PlayerIns vf+0x408(player, goods, -1): effect, count, flags, Quicksilver all the game's |
| refCategory 1 whose bullet does not fly and has `spEffectIDForShooter` (Antidote 200, Sedative 230, Lead Elixir 320) | **CURE** | BR (as UseGoods) with the matrix at the chest -> 0x1cb6680, then the consume copy |
| refCategory 1 with a flying bullet (`initVellocity` > 0) and a throw release | **THROW** | consume copy at the release; our flight; at the end BR(HitBulletID, the point) -> 0x1cb6680 |
| ids < 700 (marks, bells, key items), Insight costs, Quicksilver tools of category 1, rope molotovs, non-consumed, a throwable with no release | the game's path | the old executor (quick slot + square, the animation) |

Pure logic: `src/vr_instant_items.h` (classification, the BulletParam flight with substeps of 1/120 s, the segment
tests, the impact plan, the launch matrix, the BR builder, the cooldown table, the setting parser).
Host: `src/runtime_vr_items.c`. Before a use: a living player, PlayerIns+0x490 = 0 (no use running), no menu page,
the PGD check, the cooldown, `0x197f200`, count > 0. The consume copy writes exactly what UseGoods writes (1.1).
The throw: speed `initVellocity x clamp(hand / 7, 0.3, 1.5)` capped at `maxVellocity` (the sandbox mapping; the aimed
throw sends no hand speed: `initVellocity`); the burst is backed off 5 cm against the travel; a bullet with no burst is
requested 0.5 m before the hit point (never behind the hand) so the game's own bullet hit test decides.
An expired flight requests nothing (`launchConditionType` 0: the game's child only spawns on a hit).
The headset: the bridge remembers each throw release; `BBXR_EV_INSTANT_THROW` (goods, speed) starts drawing the
item's mesh (or a disc) along `ComputeArc` at that speed, `BBXR_EV_INSTANT_LAND` ends it (headset space: the player's
own movement during the flight is not followed).

**Balance.** One instant use per cooldown as long as the vanilla animation (`vri_cooldown_s` by `goodsUseAnim`: vial /
Iosefka 1.3 s, throws 1.0 s, papers 1.6 s, drinks 1.3 s, other 1.2 s). These lengths are **[S] estimates**: the TAE
data is not on disk; `BB_VR_INSTANT_COOLDOWN_S` overrides. Still a balance change: no commitment (the player can move,
roll and attack while "drinking").

## 3. What is built

- `src/vr_instant_items.h`, `tests/test_vr_instant_items.c` (`vr-instant-items-test`, 61 checks, passing; the
  findings/35 reference throws: Molotov 8.4 m, pebble 15.2 m from a flat 7 m/s throw at 1.3 m, the Tonitrus no-gravity
  rule, life expiry, wall / character / floor ordering, the BR offsets).
- `src/runtime_vr_items.c`, `src/vr_items.h`: the setting, the pad-thread request, the game-thread step.
- `src/runtime_vr_ui.c`: with the setting on, THROW_ORIGIN/_DIR go to the items module, USE_QUICK / USE_ITEM are offered
  to it first; a use it hands back runs the executor as before (and re-arms the throw redirect of 39 with the same
  release). The game-thread step runs from `vu_hook_menu` (patch "VR FP Camera Heading (in-game step)"). **With the
  setting off nothing changes**: the old path (animation sped up, launch redirected) is untouched.
- `src/runtime_vr_gun.c`: `vr_gun_rel_to_world` (the release into the world with the latest camera); the camera is
  recorded while the setting is on.
- `gpu/xr_bridge/bbport_xr_bridge.h` (events 6/7), `vr_game_ui.{h,cpp}` (the flight drawing).

Setting: `BB_VR_INSTANT_ITEMS=1` or `instant_items=1` in the control file (`BB_VR_FP_CONTROL` / `BB_VR_ARMS_CONTROL`,
out/fp.txt, polled 4 Hz; the key wins). **Default OFF.** Forced off online (`BB_ONLINE=1` or `BB_ONLINE_ROLE`).
Other knobs: `BB_VR_INSTANT_RAY=0`, `BB_VR_INSTANT_COOLDOWN_S`, `BB_VR_INSTANT_CHR_R`, `BB_VR_THROW_SPEED_REF/_MIN/_MAX`.
Needs `BB_VR_UI=1` and first person (the camera for the release).

## 4. Gated on a game run (nothing here was run)

| What | Tag | Log line to look for |
|---|---|---|
| The setting reached the host | | `VR instant items: ON (BB_VR_INSTANT_ITEMS); ray cast on; cooldown per use animation` |
| UseGoods with dummy -1 heals / buffs and drops the count | L | `VR instant items: item 0x400003e8: used: goods 1000 (SpEffect 3010): UseGoods (slot 0x408, dummy -1) returned 1 in N ms; count 5 -> 4; cooldown 1.3 s` |
| The quick slot and HUD count follow at 0 | L | the HUD; then `... count 1 -> 0` |
| A cure at the chest cures | L | `... cure bullet 200 (shooter SpEffect 3200) at the chest (...); consume done (0x18d0be0 -> 2); count 3 -> 2` |
| The release in the world | L | `... thrown, bullet 100 (burst 101) at 15.0 m/s (hand 0.0 m/s, initVellocity 15.0) from (x y z) along (dx dy dz); count 10 -> 9` |
| The ray cast hits the map (layer 0x2f) | U | `VR instant items: the game's ray cast 0x1bfe5a0 hit the map at (...), normal (...)`; bad hits: `... is not on the segment ...: ignored` |
| The flight's end | | `VR instant items: goods 1200 bullet 100 hit the map after 0.62 s at (...), 7.9 m from the hand` |
| The burst request is accepted | L | `VR instant items: goods 1200: bullet 101 requested at (...) along (...)`; a busy emit slot: `... (after the emit slot freed)` or `... refused bullet 101 30 times: given up` |
| The burst looks, sounds and burns like a thrown one; damage numbers match | L | (visual / enemy HP) |
| A handed-back item | | `VR instant items: item 0x...: the game's own path: goods 1250: a bullet that neither flies nor cures (rope molotov): the game's path` then `VR UI: action 1 (...) the game's own path (instant items)` |
| The cooldowns feel like the animations | S | `... refused: goods N: cooldown, 0.40 s left` |

Headset test plan (single player, `BB_VR_UI=1`, first person, the usual hooks, `BB_DEBUG_GIVE_FILE` with vials,
antidotes, molotovs, oil urns, knives, pebbles, fire paper, a rope molotov):

1. `instant_items=1` in out/fp.txt mid-game; the start line flips to ON. Remove the key: off again, a vial plays its
   animation (the old path).
2. Vial at 50 % HP: heals at once, no stab animation, count -1, HUD count follows; again within 1.3 s: refused line.
   Use the last vial: the quick slot empties like vanilla.
3. Antidote while poisoned: the poison bar clears; count -1.
4. Fire paper (blade touch): the blade glows; count -1.
5. Molotov at a wall 5 m away, at the floor, at an enemy (locked and not): our item flies, the fire burst appears where
   it lands, the enemy burns and takes about the vanilla damage (compare one vanilla throw with the setting off).
6. Knife and pebble at an enemy: the hit registers (the terminal launch).
7. Throw off a ledge / at a slope: the burst is where it lands, not mid air (shows whether the ray cast hits the map).
8. Rope molotov and a bell: the old path with the animation.
9. Fire a gun, then throw at once: watch for `refused ... times`.
10. 50 throws soak; a load screen during a flight (the flight is dropped); online: the line says forced off.

## 5. Risks

- **Crash risk of a bad request.** The BR is built field for field as UseGoods builds it, and the call is the game's
  own goods path (0x1cb6680), not the manager directly; an unreadable goods / bullet row stops before any call. The
  remaining unknowns are behavioural (layer 0x2f, the burst alone), not layout. A guest fault in the ray cast is the
  biggest crash risk (a wrong world chain during a load): the chain is checked first, `BB_VR_INSTANT_RAY=0` turns it off.
- **Item count desync.** USE is the game's own consume. CURE/THROW use a copy of it (same index lookups, same 0x18d0be0,
  same flags); the count is read before and after and logged. The consume happens at the release, so a burst refused 30
  times costs the item (logged).
- **Balance.** No commitment while using; cooldowns are estimates ([S]); hand-speed throws can be faster than vanilla
  (capped at `maxVellocity`, x1.5).
- **Look.** The flying item is drawn in headset space from the release; the player's own motion during the flight is
  not followed. The game's character plays no animation, so a mirror shows no drinking.
- **Online:** off by design (the request's bit 4 / mgr+0x78 send list and the peer's view are not checked).

## 6. Every item mapped (2026-10-13, after the first headset run)

Headset run `out/stereo_ab/run28all3/run5.log`: vials (UseGoods) and the Molotov (our flight, the ray cast hit the map,
burst 101 requested) worked; the rope molotov went the game's way ("a bullet that neither flies nor cures"); the Fire
Paper was refused by 0x197f200; Numbing Mist was put back; assign_use of 1250 failed. Every goods the held-item code
takes (`vr_held_items.h` Classify) is now mapped from its own rows (EquipParamGoods / Bullet of the dump; the test
`vr-instant-items-test` carries the same rows and checks this whole table):

| Goods | Rows | Before | After (no animation) |
|---|---|---|---|
| Vials 1000 / 703 / 706, Pellet 1110, Blue Elixir 1120, Madman's 1500, papers 1300 / 1320 / 1330 | cat 2, consumed | USE | USE (unchanged) |
| Empty Phantasm Shell 1310, Old Hunter Bone 2060 | cat 2, Quicksilver 3 / 4 | USE | USE (UseGoods pays the Quicksilver) |
| Hunter's Mark 100, Bold Hunter's Mark 1400 | cat 2 (SpEffect 2100 / 2101, the warp) | game's path (id < 700 / reusable) | **USE** |
| Antidote 1100, Sedative 1101, Lead Elixir 2030 | bullets 200 / 230 / 320 (spEffectIDForShooter) | CURE | CURE (unchanged) |
| Molotov 1200 | 100 -> burst 101 | THROW | THROW (unchanged) |
| Delayed Molotov 1201 | 105 -> 106 -> 107 (2 s) -> 108 | THROW (worked by the rules, not run) | THROW, requests 106 at the landing: the 2 s fuse of 107 is kept |
| Poison / Throwing Knife 1210 / 1240, Pebble 1230 | 121 / 120 / 170, no burst | THROW | THROW, the bullet itself 0.5 m before the hit |
| Oil Urn 1220, Pungent Blood 1260, Numbing Mist 1270, Shining Coins 1410 | 150 -> 151, 160 -> 161, 180 -> 181, 300 -> 301 | THROW | THROW (unchanged; mist was never thrown, see below) |
| **Rope Molotov 1250 / Delayed 1251** | spawners 110 / 114 -> flyers 111 / 115 -> 112 / 116 | game's path | **THROW**: the spawner is followed to its flyer (`vri_resolve_flight`), flown with the Molotov's flight (the game's 111 is a 1.7 m back-lob; the bridge already draws the Molotov's arc), burst 112 / delayed 116 requested at the landing |
| **Tiny Tonitrus 2070, Executioner's Gloves 2080, Blacksky Eye 2120, Accursed Brew 2130** | 410 / 350 (3 bullets) / 460 / 470, not consumed, Quicksilver 6 / 3 / 1 / 2 | game's path | **DIRECT**: Quicksilver paid by the game's 0x1cf9f00 (it takes nothing and returns 0 when goods 900 + 901 do not cover it) [C], then the tool's own bullet requested at the hand along the release; the game flies it (numShoot, homing, children) |
| **Choir Bell 2050, Shaman Bone Blade 2090** | 360 (heal area 2.8 m, Quicksilver 7) / 310 (attack 1002, 0.8 m, consumed) | game's path | **SPOT**: requested at the chest / 0.7 m in front of it along the view, after paying |
| Beckoning / Resonant / Sinister bells 200 / 201 / 205 / 206 / 225 / 226 | cat 2 or no ref | game's path | **game's path (kept)**: multiplayer; the room's bell code (free bells refund window, the auto-bell's direct use) is built around the game's own use, and the instant path is off online anyway |

Other fixes from the run:

- **Fire Paper refused by 0x197f200 [C cause, L effect].** The host gave the predicate the arguments of
  ChrIns::CanUseCurrentItem (`[chr+0x1c8]`, `[chr+0x78]`, vf+0x388(-1/-2), the menu flag). The square press of the old
  path goes through the quick-item key 0x1a5d241, which passes `[PGD+0x670]`, `[PGD+0xa4]`, the active right weapon id
  and the active left one (PGD+0x250 / +0x24c slots, handles at PGD+0x264, resolved by 0x1e89070; left -1 when
  `[PGD+0x248]` == 3) and 0. A paper (`isEnhance`) is checked against that weapon id (EquipParamWeapon +0x100 bit 15,
  0x197f3b0), so the different weapon argument is the likely reason. The host now passes exactly the key's arguments.
- **"Not done: assign_use goods:1250: the item is no longer there" [C].** `vr_game_ui.cpp` assign_use of a held-kind
  item meant to clear the "then use" flag (`c`) but cleared `b`, the item id: the host's equip hook compared the inventory
  entry with id 0 and refused. Now `c = 0` (and the fallback sets `c = 1`). Every wheel / dashboard pick of an item that
  was not yet in a quick slot was hit by it (the held item then said "the item is no longer in a quick slot").
- **Numbing Mist "put back (trigger)": not a mapping bug.** 1270 is a Throw item (bullet 180 in the redirect list,
  THROW on the host). It was picked by pointing at the wheel (trigger), both triggers came up (`trig_armed`), then a
  fresh trigger pull (either hand, >= 0.6) put it back: the designed cancel. Throwing is a grip release (aim) or a
  flick; no trigger is needed. If players keep pulling the trigger to throw, make the holding hand's trigger throw (or
  only the other hand's trigger cancel) in `HeldItem::Step`.
- CURE / SPOT / DIRECT check the emit slot (`[PlayerIns+0x180]` = -1) before paying, so a busy slot costs nothing.

New log lines: `... spot bullet 360 (atk 10100, ...) at the chest (...): request accepted; paid Quicksilver x7; ...`,
`... direct bullet 410 ... from the hand (...): request accepted; paid Quicksilver x6; ...`,
`... thrown, bullet 111 (the flyer of its spawner, the Molotov's flight) (burst 112) ...`,
`... refused: goods N: the bullet emit slot is busy (a weapon bullet, handle ...)`,
`... refused: goods N: not enough Quicksilver (k needed) or not in the inventory`.
Headset checks added: rope molotov burst at the landing (fire like the Molotov), delayed molotov keeps its fuse, each tool
(Tonitrus bolt, gloves' three spirits, Blacksky Eye, Brew) leaves the hand along the throw and Quicksilver drops by its
cost, Choir Bell heals around, Shaman Bone Blade hits an enemy in front, Hunter's Mark warps, Fire Paper buffs.
