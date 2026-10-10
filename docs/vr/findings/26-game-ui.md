# 26: The sandbox's VR UI in the game: menus, swing labels, stamina bar, haptics

Date: 2026-10-09. Branch `vr`. Status: **P1 to P4 built and unit-tested. Nothing has been seen in the
headset or in the game yet.** The user's game was running, so no flat run was done either. Memory layouts
come from static analysis of `out/eboot.elf` and are marked [C] or [L] as in docs 13, 14 and 20.

## What it is

The 3D sandbox (`gpu/xr_bridge/sandbox3d`) is where the VR menus, the swing HUD and the haptics were
proven. This work carries **the same code** into the game. Nothing was rewritten:

- **Menus.** `sandbox3d/ui.{h,cpp}`, `ui_layout.*`, `ui_render.*` give the quick ring, the inventory
  grid, the weapon wheel, the wrist counters and the body slots. They run inside `bbport_xr.dll`.
  - The layouts are the game's `"game"` object in `out/vr_assets/ui_layouts.json`, hot-reloaded.
  - The art is the game's own (icon atlas and SDF menu font from `out/vr_assets/ui`).
- **HUD.** The sandbox's swing and stamina HUD moved into `sandbox3d/combat_hud.{h,cpp}`
  (`DrawSwingHud`). The sandbox's `combat_view.cpp` now calls it, and so does the bridge.
- **Data and actions.** The real data comes from the game (`src/runtime_vr_ui.c`). What the user picks
  becomes the game's own button presses.

## Architecture

```
game thread (pad read, runtime_pad.c)                    bbport_xr.dll
  runtime_vr_ui_pad():                                     UI thread (one update per headset frame)
    read stamina, quick slots, weapons, items  --ABI 8-->    GameUi::Update: menus (ui.cpp), swing labels,
      (read only, guarded)          SetGameState             haptics; Build: canvases -> UiRenderer
    executor: presses d-pad / square <--TakeUiActions--      UiLayerGpu: both eyes into the UI swapchains
  stamina hooks (runtime_vr_stamina.c) --GameEvent-->      frame loop: + transparent projection layer
                                                           GetInput: vr_ui_filter.h decides what the
                                                             game sees
```

- **Rendering (design point 1).** The bridge renders the UI itself:
  - on its own thread, with the sandbox's `UiRenderer` pipeline, on the game's VkDevice and queue (under
    the queue lock);
  - into two transparent swapchains (one per eye, `BB_VR_UI_SCALE` × the recommended size, default 0.5);
  - for the views and controller poses predicted one display period ahead, as `AcquireStereo` does.
  The frame loop shows them as a second projection layer with `BLEND_TEXTURE_SOURCE_ALPHA`
  (premultiplied), after the game's layers and the HUD quad.
  - Menus are world-anchored exactly where the sandbox anchors them: the ring at the hand, the grid in
    front, the wrist panel on the forearm, the HUD below the view.
  - The runtime reprojects the layer like the sandbox's own frames.
  - When nothing is visible no layer is submitted. If the runtime rejects the layer it is dropped first
    (`DropOptionalLayer`).
  - The UI pipeline got one switch, `SetAccumulateAlpha`: it accumulates coverage instead of keeping the
    target's alpha (the sandbox keeps its old behaviour).
- **Threads.**
  - The host calls ABI 8 from the pad thread and from the stamina hook threads; each call is a copy under
    a mutex.
  - The UI thread waits on the bridge's frame condition variable.
  - The GPU work is fenced. If the previous frame is still on the shared queue, that frame's geometry is
    skipped and the last images keep showing.

### ABI 8 (`bbport_xr_bridge.h`)

| Call | From | Content |
|---|---|---|
| `SetGameState(BbXrGameState*)` | each pad read | flags (in world, stamina, quick, items, weapons, busy), stamina/max, HP, up to 8 quick slots (item ids) + the selected slot, 4 weapon slots (R1 R2 L1 L2) + active per hand, up to 320 items (id, count) |
| `GameEvent(BbXrGameEvent*)` | stamina hooks, executor | `SWING_OPEN` (cost, AtkParam, ticket), `SWING_HIT` (paid, contact, ticket), `SWING_MISS` (cost saved, ticket), `ACTION_DONE` (kind, ok, reason) |
| `TakeUiActions(BbXrUiAction*, n)` | pad read | `USE_QUICK` (slot, item), `USE_ITEM` (item), `EQUIP` (weapon slot, item) |

