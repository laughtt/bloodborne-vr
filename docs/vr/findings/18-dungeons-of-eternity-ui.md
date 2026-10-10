# 18: Dungeons of Eternity VR UI/UX, as inspiration for Bloodborne VR item, quick and lobby UI

Date: 2026-10-09. Scope: web research only. No repo code touched. Builds on 10 (HUD layer), 11 (comfort), 13 (hands/arms/weapons), 14 (hitboxes).

Legend: **[S]** sourced claim (link given). **[U]** uncertain or not found. **[D]** design proposal from this doc, not a fact about the game.

## 0. Research coverage and gaps (read first)

Searches were thorough on reviews and the Steam page, and weak on player-facing UI detail. Several Dungeons of Eternity (DoE) facts that matter most for a quick menu are **not found** in the sources reachable here:

- The **button** that opens the inventory is not named. [S] UploadVR says "Finally, you can change the button used for pulling up your inventory" (implies a rebindable button; the default is unknown) [U].
- No source describes a **radial menu, wrist panel, or floating panel** for quick items. [U]
- No source describes **how potions are carried** (belt, menu, or world grab) beyond "picked up using a magnetic grab." [S][U]
- No wiki, Reddit thread or YouTube transcript was reachable. The r/DungeonsOfEternity search returned nothing relevant. [U]
- Steam store page returned HTTP 403 to the fetch tool; its content came from a secondary summary. [U]
- Road to VR review and Road to VR's "Design Excellence in UI" award were not verified on RoadToVR itself (the award is only listed on secondary promo pages). [U]
- A Road to VR article URL cited in one search returned 404. [U]

Recommended next step: a human in a headset (or a YouTube "first 15 minutes" video of DoE on Quest 3) should check the inventory button, holster positions, and potion handling before any Bloodborne UI is locked. Several items below are marked for that check.

## 1. Inventory and equipment

| Question | What we know | Source |
|---|---|---|
| Body slots | Melee weapons are **holstered around the waist**. Ranged weapons, shields and magic staffs are placed **on the back**. | [S] UploadVR review |
| Starting loadout | "You start with a sword and an axe, as well as a bow on your back." | [S] Gaming Trend review |
| Dual wield | "you can dual wield any time you'd like" (so the hands can hold two melee weapons). | [S] Gaming Trend review |
| Swap | Not described in any source. Presumably by drawing from the holster with the other hand. | [U] |
| Slot count | Not stated. Looks like about 2 melee (waist) + 1 ranged/shield/staff (back), from the described positions. | [U] |
| Holster adjustment | "Seated gameplay is supported and adjusts holster positions for greater ease." | [S] UploadVR review |
| Item display | Equipped items are physical objects on the body. No source describes an item panel. | [S] implied |
| Loot shown | Loot is "shown as loot at the end of your dungeon," not as it is found. | [S] Gaming Trend review |
| Weapon generation | Weapons are randomly generated. "Loot is nearly endless." | [S] Steam store summary |

Design read-across: the waist-and-back holster model is the strongest validated pattern in DoE and maps directly to Bloodborne's **trick weapon on the hip/back** and **firearm in the left hand**. See Section 6.

## 2. Pop-up and quick menus

- **Inventory button**: exists and is rebindable [S][U as to default]. Pop-up type unknown [U].
- **Map**: "a physical item that you can place on the floor so everyone can view it at once." [S] UploadVR. This is a world-locked object, not a menu. It is a good pattern for a shared map or list.
- **Pause / time-stop**: not stated in any source [U].
- **Selection method** (point, touch, grab): not stated [U]. The map uses physical placement and grabbing.
- **Comfort options**: "teleport, vignetting, snap turns, and other motion sickness abatement options." [S] Gaming Trend review. Useful for any Bloodborne menu that is world-locked.

## 3. Consumables: potions and throwables

- **Potions** are "picked up using a magnetic grab" from rooms. [S] UploadVR. There is no evidence of a belt slot for potions. [U]
- **Potions** can be fabricated at your personal station ("fabricate new weapons and potions"). [S] Gaming Trend review.
- **Potions are shareable**: "sharing potions, equipping complementary weapons, and tackling dungeons as a team is a huge multiplier to the fun." [S] Gaming Trend review.
- **Throwing**: weapons "can be thrown for mid-range damage and called back using a magnetic grab." [S] UploadVR. Gaming Trend: "throwing your weapons and then recalling them had me feeling like discount Thor." [S]

So DoE's consumable model is **magnetic recall**: throw or let go, then pull back by grab. Potions are picked up by magnetic grab, not stored on a belt, as far as the sources show. [U on belt absence.]

