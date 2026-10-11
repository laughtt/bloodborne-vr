# 40 - Custom VR hands: a hand model that replaces the game's hands and holds every item

Status: phase A (design) done; stage 1 implemented in the bridge, **off by default**, not yet tried in
the headset. Code: `gpu/xr_bridge/vr_hand_model.h` (loader, poses, skinning, sockets),
`vr_held_mesh.h` (`TriBatch`, `MeshCache::Resolve`), `vr_game_ui.cpp` (`GameUi::DrawHands`),
`src/runtime_vr_fp.c` (`hide=nohands`). Tests: `vr_game_ui_test.cpp` `TestHandPoses`,
`TestHandAsset`, `TestCustomHandsInGame`.

## 1. Asset

**WebXR Input Profiles, generic hand**: `packages/assets/profiles/generic-hand/{left,right}.glb` of
https://github.com/immersive-web/webxr-input-profiles at commit
`c5356912d0c3bc43c6db8aa3926d8951a7716449` (2022-12-02, the last change to that folder).

- Licence: the repository root is the W3C Software and Document License, but
  `packages/assets/LICENSE.md` says the assets package is **MIT, Copyright (c) 2019 Amazon**, and the
  package README: "Assets are available under MIT license". The trademark note there covers
  controller models only, not the generic hand. The text is in
  `packaging/windows/licenses/webxr-input-profiles-generic-hand-LICENSE.txt` (`make_release.py`
  copies every file there into the release's `licenses/`), with a row in
  `tools/release/public/THIRD_PARTY_NOTICES.md`.
- Files: downloaded as data into `out/vr_assets_src/hands/` (plus `profile.json` and both licence
  files), copied to `out/vr_assets/hands/` where the bridge reads them. Neither folder is under git.
  sha256 `left.glb` bc677831...c2b87 (94572 bytes), `right.glb` 291790c1...f2b96 (94004 bytes).
  **To ship:** `make_release.py` has to copy `out/vr_assets_src/hands/*.glb` into the release
  assets (they are not game data, so the extractor does not make them). That is not done yet.
- Contents (checked): glTF 2.0 binary from Blender, 1360 vertices, 2314 triangles, one grey
  material, no textures needed. One skin with **25 joints named like XR_EXT_hand_tracking's joints
  without the palm** (`wrist`, `thumb-metacarpal` ... `pinky-finger-tip`). The joints are **flat**:
  all of them are children of `Armature`, each with its armature-space pose, like hand-tracking
  joint poses, so a tracked joint pose can drive the joint directly. The left file lists its joints
  in another order and has 3 extra trigger nodes, so the loader maps joints by name.
- Joint frames follow the hand-tracking convention: -Z along the bone toward the tip, +Y the back of
  the hand. Model space, right hand: fingers along -Y, back of the hand +X, thumb toward -Z. The
  left hand is the mirror in X.

## 2. Rendering and depth

What the bridge can draw into today: the game UI layer (`vr_game_ui.cpp`, ABI 8), a separate,
transparent projection layer the bridge renders on its own thread. Its pipeline has **no depth
buffer** (`Draw(..., VK_FORMAT_UNDEFINED)`). The held item (phase 2a) is drawn into
`hud_canvas.top` as CPU triangles, sorted far to near.

**Is the game's depth available to the bridge? No, not in the mode the game runs in.**

- The bridge has XR_KHR_composition_layer_depth support and depth swapchains (`stereo_depth`), but
  only for **ABI 4 AcquireStereo/ReleaseStereo**, which only the sandbox uses
  (`sandbox3d/main.cpp`).
- The game runs **alternate-eye stereo (ABI 6)**: `vk_presenter.cpp` copies each finished,
  post-processed eye image into the eye's colour swapchain (`AcquireEye`/`CopyToFlat`/`ReleaseEye`).
  No depth goes with it. The game's depth only exists in shadPS4's texture cache, as the scene
  pass's depth target, at the render resolution, in the game's own projection.
- Layer depth would not help anyway. Compositors use XR_KHR_composition_layer_depth for
  reprojection, not to depth-test one layer against another. That would need Meta's
  XR_FB_composition_layer_depth_test, which a Pimax runtime should not be assumed to have.

**Decision: stage 1 uses CPU skinning plus the existing flat-triangle path, with no depth.** A
hand is ~2300 triangles: two hands plus an item is ~5000 triangles sorted per frame on the UI thread
(well under 1 ms). `held::TriBatch` sorts the hands and the held item **together** per triangle,
so the fingers wrap the item. Side effects: the hands show through walls and draw over the menus
(the hand is usually nearer than the menu, so that mostly looks right).

**What depth needs (stage 2).** The hands have to be drawn inside the game's eye image, before it
goes to the swapchain:

1. In shadPS4, find the scene pass's depth target per eye frame (the texture cache image that is
   bound as the depth attachment of the last opaque pass before post-processing) and keep a handle
   to it with the frame (`frame->stereo`).