Item ids are the inventory's: goods are `0x40000000 | id` (Blood Vial 0x400003E8), weapons are the plain
param id. The host passes them through `bbgpu_vr_ui_state/_event/_actions` and `Vulkan::Xr`. Older
bridges (`abi < 8`) are skipped.

## Controls in the game: one button, one action (v2)

Attacks are swings now, so every controller button has exactly one job. There are no chords and no
holds, except the recenter hold. The old L3-layer chords and the right-grip flick are behind
`BB_XR_CHORDS=1`, for debugging.

The mapping is in `xr_input.cpp`; `vr_ui_filter.h` decides what the game sees while the dashboard is
open.

| Control | Action |
|---|---|
| Left stick | move |
| Left stick click | moving: **L3, the jump**. Standing: **opens/closes the DASHBOARD** (`BB_VR_UI=1`; sent on release). Held 1 s alone: recenter |
| Right stick | camera / turn |
| Right stick click | tap opens the **item wheel** (`quick_wheel`; pick with the right hand touch/point or the stick). Lock-on is off in the `game` layout (`lock_on: false`), so R3 never reaches the game |
| A | circle: dodge, sprint (the dodge is this button, not a stick gesture) |
| B | cross: interact |
| X | square: use the current quick item (weak tick) |
| Y | triangle: blood vial |
| Left trigger | **hold the gun (L2)**; a left-hand thrust shoots (`BB_XR_TRIGGERS=switch`: swap the left weapon, one d-pad-left pulse, and no shooting). While the dashboard is open: the left ray's click |
| Right trigger | **nothing in the world** (BB_XR_TRIGGERS=attack: R1). While the dashboard is open: the right ray's click; in the game's own menu: cross |
| Right grip tap | **swap the right weapon** (squeeze and release within 0.35 s, BB_XR_GRIP_TAP_MS, no swing during the press: one d-pad-right pulse, a tick). Held grip + swing is the heavy attack (R2) and never swaps |
| Left grip | L1, transform |
| Right grip | heavy/charge modifier for swings (see the tap row above) |
| In the game's own menu (Options, pause) | either stick = d-pad (hold to repeat), A or either trigger = cross, B = circle, menu button closes (`vr_ui_filter.h`, host reports `BBXR_GAME_MENU_OPEN`) |
| Swing at a staggered enemy | visceral attack (stagger: gun parry, or a charged heavy from behind) |
| Left ☰ (menu) | Options (pause), sent on release; held 1 s = recenter. Pimax keeps this button for itself, so the dashboard has Options and Recenter buttons |
| Swing right hand | R1 light / R2 heavy (gestures) |
| Thrust left hand | L2, shoot |
| Left hand at the mouth with the grip | triangle, vial |

**What the game sees.**
- While the dashboard is open the game gets **no buttons**: the triggers are the pointers' clicks there.
  Both sticks are zeroed too (no walking, no turning, no swing or weapon swap): `block_move`, default true,
  in the dashboard menu of the "game" block of `ui_layouts.json`.
- **Stick navigation** (`stick_nav`, default true): either stick moves a focus highlight (the hover style) to
  the nearest entry in the pushed direction (0.35 s, then every 0.14 s while held); A or either trigger pulls the
  focused entry (same action as pointing and pulling); B closes it. Pointing moves the focus to what the ray
  hits; a stick move takes it back, and a trigger then pulls the focus.
- A button held when the dashboard closes stays hidden until it is released, so the trigger that
  clicked "Close" does not then swap weapons.
- `L3` while walking with the dashboard open closes it and does not jump.

### The dashboard

One panel replaces the quick ring, the inventory grid and the weapon wheel. Those stay in the layout
file and can still be switched on in `game.active`.

**Opening it.** The standing L3 click opens the dashboard 0.75 m in front, 12° below the eyes, slightly
tilted. It is **world-locked where it opened**. Point at it with **either** controller's ray; that
hand's trigger clicks. The element under the ray is highlighted, with a tick. The footer names it and
says what a click does, or why it is not available.

**What is on it, top to bottom:**