## 4. Loot, stats, crafting, hub

- **Loot cards / comparisons**: no source describes floating item cards or comparison UI [U].
- **Coins**: "Coins aren't shared between the party but treasure chest loot is." [S] UploadVR.
- **Hub**: the game unfolds on a space station. "you then store them in a room filled with holographic representations of these things" (weapons are stored as holograms in a room). [S] Gaming Trend review.
- **Dungeon access**: "Each of the dungeons are accessed via a table." [S] Gaming Trend review.
- **Personal space**: "everyone has their own space where they can customize their character, fabricate new weapons and potions." [S] Gaming Trend review.
- **Exosuit / stats**: "The Exosuit is just a fancy way of allowing the player to add points to their various stats." [S] Gaming Trend review. Stat allocation is through an in-world object, not a flat menu.
- **Crafting**: "craft new, randomly-generated weapons" [S] (preview; secondary) [U on specifics].
- **Upgrades**: "Loadouts, upgrades, and character customization" [S] Steam store summary.

Design read-across: the **hub as physical storage** (weapons held as holograms, stations you walk to, a table to choose the dungeon) is directly useful for the Hunter's Dream.

## 5. Co-op and lobby (3 to 4 players)

- **Player count**: "Solo or up to 4 players in co-op." [S] Steam store summary. Bloodborne plan is 3, so the DoE 4-player model is a superset.
- **Joining**: "the real fun begins when another player joins you." [S] Gaming Trend review.
- **Shared map**: one physical map placed on the floor, visible to everyone. [S] UploadVR.
- **Coins private, chests shared**: a rule that the team can read at a glance. [S] UploadVR.
- **Party UI, ready-up, matchmaking screens**: not described in any source [U].
- **Multiplayer chat**: in-game chat and online interaction are listed. [S] Steam store summary.

## 6. Feedback, comfort, readability

- **Haptics and sound**: "subtle haptic feedback and accompanying sound design during object interactions." [S] UploadVR.
- **Hand-attached vs world-locked**: holsters are hand-free body anchors (waist and back). The map is world-locked. Menus appear to be rare. [S + U]
- **Seated play** is supported with holster adjustment. [S] UploadVR.
- **Motion sickness options**: teleport, vignette, snap turn. [S] Gaming Trend.
- **Sprint** is a stick-hold: "Sprinting is activated by holding down the left or right stick." [S] UploadVR. Relevant to our left stick click modifier (see Section 7 caution).
- **UI award**: Road to VR "Design Excellence in UI" is listed on a Discord and podcast promo page [S secondary] [U on RoadToVR primary]. An award-level UI reputation is worth checking directly.

## 7. What players praise or criticize

Praise (sourced):
- "One of co-op's best on Quest" (UploadVR, rated "Essential"). [S] https://www.uploadvr.com/dungeons-of-eternity-review/
- "discount Thor" for throw-and-recall. [S] Gaming Trend.
- "Dungeons of Eternity quickly became one of the most enjoyable VR co-op experiences I've ever tried." [S] UploadVR longswords update article (2 May 2024), https://uploadvr.com/dungeons-of-eternity-longswords-traps-spring-update

Criticism (sourced, but none about UI):
- ThumbCulture Quest 2 review: locations "bland and uninteresting," enemy variety thin, sword and dagger hitboxes "slightly iffy." [S] https://www.thumbculture.co.uk/?p=65145 (not about menus).
- No menu or inventory complaints were found in reachable reviews. [U, search limited]

Read: the sourced praise centres on **physical holsters, magnetic recall and shared map**. Nothing sourced criticises the UI, so these may be the design values to copy.

## 8. Mapping to Bloodborne VR

Bloodborne constraints (from our repo notes and the prompt):
- Consumables: Blood Vials, Quicksilver Bullets, Molotovs, Fire/Bolt Paper, Pebbles.
- Trick weapons: normal and transformed states.
- Firearms in the left hand.
- Hunter's Dream hub.
- Pimax Dream Air controllers in Touch layout. The left menu button is not available. Left stick click is already a modifier.

Mapping table:

| DoE pattern | Bloodborne equivalent | Notes |
|---|---|---|
| Melee holstered at waist, ranged on back | Trick weapon on right hip; Blunderbuss or big items on back. Left-hand firearm held in left hand. | Keep the trick weapon on the hip so it is drawn with the right hand. |
| Magnetic grab recall | Pebbles and Molotovs thrown, with grab-return optional | Only if we want thrown items to return. Bloodborne thrown items do not return, so this is a design choice, not a faithful port. |
| Potions picked up by magnetic grab | Blood Vials picked up from world, stored on a body pouch | Blood Vials are a must-have quick item. |
| Physical map on floor | Hunter's Dream map or Yharnam map as a placed object | Good for shared co-op view. |
| Hub weapons as holograms | Hunter's Dream weapon rack | Sets up the trick-weapon transform choice. |
| Holster adjust for seated play | Holster offset in settings | Required for comfort. |
| Haptics on interaction | Vibrate on quick-item use, empty slot, ammo use | Cheap, high value. |