2. The game's near/far and projection for that eye. The runtime already builds it; the alt-eye
   `EyeView` carries the pose. Add the FOV and near/far (or the raw projection matrix).
3. Either a new ABI call (`DrawEyeOverlay(cmd, eye_image, depth_image, view, proj)`) that records the
   bridge's hand pipeline into the presenter's command buffer before `CopyToFlat`, or let the
   presenter draw the hand mesh itself from vertex data the bridge publishes per frame. Both draw
   with the game's depth as a read-only depth attachment. Mind the game's depth convention and
   format (D32 or D24S8, probably reversed or not; check in RenderDoc), and that the image is at the
   game's resolution, not the swapchain's.
4. A small Vulkan mesh pipeline in the bridge (position, normal, colour; the same flat-lit look),
   reusing `sb::UiRenderer`'s device setup.

The hand pose then has to come from the eye frame's display time, not the UI layer's. That costs a
little latency, which the game's own frame already has.

## 3. Poses

- **Controller-driven (stage 1).** Per-joint flexion about the joint's own X axis (negative =
  toward the palm), applied down a virtual chain per finger that `ParseGlb` rebuilds from the bind
  pose (metacarpals hang off the wrist). Named poses: **open** (the asset's relaxed bind pose),
  **fist**, **point** (fist with the index straight) and **trigger** (open with the index curled).
  The controller pose is a bilinear blend by the analog grip `g` and trigger `t` (both in
  `FrameInput`): open (1-g)(1-t), trigger (1-g)t, point g(1-t), fist gt. So grip closes the
  middle, ring, pinky and thumb, and the trigger closes the index.
- **Thumb touch.** It is not bound as an action yet: `xr_input.cpp` only lists `thumbrest/touch`,
  `a/touch` and friends for its diagnostic log. Stage 2: bind `thumbstick/touch`, `a|b|x|y/touch`
  and `thumbrest/touch` into one "thumb down" boolean per hand. Thumb up gives a thumbs-up pose
  with the thumb out of the fist.
- **Hand tracking (stage 2).** If `xrGetSystemProperties` reports
  `XrSystemHandTrackingPropertiesEXT.supportsHandTracking`, enable XR_EXT_hand_tracking, create two
  hand trackers and locate the 26 joints each frame. Because the asset's joints are flat and use
  the same names and frames, `joints[j] = locate(j + 1)` (skip the palm) replaces `PoseJoints`.
  Fall back to the controller poses when the joints are not valid (controllers in hand, tracking
  lost). Whether the Pimax runtime exposes it while controllers are held is unknown: log it at
  start-up.
- **Placement.** Matching axes (checked against the OpenXR grip definition: +X into the right
  palm and out of the left palm, i.e. toward the user's right for both; -Z toward the thumb along
  the fist; +Y the cross product) shows that the model's axes are already the grip frame's, for
  both hands. The hand goes at the grip pose with **no rotation**, around `anchor`, the fist
  centre computed from the bind pose (the knuckles, 3 cm palm-side and 1.2 cm toward the wrist).
  Tuning lives in `hand::kHandPlacement[2]` (per-hand position and Euler offset in the grip
  frame) plus `custom_hands.scale`. Tested: the anchor lands on the grip origin, the thumb toward
  -Z, the back of the right hand toward +X and of the left toward -X.

## 4. Holding every item

All of Bloodborne's items fall in six **hold categories**. Each has a default **palm socket** (the
item's centre and orientation in the right hand's grip frame; the left hand mirrors it across X)
and a hand pose (the share of the fist for the thumb, the index and the other three):

