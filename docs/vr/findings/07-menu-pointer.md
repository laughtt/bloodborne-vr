# 07: VR laser pointer for Bloodborne's menus

Track: how to let a VR controller act as a laser pointer on Bloodborne's menus (title screen, character
creation, pause and equipment menus, shops, the Hunter's Dream doll and messengers, talk choices), on the
native PC port in cinema mode. The finished 1920x1080 game image is shown on a world-locked quad.
Controller aim poses already exist (`BbXrInput.aim[2]`, LOCAL space). The quad pose is computed in
`FrameLoop()` (`gpu/xr_bridge/bbport_xr_bridge.cpp:286-400`).

Date: 2026-10-09. Game: CUSA03173 v1.09 EU, the user's own dump at `C:/games/CUSA03173`.
Investigation only: no code was changed and the game was not launched.

Legend (same as 05): **[V]** verified here (read source, data file or primary page). **[R]** reported by
secondary sources, not checked. **[S]** speculation or inference.

---

## 0. Summary

- **The game has no pointer input in its menus.** **[V]** The 103 `menu/*.gfx` movies hold no mouse
  handlers. The stock Scaleform runtime in the eboot does support mouse events, but the menu logic lives in
  C++ dialog classes (`FrpgMenuDlg*`, `EzMenu*`). These classes move a "Cursor" movie clip in response to pad
  directions.
- **No public RE exists of Bloodborne's menu system.** **[V]** I searched for cheat tables, GitHub code and
  mod pages and found no BB menu structure, focus index or item-rect table. The closest related work is the DS1
  (Remastered) "FrpgMenuDlg" class family in metal-crow's Dark Souls 1 Overhaul, and TGA's DS3 table, which
  has a `MENUMAN` debug section. Neither transfers offsets to BB.
- **Recommendation.** Use a pointer that **drives the pad**, in three steps, each usable on its own:
  1. **Day 1.** Draw the laser and the dot. Add a "menu mode" in which a trigger drag moves the cursor one
     step per row height, the stick turns into clean d-pad pulses, and the trigger sends Cross.
  2. **Week 1.** Add a **GPU UI-draw tap** that finds the menu cursor sprite's screen rectangle among the
     Scaleform draws the renderer already classifies. That gives (a) reliable "a menu is open" detection and
     (b) a closed loop that steps the d-pad until the cursor sits under the laser. This is true
     point-and-click without any game RE.
  3. **In parallel, optional.** Memory RE of `FrpgMenuDlgCursor` / `FrpgMenuDlgListbox` for an exact focus
     index and item count. Prefer using that to compute an exact number of pad steps, not to write the
     index directly.
- **A one-hour experiment worth running first.** The eboot imports `libSceMouse` and the runtime already stubs
  `sceMouseRead` as "disconnected" (`src/runtime_services.c:359-365`). Report a connected mouse and see
  whether anything in the menus reacts. It probably will not, but it costs almost nothing.

---

## 1. What exists publicly

