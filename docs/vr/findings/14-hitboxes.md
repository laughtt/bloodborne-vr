# 14: Hit detection: attack hitboxes, hurtboxes and VR melee

Date: 2026-10-09. Inputs:
- The user's dump `C:/games/CUSA03173`, read-only. All 26,913 `.dcx` files in it are DFLT (zlib); there is no KRAK/Oodle.
- The extracted eboot and analysis scripts from 08.
- Read-only clones of DSAnimStudio, SoulsFormatsNEXT, SoulsAssetPipeline and Paramdex.
- Three web research passes.

Outputs:
- **`scripts/vr_assets/hitbox_extract.py`** (new, stdlib only). It reuses the FLVER/BND4/PARAM/FMG readers in `scripts/vr_assets/souls.py` and adds Havok, TAE and HKXPWV readers.
- Its output is **`out/vr_assets/hitbox/`** (gitignored): 46 weapons and 170 characters, generated in about 16 s.
- One-off analysis scripts are in the session scratchpad (`hit/tools/`), not in the repo.

Scope: hit detection only. Bones, IK, fingers and weapon recognition are in 13-hands-arms-weapons.md.

Legend: **[C]** confirmed here from game data or disassembly. **[L]** likely. **[S]** speculation. **[W]** community or web source, not re-verified here.
Addresses are PS4 VAs, the same space as `patches/Bloodborne.xml` (ELF vaddr + 0x400000, see 08 §1).

---

## 0. Summary

1. **The hit chain** is the same in DS3, Sekiro, Elden Ring and Bloodborne, and BB follows it exactly **[C]**:
   - **TAE event 1** (`InvokeAttackBehavior`: AttackType, AttackIndex, BehaviorJudgeID, DirectionType) is active from `start` to `end` seconds of the animation.
   - It selects a **BehaviorParam** row. Player id = `100,000,000 + EquipParamWeapon.behaviorVariationId × 1000 + judge`. Enemy id = `200,000,000 + NpcParam.behaviorVariationId × 1000 + judge`.
   - The row's refType is attack, bullet or SpEffect. For attacks, refId points to **AtkParam_Pc/_Npc**.
   - Each AtkParam row has up to **4 spheres or capsules**: radius, DmyPoly1, DmyPoly2 (−1 makes a sphere), plus hitType (tip/middle/root) and hitSourceType (weapon/body).
   - Each dummy-poly id encodes its model: `id / 1000` = 10–13 is right-weapon model slot 0–3, 20–23 is left-weapon slot 0–3, 0 is the default source. `id % 1000` is the dummy's reference id on that model.
   - The eboot computes the same two ids: player at 0x1cf9070, enemy at 0x1ce6730.
2. **Hurtboxes are the ragdoll rigid bodies** in `chr/cXXXX.chrbnd → cXXXX.HKX` **[C]**:
   - Havok 2014 "hknp": 4,803 capsules, 21 spheres and 177 convex boxes across 170 characters.
   - Each body is driven by an animation bone through the file's `hkaSkeletonMapper`. Most are capsules; the convex shapes are mostly 8-corner boxes.
   - Each body has a damage part group in `cXXXX.hkxpwv`, which selects `NpcParam.partsDamageRate1..8`.
   - That attacks test against these bodies is **[L]**: the DSAnimStudio TAE flag 39 label says "Disable Character Ragdoll Collision (Including Hurtboxes)", and the eboot has a ragdoll debug menu.
3. **Bloodborne is generous on the attack side.**
   - Player attack radii: median **0.40 m** (10th–90th percentile 0.15–0.80). Hurtbox capsule radii: median **0.094 m**.
   - Saw Cleaver R1 is one 0.40 m capsule over the folded blade (dummies 100↔110, 0.50 m long).
   - The transformed R1 is a 0.40 m capsule from the blade tip to the grip (dummies 100↔120, 1.09 m long; the tip is 1.21 m from the hand attach point).
   - The hit window opens **0.30–0.37 s** after the attack starts and lasts 0.1 s. A swing costs 20 stamina.
4. **Trick weapons are up to 4 model slots** (`WP_A_xxxx[_N].flver`).
   - Which slot shows in which form comes from EquipParamWeapon `dmypolyId_Slot{N}{Right|Left}{Hang|FormA|FormB}`.
   - Each slot's resting shape is **frame 0 of its `a999_000000[_N].hkx`**, not the skeleton reference pose.
   - Transformed attacks address slot 2 (12xxx dummies).
5. **Runtime.** The eboot still holds the dev hit-display code paths:
   - WorldChrManDbg "All Draw Hit" at `[[0x593E880]]+0x32`.
   - Per-character "Draw Hit" at ChrIns+0x358 bit 2, drawn by ChrIns::DbgDraw at 0x1cd41c0.
   - A RagdollIns menu ("Draw Ragdoll", "Draw Ragdoll Bone") and a DmypolyViewer.
   - "HitIns" in this engine is **map collision** (`FieldAreaCurrentHitIns`), not attack hits.
   - Attack-start hook points found: TAE dispatcher 0x1e249d0 → attack handler 0x1e267a0 → `GetBehaviorId` (vtable +0x1D0) → `ChrIns::RequestBehavior` 0x1cc7530.
   - The per-frame shape test itself is **not located yet**. The plan to find it is in §3.4.
6. **VR melee, ranked:**
   1. **A+B now.** A gesture fires the game's own attack, triggered on the *wind-up*. Its direction and force pick R1, R2, transform or visceral.
   2. **C for first person.** The game keeps animation, stamina, hit list and window. The controller pose places the capsule inside the window.
   3. **D (physics) only as an experiment.**
7. **Sandbox.** The `bbhitbox` v1 JSON uses the same space as the `bbmodel` assets from `extract.py`. Draw recipes are in §5.

---

## 1. How FromSoftware hit detection works

### 1.1 The chain, step by step (Bloodborne evidence)