| Category | Items (goods rows) | Socket | Pose |
|---|---|---|---|
| bottle | vials 701-706, 1000, Antidote/Sedative, Blue/Lead Elixir, Molotovs, Oil Urn, cocktails, Accursed Brew, Brain Fluid, the chalices (6100-6599, 4105) | centred in the fist, mesh +Y turned along the fist (toward the thumb) | wrapped (0.7) |
| small | pellets, shards, keys, badges, runes, gems, coldblood, materials, Pebble, coins, Blacksky Eye, Shell, Ash... (the default) | in the fingers by the palm | almost a fist |
| paper | Fire/Bolt Paper, Notebook, Summons | pinched at the thumb and index tip | thumb and index half closed |
| bell | Beckoning/Resonant/Choir/Old Hunter Bells | handle in the fist, bell below the little finger | fist |
| tool | knives, Old Hunter Bone, Shaman Bone Blade, Tiny Tonitrus, Monocular, Lantern, workshop tools, League Staff | along the fist | fist |
| weapon | anything that is not goods (weapon ids) | none: the game draws the weapon | fist; the trigger can close the index |

- One table: `hand::CategoryFor` (goods ranges, from `out/vr_assets/items/items.json` names),
  `hand::CategoryGrip` (the six defaults) and `hand::ItemGrips()` (per-item overrides by goods row,
  empty so far). `hand::GripFor(id, tuned)` picks the per-item override first. Next comes the
  sandbox's tuned pose for the 22 goods its tables have (`MeshSpec.pos/rot_deg` plus the per-kind
  `KindTweak`, already a grip-frame socket), keeping the category's finger curls. Last comes the
  category default.
- Meshes come through the shared lookup `held::MeshFor(assets, goods)`: the sandbox's tables
  first, then `items/items.json` (the extraction for every goods with a game model; that index also
  carries a `grip` category with the same six names and a size-based scale). A newly extracted mesh
  appears in the hand with no code change. Goods with no model keep the coloured disc. Stage 2:
  feed `CategoryFor` from items.json's `grip` field when present, so there is one source.
- While an item is gripped, its hand takes `HoldPose(GripFor(id))`. Before the grip closes
  (Pending), the hand follows the controller and the item shows translucent at the socket. The
  gesture logic (mouth, blade, throw) is unchanged and still uses `HeldItem::ItemPos()`.
- Weapon hands: with `custom_hands.weapon_grip` (default true), a hand whose active weapon slot
  holds a weapon keeps the weapon grip. The game still draws the weapon at its IK-driven hand
  bone (findings/24), which lands on the controller as long as the arm IK reaches. Unarmed
  (`BBXR_ITEM_NONE`, 0, or 110000) hands follow the controller.
- **One placement, both hand modes.** `held::SocketMeshWorld` (vr_held_mesh.h) gives the item's world
  matrix from the holding hand's grip pose: the item's category socket (`hand::GripFor`, with the per-item
  overrides in `ItemGrips()`: knives, Molotovs, the coin), turned by an items.json mesh's own `rot_deg`; the
  sandbox's pose is not used in game. The scale is an in-game real size (`held::InGameTarget`/`InGameCap`:
  vial 11 cm, Molotov 18 cm, urn 15 cm, knife 20 cm, bell 11 cm, pellet 3 cm; set in `MeshCache::Resolve`
  as target / the mesh's own longest side). The custom hands
  add the mesh to their triangle batch; the game's hands (default) draw it alone, at the same place.
  The gesture point (`HeldItem::ItemPos`) is unchanged.
- **Stand-ins.** Goods with no model (papers, pellets, coldblood, marks...) get a procedural mesh of
  their category (`held::StandIn`: a 7.5 x 11 cm card, an 8-sided pellet, an 8-sided bottle, boxes for
  bells and tools), tinted by kind (drinks red, buffs orange, Fire Paper red-orange, Bolt Paper pale
  blue, throws blue); the log says once per goods which mesh or stand-in is used. The disc is a last resort.