| Resource | What it has | Use for us |
|---|---|---|
| Bloodborne cheat tables, GoldHEN/shadPS4 patches | `patches/Bloodborne.xml` (in repo) and upstream ps4_cheats: FPS, post-FX, camera, debug menu, debug camera. **Nothing about menus or cursor** (grep `menu` finds only "Restore Debug Menu"). **[V]** | Only the debug menu (dev tool, not player menus) |
| "Restore Debug Menu" (Whitehawkx, auser1337) | `patches/Bloodborne.xml:1191`. The dev menu (FD4DebugMenuManager) opens with the **left touchpad side**. Needs the Nexus #253 debug fonts. **[V]** | Shows touch *position* is read somewhere (left/right halves), but by the dev menu only |
| BBDBG (stagvant, GoldHEN debug plugin) | Dev menu and freecam. Repo is now 404. **[R]** | Nothing for menus |
| TGA Dark Souls III table, <https://github.com/The-Grand-Archives/Dark-Souls-III-CT-TGA> | `CHANGELOG.md` mentions "Debug Stuff > MENUMAN > NewMenuSystem", "MENUMAN > Win64", "Menu Drop Data". **[V]** (GitHub code search) | A template for *where* a FromSoft menu manager singleton hangs. DS3 PC is a different build (Windows, CS* classes), so its offsets do not transfer |
| TGA Elden Ring table, <https://github.com/The-Grand-Archives/Elden-Ring-CT-TGA> | CSMenuMan, the newer CS* menu system with native mouse support. **[R]** | Concept only. ER's menu code is a later rewrite |
| metal-crow, Dark Souls 1 Overhaul, <https://github.com/metal-crow/Dark-Souls-1-Overhaul> (`OverhaulDLL/src/UIFeatures.cpp`) | Patches DSR's `FrpgMenuDlgFloatingPCGauge` class (allocation size, IconObj injection). **[V]** | **Same class family as BB** (`FrpgMenuDlg*`, see §2). It shows how DlgObj children are allocated and linked, which helps when reading BB's dialogs |
| DS1 mouse fix / Dark Souls Input Customizer, <https://www.nexusmods.com/darksouls/mods/1241> | Raw mouse into the camera, plus an auto cursor show/hide based on game state. **[R]** | The "detect menu state, switch input mode" idea (§5) |
| Paramdex BB `MenuPropertyLayoutParam`, <https://github.com/soulsmods/Paramdex/blob/master/BB/Defs/MenuPropertyLayoutParam.xml> | Fields: `LayoutPath`, `PropertyID`, `CaptionTextID`, `HelpTextID`. **No geometry.** **[V]** | Not useful for rectangles |
| shadPS4 issues and r/BloodbornePC | Mouse use there means keyboard-and-mouse *camera* control through pad emulation. Nobody reports menu pointer support. **[R]** | None |

Conclusion: **we would be first** to map BB's menu objects. Every structure below has to be found by us.

## 2. What the game itself contains (read from the user's dump)

Strings were scanned with throwaway Python in the session scratchpad. Offsets are **file offsets in
`C:/games/CUSA03173\eboot.bin`**. That file is a SELF (magic `4F 15 3D 1D`), so these are not image offsets.
Convert them with the segment table `scripts/prepare.py` uses, or search the mapped image at runtime.

- **Menu classes (DS1 "Frpg" lineage + EzMenu library)** **[V]** (around file offset `0x4959500`):
  `FrpgMenuDlgCursor`, `FrpgMenuDlgCursorGroup`, `FrpgMenuDlgObjCursorPos`, `FrpgMenuDlgListbox`,
  `FrpgMenuDlgButtonGroup`, `FrpgMenuDlgObjItemSelect`, `FrpgMenuDlgObjScrollBar`, `FrpgMenuDlgKeyGuide`,
  `FrpgMenuDlgEquip*List*`, `FrpgMenuDlgShopExtList`, `FrpgMenuDlgProfileSelect*`, `FrpgMenuDlgFaceEdit*`,
  `FrpgMenuDlgSelectArchitype` (character-creation origin), `FrpgMenuDlgSoulStatusList` (level up),
  `FrpgMenuDlgShortcutSelect`, `FrpgMenuDlgBloodMsgList`, ...
  Library: `EzMenuDialog`, `EzMenuDlgObj`, `EzMenuScene`, `EzMenuResourceManager`, plus a source path
  `N:\SPRJ\Source\Library\EzMenu\Source\Source\detail\Manager\EzMenuSceneManagerEventListener.cpp`.
  Manager: `SprjMenuImp` (around `0x495d3da`). Scaleform glue: `SprjScaleformImp`, `SprjScaleformPubFe*` (HUD).
  FD4 step names (`SprjStepLocal< ... >`, `FD4STEPTEMPLATEBASE_CHILD_CLASS`) sit next to them. **[V]**
  → The menu is **C++-driven**: a dialog owns DlgObjs, one of which is a cursor whose position follows a
  focus index (`ObjCursorPos`). This is the DS1/DS2 design, carried into BB **[S]**, not DS3's later CS* menus.