Controls on Pimax Dream Air (Touch layout, no left menu button):
- Left stick click is a modifier. Do not use it for sprint (DoE uses stick-hold for sprint [S]), because that conflicts with Bloodborne's dodge and existing modifier.
- Left grip or left trigger could hold the quick-item menu. [D]
- Right menu (if present) for pause and inventory. [D] Verify on hardware.

## 9. Design sketches (ranked by value/effort)

### Sketch A: Body quick slots, no menu (highest value, lowest effort)  [D]

Hold nothing. Items live on the body. Draw by reaching.

```
            [ back: big item / spare firearm ]
                       ____
  (L hand)  <-- hip R: trick weapon   (chest: Blood Vial pouch)
  firearm       reach down-left  ->  left hip: Pebbles / Molotov
```

- Left hand: firearm (held), or grab Molotov from left hip pouch.
- Right hand: trick weapon from right hip (transform by a button).
- Blood Vial: chest pouch, grab with left hand, press use.
- Effort: low (holsters are hand-tracked anchors, same as DoE).

### Sketch B: Wrist strip for consumables (medium value, medium effort)  [D]

A small non-diegetic strip on the left forearm, visible only when the left palm faces up or when a button is held.

```
   left forearm (palm up)
   +--------+--------+--------+
   | Vial x5| Bullet | Molot  |
   |  [1]   |  [2]   |  [3]   |
   +--------+--------+--------+
```

- Show only while a button is held (time does not pause in Bloodborne, so we do not pause).
- Select by touching the chosen icon with the right index finger, or by pressing a face button to cycle.
- Close on release. Effort: medium (need hand-attached UI, comfort at distance 25 to 35 cm, head tracking).

### Sketch C: Radial quick-item wheel with time slow (highest effort, most flexible)  [D]

A radial wheel around the left hand while a modifier is held. Right stick or right thumb selects. Time slows to about 0.3x.

```
            Vial
     Bullet      Molotov
            (+)
     Pebble      Fire Paper
            Bolt Paper
```

- Open: hold left grip. Select: point right hand at the segment and release trigger.
- Close: release. Pause the game: no; slow time only, to match Bloodborne's fear of stall. Effort: high (new UI layer, radial hit test, slow-mo feel).

Ranking by value/effort:
1. **Sketch A** (body slots). Highest value, lowest effort, matches DoE's sourced praise.
2. **Sketch B** (wrist strip). Medium value, medium effort; helps Blood Vials readability.
3. **Sketch C** (radial). Highest effort; only if A and B prove insufficient.

## 10. Sources

- UploadVR review, Henry Stockdale, 21 Sep 2023, updated 20 Oct 2023: https://www.uploadvr.com/dungeons-of-eternity-review/
- Gaming Trend review, Meta Quest 3: https://gamingtrend.com/feature/reviews/dungeons-of-eternity-on-meta-quest-3-virtual-dungeon-deliving-with-friends/ (score 80 "Great")
- UploadVR longswords / traps spring update, 2 May 2024: https://uploadvr.com/dungeons-of-eternity-longswords-traps-spring-update
- Road to VR unveiling article: https://roadtovr.com/?p=112619 (fetched, co-op line only)
- Road to VR roadmap article, 24 Nov 2023: https://roadtovr.com/dungeons-of-eternity-quest-3-roadmap-2024/ (planned features only)
- Meta blog pre-order post: https://www.meta.com/blog/dungeons-of-eternity-pre-orders-action-RPG/ (co-op line only)
- Steam store page (secondary summary; direct fetch 403): https://store.steampowered.com/app/3189340/Dungeons_of_Eternity
- ThumbCulture Quest 2 review: https://www.thumbculture.co.uk/?p=65145
- Road to VR "Design Excellence in UI" listing (secondary, unverified): https://discord.com/servers/dungeons-of-eternity-1113164402313674802 and https://podbay.fm/p/ruff-talk-vr/e/1718002800
- Checkpoint Gaming VR Corner: https://checkpointgaming.net/features/2023/10/vr-corner-dungeons-of-eternity/ (403, not read)

Not reachable or not found: r/DungeonsOfEternity, YouTube tutorials, a fan wiki, MIXED review.