- **More items held.** Bells (200, 201, 205, 206, 225, 226, 2050, 4312) go in the right hand and fire
  with the head gesture (`ItemInfo::bell`, hint TO THE HEAD). Marks (100, 1400, 2090) are `Kind::Crush`:
  squeeze >= `held.crush_squeeze` (0.92) for `held.crush_hold_s` (0.4 s) uses them. Keys, bullets and the
  rest are still used at once.
- **Weapon hide.** While an item is gripped the bridge sends `BBXR_UI_HELD` (hand, item id; every 2 s,
  and `a = -1` before the use / throw actions or when it is put back) so the host hides that hand's
  weapon; `held.hide_weapon` (default true) turns it off.
- **Offline look.** `bbport_xr_game_ui_test.exe out/vr_assets --render DIR [view]` writes
  `held_<goods>_{hands,game}.png` and `held_sheet.png` (a CPU rasteriser of the emitted triangles; view
  1-6 looks along the grip frame's +X, -X, +Y, -Y, +Z, -Z).
- Open: the hand model's fingers curl about the grip's Z axis (a fist through-axis of Z), the category
  sockets turn +Y to -Z (through the fist), but the sandbox's tuned rows keep their own pose (vial neck
  along +Y: held by the neck, the knife blade along the forearm). Judge in the headset; `KindTweak` is the
  place to turn them.

## 5. Grabbing in the world (design only)