- **Input stack**: `DLUserInputPressDetector`, `...RepeatDetector`, `...HoldDetector`, `...FillipDetector`
  (flick), `...Suppressor`, `...Recorder`. These are virtual-pad devices. **No mouse or touch device class names
  turned up** (no `DLUserInputDeviceMouse` or similar). **[V]**
- **Scaleform GFx 4.x runtime (stock)**: full AS3 mouse API strings (`flash.events.MouseEvent`, `mouseOver`,
  `getMouseTopMostEntity`, `setMouseCursorType`), CLIK `FocusManager` (`moveFocus`, `setFocus`,
  `setControllerFocusGroup`). **[V]** This is the library's code, not proof the game routes events to it.
- **`libSceMouse` is imported.** **[V]** The port binds `sceMouseInit/Open/Read/Close` and `sceMouseRead`
  returns "not connected" (`src/runtime_services.c:359-365`, `src/import_names.inc:253-258`). Who calls it is
  unknown. Candidates: Scaleform's Orbis platform layer, the FD4 dev input, or nothing. **[S]**
- **Menu movies** (`dvdroot_ps4/menu/*.gfx`, uncompressed `GFX` v11, AS3) **[V]**:
  - **No** `mouse*`, `rollOver`, `onPress`, `onRelease`, `click` or `hitTest` strings in any of the 103 movies.
  - Each list movie has a `Cursor` / `ItemComp_Cursor_anim_NN` clip (e.g. `LevelUp_fla.ItemComp_Cursor_anim_64`,
    `BloodMessageEdit_fla.ItemComp_Cursor_anim_21`), `ItemList_N`, `ScrollBarV` and `KeyGuide` clips.
    Textures: `Title_Cursor.tif`, `MENU_Common_0000N.tga`, `KG_*.tga` (key guide icons, including
    `KG_TP_R.tga` for the touchpad).
  - Movies covering the user's targets: `title.gfx`, `profileselect.gfx`, `chrmake*.gfx` (12 files),
    `ingametop.gfx`, `inventory.gfx`, `equipment.gfx`, `levelup.gfx`, `weaponupgrade.gfx`, `shop*`/`depository.gfx`,
    `warpmenu.gfx` (headstone travel), `talkchoice1-3.gfx` (doll and NPC talk), `commandlist.gfx`,
    `optionsetting.gfx`, `messagebox.gfx`, `systemdialog.gfx`, `numselect.gfx`, `itemquantityselect.gfx`.
