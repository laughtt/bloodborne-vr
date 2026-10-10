# 36 - Consumables as hand-held objects: the item wheel, rub-on-the-blade, bring-to-the-head

Date: 2026-10-10. Track: VR body and interaction. Built and tested in the sandbox
(`gpu/xr_bridge/sandbox3d`, headless tests); nothing run against the game.

Code: `consumables_config.{h,cpp}` (items, sample inventory, settings), `consumables.{h,cpp}` (wheel,
gestures, player state, log), `scene.cpp` (wiring, drawing, glow, HUD), `throwables.cpp` (`TakeFor`,
`ThrownGoods`), `scripts/vr_assets/consumables_extract.py` (`extract.py --consumables`). Test:
`consumables_test.cpp` (`bbport_xr_sandbox3d_consume_test.exe out/vr_assets`).

## 1. Design

R3 held opens a wheel of 3D objects (arc or shelf, below eye level, configurable); you take one with a
grip. What happens next depends on the item: throw (the throwables of doc 35), rub along the weapon
(weapon buffs), bring to the mouth (ingested things, bells), crush in the fist (marks, bone blade).
Weapons are put away while the wheel is open and the hands are free; they return when the wheel closes
with nothing held or the item is used, thrown or dropped. The old card wheel stays available
(`"wheel": "cards"`). Settings and keys: sandbox README.

## 2. Which gesture per item (data from the dump)

`items.json` / `effects.json` show what each good is: Fire / Bolt Paper and the Empty Phantasm Shell are
weapon buffs (SpEffect 2200 / 2220 / 2210, 60 s, +80 fire / lightning / arcane). **Bone Marrow Ash is
SpEffect 2230 "weapon buff [gun]"** (wepParamChange 2), so it buffs the left (gun) weapon, taken with the
right hand. Old Hunter Bone, Choir Bell, Phantasm Shell, Tonitrus, Executioner's Gloves, Blacksky Eye
and Accursed Brew are "not consumed" but cost quicksilver bullets (`bulletConsumeNum` 4, 7, 3, 6, 3, 1,
2); the sandbox charges those bullets. Reusable (`isConsume` false): Hunter's Mark, the bells.

Decisions where the request left room: bells are rung next to the ear (head gesture, not used up);
Hunter's Mark / Bold Hunter's Mark (return to the Dream, a no-op in the sandbox) and the Shaman Bone
Blade are crushed (a hard fist for 0.5 s); Old Hunter Bone is brought to the head (16 s buff).
Thresholds: grab 0.6 / release 0.35 squeeze, 6.5 cm; mouth radius 12 cm, below the eyes by 3 cm, 0.3 s;
rub contact 10 cm, 50 % of the blade within 2.5 s (either hand takes a paper, it is rubbed onto the other hand's weapon), the first 25 % of the weapon is handle; crush 0.97 for
0.5 s. Durations come from `effects.json` (60 s default where it has none; Madman's Knowledge is
instant).

## 3. Models

`EquipParamGoods.modelId` is 0 here too, c0000's chrbnd has no item props, and no `parts/` file is an
item. As for the throwables (doc 35 section 2) the only 3D data is the effect meshes of
`sfx/frpg_sfxbnd_commoneffects.ffxbnd.dcx`. Candidates found by material name (guesses):
`s10009` (material `hp`, a vial, otherwise only in the Numbing Mist effects) for the Blood Vial,
`s10015` / `s10022` (`ariannas_blood` vials) for Iosefka's Blood Vial / Blood of Adeline, `s10023`
(`Medicine_of_lead` flask) for the Lead Elixir, `s10025` (`c2730_Bell_cloth`, 13 cm) for the four bells.
The throwables keep their doc 35 meshes. Not found: papers, pellets, elixir bottles, ash packet, marks,
bones (`wp_a_0900`, 14 cm, material `c2910_Body`, was not identified). Those use procedural stand-ins
(sheet, pellet, round flask, syringe, packet, tablet, bone), tinted per item. Extracted meshes stay in
`out/vr_assets/` (git-ignored).

## 4. Verified

The tests cover every rule above (see the README list), including the wheel reopening after use,
put-back, drop, throw and a close with an item still held, and the weapons being stowed and restored.

## 5. Known gap for the game port: weapons and the world

In the sandbox the held weapons collide with the props, the floor and the enemy (`physics_weapon`). **In
the game the weapons do not collide with the world like this**: the hit shapes only act during attack
frames, so a weapon passes through walls and bodies. For the game: use the weapon segment (grip to far
end) as pure geometry for the rub test, do not depend on physics contacts, and decide separately whether
VR needs weapon-world collision (stowing the weapon while the wheel is open already avoids hand-versus-
weapon clashes).

## 6. Simplification and the "wheel does not open" fix

Decisions: the wheel holds consumables only (the throwables are on the table / rack again;
`throwables_on_wheel` and `hide_rack` bring the old behaviour back); a weapon buff picked from the wheel
applies at once to the weapon in the other hand (the right one when that hand has none), the rub gesture is
behind `buff_rub` (default off); heals, cures and body buffs are still brought to the mouth.

The wheel that "sometimes did not respond": found by random button mashing in the tests
(`TestMashThenWheel`, `TestGripDoesNotBlock` in `vr_game_ui_test.cpp`, `TestMashThenClick` in
`consumables_test.cpp`; after the mashing everything is released and a clean R3 must open the wheel).
- Shared menu code (`sandbox3d/ui.cpp`, also the game wheel): `PressedExact` refused to open a plain
  binding (`r3`, `l3`) while any other binding's modifier was held. When a layout has a grip chord
  (`right_grip+b`, `left_grip+x`) the grip counts as that modifier, and the hand that clicks R3 / L3 is
  usually squeezing its grip, so the menu stayed shut ("other modifier held 1" in the log). A binding without
  modifiers is now only blocked by held buttons, not by a squeezed grip or trigger.
- `vr_ui_filter.h`: the R3 / L3 episodes (`r3_down`, `r3_held_fired`, the tap pulses, the L3 class) were not
  cleared while the filter was off (no player in the world) or `r3_menu` was off; a press that ended then
  left the next one half done (an instant "hold" or a phantom tap). They are reset now, and a clock that goes
  back restarts the hold.
- `consumables.cpp`: a press while the head is not tracked used to be dropped silently; it is kept for 1 s
  and logged (`wheel_wait`), and every R3 press is logged (`R3 pressed, wheel open/shut`) so a missing press
  shows in `sandbox3d_consumables.jsonl` (no event = the press never reached the wheel).
The sandbox wheel's own toggle (open flag, press edge) did not get stuck in 12 rounds of mashing; the
14:11 run's log had no `not opened` line, so the stuck case there could not be reproduced offline.