| Row | Content | A click does |
|---|---|---|
| Title | "Dashboard", the quick-click mode, `x` | The mode toggles **USE** (default) or **SELECT** (only make it the current quick item; X then uses it) |
| Hands | **What each hand holds now**, with its icon and name (LEFT HAND / RIGHT HAND), "nothing" in red when empty | — |
| Weapon slots | R1 R2 L1 L2 with icons, the held ones framed and marked "held" | A slot not held: **switch to it** (the game's own d-pad swap until that slot is held). The held one or an empty one: disabled with the reason |
| Quick items | The 6 quick slots with counts; the current one marked `X` | USE: d-pad down to it, then square, and the dashboard closes so you see it. SELECT: d-pad down only |
| Inventory tabs | All, Heal, Throw, Buff, Tools, Ammo, Weapons; `<` `>` pages | — |
| Inventory | 10 × 2 cards with live counts | An item in a quick slot (marked `Q2` and so on): use or select it through that slot. Any other item, and the weapons tab: **disabled with the reason** (below) |
| Buttons | Options, Gestures (touchpad), Tool / bullet (d-pad up), Next quick (d-pad down), Swap left, Swap right, Recenter, Close | One press of that button (Options and Gestures close the dashboard so the game's menu shows); Recenter places the screen and HUD again |

**Inventory clicks.**
- **A weapon** (weapons tab) is equipped into its hand's **held** slot, so it is in the hand at once.
  This uses the game's own equip command (below).
- **An item in no quick slot** goes into the first empty quick slot through the same command (the
  current slot when all six are full). In USE mode it is then used.
- Both need a game-thread hook that has run in the last 2 s: the in-game step patch
  ("VR FP Camera Heading (in-game step)", already in the VR set). Without it the cards are disabled
  and say so (`BBXR_GAME_CAN_EQUIP`).

**Errors are always shown.** Every action shows a head-locked message and logs its result, or why it
was not done:
- the hook is not installed;
- a game menu is open;
- the item is gone;
- the game did not take it;
- no answer within 8 s;
- busy with the previous action.

Nothing stays pending.

**Look.** `ui_game_dashboard.png` from `bbport_xr_sandbox3d.exe --offline` renders the dashboard with
made-up data and the right ray on a quick item (`ui_scene.cpp`).

### Equip paths: the game's own command (hook "VR UI Equip (HUD input)")

All facts below come from static analysis (`scratchpad/inv2`). The call itself is new and has not
run in the game yet.

- **The command** [C]: `0x1f63e00(edi = slot, rsi = record)` is what the equipment and item menus queue
  (command vtable 0x5745c20, execute 0x2018b60). It reads only `record+0x48` (the inventory index)
  and `+0x4c` (the item id with its category bits).
- **Slots**: 0..15 are the hand and armour slots (0 left 1, 1 right 1, 2 left 2, 3 right 2).
  0x10..0x15 are the quick slots.
- **What it does**, the same as the menu:
  1. If the item sits in another slot, that slot is cleared first (a weapon slot gets Bare Fists).
  2. The slot is written through the game's setter 0x18ccb90.
  3. The quick item is re-checked (0x18cf920).
  4. The player and an online session are told (0x1cfa610).
- **Where it runs.**
  - Primary site: the **in-game step hook** ("VR FP Camera Heading (in-game step)", 0x1d3be5a). It is
    the game thread, runs every gameplay frame, and is already part of the VR hook set. A second
    handler slot was added for it (`bb_hook_set_step_extra`).
  - First try: the entry of the HUD input handler 0x1a5b970 ("VR UI Equip (HUD input)"). It was
    installed in the all14 headset run but never ran during gameplay: it seems to run only while a game
    menu is up. It stays as a second site that is harmless; a request is taken once
    (compare-and-swap), by whichever site runs first.
  - The menu's own path is the command stub 0x2018b60, which only loads slot/record and jumps to
    0x1f63e00. Nothing else is set up, so a call from another point of the game thread gets the same
    call.
- **The handler** (`src/runtime_vr_ui.c`):
  - It runs only for a request from the headset.
  - It checks a living player, the menu manager `[0x5962878]` (which the command writes), the
    PlayerGameData vtables, that no game menu is open (`mm+0x3c == 0`), and that the inventory entry
    still holds that id.
  - It calls the command with a zeroed 0x60-byte record and reads the slot back.
  - It logs `VR UI: equip: 0x1f63e00(slot 0x1, inventory index 77, id 0x6acfc0): done; the slot held
    0xf4240, now 0x6acfc0`.
- **Nothing else is written.**
- **Not used:**
  - **Use from the inventory without a quick slot**: the game sets PGD+0x458, mm+0x1e8 and a flag
    after a "can use" check (0x2018f20). The dashboard puts the item into a quick slot instead.
  - **Switch weapons without the d-pad**: the swap belongs to the animation (0x1e216b0). The d-pad
    pulse is kept.

### Live check of the reader (read-only)

`scratchpad/gameui/peek.py` reads the running game's memory with ReadProcessMemory; it writes nothing.
On the user's session it showed:
- the three vtables matching;
- keyCap 64, normalCap 1984;
- 14 inventory entries:
  - Saw Cleaver 7000000 and Hunter Blunderbuss 6000000 **owned but not equipped**;
  - all four weapon slots Bare Fists 1000000 (handles 0x80800048-4b, inventory indexes 65-68);
  - armour 230000-233000;
  - Quicksilver Bullets ×20, Hunter's Mark, Notebook;
  - **no Blood Vial**;
- all six quick slots empty (-1);
- PGD+0x458 = -1.

So the offsets are right. The character simply has no weapon equipped, no vial and no quick items.
That also explains why the trigger "does nothing" (it swaps fists for fists) and why the user was not
sure they held a weapon.

**Display.** The dashboard now shows Bare Fists as an empty slot ("nothing" in red under LEFT/RIGHT
HAND). The probe's `R 1000000/1000000` means bare fists in both slots.

### Weapon swap by trigger: what was checked

The trigger's d-pad pulse runs: `xr_input.cpp` (one 120 ms pulse per pull) → the bridge's `GetInput`
(the UI filter only hides it while the dashboard is open) → `runtime_pad.c` `sample_vr` (ORed into the
game's buttons) → the first-person pad filter (`runtime_vr_fp.c`; it changes only the sticks). Nothing
in that path drops it.

**Fixed:** the left trigger also set the pad's analog **L2 to the trigger value** in swap mode. A pull
read as L2, the gun, through the analog axis, so the left trigger could shoot instead of swapping. In
swap mode L2 is now only the shoot gesture's.

**New logs.** Each pull logs `left/right trigger pulled: d-pad … pulse 120 ms`, at most once per 2 s
per hand. With `BB_VR_UI=1` (or `BB_VR_UI_PROBE=1`) every d-pad left/right that reaches the game logs
that hand's two slots and the held one:

```
VR UI: d-pad right reached the game: right slots 7000000 / 110000 (110000 = bare fists), held slot 0
VR UI: ... 0.6 s later the right hand holds slot 0 (NOT swapped: the game ignored it (busy, or the other slot empty))
```

The likely cause of "not working" is an **empty second slot**: a new character has one right weapon,
and d-pad right with only bare fists in the other slot shows little. The log settles it.

## Swing labels, stamina bar, haptics (P1)

**Stamina bar.** The real stamina comes from the data module (doc 20, `[[[[0x593E878]+0x60]+0x3B0]+0x20]`
+0x134/+0x138), read each pad poll.
- The bar is the sandbox's: −50..max, green above 0, red below.
- A white ghost shows the open tickets' costs. An orange flash shows where a hit took its cost, and a
  red blink shows a refusal.
- "SP 71 / 91" sits beside it.
- It is head-locked 0.9 m away, 24° below the view, and also above the left wrist.
- `bar: "action"` shows it only for 3 s after a swing or a cost.

**Labels.** Each swing gets a label:
1. **The swing.** `xr_input`'s gesture (an R1 or R2 pulse with a gesture fired) shows **LIGHT** or
   **HEAVY**, "paid only if it hits", with the peak hand speed.
2. **Its ticket opens.** The ticket from `runtime_vr_stamina.c` (`BB_VR_STAMINA=defer` plus the stamina
   hooks) gives the cost: "−20 if it hits".
3. **Its ticket closes.** The result is **HIT −20** (a character or object), **CLANG −20** (a wall the
   weapon bounced off) or **MISS free**.
- At 0 or below the label is **NO STAMINA** (the game refuses the attack).
- While the menus have the hands the label is **BLOCKED**.
- Without the hooks the label says "no stamina ticket: BB_VR_STAMINA=defer and the stamina hooks".

**Haptics** (right hand, `game.haptics` in the layout file):

| Event | Haptic |
|---|---|
| A swing registers | weak tick: light 0.2 × 25 ms, heavy 0.4 × 35 ms |
| The swing hits | pulse: light 0.6 × 60 ms, heavy 1.0 × 120 ms |
| No stamina | two buzzes, 0.5 × 50 ms, 0.1 s apart |
| Menu use (hover, select, empty) | the sandbox's own haptics |

With `BB_VR_UI=1`, `xr_input.cpp`'s `Fire()` no longer adds its own 0.6/1.0 × 60 ms pulse on a swing
gesture, so the weak tick is the only swing feedback and the strong pulse means a hit. Without the UI
the pulse stays.

## Memory structures (P2/P3; static analysis, read only)

All VAs are in patch-XML space. The layout is in `src/vr_ui_game.h`.

**Roots** [C]:
- GameDataMan = `[0x593B130]`. Its constructor is 0x18e4e80; the pointer is stored at 0x196c918.
- PlayerGameData PGD = `[GameDataMan+0x8]`. The same object is PlayerIns vfunc +0x1C8. 0x1a5bb12 uses
  exactly this chain before it cycles the quick slots.

**Sub-objects** [C]. The reader checks all three vtables before it trusts anything:

| Object | Address | vtable |
|---|---|---|
| EquipGameData | PGD+0x1D0 | 0x576F130 |
| EquipInventoryData | PGD+0x328 | 0x576F320 |
| EquipItemData | PGD+0x3D0 | 0x576F360 |

**Inventory** [C]. Evidence: the init 0x1ec5340, the printer 0x18dc460, add-item 0x18da3f0 and
GetItemNum 0x18cf450.

| Field | Address | Type |
|---|---|---|
| normal capacity | PGD+0x33C | u32 |
| key capacity | PGD+0x34C | u32 (0x40) |
| normal array | PGD+0x370 | ptr |
| key array | PGD+0x380 | ptr |
| tail (highest used index) | PGD+0x3B0 | s32 |

- Index `i` is `key[i]` when `i < keyCap`, else `normal[i-keyCap]`.
- An entry is 16 bytes: u32 handle (0 = empty), u32 id with category bits (weapon 0, armor 0x1…,
  accessory 0x2…, goods 0x4…), s32 quantity, u32 sort key.

**Quick items** [C]:
- 6 slots of `{u32, s32 inventory index}` at PGD+0x3E8; −1 means empty. The game cycles 6 slots
  (`mod 6`), and the debug printer prints Slot0 to Slot5.
- **The selection is an inventory index**: s32 at PGD+0x418. The active slot is the one holding that
  index.
- D-pad down (0x18df170, action 0x286) steps to the next non-empty slot and wraps. Square uses the
  selection (GetUseItemId 0x18d1ef0).

**Weapons** [C]:
- ChrAsm at PGD+0x240. Ids are u32[16] at PGD+0x2A4: 0 left 1, 1 right 1, 2 left 2, 3 right 2.
- Active right slot is s32 PGD+0x250; active left slot is s32 PGD+0x24C.
- **Open [S]:** whether an upgraded weapon's id carries its level. The UI falls back to the base row for
  its icon.

**Not used:**
- "Menu open / input blocked": a bitmask built by 0x1b6b7e0 from `[0x5962878]`. It is [L], not read
  yet; see the probe.
- PGD+0x458 `m_useRequestItemFromMenu`: the game's menu-use path [C]. **Not written.** Nothing shows
  who resets it, and a stale value would make every square use that item.

## Actions (design point 3)

Two mechanisms. Neither writes game memory itself.

**Buttons.** The executor (`src/vr_ui_game.h`, `runtime_vr_ui.c`) adds button presses to the pad state
the game reads, closed-loop on the memory above. Each press is held `press_s` and released as long
(0.06 s, `BB_VR_UI_PRESS_S`); the next press waits until the game answers.

| Action | How | Gives up when |
|---|---|---|
| **Use a quick item** | d-pad down until PGD+0x418 is the wanted slot's index, then square | the item left the slot, no answer in 0.8 s, more presses than slots, no player |
| **Make a quick item current** (SELECT mode) | the same without square | the same |
| **Switch to a weapon slot** | d-pad right or left until that hand holds the slot | the same |
| **A dashboard button** | one press of that button (Options, touchpad, d-pad) | — |

**The game's own command.** `0x1f63e00` is called by the "VR UI Equip (HUD input)" hook on the game
thread (see "Equip paths").

| Action | How |
|---|---|
| **Equip a weapon from the inventory** | into the held slot of its hand |
| **Put an item into a quick slot** | the first empty slot, else the current one; in USE mode it is then used through its slot |

Every action ends in a message: done, the reason it was not done, or no answer within 8 s.

## Phases

| Phase | Status |
|---|---|
| **P1** stamina bar, swing labels, haptics | Built. The wrist bar is the default (`hud.head` false, as the user asked). The costs and HIT/MISS need `BB_VR_STAMINA=defer` and the stamina hooks |
| **P2** quick items with live data, using them | Built. The reader was confirmed live (read-only peek, above) |
| **P3** inventory | Now the dashboard's inventory. Using items works through a quick slot (assigned by the game's command when the hook is on) |
| **P4** weapons, wrist counters, body slots | Switching weapon slots (d-pad) and equipping from the inventory (the game's command) are in the dashboard; wrist counters are on; body slots are built but off by default |
| **v2** one button one action, dashboard with rays | Built and tested offline (see the controls above) |

### Tests (all pass)

| Test | Checks |
|---|---|
| `bbport_xr_game_ui_test.exe out/vr_assets` | 204. The filter (R3 plain lock-on, standing L3, walking L3, engaged, sticky triggers, combo claims), item ids, labels and haptics, the single menus, and **the dashboard**: an old game section replaced (`.bak` kept); rays from either hand on weapon slots, quick slots, inventory cards and buttons; `EQUIP`, `USE_QUICK`, `SELECT_QUICK`, `PRESS` (touchpad, d-pad up); Recenter; disabled elements; `ASSIGN_QUICK` + use and `EQUIP_ITEM` with the hook; an action that is never answered (busy, then given up after 8 s); trigger hidden until released |
| `vr-ui-test` | 52. Reader and executor, plus select-only and plain presses |
| `bbport_xr_math_test` | 2964. Adds the L3 layer without chords and the menu-button tap |
| `hooks-test` | Adds the VR UI window (20 real bytes at 0x1a5b970 install) |
| Unchanged and passing | sandbox ui 115, combat 162, sandbox 142, locomotion 78; bridge stereo 39, swing 113, dash 92; vr-stamina 103 (C and C++), vr-head, vr-turn, vr-arms, vr-dash, vr-stereo, vr-stereo-hooks, ui-composition; `tests/test_patches.py`, `tests/test_hooks.py` |

**Picture.** `bbport_xr_sandbox3d.exe --offline --out DIR` renders `ui_game_dashboard.png`.

### Binaries

- `out/xr-bridge/bbport_xr.dll`: ABI 8; it imports `vulkan-1.dll`.
- `out/gpu/bb-probe-ui2.exe`.

## How to launch and what to test

Put `out/xr-bridge/bbport_xr.dll` next to the exe and run as usual, plus:

```
BB_VR=1 BB_VR_UI=1 BB_XR_MODE=alt
BB_HOOKS="...;VR FP Camera Heading (in-game step)"   # (already in the VR set) runs the equip command
BB_VR_STAMINA=defer + "VR Stamina Cost (behavior);VR Stamina Hits" in BB_HOOKS   # the labels' costs
BB_VR_UI_PROBE=1                          # optional: what the reader sees, every 2 s
```

`BB_XR_CHORDS=1` brings back the old L3 chords.

Steps:
1. **Equip.** Stand still and click the left stick: the dashboard opens in front.
   - The hand panels say what you hold ("nothing" now).
   - Weapons tab: point at the Saw Cleaver and pull the trigger. It is equipped into the right hand.
     The log has `VR UI: equip: 0x1f63e00(slot 0x1 …): done …`.
   - Do the same with the Blunderbuss for the left hand.
2. **Swap.** Pull a trigger outside the dashboard: `d-pad right reached the game … slots …` and `0.6 s
   later … swapped / NOT swapped`.
3. **Quick items.** In the All tab, point at the bullets or any goods: they go into a quick slot and are
   used (USE mode). The quick row shows it and X uses it.
4. **Buttons.** Options, Gestures, Tool / bullet and Recenter.
5. **Messages.** Without the hook the cards say "needs the equip hook", and a click tells why. Nothing
   stays pending.

## Risks

- **The equip call is new.**
  - It is the game's own menu command with the menu's arguments, called at the start of the HUD input
    handler on the game thread. If the game's state at that point differs from the menu's (an
    animation, an online session), the game decides; the handler then reads the slot back and reports.
  - It is behind the opt-in patch.
  - A risk that remains: equipping while attacking. The menus allow it, so the game should too.
- **The in-game step.** If it does not run (a cutscene, loading), requests fail after 2 s with "the
  game-thread hook did not run". The HUD input handler (0x1a5b970) only runs while a game menu is up,
  and there the handler refuses anyway ("a game menu is open").
- **Presses.** A press can still land where the game ignores it; that times out with a message.
- **Rendering and compositing.** As before: one small submit per headset frame, a second projection
  layer with alpha.
- **The menu-button tap.** Options is sent on release, so it is at most 1 s late. On Pimax the button is
  the system's: use the dashboard's Options.

## Files

| Area | Files |
|---|---|
| Bridge | `gpu/xr_bridge/vr_game_ui.{h,cpp}` (the UI on game state), `vr_ui_filter.h` (input rules), `vr_ui_layer.{h,cpp}` (Vulkan), `bbport_xr_bridge.{h,cpp}` (ABI 8, UI thread, layer, GetInput filter), `vr_game_ui_test.cpp`, `CMakeLists.txt` |
| Shared with the sandbox | `sandbox3d/combat_hud.{h,cpp}` (new, from `combat_view.cpp`); `ui.{h,cpp}`: `UiWorld`/`UiAction`/live data, `LoadGame`, `Build()` without lines, `Engaged`, `UsesStick`, `CloseAll`, exact-match bindings, `confirm`, consumables only count down, body-slot grab needs the hand on the slot first; `ui_layout.*`: the `"game"` object; `ui_render.*`: `UiPose`, `UiItem::Consumable`, `SetAccumulateAlpha`; the sandbox's swings are blocked by `ui.Engaged()` |
| Host | `src/runtime_vr_ui.c`, `src/vr_ui_game.h`, `tests/test_vr_ui.c`; hooks in `runtime_pad.c`, `probe.c`, `runtime.h`, `runtime_vr_stamina.c` (ticket events); `gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`, `vk_xr.{h,cpp}` |

## Menus in the desktop window (recordings)

The menus, wrist counters, toasts and held-item visuals reach the headset as a projection layer (ABI 8) that only the runtime composites, so a screen recording of the desktop window (OBS) misses them. `BB_XR_MIRROR_UI` draws them into the window too.

- **How.** The UI thread draws the same UI geometry a third time in the same submission as the two eye images, from the view the latest flat (mono) image was rendered with (`ReleaseFlatView`: head pose and frustum), into a ring of 3 bridge-owned images (up to 1920x1080, square pixels, cleared to transparent, premultiplied alpha, left in `SHADER_READ_ONLY_OPTIMAL`; `vr_mirror_ui.h` has the math). Bridge ABI 9 `GetMirrorUi` hands the latest finished image to the host; the presenter fits it into the window where the game frame landed (the same letterbox rectangle) and `BbOverlay` draws it with ImGui behind the settings menu. Cost: one more UI pass on the GPU, one textured quad in the window.
- **Option.** `BB_XR_MIRROR_UI=1|0`; default on with `BB_XR_MODE=mono`, off otherwise. It needs `BB_VR_UI=1`. Live: a file `bb_xr_mirror_ui.txt` in the working directory containing `1`/`0` (or `on`/`off`), polled once a second.
- **Limits.**
  - Mono only. In `alt` mode the window shows alternating eyes and there is no single view to draw the UI from: record in mono.
  - The view is the one of the last flat image (up to a frame old); wrist and hand menus can trail by that much in fast motion.
  - The image is premultiplied, ImGui blends straight: translucent edges come out slightly thinner, opaque parts are exact.
  - The 2D game HUD layer (`BB_XR_HUD`) is not part of this; it is in or over the game image as before.
