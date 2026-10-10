# 17: VR item menu: catalog of Bloodborne items and effects, and menu design research

Date: 2026-10-09. Inputs: the user's dump `C:/games/CUSA03173` (read only, never written).
Data written to `out/vr_assets/items/` (git-ignored): `items.json`, `effects.json`, `weapons.json`.
Scripts (scratch, not in repo): `scratchpad/dump_all.py`, run with `python -I`.

## Part A: catalog from the game files

### Sources and method
- Params: `param/gameparam/gameparam.parambnd.dcx` (EquipParamGoods, SpEffectParam, Bullet, Magic, BehaviorParam, EquipParamWeapon), layouts from `paramdef/paramdef.paramdefbnd.dcx`, read with `scripts/vr_assets/souls.py`.
- Names: `msg/engus/item.msgbnd.dcx`. FMG 10 = item names (goods), FMG 11 = weapon names.
- Every param layout matched its paramdef row size (no mismatches). Nothing was guessed about field meanings; enum values are kept raw.
- The FMG names are the source of truth for names. Japanese `row_name` text is kept in the JSON, but it is not translated.

### Counts
| Set | Count | Notes |
|---|---|---|
| EquipParamGoods rows | 295 | 251 have a real FMG name; 29 are `*` placeholders and 15 have no name, so 44 are excluded |
| Named goods, by category (rule based, see below) | 251 | no use effect 129, buff/applied effect 89, bullet tool/cure 15, thrown/fired 11, restorative 6, gun ammo 1 |
| EquipParamWeapon rows | 1090 | 1005 named (system dummies and `*` removed); this includes every `+N` upgrade row |
| SpEffectParam rows referenced | 121 | from goods, weapon residents, and Bullet spEffectIds of goods' bullets (overlapping; the file lists each row once with its references) |

Category rules (from fields, not names):
- `bullets (gun ammo)`: `useBulletMaxNum > 0`.
- `restorative (HP vial)`: goods refCategory 2 (SpEffect) whose SpEffect has `transfusionHpRate`, `changeHpPoint` or `changeHpRate` set.
- `buff / applied effect (SpEffect)`: other refCategory 2. This bucket is coarse: it also holds summon bells and multiplayer/system items.
- `thrown / fired (Bullet param, damage)`: refCategory 1 with a Bullet that has `atkId_Bullet > 0` and `dist > 0`.
- `Bullet-driven tool or cure`: refCategory 1, other Bullets (cures, music box, bells, area heal).
- `no use effect`: refCategory 0 (key items, gestures, materials, etc.).

Field meaning: goods `refCategory` 1 points into `Bullet`, 2 points into `SpEffectParam`. `refId` is the row id in that table. `magicId` and `behaviorId` are unused by named goods.

### 10 examples per category
Restorative (HP vial), all 6:
- 1000 Blood Vial: SpEffect 3010, transfusion HP -40%, max 20
- 703 Iosefka's Blood Vial: SpEffect 3020, -70%
- 706 Blood of Adeline: SpEffect 3040, -50%, 1 s
- 701 Blood of Arianna, 702 Blood of Adella, 705 Queenly Blood: -25% (SpEffect 3030/3060), 1 s

Gun ammo (1 row): 900 Quicksilver Bullets, max 20.

Thrown / fired (Bullet param, damage) (10 of 11):
- 1200 Molotov Cocktail: Bullet 100, atk 16, range 6
- 1201 Delayed Molotov: Bullet 105
- 1210 Poison Knife: Bullet 121, atk 1051, range 8
- 1220 Oil Urn: Bullet 150
- 1230 Pebble: Bullet 170, atk 1000, range 10
- 1240 Throwing Knife: Bullet 120, atk 1050
- 2000 Augur of Ebrietas: Bullet 370 (tentacle), atk 12010
- 2070 Tiny Tonitrus, 2080 Executioner's Gloves, 2120 Blacksky Eye, 2130 Accursed Brew

Bullet-driven tool or cure (10 of 15):
- 700 Tiny Music Box: Bullet 400, SpEffect 4640, 1 s
- 1100 Antidote: Bullet 200 (poison cure), shooter SpEffect 3200
- 1101 Sedative: Bullet 230 (madness cure)
- 1250 Rope Molotov Cocktail, 1251 Delayed Rope Molotov (back-throw variants)
- 1260 Pungent Blood Cocktail, 1270 Numbing Mist
- 1410 Shining Coins, 2010 A Call Beyond, 2020 Beast Roar
- 2030 Lead Elixir (Bullet 320), 2050 Choir Bell (area heal, SpEffect 3100/3110 "HP change +-400")