- **Triggering the pickup.** The runtime only acts through the game's own buttons: the executor in
  `src/runtime_vr_ui.c` / `src/vr_ui_game.h` takes `BbXrUiAction`s from the bridge, and
  `BBXR_UI_PRESS` presses any pad button once (the dashboard's buttons use it). A world pickup is
  the game's interact action. The game's prompt names the button: use `BBXR_UI_PRESS` with it.
  Nothing is written to game memory. (`VA_ADD_ITEM` 0x1f5c8c0, the pickup / messenger-gift routine,
  exists but would skip the game's pickup logic; avoid it.)
- **Proximity.** No reader exists yet. The game shows an interact prompt (the Scaleform HUD element
  `ActionButtonTop`, findings/10) only when a pickup or door is in range. Options, cheapest first:
  (a) a "prompt visible" bit from the HUD/menu state, found the way findings/26 found the menu page
  at `[VA_MENU_MAN]+0x3c`; (b) the pickup's world position from the item-lot / treasure entity the
  prompt refers to, for a real "hand near the glow" test; (c) no game read: squeeze the grip with
  the hand low and in front while a prompt is up.
- **The gesture.** The hand within ~0.35 m of the item (or (c)'s zone) with the grip squeezed from
  open (the sandbox's grab threshold) presses interact once. A pulse, then the game's pickup list
  confirms it. Debounce it like the held items (`lockout_s`), and block it while a menu is open or
  an item is in that hand. For corpses and item glows, (b) is needed to make it feel like grabbing
  instead of a button.

## 6. Settings, enabling, testing

- Bridge: the `"game"` object of `ui_layouts.json`:
  `"custom_hands": {"on": false, "color": [0.30, 0.22, 0.17, 1.0], "scale": 1.0, "weapon_grip": true}`
  (or `"custom_hands": true`). The environment variable `BB_XR_CUSTOM_HANDS=1` or `0` overrides `on`.
  The models load once, from `<assets>/hands/` (`out/vr_assets/hands/`), the first time the option
  is on. The log says `game UI: custom hands: ... loaded` or why not.
- Runtime: `hide=nohands` in `fp.txt` hides `hands_skin` plus the skin's hands (display group 44,
  new bit 28; token `skinhands` alone). Only the weapons remain. It is a runtime change, so it needs
  `build.sh` and is untested in game.
- Headset test: close the game, copy `out/xr-bridge/bbport_xr.dll` over `out/bbport_xr.dll`, and
  run with `BB_VR_UI=1 BB_XR_CUSTOM_HANDS=1` (and `hide=nohands` after a runtime build). Check:
  the hands sit in the controllers (tune `kHandPlacement`), grip and trigger close them, a vial
  picked on the wheel sits in the fist, and the weapon hand keeps its grip.

## 7. Stages

1. **Done (this change):** asset and licence, glb loader, open/fist/point/trigger blend, CPU
   skinning, flat-lit drawing sorted with the held item, palm sockets by category and per item,
   setting off by default, `hide=nohands`, tests.
2. **Next:** game-depth occlusion (section 2: presenter hook, shadPS4 depth target, mesh pipeline);
   XR_EXT_hand_tracking with fallback; thumb touch; items.json `grip` into `CategoryFor`; ship the
   glbs in the release; `Kind::Show` so bells and keys can be held too; in-headset tuning of
   `kHandPlacement` and the category sockets.
3. **Later:** world grab (section 5), weapon hand-off (draw the weapon at the custom hand when IK is
   off), shadows or ambient tint from the game's lighting.

## 8. Alternatives the user raised: the game's own hands

**A. In the engine: finger bones on the game's hands (preferred first).** `src/runtime_vr_arms.c`
already rewrites both arm subtrees in the AnimationToModel hook. It finds `%sFinger0..4` by name,
but uses them only to bind the hand frame: the fingers keep their animated locals and follow the
hand rigidly. The hunter skeleton (`out/vr_assets/hunter.json`) has three bones per finger:
`R_Finger0/01/02` (thumb) through `R_Finger4/41/42`, parents chained under `R_Hand` (43). Both the
skin's hand mesh (FC_M_0000 group 44, 2949 vertices, dominant bone `R_Finger22`) and the arm
armour are skinned to them, so curling those 15 bones per side curls the game's own hands, with
the game's lighting, shadows and blood. What A needs:
- The analog grip and trigger in the runtime. `bbgpu_vr_hands` carries the grip poses and the
  head, and `BbXrInput` only digital buttons. Add the two floats per hand to the H1 snapshot. The
  bridge already has them: `xr_input.cpp` `analog_grip/analog_trigger`, and `BbXrStereoFrame`.
- Per-finger flexion after the IK writes the hand: for each finger bone, local rotation
  times a flex about the bone's bend axis, then the model transforms rebuilt down the chain (as
  the arm chain already is). The angle tables and the open/point/fist bilinear blend of
  `vr_hand_model.h` carry over, but the bend axis per bone has to be measured on the hunter's
  bind pose (FromSoftware bone frames, not the hand-tracking frames).
- The weapon or item grip: a fixed curl while a weapon is in the hand (the game's own grip
  animation already does that, so leave the weapon hand animated unless the user opens it).
- Runtime change: needs `build.sh` and a game restart. Not done here: the game was running and the
  task said not to run `build.sh`.

**Gloves and sleeves (slot 4).** In the extracted outfit (`am_m_2200`), slot 4 is **one mesh,
one material ("arm"), no display-mask group** (mask -1, 8404 vertices, dominant bone
`Support_R_ForeArmTwist`). There is no submesh or material to hide the sleeve and keep the glove.
The split would have to be per vertex: the glove is the vertices weighted mostly to `Hand` and the
`Finger*` bones. In the engine that means changing the vertex or index data of the loaded FLVER
(fragile). Bridge-side it is a filter at extraction time. Other outfits may differ: check each
`am_*` part's materials before ruling the split out.

**B. Bridge-side, the user's own glove mesh.** This is stage 1 of this document with the asset
swapped: extract the glove vertices (the per-vertex split above) and their skin weights from the
user's own FLVER with `scripts/vr_assets/extract.py` (nothing shipped), and map the hunter's
`Hand`/`Finger*` bones to the 25 joints the poser drives: `PoseJoints` needs a parent table and
bind frames, which `sb::Model` already has. Then draw it through the same `TriBatch` path. It
solves sleeves completely and works in menus, but has stage 1's no-depth limit and lighting that
does not match the game. The WebXR hand remains the fallback when no glove can be extracted.