- **Touchpad in the vanilla game**: a touchpad *press* opens the gesture menu (`gesturetop.gfx`). **[R]** No
  vanilla menu reads touch *coordinates*. Only the dev menu patch uses the pad halves. **[V]**/**[S]** The
  touchpad is not a way in.

## 3. Approach A: game memory (read the focus, maybe set it)

**Idea.** Find the live `FrpgMenuDlg*` objects. Read the focused index, item count, visible window (scroll
offset), and the cursor/item positions in 1920x1080 space. Then either set the index directly, or (safer)
press up/down exactly `target - current` times.

**How to find it with the port's tools.**
1. **Static, about 1-2 days.** Load the extracted ELF (the `prepare.py` output that `probe.c` maps) in Ghidra
   (x86-64, image base 0 so addresses equal the patch-XML offsets). Find xrefs to the strings
   `FrpgMenuDlgCursor`, `FrpgMenuDlgObjCursorPos`, `FrpgMenuDlgListbox` and `SprjMenuImp`. FD4 classes usually
   hand back their name from a virtual or a static type descriptor, so an xref leads to the **vtable**. The
   constructor that stores `this` into a `.data` global gives the **`SprjMenuImp` singleton** (the DS3
   `MenuMan` analogue). Note the vtable image offsets.
2. **Dynamic, about 1-2 days.** The game is ordinary memory inside `bb-probe.exe`. **Cheat Engine can attach
   to it directly** (no PS4 debugger needed), as can the runtime itself.
   - The image base is a `VirtualAlloc` result (`src/probe.c:54`) and is not printed today. A one-line log of
     `image`, or reading it from the "guest offset" crash formatter (`probe.c:104`), gives it.
   - Scan for qwords equal to `image + vtable` to list live cursor and listbox objects while a menu is open.
   - Then run Cheat Engine's "changed / unchanged / increased by 1" scans while pressing down in a list.
     Look for a small int (the index) and a float pair (the cursor X/Y in movie units). Repeat in three menus
     (inventory, level up, talk choice) to confirm the field offsets are shared by the base class.
   - Look for the item count and the visible-window start next to the index (the scroll lists).
3. **Write test.** Write the index while a list is open and check that the highlight, help text, item
   preview and key guide all update. They likely will **not** fully update, because the dialog's
   "focus changed" handler runs on pad input (§3 risks).

**Feasibility:** medium. The class names are present and the process is attachable, which makes this far
easier than on a PS4. But every menu type (lists, icon grids, sliders, tabs, talk choices, the
character-creation multislider) is its own subclass, so coverage grows menu by menu.

**Effort:** about 3-5 days to read index and count for lists. About 1-2 weeks to cover grids, tabs and
character creation and to get item rectangles.

**Risks:**
- Writing the focus index skips the dialog's change callbacks: stale help text, a wrong preview model, and in
  the worst case an action on the wrong item. **Prefer reading only and emitting pad steps.**
- Item rectangles may exist only inside Scaleform (the C++ side sets frame or index, and the movie lays out
  rows). The renderer approach (§4) gets them more cheaply.
- Offsets are tied to v1.09 EU. Fine for this port, which targets one build.

## 4. Approach B: image or draw based (we own the frame)

### B1. Pixel CV on the final image (not recommended as the main path)
Read back the 1920x1080 UI image and find the highlight bar (BB's cursor is a bright, gold-white, low-alpha
horizontal gradient bar or an icon frame). Cost: a small compute shader or a downsampled readback each frame,
plus 1 frame of latency.
**Robustness: poor to fair.** The highlight differs per menu (bars, icon frames, tabs, slider knobs), it
animates (pulsing alpha), and the scene shows through the menus. This needs per-menu tuning, and moving
game backgrounds cause false hits. Use it only as a debugging aid.

### B2. GPU UI-draw tap (recommended)
The renderer already isolates Scaleform draws: `UiComposition::MovieShader()` lists the Scaleform vertex
shader hashes (`renderer_vulkan/ui_composition.h:27-30`), and `NativeViewport()` matches their 1920x1080
viewport. **[V]** For each such draw in a frame, record:
- the bound texture (guest address plus size/format, or a content hash taken once), and
- its **screen-space bounding box**. Either decode the vertex buffer and apply the Scaleform 2D matrix from
  the VS constants on the CPU (Scaleform's batch VS is `pos.xy = dot(mat[0..1], pos)`; check this in the dumped
  shader **[S]**), or add a recompiler-emitted atomic min/max of `gl_Position` into a small SSBO for draws with
  those VS hashes. The second way is exact and needs no vertex decoding.

From this table:
- **Cursor rectangle.** The cursor sprite is the draw whose texture is the cursor texture (`Title_Cursor`,
  the `MENU_Common_*` cursor frame, the per-movie `ItemComp_Cursor_anim` art). Learn the set once with a dev
  "learn" mode: press down and record which textured draw moved by about one row while everything else stayed
  still. Store the hashes in a small JSON per game build.
- **Menu open.** At least one cursor-texture draw this frame → menu mode. Gameplay HUD movies (`fe.gfx`,
  gauges) never draw the cursor.
- **Row pitch.** The cursor's displacement after a single step, learned live.
- Optional: glyph draws from the font atlas give the text-line boxes, which are the item rectangles, for an
  "item under the laser" highlight even before the cursor moves there.

**Closed loop (pointer → pad).** Each frame in menu mode:
```
P   = laser hit on the quad, in 1920x1080 guest pixels
C   = cursor rect (from the tap), pitch = learned row/column step
if P inside C (padded by pitch/2):  idle; trigger press → Cross
else:
   dir = dominant axis of (P - center(C)), only if |delta| > pitch*0.6 on that axis
   emit one d-pad pulse (2 game frames down, then up)
   wait until C changes or 150 ms passes (no-move → list end / wrap guard: stop and don't repeat)
```
- **Scrolling lists:** when the cursor stays at the list edge while rows scroll, keep pulsing only while the laser
  is beyond the edge (an auto-scroll zone). Stop as soon as a pulse produces no visible change.
- **Wrap-around:** if a pulse moves the cursor the opposite way (a jump to the other end), stop.
- **Sliders and steppers** (options, character creation): the cursor rect is the whole row, so horizontal
  laser motion inside it emits nothing. Left/right values change only with the stick, or with an explicit
  "drag on row" gesture (trigger held, horizontal drag = left/right pulses).
- **Tabs** (equipment and inventory categories): a laser over the tab strip with the trigger → L1/R1 pulses
  toward the hovered tab. Learn the tab strip textures the same way.
- Click = Cross. B/back = Circle on the other controller's trigger or face button (unchanged).

**Feasibility:** high. This uses data the renderer already sees, needs no game RE, and works the same in
every menu that draws a cursor sprite.
**Effort:** 3-5 days (draw tap + learn mode + closed loop + JSON), plus tuning.
**Risks:**
- Scaleform batching may merge the cursor with other sprites in one draw. Then split by the texture region
  (UVs), or fall back to the SSBO min/max per texture.
- The cursor may be drawn by a *masked* or animated clip (multiple frames, a glow pass). Take the union box of
  the learned set.
- The UI draw path runs on the GPU command thread. Keep the tap a fixed-size per-frame array, with no
  allocations and no readback stalls (read the previous frame's SSBO).
- Pulses must be at least 2 game frames long so the game's PressDetector sees them (the port's `BB_PAD_FILE`
  injection proves holding works; time it against the game's frame, not the headset's).

## 5. Approach C: cheap fallbacks and hybrids (shippable first)

| Idea | How | Value | Effort |
|---|---|---|---|
| **Laser ray + dot** | Ray from `aim[hand]` against the quad plane (`screen_pose`, size `screen_width x height`). The hit gives `uv` → guest pixels `uv*1920x1080`. Draw the dot as a small quad layer at the hit point (same way as the hand markers, `bbport_xr_bridge.cpp:402+`) and a thin beam quad. Convert the aim pose into `g->space` with `xrLocateSpace` when the screen is in STAGE space. | Feedback. Needed by every other option | 0.5 day |
| **Laser "drag" = d-pad** | Trigger held: accumulate hit-point motion; every `pitch` px (default 60 px vertical, 120 px horizontal, tunable) emit one d-pad pulse in that direction. Quick release with little motion = Cross. | Feels like scrolling a phone. Works in every menu, no detection needed | 0.5 day |
| **Stick → d-pad in menu mode** | With a deadzone of 0.5, hysteresis, a first repeat at 350 ms then every 120 ms, and only the dominant axis. VR sticks are noisy and diagonal, and this gives clean single steps. | Fixes "painful" navigation directly | 0.5 day |
| **Menu-mode detection** | Day 1: a manual toggle (for example, a click on the off-hand stick, or auto-on for 10 s after Options). Week 1: from the UI-draw tap (cursor sprite present, §4). Later: from `SprjMenuImp` state (§3). | Stops the trigger and drag mapping from interfering with combat | toggle 0.2 day |
| **Touchpad** | The game ignores touch coordinates (only the press, for gestures). | None, except mapping the gesture menu to a button | — |
| **sceMouse experiment** | Return `connected=1` with dx/dy/buttons from the laser in `mouse_read` (`runtime_services.c:360`), and log whether `sceMouseOpen` is ever called. | If the FD4 or Scaleform layer forwards mouse input, the movies still have no handlers, so expect nothing. **Probability ≤10%** **[S]**, cost about 1 h | 1 h |

**Where it plugs in.** The bridge already owns the poses and the quad pose. The pointer logic (hit test,
mode, pulse generator) belongs in `xr_input.cpp` / the bridge, which outputs the d-pad and Cross bits through
the existing `BbVrPad` → `sample_vr()` path (`src/runtime_pad.c:278`). No new game-side interface is needed.
For the closed loop, the renderer exports the cursor rect (one small struct per frame) to the bridge.

## 6. Recommended plan

**Day 1 (ship):**
1. Laser ray + dot on the quad, from the dominant hand's `aim`, with uv → guest pixel mapping. Log hit pixels.
2. Menu mode with a manual toggle: in menu mode the stick becomes d-pad pulses with repeat, a trigger drag
   becomes d-pad steps, a trigger tap becomes Cross, and the off-hand trigger becomes Circle. Haptic tick on
   each emitted step (the existing haptics path).
3. Run the 1-hour sceMouse experiment and record the result here.

**Week 1:**
4. UI-draw tap in `vk_rasterizer.cpp` for the `MovieShader` hashes: per-draw texture key + screen bbox
   (SSBO min/max, or CPU decode). Dump it with an env var (`BB_UI_DRAW_LOG`) for one frame on a trigger file,
   in the same style as `BB_SCENE_DEBUG`.
5. Learn mode: mark cursor textures per menu family (title, profile select, char-make lists, pause/ingame top,
   inventory/equipment grids, level up, shop, talk choice, warp, options, message box). Save to
   `bbvr_menu_cursors.json`.
6. Automatic menu mode (cursor present) + the closed-loop point-and-click from §4, with edge, wrap and
   no-move guards. Keep drag mode as the fallback when no cursor is detected.

**Weeks 2-3 (optional, in parallel by someone with Ghidra):**
7. Static xrefs to `FrpgMenuDlgCursor` / `FrpgMenuDlgListbox` / `SprjMenuImp` → vtables and the singleton.
8. Cheat Engine on `bb-probe.exe` (log the image base first) → focus index, count and scroll start for
   lists. Use them to replace closed-loop guessing with exact step counts and to give an exact "menu open"
   flag. Do not write the index unless tests show every callback still fires.
9. Character creation's multislider and icon grids: map them to drag gestures (horizontal drag on a focused
   slider = left/right pulses, scaled by distance).

**Concrete next steps for whoever picks this up:**
- Grep targets in the extracted ELF: the strings `FrpgMenuDlgCursor`, `FrpgMenuDlgObjCursorPos`,
  `FrpgMenuDlgListbox`, `FrpgMenuDlgCursorGroup`, `EzMenuDialog`, `SprjMenuImp` (SELF file offsets
  around `0x49595b8`, `0x49596e9`, `0x495d3da`, `0x48a2eb4`; convert them to image offsets first).
- Cheat Engine scan recipe: open the inventory → "unknown initial value (4 bytes)" → press down → "increased
  by 1" → press up → "decreased by 1" → close and open → "unchanged" ... then "find out what accesses" on the hit
  leads to the owning object and the vtable.
- Shader side: dump one menu frame with `BB_SCENE_DEBUG`, list the `MovieShader` draws with their textures,
  and confirm the Scaleform VS matrix layout before choosing CPU decode or the SSBO.

## 7. Risks and open questions

- Closed-loop navigation can overshoot in lists that accelerate on hold. Always use single short pulses and
  wait for the cursor to move.
- Some confirmations need a held Cross or a second dialog (for example, "Are you sure?" message boxes). That's fine: the
  loop just keeps working on whatever cursor is visible.
- In-world "menus" (the doll, messengers, lamps) start with Triangle/interaction prompts in the world, which
  is gameplay, not a menu. Only the talk-choice or menu dialog that opens afterwards gets the pointer.
- The headset-locked mode (`BB_XR_HEADLOCK`) makes the pointer useless (the screen moves with the head).
  Allow the pointer only on the world-locked quad.
- When the HUD moves to its own layer (PLAN Phase 5), the same tap still works: it reads Scaleform draws,
  not the composed image.

## Cross-links

- **05-bloodborne-engine-modding.md §5**: Scaleform HUD and menus, and the UI pass boundary this tap reuses.
- **PLAN.md Phase 5**: HUD and menus on their own quad. Menus and loading screens go to a world-space panel,
  which is the natural home for the pointer.
- **06-crosscheck.md C7**: the BB class names are analogies unless read from the eboot. §2 here reads them
  from the eboot.