Buff / applied effect (SpEffect) (10 examples):
- 1300 Fire Paper: SpEffect 2200, fire attack +80, 60 s
- 1320 Bolt Paper: SpEffect 2220, lightning attack +80, 60 s
- 1310 Empty Phantasm Shell: SpEffect 2210, arcane attack +80, 60 s
- 1330 Bone Marrow Ash: SpEffect 2230, 60 s (no label-able field; see raw)
- 1110 Beast Blood Pellet: SpEffect 3240, 60 s (beast gauge permission)
- 1120 Blue Elixir: SpEffect 2014, 30 s (stealth-like, row name "stone hat")
- 704 Blood Dreg: SpEffect 2160 (sanity recovery)
- 100 Hunter's Mark: SpEffect 2100
- 200 Beckoning Bell: SpEffect 9000 (multiplayer request); 205 Small Resonant Bell (9005); 225 Sinister Resonant Bell (9025)
- 111 Silencing Blank: SpEffect 7 (session leave, system)

No use effect (10 examples): 120 Notebook, 130 Vileblood Register, 131 League Staff, 800 Point Forward, 801 Hunter's Salutation, 802 Conviction, 803 Joy, 804 Shake off Cape, 805 Church Bow (Male), 806 Curtsy. Most of the 800 to 822 block are gesture/emote names.

### Weapons (weapons.json, 1005 rows)
Attack base fields are read directly: `attackBasePhysics`, `attackBaseMagic`, `attackBaseFire`, `attackBaseThunder`, `attackBaseStamina`. Scaling uses `correctStrength/Agility/Magic/Faith` (grade inputs, kept raw). Examples:
- 2000000 Chikage: physical 92, stamina 40, correct STR 15 / AGI 45 / MAG 60 / FAI 33
- 5100000 Burial Blade: physical 80, arcane 30, stamina 90
- 7110100 Uncanny Saw Spear +1: physical 93
- 9010200 Uncanny Beast Claw +2: physical 89, resident SpEffects 240, 241, 6660
- 14100400 Evelyn +4: physical 92, correct MAG 93 only
- 26000300 Holy Moonlight Sword +3: physical 117, arcane 65, resident SpEffect 6080
- 29000400 Bloodletter +4: physical 126, correct MAG 86
- 1000000 Bare Fists (placeholder for an empty hand)

375 weapons reference at least one resident SpEffect. The residents are raw ids and their effects are in effects.json.

Not determinable from these tables:
- Trick or transform forms. The `dmypolyId_*Form*` fields are blade socket points (almost every weapon has a value), so they do not flag transformations. `wepmotionCategory` and `wepmotionOneHandId`/`BothHandId` are kept raw.
- Visceral attacks. No table was identified that lists them by weapon.
- Weapon category codes (`weaponCategory`, `wepmotionCategory`, `spAttribute`) are raw ids. Their meanings are not mapped, so they are not guessed.

### Effects (effects.json, 121 SpEffect rows)
Each row has `summary` (labels only for the known fields: attack +/x, defense, heal, duration, poison buildup, resistances), `duration_s` (`effectEndurance`), and `nondefault` (every field that differs from the column mode, raw). Known findings:
- Paper buffs: `fireAttackPower` 80 (2200), `thunderAttackPower` 80 (2220), `magicAttackPower` 80 (2210), 60 s each.
- Vials: `transfusionHpRate` -40/-50/-70/-25 (negative = heal).
- Poison: SpEffect 2120 "HP poison pot (weak)", referenced by the Poison Knife bullet, gives poison buildup +70, 40 s, and HP change x0.1.

Not found: frenzy, rapid poison and slow poison as named effects. Only the Poison Knife poison buildup was identified. Frenzy and rapid poison are open.

### Open for Part A
- Blood Bullets (901) is not flagged as ammo because `useBulletMaxNum` is 0. Blood Bullets, Quicksilver Bullets (1020) and other ammo need checking.
- Item-to-effect for the Tiny Music Box and other tools comes from Bullet fields, not from a named mechanism.
- Which goods are actually equippable as quick items in-game is not in these tables (the quick slots are runtime state).

## Part B: VR inventory and quick-menu design research

Search results were thin for most games. Where no source was found, the entry says so and nothing is inferred.

Dungeons of Eternity: no source found on its inventory. The closest hit was a dev blog (georgonzola), which is a different project. Its radial menu is opened with the thumbstick and aimed by thumbstick direction, with a dwell progress bar to select. Source: https://georgonzola.itch.io/vr-dev-blog/devlog/472891/060123-radial-menu-progress (not DoE; used only as a radial-select pattern).