| Step | What happens | BB evidence |
|---|---|---|
| 1. TAE event | Animation event **1 `InvokeAttackBehavior`** `{s32 AttackType, s32 AttackIndex, s32 BehaviorJudgeID, u8 DirectionType, u8 ?, s16 StateInfo}`. The attack is live only between the event's `start` and `end`, which are seconds on a 30 Hz grid. Sibling events: **5** `InvokeCommonBehavior`, **2** `InvokeBulletBehavior`, **304** `InvokeThrowAttackBehavior`, **307** `InvokePCBehavior`, and **318 `InvokeGunBehavior`** (BB only; same layout as 1). | DSAnimStudio `Res/TAE.Template.BB.xml` **[W]**. 1,186 event-1 records in the 80 TAE files of `c0000.anibnd` parsed with this layout (46 in `a112.tae`) **[C]** |
| 2. Behavior id | Player: `100,000,000 + behaviorVariationId·1000 + judge` in **BehaviorParam_PC**. Enemy: `200,000,000 + NpcParam.behaviorVariationId·1000 + judge` in **BehaviorParam**. If the player row is missing, the game retries with variation `⌊var/1000⌋·1000`. Event 5 uses the judge id alone. | Saw Cleaver var 700 has 62 rows 100,700,000–100,700,950. Gascoigne var 27100 has 227,100,100 and up **[C]**. Same constants in DSAnimStudio `TaeActionSimulationEnvironment.GetBehaviorParamFromEvBox` **[W]** and in the eboot (0x1cf91b8 `lea ebx,[r15+rax+0x5f5e100]`; enemy 0x1ce67a8 `…+0xbebc200`) **[C]** |
| 3. BehaviorParam | `refType` 0 = attack, 1 = bullet, 2 = SpEffect. `refId` = row of AtkParam/Bullet/SpEffect. Also `stamina`, `mp`, `category`. | Paramdex BB `BehaviorParam.xml`. Rows checked **[C]** |
| 4. AtkParam | **4 hits** (`hit0..3`): `Radius` (f32 +0x00), `DmyPoly1` (s16 +0x2C), `DmyPoly2` (s16 +0x34; −1 = sphere), `hitType` (u8 +0x6A: 0 tip, 1 middle, 2 root). Then `hitSourceType` (+0x7C: 0 weapon, 1 body), `dmgLevel` (+0x72, the victim's stagger level), `mapHitType` (+0x73), `throwTypeId` (+0x68), `hitStopTime` (+0x14), `knockbackDist` (+0x10), motion values `atk*Correction` (+0x3E…), flat `atk*` (+0x50…), and `isShareHitList` (shares a hit list across events). The row is 164 bytes. | Offsets from the game's own `paramdef/AtkParam.paramdef` (v201) and Paramdex **[C]** |
| 5. Dummy-poly id | `id/1000`: 0 = default source (the attacking hand's slot 0 for weapon hits, else the body). **10–13 = right-weapon model slot 0–3**, 20–23 = left slot 0–3. `id%1000` = dummy ReferenceID on that model. A missing dummy falls back to the body model with the same id. | DSAnimStudio `NewChrAsm.GetDummyPolySpawnPlace` **[W]**. Saw Cleaver form A uses 10100/10110, form B 12100/12120 **[C]** |
| 6. Shape | Sphere at DmyPoly1, or a capsule from DmyPoly1 to DmyPoly2, with the radius, at the dummies' current world positions. Several dummies may share one id; "they will simply all be used". | soulsmodding `format:flver` **[W]**. 3 copies of dummy 206 on the Saw Cleaver **[C]** |
| 7. Test | The attack shapes are tested against hurtboxes (§1.5) while the window is open. The game keeps a per-attack hit list, so each target is hit once per attack. On a hit: damage from the motion values, the victim's damage animation from `dmgLevel`, hitstop, knockback. Hitting map geometry ("clang") follows `mapHitType` and TAE flags 56/57. | **[L]**. The shape test itself is not located yet (§3.4) |

### 1.2 Differences between games

| | DeS / DS1 | DS3 | **Bloodborne** | Sekiro / Elden Ring | AC6 |
|---|---|---|---|---|---|
| Hits per AtkParam | 4 | 16 | **4** | 16 | 16, plus fan and extend shapes |
| Dummy-id model prefix | not checked | 10/11/12/13, 20/21/22/23 | **same as DS3** | same; ER also encodes a base in judge ≥ 1000 | parts 1–4, 100+ |
| Weapon model slots | not checked | 4 | **4** (trick forms) | 4 | parts |
| TAE version | 0x1000B | 0x1000C | **0x1000C, 64-bit** | 0x1000D | 0x1000D |
| Character physics files | not checked | not checked | **hknp, Havok 2014.1.0-r1** **[C]** | not checked | not checked |
| Ragdoll metadata | HKXPWV (DS1 layout) | HKXPWV (DS3 layout) | **HKXPWV BB layout** (8-byte map entries, 16-byte ragdoll entries) | — | — |

Sources: DSAnimStudio `ParamData.AtkParam.cs`, `HKXPWV.cs`, SoulsFormats TAE **[W]**. BB columns **[C]**.

### 1.3 Dummy polys, the attach points

- FLVER2 dummies (64 bytes): `Position, Color, Forward, ReferenceID, ParentBoneIndex, Upward, AttachBoneIndex, Flag1, UseUpwardVector` **[C]**.
  - The position is given in the space of the **parent bone** (bind pose).
  - It **follows the attach bone** when Flag1 is set: `world = pos · bind(parent) · bind(attach)⁻¹ · current(attach)`. This is DSAnimStudio `NewDummyPolyInfo`/`UpdateFlverBone` **[W]**, verified on weapon slots here.
- Saw Cleaver (`WP_A_0800`):
  - Bones `handle` (root), `edge` (the hinged blade), `stopper`.
  - 120 = grip, on `handle`.
  - 100/110 = blade end and base, on `edge`.
  - 130 = mid blade.
  - 131 = a point 0.3 m off the blade.
  - 206 ×3 = blood/SFX points along the edge.
  - The blade trail (TAE event 118 `SpawnFFX_Blade`) runs from base 110 to tip 100 **[C]**.
- Body dummies on `c0000.flver` that matter here **[C]**:
  - **300** = right-hand weapon attach, **340** = left-hand attach (Blade of Mercy's second blade, FormB), **304/344** = fists.
  - **4xx** = sheathed or hung positions (414 Saw Cleaver, 416 Kirkhammer hammer head).
  - **131** = 0.65 m in front of the chest at 1.38 m height: the centre of the 0.6 m "parry judgment" sphere (AtkParam 350, §1.6).
  - **5** = right foot (the "close range" kick).

### 1.4 TAE events that matter for hits

| Event | Meaning (DSAnimStudio BB template) **[W]** | VR use |
|---|---|---|
| 1 InvokeAttackBehavior | attack window, `[start,end]` s | the hit window; time gestures and haptics to it |
| 0 ChrActionFlag, flag 1/4 | "Cancel – RH attack input check" / "Cancel – RH attack" | when the next R1 is accepted (combo buffer) |
| 0, flag 5 / 73 | "Parry possible" / "Parry possible state" | gun-parry window. **Gascoigne: exactly one 30 Hz tick at the start of each axe window [C]** |
| 0, flag 8 / 24 | invincible (i-frames) / hyper armor | show in sandbox. Hyper armor stops staggers |
| 0, flag 39 | "Disable character ragdoll collision (including hurtboxes)" | the hurtbox switch |
| 0, flag 41 / 42 / 59 | vulnerable +40% damage taken / whiff transition and +12.5% sweet spot / weak −20% | damage modifiers inside animations |
| 32 SetWeaponStyle | 5 = "one-hand (right weapon transformed)" | marks a transform |
| 118 SpawnFFX_Blade | trail from base dummy to tip dummy, on body/left/right weapon | a ready-made "blade line" per attack |
| 304 / 321 | throw attack / "Invoke player throw" | visceral and backstab |
| 318 InvokeGunBehavior | gun shot (bullet behavior) | gun parry |
| 500 IgnoreHitsPartsMask | NPC part id | part-specific invulnerability |

### 1.5 Hurtboxes

- **Source [C].** Each `chr/cXXXX.chrbnd.dcx` holds `cXXXX.HKX`, a Havok 2014.1.0-r1 packfile containing:
  - `hknpPhysicsSceneData` and `hknpRagdollData`
  - one shape per body: hknpCapsuleShape, hknpSphereShape or hknpConvexPolytopeShape
  - ragdoll constraints and two `hkaSkeletonMapper`s (animation→ragdoll and back)

  Member offsets come from the file's own hkClass reflection in `__types__`:
  - `hknpBodyCinfo`: shape +0, name +32, position +48, orientation +64.
  - Capsule `a/b` at +64/+80, radius = `convexRadius` at +20.
  - Convex vertices: `hkRelArray` at +40.
- **Bodies, by example [C]:**
  - Player: 16 capsules. Pelvis r 0.123, Spine 0.099/0.144, Head 0.094, Thigh 0.074, Calf 0.065, Foot 0.066, UpperArm 0.050, Forearm 0.045, Hand 0.048.
  - Huntsman c2600: 22 capsules (adds clavicles, 2 neck, toes).
  - c2130: four concentric stomach spheres, r 0.30/0.56/0.73/0.99.
  - Hound c1000: convex boxes for paws and toes.
- **What drives each body [C].** The animation→ragdoll mapper (skeleton B = ragdoll) keyframes the bodies from animation bones:
  - c0000: Master→Ragdoll_Pelvis01, Spine1→Ragdoll_Spine01.
  - c2710: Gascoigne's left thigh is driven by `L_Support_Thigh`.

  The extractor stores each hurtbox in model space and also local to that bone. Overlaying the c0000 capsules on the hunter body mesh (`out/vr_assets/hunter.json`) lines up limb for limb, so for c0000 the ragdoll reference pose equals the FLVER bind pose (not checked for enemies).
- **Damage parts [C].** In `cXXXX.hkxpwv` (87 characters), each ragdoll bone entry is `{RagdollParamId, DamageAnimId, NPCPartGroupIndex, …}`.
  - Player groups: 1 = torso and upper arms, 2 = forearms and hands, 3 = pelvis and legs, **31 = head** (extra byte 9).
  - Enemies use groups 0–29.
  - Group → `NpcParam.partsDamageRate1..8` / `weakPartsDamageRate` (default 1.5) is **[L]**. Which group counts as "weak" is open.
- **Not hurtboxes [C].**
  - `NpcParam.hitRadius/hitHeight` (e.g. 0.4/2.0 m) and `subHit*` are the character's movement/push capsule ("当たりカプセル").
  - `useRagdoll` decides whether the *player body* collides with an enemy's ragdoll (big enemies only).
- **Bullets [W].** Meowmaritus's comparison (via 3DJuegos) found enemy bullet hit spheres about a quarter of their visual size and player bullets much larger. This matters for VR aiming.

### 1.6 Throws: visceral, backstab, gun parry

- **Visceral damage [C].** The visceral animations (`a100_033000…`, the ThrowParam `atkAnimId` 33000) carry TAE event **321 "Invoke Player Throw"** with params (240, judge 830–849). Those judges resolve through the throw virtual weapon, variation **80**, to BehaviorParam_PC 100,080,830… → AtkParam 80830–80870: a 0.4 m capsule on 10100–10110, MV 50–100 for the first step and 250–840 for the second tier (separate "boss" rows).
- **Visceral trigger [L].** Eligibility is geometric, from ThrowParam (below), not an attack hitbox: R1 while the target is in a throwable state, within `Dist`, inside the angle and height ranges, plus a sphere cast between the two.
- **"Parry judgment" rows [C], role open.** Six weapons (including the Saw Cleaver) have judge **950 "…_投げ判定"** (throw judgment) → AtkParam **350 "共通攻撃【パリィ判定】"**: a 0.6 m sphere at body dummy 131, hitSourceType body. Only one player TAE event uses judge 950 (`a104_042620`, Rifle Spear), so it is not the general visceral check.
- **ThrowParam [C].** 221 rows with throwType **8 "銃崩し"** (gun-break: frontal visceral after a parry, 91 rows), **9 "背面崩し"** (back-break: backstab visceral, 73 rows), 0 and 12. Examples:

  | row | Dist | angle between facings | Y range | target within ±… of my forward | anims (atk/def) | attacker attach dummy |
  |---|---|---|---|---|---|---|
  | 21000 gun-break PC→Hound | 2.5 m | 90–180° | +0.5/−0.65 m | 60° | 33000 / 2510 | 161 |
  | 11000 back-break PC→Hound | 2.5 m | 0–60° | +0.5/−0.65 m | 90° | 33200 / 2500 | 181 |
  | 101 gun-break PC→PC | 1.5 m | 90–180° | +0.5/−0.65 m | 60° | 33000 / 9000 | 231 |

  Other fields: sphere-cast radius rate 0.8, `AtkEnableState` 1, `throwTypeId` (links to AtkParam).
- **How the pieces connect [L].** The check sphere touches a victim in a throwable state (parried, staggered from behind) and the ThrowParam geometry passes. Then both play the paired animations, and the victim is attached at the attack dummy.
- **Gun parry [C] + [L].** Gascoigne's TAE marks "parry possible" (event 0 flag 5) for one tick at the start of each attack window. Common enemies (c2600, c1000) have no flag 5 in their attack animations, so their gun-parry vulnerability comes from elsewhere (open). The shot is event 318 → BehaviorParam refType bullet → Bullet → AtkParam.

### 1.7 Trick weapons

- A weapon has up to 4 FLVERs, `WP_A_xxxx.flver` and `_1/_2/_3` (bnd ids 200+slot), and per-slot anibnds (400+slot) holding `skeleton[_N].HKX`, a TAE, `a999_000000[_N].hkx` and weapon animations such as `a112_033300.hkx` **[C]**.
- **Visibility and attachment [C]:** `dmypolyId_Slot{N}{Right|Left}{Hang|FormA|FormB}` gives the body dummy each slot attaches to in each state (−1 = hidden).

  | weapon | slots |
  |---|---|
  | Saw Cleaver | slot 0 at 300 in A (sheath 414), slot 2 at 300 in B |
  | Kirkhammer | slot 0 sword at 300 in A, slot 1 hammer head hung at 416 in A, slot 2 assembled hammer at 322 in B |
  | Blade of Mercy / Rakuyo | slot 2 at 300 and **slot 3 at 340** (left hand) in B |

- **Shape per form [C].**
  - Saw Cleaver: `skeleton.HKX` differs from `skeleton_2.HKX` only in the `edge` bone, so the blade is folded or extended.
  - Hunter Axe: both skeleton reference poses are *extended*. The collapsed shaft exists only in `a999_000000.hkx`, where frame 0 moves `B_Shaft_00` up 0.42 m and the axe head 0.6 m.
  - The extractor therefore poses each slot with frame 0 of its `a999_000000[_N].hkx`, using a spline decoder ported from SoulsAssetPipeline:

    | weapon | form A | form B |
    |---|---|---|
    | Hunter Axe | axe head (dummy 100) 0.67 m from the hand attach point | 1.36 m |
    | Saw Cleaver | blade folded beside the handle; capsule ends 0.45 m out | blade tip 1.21 m out; the whole model also shifts 0.13 m along the handle |

- **Attacks pick the form through the dummy prefix [C]:** form A R1 uses 10xxx and form B R1 uses 12xxx. The transform attacks (judges 390/395 from A, 790/795 from B) already use the target form's slot.
- **Moveset TAE [C]:**
  - The player's moves for a weapon live in `c0000.anibnd → aNNN.tae` (e.g. Saw Cleaver = `a112`).
  - When `wepmotionCategory` ≥ 100 it *is* NNN; this holds for all 17 weapons where both are known.
  - Older categories go through a remap we have not found: 28→112, 38→122, 30→103, 48→105, 29→114, 37→121, 47→106, 36→104, 40→124, 22→116, 35→110, 24→117, 31→118, 39→123. These pairs come from weapon-anim names.
  - The remap is not in any eboot table we scanned. It is probably in the compiled `action/script/c0000.hks` **[S]**.

### 1.8 Community resources

- **DSAnimStudio** (Meowmaritus) **[W]**:
  - Hitbox simulation from TAE + params + dummies: `TaeEditor/TaeActionSimulationEnvironment.cs`, `NewChrAsm.cs`, `ParamData/ParamData.AtkParam.cs`, `NewDummyPolyInfo.cs`.
  - TAE templates per game: `Res/TAE.Template.BB.xml`.
  - It colours hit types tip (231,186,50), middle (230,26,26), root (26,26,230), and draws throws in cyan.
  - It draws **no hurtboxes**.
  - Its README lists BB for TAE editing; we found no published BB hitbox video.
- **SoulsFormatsNEXT** (FLVER, TAE, PARAM, BND4 layouts) and **SoulsAssetPipeline** (`Animation/HKX/SplineCompressedAnimation.cs`) **[W]**.
- **Paramdex** `BB/Defs`, `BB/Names` and the DeS `Tdfs` enums **[W]**. The game also ships its own paramdefs (`paramdef/paramdef.paramdefbnd.dcx`, version 201, with Japanese descriptions), which the extractor uses **[C]**.
- **Souls Modding Wiki** (soulsmodding.com): `format:tae`, `format:flver`, `bb-refmat:main`, `bb-refmat:param:throwparam` (throwType enum from DS3 era: grab 0, backstab 1, parry 8; BB's own data uses 8 and 9, §1.6). The FromSoftware modding Discord ("?ServerName?") is linked from the wiki home page, discord.gg/wzMynmW **[W]**.
- Elden Ring runtime naming (`eldenring` crate docs: `WorldChrManDbgFlags`, `CSChrPhysicsModule`) as a cross-reference **[W]**.
- No public source documents BB hurtboxes, hkxpwv part groups or the weapon-form posing; §1.5–1.7 is new.

---

## 2. The data in the dump

### 2.1 Where things are **[C]**

| What | File (under `dvdroot_ps4/`) | Content |
|---|---|---|
| Params | `param/gameparam/gameparam.parambnd.dcx` (DFLT, 17.9 MB) | 65 PARAMs, 64-bit layout (format byte 0x04), row names in UTF-16 Japanese. EquipParamWeapon 1,090 rows × 316 B; AtkParam_Pc 1,877 / AtkParam_Npc 6,545 × 164 B; BehaviorParam_PC 1,695 / BehaviorParam 7,173 × 32 B; NpcParam 6,064 × 388 B; ThrowParam 221 × 64 B |
| Param definitions | `paramdef/paramdef.paramdefbnd.dcx` | 63 PARAMDEF v201 (0xD0-byte fields, internal names such as `hit0_DmyPoly1`) |
| Player moves | `chr/c0000.anibnd.dcx` | `skeleton.HKX` + 80 TAE files (a00…a227); animations are in `c0000_a*.anibnd.dcx` |
| Player body | `chr/c0000.chrbnd.dcx` | `c0000.flver` (310 bones, 358 dummies), `c0000.HKX` (ragdoll, mappers), `c0000.hkxpwv`, `c0000.bsipwv` |
| Weapons | `parts/wp_a_XXXX.partsbnd.dcx` (+`_l`) | `WP_A_XXXX[_N].flver`, per-slot `.anibnd` (skeleton, TAE, a999 and transform animations), TPF |
| Enemies | `chr/cXXXX.chrbnd.dcx`, `cXXXX.anibnd.dcx` | `.flver`, `.HKX` (ragdoll), `.hkxpwv` (87 of them), `_c.hkx` cloth, `.clm2`. NpcParam rows `cXXXX·100 + variant` |
| Names | `msg/engus/item.msgbnd.dcx` | `武器名.fmg` (weapon names by EquipParamWeapon id), `NPC名.fmg` (by `NpcParam.nameId`) |

### 2.2 Worked example: Saw Cleaver (`7000000 ノコナタ`) **[C]**

- EquipParamWeapon 7000000: `behaviorVariationId 700`, `equipModelId 800` (→ `wp_a_0800`), wepmotionCategory 28 (→ TAE `a112`), slots 0 (A) and 2 (B), sheath dummy 414.
- Behaviors 100,700,000–100,700,950, 62 rows. Selected rows:

| judge | name (JP row) | AtkParam | shape | stamina | MV phys | dmgLevel | TAE window(s) |
|---|---|---|---|---|---|---|---|
| 0 | モードA_小1 (R1 #1) | 700000 | capsule r 0.40, 10100→10110 | 20 | 100 | 1 | a112_030000 0.367–0.467 s |
| 10/20/30/40 | モードA_小2…5 | 700010… | same | 20 | 102/104/106/109 | 1 | 030100 0.333–0.433, 030200/030300 0.300–0.400, 030400 0.367–0.467 |
| 150 / 170 | charge small / large (R2) | 700150/700170 | same | 35 / 50 | 120 / 190 | 2 | 032620 0.300–0.400 / 032630 0.267–0.367 |
| 300 | モードA_近距離 (close range) | 700300 | **sphere r 0.50 at body dummy 5 (R_Foot)** | 10 | stamina MV 300 | 5 | — |
| 340 / 341 | plunge falling / landing | 700340/341 | sphere r 1.0 / 1.5 at 10120 | 0 / 50 | 140 / 90 | 6 / 2 | 032400 0.367–1.0 (AttackType 62) |
| 390 / 395 | transform attack A→B | 700390/395 | capsule r 0.40, **12100→12120** | 23 | 130 | 2 | 033310 0.467–0.567 / 033300 0.400–0.500 |
| 400/410/420 | モードB_小1…3 (form B R1) | 700400… | capsule r 0.40, 12100→12120 | 24 | 97/100/102 | 1 | 040000 0.567–0.700, 040100 0.467–0.567, 040200 0.467–0.600 |
| 950 | 投げ判定 (throw judgment) | **350** | sphere r 0.60 at body dummy 131 | 0 | — | 0 | none in `a112` (§1.6) |

Geometry, in bbmodel space (metres, X mirrored):
- Form A capsule: (0.149, 0.078, −0.004) → (0.149, −0.419, −0.004), beside the handle.
- Form B capsule: tip (0.079, −1.209, 0.008) → grip (−0.031, −0.121, 0.006).
- R1 hit stop: 0.09 s. Knockback: 0.75–0.85 m.

### 2.3 Other weapons (rest pose of the form's slot) **[C]**

| Weapon | Form A R1 | Form B R1 |
|---|---|---|
| Hunter Axe (5000000) | sphere r 0.5 at the head + capsule r 0.2 head→pommel; window 0.53–0.67 s; 25 stamina | 3 capsules (r 0.5 / 0.2 / 0.1 root); 27 stamina |
| Kirkhammer (8000000) | capsule r 0.4 on the sword; 0.47–0.57 s; 16 stamina | capsule r 0.2 + **sphere r 0.6 at the hammer head** + capsule r 0.05 root; 0.70–0.83 s; 30 stamina |
| Threaded Cane (22000000) | capsule r 0.3; 0.43–0.53 s; 17 stamina | 4 capsules along the whip (r 0.15/0.4/0.15/0.15 root). The whip's points move with its own attack animations (`a127_04xxxx` in `WP_A_1070_2.anibnd`), so the JSON rest pose is only a reference |
| Blade of Mercy (4000000) | slot 0 | slots 2 + 3 (3 at the left hand); two hit points use 20xxx (left-weapon) dummies and fall back to the body |

### 2.4 Enemy attacks **[C]**

- **Father Gascoigne (c2710, var 27100), 29 behaviors:**
  - Axe combo: capsule r 0.35 between body dummies 10 and 11 on bone `Weapon_00` (the axe lives in the body model).
  - Axe slam: r 0.52.
  - Back kick: r 0.45 at dummy 3 on `R Foot`.
  - Beast roar: a 5 m sphere at dummy 203.
  - Gun: a bullet.
  - Windows: axe combo #1 0.533–0.633 s, with **parry-possible 0.533–0.567 s**.
- **Huntsman (c2600):** the saw combo has **two** capsules, the saw (20→21 on `Weapon_03`) and the arm (L Hand→L Forearm). The arm is part of the attack.
- **Across all 170 characters:** 5,419 NPC behaviors; 3,641 with TAE windows; 427 with parry windows in 23 characters.

---

## 3. Runtime: live hit data and hook points

### 3.1 Names in this eboot (UTF-16 debug strings) **[C]**

- ChrIns modules:
  - `SprjChrDamageModule` (`SetGuardLevel`, `SetDmgLevel`, `SetDamageDir`, `SetHeadShoot`)
  - `SprjChrHitStopModule`, `SprjChrKnockBackModule`
  - `SprjChrTimeActModule` (TAE), `SprjChrBehaviorModule`
  - `SprjChrPhysicsModule` ("HavokChara"), `SprjChrMaterialModule`
  - `CSHitFloorInfo`, `SprjChrPhysicsMapHitEvent`
- Others: `RagdollIns`, `SprjBulletManager` (global 0x593E8D0 **[L]**), `HkxpwvRepository` (0x5940220 **[L]**), `FD4HavokManager`. The globals come from 08's singleton heuristic.
- **`HitIns` / `FieldAreaCurrentHitIns` is map collision** (the field's hit instance). It is not the attack system, despite the DS3-era habit of calling attack hits "HitIns".
- Script-level hit helpers: `IsHitAttack`, `SetHitMask`, `AddHitMaskByBit`, `CheckChrHit_Wall`.

### 3.2 Debug hit display left in the retail build

| Item | Where | What it does | Status |
|---|---|---|---|
| **All Draw Hit 【%s】** | WorldChrManDbg menu fn 0x1d20510; flag byte `[[0x593E880]] + 0x32` (pointer global 0x593E880) | global "draw hits for every character". Neighbours in the same menu: All No Damage / No Hit / No Attack / No Move | **[C]** registration and flag |
| Per-character Draw Hit / Draw Direction / Draw Dummy Polygon | ChrIns `u16 +0x358`: bit 2 Draw Hit, bit 7 Draw Direction (printed by fn 0x1ccf620) | per-character debug | **[C]** |
| ChrIns::DbgDraw | fn **0x1cd41c0**: draws if `(ChrIns+0x358 & 7)` or AllDrawHit. Uses a debug-draw context object and primitive emitters **0x162a630** (shape/segment, 29 call sites in 14 functions) and **0x135e040** (text label: the character name) | reads a module object (ChrIns+0x3B0 → +0x68: floats +0x354/+0x358, a position +0x1E0), draws a shape from it plus the character's name. Most likely the NpcParam hit capsule **[L]** | **[C]** the code exists. Whether the retail renderer shows debug primitives is **[S]**, untested |
| RagdollIns menu | fn 0x1c13180: "Draw Ragdoll", "DrawRigidIndex(-1:All)", "Draw Constraint", "Draw Ragdoll Bone", "Draw RagdollParamId of Pwv" | hurtbox (ragdoll body) display | **[C]** menu strings and registration |
| DmypolyViewer | 0x2153b60/0x2153be0; "Draw Dmypoly : OFF/(ALL)/(%d)" at 0x2154e90 | dummy-poly display per model | **[C]** |
| SprjDbgDispStep | step `STEP_Update` (UTF-16 names) | debug display step | **[C]** names |

The community "Restore Debug Menu" patch (Whitehawkx/auser1337, `patches/Bloodborne.xml`) exposes the dev menu. Nobody has published BB hitbox drawing through it **[W: none found]**.

### 3.3 The attack invocation path **[C]** (addresses verified by disassembly)

```
TAE event dispatcher 0x1e249d0 (virtual; vtable slot at 0x5735170; owner class name not resolved)
  └─ 0x1e267a0  attack-behavior event handler (event 1, or its twin 318 with the same layout)
       rsi = event; [rsi+8] -> params: +0 AttackType, +8 BehaviorJudgeID, +0xC DirectionType, +0xD flag
       hand/weapon via ChrIns vfuncs +0x380/+0x388; EquipParamWeapon lookup 0x2329aa0 (weaponCategory +0xE2)
       ChrIns vfunc +0x1D0 GetBehaviorId(judge, hand, flags, isItem)
           player 0x1cf9070: 100,000,000 + var*1000 + judge (fallback var -> (var/1000)*1000; 3000 = thrown item)
           enemy  0x1ce6730: 200,000,000 + NpcParam.behaviorVariationId*1000 + judge
       BehaviorParam lookup 0x23133c0(out, id, isPc)  [SoloParamRepository slot 12 + isPc]
       cost check: BehaviorParam mp (+0x18) / heroPoint (+0x1D) against the character's stats object
  └─ 0x1cc7530  ChrIns::RequestBehavior(behaviorId, attackType, dirFlag, hand, ...)
       BehaviorParam again; stamina = -(stamina * SpEffect rates), MP, heroPoint; weapon scaling vfuncs +0x2A8/+0x2B0
       0x1cb61c0(request, ChrIns+0x178, ChrIns): queue it; ChrIns+0x188 = 1
```

- **SoloParamRepository** is at global 0x5940340. Slot *i* has its row count at `+0x70 + i·0x48` and a resource at `+0x78 + i·0x48`; `→ +0x70 → +0x70` is the PARAM file (rows are binary-searched).
  - Slot 0 = EquipParamWeapon (the lookup rounds the id down to a multiple of 100).
  - Slots 12/13 = BehaviorParam / BehaviorParam_PC.
  - **Slots 7/8 = AtkParam_Npc / AtkParam_Pc**: lookup 0x2312fd0, 73 call sites **[L]** (same order as DS3's repository).
- Two AtkParam consumers, 0x1cb8430 and 0x1cb8730, read an attack record with `+0x3C = AtkParam id, +0x40 = isPc` **[L]**, probably the receiving side's damage path.

### 3.4 Finding the per-frame shape test (next RE step)

Static search for code reading AtkParam `+0x2C/+0x34` found nothing. The game copies the row pointer into a record and reads it elsewhere. Because the port runs the eboot natively, a dynamic approach is cheap:

1. **Guard-page trace.**
   - Find the AtkParam_Pc row 700000: SoloParamRepository slot 8, binary search.
   - Set `PAGE_GUARD` on its page and log `RIP` plus a 4-frame stack in a vectored exception handler (re-arm after each hit).
   - Do one Saw Cleaver R1. The readers of `+0x2C/+0x34` during frames 11–14 of a112_030000 are the hit-shape builder.
2. From there, follow these calls:
   - (a) the dummy-poly world-matrix query (model instance → FLVER dummy → bone matrix palette)
   - (b) the hknp query (shape cast or closest points) against bodies, which should be the ragdoll bodies from §1.5
   - (c) the hit-list insert and the "hit dealt" callback (for haptics)
3. Cross-check by enabling **All Draw Hit** (`[[0x593E880]]+0x32 = 1`) and hooking emitter **0x162a630**. Even if the retail renderer drops debug primitives, a host-call hook can collect them and redraw them in the VR overlay. That gives game-truth debug geometry for free.

### 3.5 Where hooks go

| Goal | Hook | Notes |
|---|---|---|
| "Player attack started": haptics, sandbox mirror, gesture timing | entry of **0x1cc7530** (`rdi` = ChrIns, `esi` = behavior id, `edx` = AttackType); filter on the player ChrIns | behavior id → AtkParam → shapes and windows come from our JSON. Uses the existing host-call hook system (`src/runtime_hooks.c`) |
| Which hand/judge fired | **0x1e267a0** (event params at `[[rsi+8]]`) | also catches gun events with the same layout |
| Live hitbox overlay (debug) | ChrIns+0x358 bit 2 / AllDrawHit flag + **0x162a630/0x135e040** collection, *or* draw our JSON capsules on the live weapon model's world matrix | the second needs the weapon instance matrix (13 covers weapon recognition) |
| Move capsules to the VR weapon (option C) | the dummy-poly world query found in §3.4 step 2a, only for the attacking weapon slot during an active window | swap in `controllerWeaponWorld · slotDummyPos`. Keep the radius, hit list and window |
| Hit feedback | the hit-dealt callback (§3.4 step 2c) | haptic intensity from `dmgLevel`/hitstop |

---

## 4. VR melee design options

**Existing input** (`gpu/xr_bridge/xr_input.cpp`):
- A right-hand swing faster than 2.0 m/s → R1.
- With grip held, or faster than 3.5 m/s → R2.
- A left-hand thrust faster than 1.8 m/s → L2 (gun).
- Left grip at the mouth → vial.
- Re-arm below 1.0 m/s; presses last 100 ms.

**Timing facts that shape everything [C]:**
- The game's hit window opens **0.30–0.57 s after the attack starts**: Saw Cleaver R1 0.30–0.37, transformed 0.47–0.57, Hunter Axe 0.53, Kirkhammer B 0.70.
- It stays open about 0.1 s.
- A real swing takes about 0.2–0.3 s, so firing on the strike puts the in-game hit after the arm has stopped.

| | Option | Feasibility | Game feel | Balance |
|---|---|---|---|---|
| **A** | Gesture → attack button (exists) | done | input-to-hit lag about 0.35–0.6 s; the swing direction is ignored; combos work (the game buffers through TAE cancel flags 1/4) | unchanged: stamina 16–50 per swing, motion values and poise are all the game's |
| **B** | A plus classification: wind-up start fires; direction and force pick the input | 1–2 weeks, input layer only | much better timing if fired on the **backswing** (wind-up detection hides ~0.2–0.3 s of startup). Direction can only choose among inputs the game offers at that moment. The chain's own swing directions (R1 #1 vs #2) cannot be chosen | unchanged; R2 for heavy swings costs more stamina and time |
| **C** | Animation plays; during the TAE window the attack capsules follow the controller-driven weapon | after §3.4 (find the dummy query), then 2–4 weeks | you hit what you swing at; first person feels right. In third person, needs the avatar's weapon IK'd to the controller (13) or hits come from an invisible blade. The window can be stretched about 0.1 s for timing tolerance | kept: same AtkParam, hit list, window, stamina. Knobs: radius scale (0.4 m is huge for a controller-tracked blade; 0.5–0.75×), window stretch |
| **D** | Physics: our own sweep vs live hurtboxes, then inject damage | research: needs enemy bone matrices, a damage-injection path (RequestBehavior with a bullet-type behavior at the contact, plus a bullet-position hook) and anti-flail rules | most "real". No player attack animation (ERVR's known problem: AI reacts oddly, you can roll mid-swing). Hitstop and poise still work if routed through AtkParam | risky: we must charge stamina and enforce cooldown = the equivalent attack's length. No swing-speed damage scaling, or clamp to 0.8–1.1× |

**Recommendation.**
1. Ship **A→B** first. Here is a gesture map that uses only the game's own moves:

   | Gesture | Input |
   |---|---|
   | Lateral slash (wind-up detected) | R1 |
   | Overhead or very fast swing, or a held raised wind-up | R2 hold/release (charged) |
   | Forward thrust with the right hand when an enemy is staggered in front | R1 (the game runs the visceral check from ThrowParam: Dist 1.5–2.5 m, ±60°, sphere cast) |
   | Quick wrist flick about the controller's long axis | L1 (transform; mid-combo it becomes the transform attack) |
   | Left thrust | L2 (gun; exists) |

   Hold the virtual button until the game consumes it or the TAE input-check window closes, not a fixed 100 ms.
2. Make **C** the target for first person (PLAN §6 puts first person in v1).
3. Keep **D** to sandbox experiments. The sandbox can score C/D offline: replay `.bbxrrec` swings (12 §4), sweep the weapon capsules against a hurtbox set, and count hits and misses before touching the game.

**Gun parry under VR latency.** Hunter-type enemies give one 30 Hz tick (33 ms) at the start of the active window. The bullet's travel time is in that budget too. Anything we add (gesture detection, the 100 ms press) eats it. Fire L2 on thrust *onset*, not at peak speed. A wider window would be a balance change: make it optional and off by default.

Field notes **[W]**:
- Skyrim VR's flail exploit came from large melee volumes ("hitboxes way too big"); BB's 0.4 m radii would have the same problem under C/D without a scale knob.
- Skyrim keeps thrusts out ("slash-only to keep things balanced").
- Asgard's Wrath is "mostly canned animations", with parries made by weapon angle.
- DI2's UEVR mod exposes a "swing effort" slider (gesture strictness).
- Melee sweeps should sample intermediate poses at low frame rates (Unity forum, PhilSA). At 90 Hz, a 6 m/s tip moves 6.7 cm per frame: with r ≥ 0.15 m one sweep step is enough, and with VR-scaled radii use 2–4 substeps.

---

## 5. Sandbox data: `bbhitbox` v1

### 5.1 Running it

```
python scripts/vr_assets/hitbox_extract.py --game C:/games/CUSA03173 --weapons all --chr all --npc-tae
#   -> out/vr_assets/hitbox/{index.json, wp_<equipId>.json x46, chr_cXXXX.json x170}   (~17 MB, ~16 s)
#   --weapons trick | 7000000,8000000      --chr c0000,c2600 | none      --no-tae
```

- It refuses to write inside the dump.
- `--weapons all` keeps the +0 row of every weapon with a model, skipping the x090000 "wandering" NPC copies and duplicate models.
- `--chr all` takes every chrbnd that has a `.HKX`.

### 5.2 Format

Every file has `"format": "bbhitbox", "version": 1` and `"space": "right-handed meters, +X right, +Y up, -Z forward (FromSoftware X mirrored)"`, the same as the `bbmodel` files from `extract.py`. A raw FromSoftware point maps to `(−x, y, z)`.

**Weapon file** `wp_<id>.json`:

```
{ kind:"weapon", id, name_en, name_jp, model:"wp_a_0800", model_json:"wp_a_0800.json", hand:"right",
  behavior_variation, wepmotion_category, tae_category:"a112", tae_source:"weapon anibnd"|"wepmotionCategory"|null,
  forms: {A:[0], B:[2]},                         // slots visible per form
  slots: [ { slot, flver, pose:"a999_000000_2.hkx frame 0"|"flver bind"|"...reference pose",
             attach_body_dummy: {right:{hang,form_a,form_b}, left:{...}},   // body dummy ids (300 = right hand)
             dummies: [ {id, pos[3], fwd[3], up[3], bone} ] } ],            // weapon-slot model space, posed
  attacks: [ { judge, behavior_id, name_jp, ref:"attack"|"bullet"|"speffect", ref_id, stamina,
               atk_name_jp, hit_source:"weapon"|"body", motion_value{phys,mag,fire,thun,stam}, atk_flat{...},
               dmg_level, hit_stop_time, knockback_dist, throw_type_id, map_hit_type, form:"A"|"B"|"A/B"|null,
               hits: [ { index, radius, hit_type:"tip"|"middle"|"root", shape:"capsule"|"sphere",
                         p1:{dmy, local_id, src:"right_weapon"|"left_weapon"|"body", slot?, pos[3], count,
                             bone?, pos_local?, fallback?, unresolved?},
                         p2:{...}? } ],
               tae: [ {anim:"a112_030000", start, end, attack_type, attack_index, direction} ]?,   // seconds
               trails: { "<anim>": [ {start, end, ffx, source, base_dmy, tip_dmy, fx_slot} ] }? } ] }
```

**Character file** `chr_cXXXX.json`:

```
{ kind:"character", chr, name_en,
  npc_rows: [ {id, name_jp, name_en, behavior_variation, hit_radius, hit_height, parts_damage_rate[8], weak_parts_damage_rate} ],
  hurtboxes: [ {body, shape:"capsule"|"sphere"|"convex", radius, a[3], b[3], bone, a_local[3]?, b_local[3]?,
                points[]?, points_local[]?, part_group?, damage_anim?} ],
  body_dummies: [ {id, pos, fwd, up, bone, pos_local?} ],
  attacks: [ {variation, judge, behavior_id, name_jp, ref, ref_id, ...same attack summary..., hits,
              tae?, parry_windows?: { "<anim>": [ {start, end, parry_flag} ] } } ] }
```

- `a/b`, `pos` and `points` are model space at bind pose.
- `*_local` values are in the space of `bone` at its bind pose, so `world = boneWorld · local` once that bone is posed.
- Every `bone` field is a bone **name**, never an index. The hunter body in `hunter.json` merges several FLVERs (316 bones against 310 in `c0000.flver`), so indices would not carry over. For the player, the names match `out/vr_assets/hunter.json` (`L_Thigh`, `Spine1`, `Master`, …).

**`index.json`:** `{format:"bbhitbox-index", weapons:[{id, name_en, model, file, attacks, hits, tae}], characters:[{chr, name_en, file, hurtboxes, attacks}]}`.

### 5.3 Drawing recipes

1. **Weapon in hand.**
   - Load `model_json` (bbmodel) and `wp_<id>.json`.
   - Put the weapon-model origin at the hand attach frame. In game that is body dummy `attach_body_dummy.right.form_a` (300). In VR, `weaponWorld = gripPose · T_grip→300`; the calibration is in 13.
   - For the current form, draw each hit from `p1.pos` to `p2.pos` (weapon model space) with `radius`, coloured by `hit_type` (DSAnimStudio colours, §1.8). Use attacks with `form` equal to the current form.
   - The bbmodel meshes are FLVER bind pose. For slots whose `pose` comes from an a999 frame (Saw Cleaver B, Hunter Axe A/B), the mesh needs the same pose to match the dummies.
2. **Attack timing.** On a gesture (option A/B) start a timer at the attack's `tae[0].anim` and highlight its capsules between `start` and `end`. That shows the startup lag of §4 directly in the headset.
3. **Hurtboxes.** Pose each hurtbox from `bone` (`a_local/b_local`), or draw `a/b` statically. Colour by `part_group` (player: 1 torso, 2 arms, 3 legs, 31 head). Convex bodies: draw `points` as a hull or as small spheres of `radius`.
4. **Enemy attacks and parry.** Pose `p1.pos_local` on `p1.bone` (e.g. `Weapon_00`) and show `parry_windows` as a flash. This is a gun-parry trainer without the game.
5. **Sweep test for C/D tuning.** Each frame, test the weapon capsule (controller) against each hurtbox capsule with segment–segment distance ≤ r₁ + r₂. Use substeps for fast swings. Log hits per swing against the A/B ground truth.

### 5.4 Validation done, and limits

- **Done [C]:**
  - Row sizes from the game's paramdefs equal the PARAM row strides for all params read. The extractor refuses on mismatch, through `souls.param_table`.
  - The c0000 hurtboxes overlay the hunter mesh limb for limb.
  - The Saw Cleaver form A capsule sits on the blade dummies and mesh.
  - The FLVER bind of slot 0 equals `skeleton.HKX` exactly: rotation diff 0.0, so the Euler order XZY is right.
  - The form A/B shapes differ as expected (Hunter Axe head 0.67 m ↔ 1.36 m from the attach point).
- **Limits:**
  - **No TAE timing for 9 weapons** (wepmotionCategory < 100 and no weapon animations to name the moveset): Bare Fists, Blade of Mercy, Ludwig's Holy Blade, Tonitrus, Hunter Blunderbuss, Wooden/Loch Shield, Hunter's Torch, Kos Parasite.
  - Whip points are rest pose only. A next step is to sample the weapon's own attack animation at the window midpoint; the spline decoder already exists in the script.
  - Left-weapon (20xxx) points on right-hand weapons fall back to body dummies (`fallback: true`), as DSAnimStudio does.
  - Enemy points assume the weapon is part of the body model (true for c2600/c2710).
  - Part-group semantics are open.
  - Gun-parry vulnerability of common enemies is open (§1.6).

---

## 6. Open questions / next steps

1. **Find the shape test** (§3.4 guard-page trace) and the hit-dealt callback. Effort: about 1–2 days with the hook system.
2. **Does the retail renderer draw debug primitives?** Flip `[[0x593E880]]+0x32` in a run and watch, or hook 0x162a630 and count calls.
3. **The wepmotionCategory < 100 → aNNN remap.** Look in `action/script/c0000.hks` (HavokScript bytecode) for constants 112/122/103.
4. **Part groups → `partsDamageRate` slots and "weak" parts.** Test by hitting an hkxpwv group-7 body on a c3000 enemy.
5. **Gun-parry rules for common enemies.** No TAE flag 5. Check SpEffect, NpcParam or AtkParam fields during their windups.
6. Sample whip and animated weapon points per attack (weapon animation at the window midpoint) for the Threaded Cane and Hunter's Whip.
7. Wire option B in `xr_input.cpp`. Measure the wind-up-to-strike interval on real swings (recorder, 12 §4) and pick the trigger point per weapon class from the `tae` windows.

---

## 7. Sources

Code read (read-only clones):
- https://github.com/Meowmaritus/DSAnimStudio: `DSAnimStudioNETCore/Res/TAE.Template.BB.xml`, `TaeEditor/TaeActionSimulationEnvironment.cs`, `NewChrAsm.cs`, `ParamData/ParamData.AtkParam.cs`, `NewDummyPolyInfo.cs`, `NewDummyPolyManager.cs`, `HKXPWV.cs`.
- https://github.com/soulsmods/SoulsFormatsNEXT: `Formats/FLVER/Dummy.cs`, `FLVER2.cs`, `TAE/*.cs`, `PARAM/*.cs`, `Binder/*.cs`.
- https://github.com/Meowmaritus/SoulsAssetPipeline: `Animation/HKX/SplineCompressedAnimation.cs`.
- https://github.com/soulsmods/Paramdex: `BB/Defs`, `BB/Names`, `DES/Tdfs`.

Web **[W]**:
- Souls modding wiki: http://www.soulsmodding.com/doku.php?id=format:tae, http://www.soulsmodding.com/doku.php?id=format:flver, http://www.soulsmodding.com/doku.php?id=bb-refmat:main, http://www.soulsmodding.com/doku.php?id=bb-refmat:param:throwparam
- DSAnimStudio releases (hitbox and DummyPoly notes): https://github.com/Meowmaritus/DSAnimStudio/releases?page=5
- Elden Ring runtime naming: https://docs.rs/eldenring/latest/eldenring/cs/struct.WorldChrManDbgFlags.html, https://docs.rs/eldenring/latest/eldenring/cs/struct.CSChrPhysicsModule.html
- Bullet hitbox sizes (Meowmaritus): https://www.3djuegos.com/juegos/bloodborne/noticias/bloodborne-dificil-no-injusto-fromsoftware-tambien-utiliza-truco-diseno-que-otros-juegos-sencillos
- ERVR motion melee (search excerpt only): https://www.nexusmods.com/eldenring/mods/10711
- Luke Ross on motion controls: https://mixed-news.com/en/this-vr-modder-makes-10000-a-month/
- UploadVR, Elden Ring VR hands-on: https://uploadvr.com/elden-ring-vr-mod-gameplay-hand-on/
- Skyrim VR:
  - INI defaults (`fMeleeLinearVelocityThreshold 2.0`, angular 0.5, `fMeleeWeaponHavokScale 1.75`): https://stepmodifications.org/forum/topic/16196-skyrim-vr-default-values-for-all-valid-ini-settings/
  - Flail complaint: https://steamcommunity.com/app/611670/discussions/0/1697169163415712308/?ctp=3
  - Slash-only on PSVR: https://blog.playstation.com/archive/2017/06/19/how-it-feels-to-play-the-elder-scrolls-v-skyrim-on-playstation-vr
- PLANCK (search excerpt only): https://www.nexusmods.com/skyrimspecialedition/mods/66025
- Other VR games:
  - Asgard's Wrath review: https://uploadvr.com/asgards-wrath-review/
  - Blade & Sorcery: https://uploadvr.com/hands-on-with-blade-sorcerys-brutally-realistic-vr-combat-system/
  - Until You Fall: https://roadtovr.com/until-you-fall-gameplay-video-vr-sword-combat/
- UEVR melee: https://github.com/jbusfield/DI2_UEVR/releases/, https://github.com/Ashok0/SystemShock-UEVR
- Melee sweep sampling: https://discussions.unity.com/threads/thoughts-on-implementing-precise-framerate-safe-melee-weapon-collisions.462087/
- Bloodborne first-person mod and motion controls: https://uploadvr.com/bloodborne-first-person-mod
