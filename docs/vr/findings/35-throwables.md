# 35 - Hand-thrown goods in VR: Bullet values, models, and the sandbox throw

Date: 2026-10-10. Track: VR body and interaction (docs 13, 16). Built and tested in the sandbox
(`gpu/xr_bridge/sandbox3d`, headless tests); nothing run against the game. The game-side hook (read the
VR hand velocity, spawn the Bullet from the hand) is not part of this step: section 5 says what it needs.

Code: `sandbox3d/throw_config.{h,cpp}` (the 15 rows, the `throwables` config), `throwables.{h,cpp}` (rack,
grab, hand velocity, Bullet flight, impacts, effects, procedural meshes), `scene.cpp` (wiring, drawing),
`scripts/vr_assets/throwables_extract.py` (+ `extract.py --throwables [--sfx-mesh ID]`). Test:
`sandbox3d/throwables_test.cpp` (`bbport_xr_sandbox3d_throw_test.exe out/vr_assets`, 408 checks).

## 1. What a thrown good is in the game

`EquipParamGoods` (ids 1200-2130) -> `refCategory 1`, `refId` -> a `Bullet` row. The Bullet row is the whole
flight: `initVellocity`, `gravityInRange` (for the first `dist` metres of travel), `gravityOutRange`,
`accelInRange`, `maxVellocity`, `life`, `numShoot`, `shootAngle`, `isPenetrate`, the flight/hit SFX ids and
`HitBulletID` (the burst bullet that does the damage/status). The 15 hand-thrown goods and their rows (dump
values; `out/vr_assets/throwables/throwables.json` has every field):

| Goods | Item | init m/s | gravity in / out | accel | max | life s | dist m | other |
|---|---|---|---|---|---|---|---|---|
| 1200 | Molotov Cocktail | 15 | 8 / 12 | - | 50 | 4 | 6 | shatters, bullet 101 |
| 1201 | Delayed Molotov | 15 | 40 / 20 | - | 15 | 4 | 2 | penetrates, bullet 106 |
| 1210 | Poison Knife | 20 | 0 / 9.8 | 10 | 50 | 2 | 8 | attack SFX |
| 1220 | Oil Urn | 15 | 8 / 12 | - | 50 | 4 | 6 | bullet 151 |
| 1230 | Pebble | 20 | 3 / 16 | - | 20 | 4 | 10 | |
| 1240 | Throwing Knife | 20 | 0 / 9.8 | 10 | 50 | 2 | 8 | attack SFX |
| 1250 / 1251 | (Delayed) Rope Molotov | 0 | 0 | - | 0 | 0.03 | 0 | back throw, no flight |
| 1260 | Pungent Blood Cocktail | 15 | - / 12 | - | 50 | 4 | 0 | penetrates, bullet 161 |
| 1270 | Numbing Mist | 15 | 8 / 12 | - | 50 | 4 | 6 | bullet 181 |
| 1410 | Shining Coins | 2 | 9.8 / 9.8 | - | 20 | 2.04 | 20 | penetrates (a lure) |
| 2070 | Tiny Tonitrus | 33 | 500 / 60 | - | 33 | 0.2 | 15 | penetrates, bullet 411 |
| 2080 | Executioner's Gloves | 4 | - | 40 | 4 | 3 | 20 | 3 bullets, shootAngle -20 |
| 2120 | Blacksky Eye | 30 | - | - | 30 | 0.54 | 16 | |
| 2130 | Accursed Brew | 15 | 3 / 7 | 10 | 20 | 0.5 | 7.6 | |

The game throws along the character's facing at exactly `initVellocity` (the button press, the use
animation 2/3/13/34 only times the release). Notes from the numbers:

- `maxVellocity` caps the speed, so a capped bullet turns downward instead of arcing: the Delayed Molotov
  (15 m/s, cap 15, gravity 40 then 20) falls in a short steep curve. The sandbox integrates exactly this.
- Tiny Tonitrus' `gravityInRange` 500 with a 0.2 s life is not a gravity (it would drop the bolt 10 m); the
  sandbox, like the inventory's "nearly straight", reads any in-range gravity of 100+ as 0.
- The delayed items' fuse is not in the Bullet row (it comes from the burst bullet / SFX); the sandbox uses
  1.5 s (`fuse_s`).
- Rope Molotovs have no flight (`life` 0.03 s, `dist` 0): the burst bullet spawns at the player.

## 2. Models: found for 14 of 15 (as flight-effect meshes)

`EquipParamGoods.modelId` is 0 for every thrown good and no `obj/`/`parts/` model is named after one, so the
only 3D data is in the flight SFX: `sfx/frpg_sfxbnd_commoneffects.ffxbnd.dcx` holds 84 `s#####.flver`
meshes and the `f0000<sfxId>.fxr` effects. FXR files list the ids of the meshes their model layers draw
as plain 32-bit words (no need to parse the whole format): a scan of each `.fxr` for words equal to a
bundle mesh id finds them. The generic ones (`s05000`, `s05240`, `s02504`, `s08000` ... in hundreds of
effects) are glow cards and rings; the item meshes are the ones used by one or two effects:

| Item | Mesh | How found |
|---|---|---|
| Molotov, Delayed, Rope, Delayed Rope | `s10010` (0.15 x 0.41 x 0.15 m, material 火炎壺 "fire pot") | in `f000020100` and 3 other effects only |
| Oil Urn | `s10014` (0.29 x 0.38 m, 油壺 "oil jar") | `f000020101` (and `f000020001`) only |
| Pebble | `s10012` (0.17 m, irregular stone) | `f000020103`, `f000020003`, the 250-257 throw effects |
| Pungent Blood Cocktail | `s10015` (material `ariannas_blood`, vial) | `f000020107` / `f000020007` only |
| Numbing Mist | `s10009` (material `hp`, vial) | `f000020108` / `f000020008` only |
| Shining Coins | `s10004` (0.08 m disc, material `coin`) | `f000020133`, `f00002025x` |
| Executioner's Gloves | `s11021` (0.13 x 0.17 m, material `body`) | `f000202008` only |
| Poison Knife, Throwing Knife | `s10020` (0.21 m, material `Knife_03`) | by name: the flight `.fxr` (20104/20105) list only generic meshes plus `s10016` (a 0.41 m blade, used by 389 effects) |
| Tiny Tonitrus | `s11010` (0.27 m, material `thundermagic`) | by name: only `f000200007` uses it; the stone itself is a guess |
| Accursed Brew | `s10023` (material `Medicine_of_lead`, small flask) | by name: `f000202051` is not in the bundle |
| Blacksky Eye | none (`f000202050` is not in the bundle) | procedural sphere |

The identification is by effect usage and material names, not by an explicit item -> mesh link: the knives
(either mesh), Tonitrus and the Brew are the weakest. All meshes are untextured in the sandbox (the
`S_*.mtd` materials are particle shaders); they are drawn flat with a tint. `extract.py --throwables`
writes `out/vr_assets/throwables/th_<goods>.json/.bin` (bbmodel, recentred on the bounding box so the grip
offset is about the centre) and `throwables.json` (the Bullet rows above plus `fxr_mesh_refs`);
`--sfx-mesh 10016` exports any other bundle mesh as `sfx_s10016`. `out/vr_assets` is git-ignored.
Procedural stand-ins (bottle, urn, vial, knife, stone, disc, sphere) are used where no mesh file exists.

## 3. The sandbox

Table with the 15 items (`throwables.rack_*`), grab with the grip (nearest within 16 cm of the hand,
haptics), carried with a per-item grip offset, thrown on grip release. Details and keys: sandbox3d
README. The throw mapping:

1. Hand velocity: samples of the held item's point over the last `smooth_ms` (80 ms); velocity =
   `lerp(mean over the window, fastest consecutive step, peak_weight 0.5)`. The point is the item (grip +
   offset), so a wrist flick counts.
2. Direction = that velocity; speed = `initVellocity x clamp(|v| / 7 m/s, 0.3, 1.5)`, capped at
   `maxVellocity`. A hard throw (7 m/s) is exactly the game's throw. Under 1.2 m/s the item is only
   dropped with the hand's velocity (normal gravity, lands on the table or floor, can be picked up again).
3. Flight: the Bullet rules of section 1 at <= 4 ms / 2 cm steps against the floor, the table, the post, the
   wall and the enemy's hurtboxes (a sphere of `radius_m`). Penetrating bullets pass through each enemy
   part once (logged) and stop at the world. `num_shoot` bullets fan by `shootAngle` about the vertical.
4. Back throws drop from the hand and burst where they land; delayed items rest `fuse_s` and burst.
5. Impact: a wireframe marker per type for the effect's lifetime, the log line, and for knives, pebbles,
   coins and gloves the item stays stuck for 3 s.
6. `mode: "game"`: the trigger of the holding hand throws along the view (+5 deg) at the exact
   `initVellocity` (to compare the feel with the hand throw); the grip alone drops.

Measured in the tests (flat throw at 7 m/s from 1.3 m, against an independent 0.2 ms integration of the
same rules): Pebble 15.0 m, Molotov 8.3 m, Oil Urn 8.2 m, Pungent Blood 6.9 m, Numbing Mist 8.3 m, Coins
1.0 m, Throwing/Poison Knife 20.1 m (flat for 8 m, then falls), Accursed Brew 8.8 m (expires at 0.5 s),
Delayed Molotov 3.9 m (against the Molotov's 8.3 m), Tiny Tonitrus ends at 0.2 s / 6.6 m, the Gloves fan
at -20/0/+20 degrees at 4 m/s for 12 m. All within 3 % + 0.15 m of the reference.

## 4. Known gaps

- Hit effects are wireframe markers (rings, flames, bolts), not the game's particles; no damage, no status.
- Item meshes untextured; knife/Tonitrus/Brew/Gloves meshes are best guesses (section 2); the held
  orientation of the knives (tip direction) is a guess, tunable with `items.<name>.grip.rotation_deg`.
- The Delayed Molotov's fuse (1.5 s) and the rope molotov's drop are inferred, as is the rest/stick time.
- The bullet is a sphere vs the sandbox's colliders; the game's Bullet hit radius (`hitRadius`) is unused.
- Rotation of the hand (angular velocity) is not added to the release velocity except through the item's
  offset from the grip; throws are always along the hand's velocity, not the aim.

## 5. Mapping it back to the game (not done here)

The sandbox's inputs and outputs are the game's own, so the hook is small:

- Input: when a goods item with `refCategory 1` is used in VR first person, instead of starting the
  throw animation's bullet along the character's facing, read the right controller (the grip pose and its
  velocity over ~80 ms, `HandVelocity::Estimate` is plain C++ and header-friendly) and the VR grip state.
- Output: spawn the item's Bullet (`refId`) from the hand position with `direction = v/|v|` and the speed
  `initVellocity x clamp(|v|/7, 0.3, 1.5)` (the engine applies gravity, `dist`, `maxVellocity`, `life`,
  `numShoot`/`shootAngle` itself), or keep the game's bullet and only replace its origin and direction.
  Rope Molotovs and the other no-flight rows keep the game behaviour.
- The hold phase (item in the hand) needs the goods' model: the engine has none, so draw the
  `th_<goods>` meshes (extracted at install time from the user's own dump) at the hand as the sandbox does,
  or skip the visual and keep the controller's own model.
- Re-check the numbers there: the sandbox values were read from the dump, not observed in flight.