Half-Life: Alyx: wrist pockets (two semi-hidden slots, syringes), an over-the-shoulder ammo pocket on the off hand, weapon-hand quick select, a gravity glove for pulling items at range. Not a menu. Sources: https://www.patreon.com/posts/35933257 (fan FakeVR mod, not native controls; treat with care), https://steamcommunity.com/app/546560/discussions/0/1861616237339477679. Placement: hand-attached and body-attached.

Bonelab: the radial menu opens with B on Quest, per a Gaming Trend review (https://gamingtrend.com/reviews/bonelab-review-the-future-isnt-here-yet/). Body slots and holsters were not confirmed. The WristHub mod (https://thunderstore.io/c/bonelab/p/MrAssBurgers/WristHub/v/2.0.3/) mentions a resizable native radial menu.

Skyrim VR: favorites menu opens from a right-controller button but pauses the game. Mods: SpellWheelVR (modeled on Blade and Sorcery's spell wheel; shows favorites only; https://fkmods.freeflarum.com/d/80) and Nexus 26468 (button config for Touch). Source: https://steamcommunity.com/app/611670/discussions/0/1696043806571518806, https://nexusmods.com/skyrimspecialedition/mods/26468.

Elden Ring VR mod (Luke Ross): the quick menu pauses the whole game. Inventory placement was not confirmed by the sources. Sources: https://uploadvr.com/elden-ring-vr-mod-gameplay-hand-on/, https://80.lv/articles/elden-ring-vr-mod-gameplay-revealed.

Titanfall 2 VR mod (CircuitLord): menus are said to be fully implemented for VR, but the layout was not described. Source: https://www.uploadvr.com/titanfall-2s-vr-mod-is-stellar-and-its-installer-is-the-best-thing-about-it/ and https://aftermath.site/titanfall-2-vr-mod-circuitlord/.

Asgard's Wrath 2: a six-slot belt that opens while holding the left stick (per a review). The original game had a 2D depository, a quick select wheel, and holsters. Sources: https://gamecritics.com/elijah-beahm/asgards-wrath-2-review/, https://roadtovr.com/asgards-wrath-behind-the-scenes-sanzaru-games-oculus-studios/2/.

Blade and Sorcery: no direct source found. The spell wheel idea is confirmed only through the Skyrim SpellWheelVR page.

General: https://uxdesign.cc/how-to-design-virtual-reality-menus-that-do-not-suck-9c06eb1df865 and the comparison of radial and panel menus at https://repositorio.inesctec.pt/items/c68edb10-6b87-4e54-b376-f8eacada86eb/full.

### Controls we have (from xr_input.cpp header)
- Left stick click (L3) is a modifier and must be held. Left menu button is not available on Pimax, so it cannot open the menu.
- Right thumbstick drives the camera. R3 is lock-on. Triggers, grips and face buttons are used for attacks and actions.
- The VR layer can only press PS4 pad buttons and the d-pad. The equipped quick items are not readable from memory yet (open).

### Recommended designs
1. Radial quick menu (primary). Hold L3 to open a radial with the quick slots (Blood Vial, Quicksilver Bullets, Molotov, Fire/Bolt Paper, Antidote, Pebble). Select by pointing the right hand's laser or by right stick direction, release to confirm. Pressing the right stick direction (or d-pad via the pad) sends the chosen slot. Show icon and count (from maxNum and the item's counter). This fits the L3 modifier already used for the chord.
2. Wrist/belt quick slots (secondary). Two glanceable slots on the left forearm (like Half-Life: Alyx wrist pockets) hold the two most used items: Blood Vial and one bullet/throwable. Use grip or a short press to use. No menu needed; the counts are shown on the forearm.
3. Full grid in front of you (fuller inventory). Open with a dedicated press (not L3 alone), show a head-locked panel at about 1.5 m with a grid by category (vials, ammo, throwables, paper, tools). Select with the laser; close on the same button. Whether the game pauses while its own menus are open is not verified here; check before relying on it.

Open questions:
- Can the active quick slots be read from memory (equipped items and counts)? Needed for icons and counts.
- The game's own item-use path for quick items: does a pad press use the current quick slot, or must the item be selected first in the game menu?
- Does Bloodborne pause in its own menus, and can the VR panel reuse that pause?

> Review note: holding L3 alone is already recenter (1 s hold, `xr_input_logic.h`), so the radial
> needs another trigger, e.g. a grip/reach gesture. Doc 18 recommends body quick slots first
> (sketch A); the sandbox menu kit prototypes radial, grid, wrist and body-slot presets.
