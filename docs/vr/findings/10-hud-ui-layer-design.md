# 10: VR HUD and UI layers: Scaleform split, menu detection, cursor tracking

Track: design for PLAN Phase 5 (HUD and UI) and for the "UI-draw tap" that 07 §4 B2 recommends. It covers
how the renderer separates Bloodborne's Scaleform UI from the 3D scene today, how to turn that into separate
OpenXR layers (HUD, menu panel, fades, movies, world markers), and how to detect menus and the highlighted
element for the laser pointer.

Date: 2026-10-09. Branch `vr` at bb741e9. Other engineers have uncommitted ABI 3 / Phase 2 work in
`gpu/xr_bridge/*`, `vk_xr.*`, `vk_presenter.cpp`, `bbgpu.*` and `runtime_pad.c`, so line numbers in those files
may move. Line numbers in `vk_rasterizer.cpp`, `vk_temporal_upscaler.*` and `ui_composition.h` are from the tree
as read today.

Method: read-only. I read the code. I decompiled the five Scaleform vertex shaders and candidate pixel shaders
from the port's own shader cache (`user/cache/CUSA03173/*.spv`, using `spirv-cross` into the session scratchpad).
I read strings from the user's `C:/games/CUSA03173\dvdroot_ps4\menu\*.gfx`. I measured pixels in the 61 raw
dumps behind `out/vr_frames/*.bmp` (guest display buffer and presented image, 1080p output; title → FMV →
Hunter's Dream → equipment menu). Nothing was built or launched.

Legend: **[V]** verified in code or game data. **[M]** measured in the captured frames. **[L]** likely (strong
circumstantial evidence). **[S]** speculation or estimate.

---

## 0. Summary

- **The 3D/UI boundary is exact and already in the code [V].** The first draw of a frame whose vertex shader is one
  of the five Scaleform hashes, with one RGBA8 1920x1080 target that is not a display buffer, starts the UI
  phase (`TemporalUpscaler::OnDraw`, `vk_temporal_upscaler.cpp:1304-1344`). Everything 2D comes after it, in one
  contiguous phase, into the tonemapped LDR target, with the scene's D32S8 as stencil for masks.
- **Everything 2D is Scaleform [V], including fades and the FMV host:**
  - `fe.gfx`: the gameplay HUD, including the world markers: lock-on `TargetCursor`, enemy HP bars and damage
    `EnemyTag0..7`/`Damage`, co-op tags `RemotePCTag0..6`
  - `caption.gfx`: subtitles
  - `fade.gfx`, `black.gfx`: fades
  - about 100 menu movies
  - `nowloading.gfx`: loading screens
  - `movie.gfx`: the FMV host, a `MENU_DummyMovie` bitmap. The video comes in as AvPlayer NV12 frames, and a
    YUV→RGB pixel shader `9b827594` exists in the cache.
- **At 1080p output there is no separate UI image today [V].** Only non-1080p outputs redirect UI draws into
  `ui_image`. At 1080p the HUD is drawn straight into the guest frame. VR needs the redirect at every output size.
- **The game's alpha is not coverage [M][V].** Scaleform's straight-alpha pixel shader, with SRC_ALPHA /
  ONE_MINUS_SRC_ALPHA on the alpha channel too, leaves `A = As² + Ad(1−As)`. Measured: opaque white HUD text keeps
  alpha 234-249 and semi-transparent bars drop to 192-195. A transparent `ui_image` would therefore get alpha that
  is too low. **Fix:** in VR mode, override the alpha blend of UI pipelines to ONE/ONE_MINUS_SRC_ALPHA (§1.5).
  The result is a premultiplied HUD with true coverage, at zero runtime cost.
- **Exact per-sprite screen rectangles come from the CPU, with no shader change [V].** Scaleform vertex shaders
  apply a 2x4 matrix from constant dwords 8..15, or from per-batch 96-byte entries indexed per vertex. Vertex and
  constant data are ordinary host memory in this native port.
- **Generic "menu open" signal [V].** Every menu movie embeds `KG_*` key-guide bitmaps and a `Cursor` clip:
  title 13 KG, equipment 13, pause `ingametop` 7, talk choice 7, messagebox 10, level up 11, inventory 13,
  warp 9. `fe.gfx`, `caption`, `fade`, `black` and `movie` embed none. Learning the about 20 KG texture hashes
  once gives a menu flag for all menus. The `Cursor` art, learned per menu family, gives the highlight rectangle.
- **World markers are the hard part.** The game projects them on the CPU. With 08's render-side head pose
  (H1/H2), it is unknown whether the HUD code projects with the game camera or the render camera. Test T0 (§2.6)
  answers it on day one of Phase 2. The final design extracts marker draws into their own image and places one
  quad per marker at its scene depth.
- **Sequencing:** land the HUD split together with Phase 2 (`BB_XR_MODE=mono`, projection layer plus depth).
  Otherwise the runtime reprojects the HUD with the world's depth (ASW/SSW smear).
- **Side benefit [L]:** handing over scene N and HUD N at the flip of frame N removes one game frame of latency.
  Today's cinema path shows the display buffer, which holds frame N−1 (§1.6).

## 1. Today's UI path, exact boundaries

### 1.1 Frame order, targets and detection

| # | Pass (game order) | Target | How the port recognises it | Code |
|---|---|---|---|---|
| 1 | **Display pass**: copies the previous frame's UI target into a display buffer (sRGB) | guest display buffer; with `display_redirect`, the port's output-size display image | first pass whose color target is a registered display buffer | `vk_rasterizer.cpp:957-973` (frame start: `OnDisplayPass`, `OnFrameStart`), `vk_temporal_upscaler.cpp:1800-1853` (redirect), `1864-1890` (samples `ui_image`) [V] |
| 2 | Shadows | 4096² D32 | — | `docs/upscaler.md` frame table [V] |
| 3 | G-buffer | 6 MRT + **scene D32S8** | ≥5 MRT + depth → `gbuffer_draw`; 864-B constants → `CameraMotion::OnConstants` | `vk_rasterizer.cpp:265-270`, `2096-2101`; `vk_camera_motion.cpp:221-241` [V] |
| 4 | Lighting, scene color RGBA16F, fog, half-res FX, post/bloom | — | `OnSceneColor` (274-295); at 1080p the HDR upscale runs at CS `0x9a9cf8a9` | `vk_temporal_upscaler.cpp:360-370` [V] |
| 5 | Tonemap → RGBA8 1920x1080, then game AA ping-pong | `ldr_target` = last render-size RGBA8 before the UI | tonemap/post VS `0x0b0acf50` is *not* UI | `OnColorTarget` 1291-1302; `tests/test_ui_composition.cpp:20` [V] |
| 6 | **UI**: HUD, subtitles, menus, fades, loading screens, FMV quad | **same RGBA8 target** + scene D32S8 (stencil masks) | first draw with VS ∈ {`34e8a281`,`81d336ce`,`09957251`,`24042a9b`,`a400228b`}, 1 MRT, RGBA8, render size, not a display buffer | `vk_rasterizer.cpp:979-984`, `1194-1199`; `vk_temporal_upscaler.cpp:1304-1344`; `ui_composition.h:25-30` [V] |
| 7 | EOP flip of the buffer written in step 1, which holds frame N−1 | — | `SubmitFlipInternal` → `PrepareFrame` on the GPU thread; `Flip` → `Present` on the present thread | `driver.cpp:561-583`, `292-299`; `vk_presenter.cpp:359-500`, `750-934` [V] |

There are two modes at the boundary:

- **1080p output** (`Scaled()` false, `vk_temporal_upscaler.cpp:1287-1289`). `OnDraw` returns at 1307. Nothing is
  redirected. The HUD goes straight into the guest LDR target, and a HUD-free scene exists only until the first UI
  draw. `ui_trigger_vs` (`34e8a281`) only turns jitter off (1306). [V]
- **Other outputs** (or `BB_RENDER_RES`). `UiComposition::Choose` (`ui_composition.h:13-18`) picks one of:
  - **Temporal**: `RunScaled` (1528). TAA, DLSS or FSR4 write `output_image`, which is blitted into `ui_image`
    (1641-1661). FSR3 writes `ui_image` directly (1710).
  - **Copy**: `RunUiOnly` (1461) blits the background into `ui_image` (1503-1513).

  Then `ui_phase` turns on. Every later draw into `ui_color` is redirected into `ui_image` (`RedirectColor`
  1794-1798), and its depth/stencil into `ui_depth_image` (`RedirectDepth` 1856-1862). `PrepareUiDepth`
  (1403-1459) blits the scene depth and clears stencil to 0. Viewports and scissors are scaled by `target_scale`
  (`vk_rasterizer.cpp:2904-2957`, `3261-3297`). So `output_image` is the scene without HUD (TAA/DLSS/FSR4 only),
  and `ui_image` is scene plus HUD. [V]
- The **ImGui overlay** is separate. `Presenter::Present` draws it on the swapchain image after the game frame
  (`vk_presenter.cpp:879-880`). The VR cinema copies that window image (the `Xr::Get()` block at 904-911 in the
  working tree, `CopyToFlat` 684-747). The presenter TODO at 896-903 already plans an
  `AcquireOverlay`/`ReleaseOverlay` quad for ImGui. The HUD layers below generalise that mechanism. [V]

### 1.2 Scaleform shaders (decompiled from the cache)

All five vertex shaders write `gl_Position.z = 0, w = 1`, with no clip-disable conversion. So screen pixels are
`NDC · (xscale, yscale) + (xoffset, yoffset)` from `regs.viewports[0]`: 960/960 and −540/540 for the native
1920x1080 viewport. [V]

| VS | Inputs | Position | Constants (VS buffer 0 unless noted, dwords) | Role |
|---|---|---|---|---|
| `34e8a281` | attr0 = clip-space xyz, attr1 = color | attr0.xy, already NDC | none | first UI draw of each frame (`ui_trigger_vs` default, `vk_temporal_upscaler.h:214`) [V]; solid rectangles and clears [S] |
| `81d336ce` | attr0, attr1 passed through; attr2 = position | `M·(x,y,0,1)`, M = dwords 8..15 (2 rows) | 0..7 color transform, 8..15 matrix | single shape with vertex color/UV [S] |
| `24042a9b` | attr0 passed; attr1 = position | M = 8..15 | 0..3 add, 4..7 mul (if paired with the PS below), 8..15 matrix, 16..23 texgen (UV from position) | bitmap fills [L] |
| `09957251` | attr0 = color, .z = batch index as UNORM8; attr1 = position | per-batch M | entry `i = round(255·attr0.z)`, 6×vec4: [add, mul, M0, M1, texgen0, texgen1] | **batched**: one draw, many sprites, each with its own matrix [V] |
| `a400228b` | attr0 = position; attr1.x = batch index (uint) | per-batch M from **buffer 1** | 2×vec4 per entry: [M0, M1]; color output = 1 | batched solid shapes, probably stencil masks [S] |

Pixel shaders [V] (pairing with a VS is **[L]** until a capture):

- `3bf7d476`: `c = tex(uv)·attr2 + attr1`, alpha × attr0.a, **RGB premultiplied** on output.
- `babda34d`: the same with **straight** RGB.
- `9b827594`: two textures (Y and UV planes), BT.709 limited-range YUV→RGB (1.1644, 1.7927, −0.2133, −0.5329,
  2.1124), alpha 1. This is the FMV shader. AvPlayer writes NV12 frames (`avplayer_source.cpp:262-264`).

07 §4 B2 marked "the batch VS is `pos.xy = dot(mat[0..1], pos)`" as [S]. It is now [V], plus the batch indexing.

### 1.3 What each element is, and its VR class

| Element | Movie / clip names [V] | Screen or world | VR class |
|---|---|---|---|
| HP and stamina bars, quick items, vials, bullets | `fe.gfx` PlayerHUD, GaugeBase/Gauge1/Gauge2/BarFrame, ItemTop/Item_0..12, TriDrug, NAmmoItem | screen, top-left, about (110..520, 40..220) [M] | HUD |
| Blood echoes, insight | TotalSoul/GetSoul, HeroPoint | screen, top-right, about (1580..1780, 60..115) [M] | HUD |
| Boss bar, item pickups, action prompts, status gauges, timers, area name, autosave | BossTop, GetItemList, ActionButtonTop, ConditionGaugeList, TimerText/CautionText, MapName, AutoSaveIcon | screen | HUD |
| Banners (YOU DIED, PREY SLAUGHTERED…) | TextEffect_Dead/KillDemon/KillTarget/SoulGet/BlackClear/YouWin/YouLose/Draw | screen, centre band | HUD |
| NPC dialogue, subtitles | `fe.gfx` TalkWindow/@Name; `caption.gfx` nCaptionWindow | screen, bottom | HUD (in THEATER they are part of the image) |
| **Lock-on reticle** | `fe.gfx` TargetCursor | **world**, projected by game code | MARKER |
| **Enemy HP bars, damage numbers** | EnemyTag0..7, Damage/dDamage | **world** | MARKER |
| **Co-op / invader tags** | RemotePCTag0..6 | **world** | MARKER |
| Fades | `fade.gfx` (FadeIn/Normal/FadeOut, NowLoading_1), `black.gfx` | full screen | FADE |
| Loading | `nowloading.gfx`, `nowloading2.gfx` | full screen | 2D |
| Menus | title, profileselect, chrmake*, ingametop (pause), inventory, equipment, levelup, shops, warpmenu, talkchoice1-3, messagebox, systemdialog, optionsetting… (103 `.gfx` files, each with `KG_*` and a `Cursor` clip) | screen or full screen | MENU |
| FMV | `movie.gfx` + PS `9b827594` + AvPlayer | full screen | THEATER |

### 1.4 What the frames show [M]

The frames are `out/vr_frames` (from the raw dumps; display buffer = copy of the UI target):
- Title: a full-screen Scaleform image. The "Play Online" highlight is a gold gradient bar at about
  (800,660)–(1180,700). "Play Offline" sits about 40 px lower.
- Hunter's Dream: the HUD covers about 3% of the screen, in the corners. The alpha channel is 255 on every scene
  pixel and lower only under HUD elements.
- Equipment menu "Choose weapon":
  - opaque book background; no scene shows through
  - selected row frame at about (110,264)–(905,356): a thin beige border (≈215,209,175) and a blue-grey fill
    (≈66,72,84); unselected rows are ≈47,48,50
  - row pitch about 90 px
  - alpha 183-206 across the whole screen

### 1.5 Alpha: why the HUD needs a blend override

Under the HUD, alpha is 192-204 on the semi-transparent HP and stamina bars, 234-249 on opaque white digits, and
as low as 136 where several layers stack. That fits `A' = As² + Ad(1−As)` starting from `Ad = 1`: the same
SRC_ALPHA/ONE_MINUS_SRC_ALPHA factors for color and alpha, with the straight-alpha PS `babda34d`. Opaque gives 1,
and As = 0.5 gives 0.75 (191). **[L]** The capture in Phase A confirms the factors; `FrameCapture` logs only color
factors today (`vk_rasterizer.cpp:1132-1140`).

- On a `ui_image` cleared to (0,0,0,0), the color comes out **premultiplied** (correct), but alpha is As² for one
  layer. Semi-transparent panels would show too much of the world.
- **Override (VR layer mode only)** for pipelines whose VS is a UI hash:
  - if the color blend is "over" (dst = ONE_MINUS_SRC_ALPHA), set alpha factors to ONE / ONE_MINUS_SRC_ALPHA / ADD
    with `separate_alpha_blend = 1`;
  - if it is additive (dst = ONE), set alpha to ZERO / ONE;
  - leave all other blends alone.

  The result is `A = 1 − Π(1−As)`, true coverage, with premultiplied color for both the straight and the
  premultiplied PS variants. Hook: `vk_pipeline_cache.cpp:618-619`, where the key copies `blend_control`.
  `vk_graphics_pipeline.cpp:296-309` already honours separate alpha. The key needs one extra bit, so non-VR
  pipelines are unchanged.
- **Fallback if exotic blend modes show up:** dual-target matting. The recompiler duplicates the PS output into a
  second attachment cleared white, so one draw writes the UI over black and over white, and
  `α = 1 − (W − B)` per channel. This is exact for every blend that is linear in the destination. It costs 2× the
  UI fill (about 0.1-0.2 ms [S]) and a recompiler change. Use it only if the override shows errors.

### 1.6 Timing and latency

- The display pass at the start of frame N copies UI(N−1). The EOP flip at the end of frame N presents that buffer
  ("the flip itself runs a frame behind", `vk_frame_capture.h:27-28`). [V] That the EOP flip of frame N runs after
  all of frame N's draws is [L].
- When `PrepareFrame` runs for that flip (`vk_presenter.cpp:359`, GPU thread), scene N and UI N are already
  recorded. Handing those to the bridge instead of the display buffer saves one game frame (about 11 ms at 90 Hz).
  **[L]** Check that the two-stage draw pipe has drained before `PrepareFrame` records.

## 2. VR HUD design

### 2.1 Presentation states

| State | Entered when (signals in §3.1) | Scene | UI image shown as | Pointer |
|---|---|---|---|---|
| **GAMEPLAY** | a 3D scene this frame, no menu signal | projection layer (Phase 2+) or cinema quad | **HUD layer**: cylinder (or quad), default D = 2.0 m, 56°×31.5°, head-locked with optional lazy follow; markers per §2.6 | off |
| **MENU** | `menu_open` (KG or cursor) with a 3D scene | as gameplay, optionally dimmed (fade 0.3) | **world-locked panel** at the cinema-screen placement (2.6 m, 3.2 m wide, `FrameLoop` rules) | **on** |
| **FULLSCREEN_2D** | no G-buffer pass this frame (title, loading), or UI coverage ≥ 0.98 | hidden (dark void) | the same panel, opaque | on (title, loading screens with a prompt) |
| **THEATER** | `fmv_active`, or `cutscene_active` (optional) | hidden | **composite** (scene snapshot + UI) on the cinema screen, as Phase 1 | off |
| FADE (modifier) | a FADE-class draw with alpha a | eye images × (1−a) | HUD × (1−a) if the fade was drawn after the other UI | — |

Hysteresis: enter after 2 consecutive game frames, leave after 6 (a Scaleform movie can skip a frame).

### 2.2 Renderer changes (Phase C)

1. **Layer mode at every output size.** `UiComposition::Choose` gets a `layer` input. With VR on, `OnDraw` must not
   return at 1307 for 1080p.
2. **Scene snapshot instead of background copy** at the boundary:
   - Temporal path: keep `output_image` as the scene image and skip the blit into `ui_image` (1641-1661). FSR3
     writes `ui_image` (1710), so in VR mode it needs another output; or VR supports TAA/DLSS/FSR4 only (PLAN).
   - Copy path (`RunUiOnly` 1491-1521, and 1080p, where the HDR upscale already ran): blit the background into a new
     `vr_scene_image`.
   - Clear `ui_image` to (0,0,0,0). Keep `PrepareUiDepth`, because the stencil masks need it. Whether any UI draw
     depth-tests at all is logged in Phase A; if none does, skip the depth copy (0.05 ms) [S].
3. **UI image size from a VR setting** (default 1920x1080 = native Scaleform coordinates, `target_scale` 1)
   instead of the output size (`EnsureUiResources` 1346, called with `target_width/height` at 1468 and 1548).
4. **Alpha override** (§1.5).
5. **Mirror window.** In layer mode the next display pass would sample the transparent `ui_image`
   (`RedirectSampled` 1864-1890). Return a composite (`scene·(1−α) + ui`) made by a small pass at that point
   (about 0.03 ms [S]). Otherwise the desktop shows the HUD on black.
6. **Hand-off** at `PrepareFrame` (§1.6):
   - copy the scene to the eye or cinema swapchain;
   - resolve `ui_image` into the HUD swapchain (unpremultiply → sRGB decode → premultiply → write through an sRGB
     view, applying the fade factor; about 0.05 ms [S]);
   - publish `BbXrUiState` (§3.5).
7. **Off without VR:** none of this runs; byte-identical output, tested in §5.5.

### 2.3 Bridge changes (MIT)

- **ABI** (the next version after the in-progress ABI 3):
  - `AcquireLayer(id, w, h, src_format, &image, &format)` / `ReleaseLayer(id)`, modelled on `AcquireFlat`
    (`bbport_xr_bridge.cpp:695-748`), with ids OVERLAY (ImGui, replacing the presenter TODO's `AcquireOverlay`),
    HUD and MARKERS
  - `SetUiState(const BbXrUiState*)`
- **`FrameLoop` layer list** (`bbport_xr_bridge.cpp:385-433`), back to front:
  1. scene projection or cinema quad
  2. marker quads (up to 8)
  3. HUD cylinder or quad
  4. menu panel
  5. ImGui overlay
  6. laser dot
  7. hands

  `XR_MIN_COMPOSITION_LAYERS_SUPPORTED` is 16 (`openxr.h`), so merge markers into one quad if the count would
  exceed it.
- **HUD layer format:** `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`, premultiplied (no
  `UNPREMULTIPLIED` bit), sRGB swapchain (`PickFormat` 497-527). Use `XR_KHR_composition_layer_cylinder` when the
  runtime has it (the in-progress `BbXrScreen.radius` already models curved screens); otherwise a quad.
- **HUD size:** 56° keeps the corner widgets within about ±28° horizontally. At 1920 px that is about 34 px/deg,
  at or above Pimax Crystal density, so no UI supersampling is needed [S].

### 2.4 Fades and dims

- **FADE-class draw:**
  - a full-viewport rectangle (≥1900x1060)
  - no stencil write
  - solid black: no texture, or color transform RGB ≈ 0
  - VS one of `34e8a281`/`81d336ce`/`24042a9b`

  Alpha = vertex alpha × mul.a + add.a from the decoded constants [L].
- **GAMEPLAY / MENU:** skip the draw (classified before `BindResources`, §3.2) and publish `fade_alpha` plus
  `fade_above_hud` (true if UI draws came before it). The resolve passes multiply the eye images, and the HUD when
  `fade_above_hud`, by (1−a). No extra XR layer and no `XR_KHR_composition_layer_color_scale_bias` are needed.
- **FULLSCREEN_2D / THEATER:** keep the draw; it is part of the 2D image.
- **Menu background dims:** the same mechanism with a partial alpha.

### 2.5 FMV, cutscenes, loading

- **FMV:** `fmv_active` = AvPlayer delivered a video frame in the last 250 ms (a host-side flag set in
  `AvPlayerSource::GetVideoData`, `avplayer_source.cpp:352`), or PS `9b827594` was drawn this frame → THEATER.
  Both [L]. The FMV quad may be drawn before the first Scaleform VS draw; then it lands in the scene snapshot.
  THEATER shows the composite, so this does not matter.
- **Real-time cutscenes:** 08 §3 shows the Remo camera written into `SprjCamera+0x18` (0x1e08e13) and selected into
  `+8` by flags (0x1e08e00). Reading `[[0x593E8F8]+8] == [[0x593E8F8]+0x18]` once per frame gives
  `cutscene_active`; guest memory is host memory [L]. Fallback [S]: `caption.gfx` drawn while `fe.gfx` PlayerHUD
  is absent. Default THEATER (PLAN), with an option for stereo cutscenes.
- **Title and loading screens:** no G-buffer pass this frame (`OnGBufferPass` never called, `vk_rasterizer.cpp:267-270`)
  → FULLSCREEN_2D [V signal].

### 2.6 Lock-on reticle and other world markers

The game positions `TargetCursor`, `EnemyTag*` and `Damage` in screen space with its own projection. 08 §7
injects the head pose only into render-side camera copies (H1 0x22de113, H2 0x22de386).

**Open question:** does the HUD code (`SprjScaleformPubFe*`, 07 §2) project with the game camera (`SprjCamera`)
or with the render context?

**Test T0** (first day of Phase 2): lock on to an enemy, turn your head, and watch the desktop mirror (render
view).
- The dot stays on the enemy → the HUD uses the render camera.
- The dot stays at a fixed screen spot while the world moves → it uses the game camera.

The three tiers:

- **Tier 1 (Phase C, approximate).** Markers stay in the HUD image.
  - Game camera: show the HUD as a quad locked to the rig (the LOCAL origin, which H1 maps to the game camera),
    spanning the game frustum (43° vertical, 16:9 → 71.6°×43° at D), so marker directions match the world.
  - Render camera: lock the HUD quad to the frame's render pose and span the render frustum.

  Either way the corner widgets land at the frustum edges, and the depth is D, not the enemy's. That is why Tier 2
  exists. Keep "compact HUD (head-locked, 56°)" as an option.
- **Tier 2 (Phase E, recommended).**
  - **Redirect:** classify marker draws by learned signatures (TargetCursor art, EnemyTag frame and bar, damage
    digits next to a tag). Redirect them per draw into `marker_image`: `RedirectColor` picks the image by draw
    class, the class becomes part of `RedirectState()` (`vk_temporal_upscaler.h:74-77`) so the begin memo
    (`vk_rasterizer.cpp:2634-2654`) breaks only when the class changes, and both images share `ui_depth_image`.
  - **Depth per marker:** a compute pass at the end of the UI phase reads the scene depth:
    - lock-on: minimum depth in a 9×9 window at the rectangle's centre;
    - tags: 25th percentile in a box below the bar (x0..x1, y1..y1 + 1.5·width);
    - damage numbers: the depth of the nearest tag.

    Linearise with `z_view = B/(z − A)` from `CameraMotion` `proj[2..3]`. Read back the previous frame's results
    (no stall) and smooth them (τ about 80 ms).
  - **Placement:** the bridge places one quad per marker at
    `pose_frame · (x_ndc·z/sx, y_ndc·z/sy, −z) · world_scale`, with `imageRect` = the marker's rectangle and size
    `rect_ndc·z/s`. Each eye then sees the marker at the enemy's depth. If no depth is valid, use the HUD depth.
  - **Rejected alternative:** a stereo HUD (UI recorded twice with per-draw disparity) would double UI recording on
    the bottleneck stage-B thread for the same visual result.
- **Tier 3 (later, RE):** hook the Fe marker update to read world positions directly. This is exact and needs no
  depth heuristics.

## 3. Menu detection and cursor tracking

### 3.1 Signals, ranked

| Signal | How | Reliability | Needs |
|---|---|---|---|
| **`KG_*` key-guide bitmap drawn** | texture content hash ∈ learned KG set | high: every menu movie has KG_*, the HUD, fade and FMV movies have none [V]; `nowloading` has 2, so exclude the loading context | learning once (about 20 textures) |
| **Cursor signature drawn** | learned per menu family (`Cursor`, `dpCursor`/`epCursor` in ingametop, `ItemComp_Cursor_anim_NN` in levelup) | high where learned | learning per family |
| Context fingerprint | hash of the frame's non-dynamic UI texture keys minus the HUD set → profile lookup | medium-high | learning |
| No 3D scene | no G-buffer pass | high (title, loading, FMV) [V] | none |
| Opaque coverage | UI alpha ≥ 0.98 at 16×9 probe points (layer mode) | high for full-screen menus | layer mode |
| HUD present | `fe.gfx` PlayerHUD bar signatures | medium | learning |
| UI draw count, glyph count | counters | low, tie-break only | none |
| Game memory (07 §3) | `SprjMenuImp`, `FrpgMenuDlg*` | exact | RE |

`menu_open = KG ∨ cursor ∨ context ∈ menus`, with hysteresis of 2 frames on and 6 off. Until a profile exists,
use 07's manual toggle.

### 3.2 The UI-draw tap

- **Where.**
  - In `Rasterizer::DrawRecord`, right after `PrepareRenderState` and `upscaler->OnDraw` (`vk_rasterizer.cpp:978-984`),
    classify the draw. This needs the VS and PS hash, the T#, the constants and vertex V#s (the tap fetches them
    like `ResolveVertexBuffers` does at 1727-1728), so classification can happen **before** `BindResources`
    (989), and a suppressed FADE draw can return early.
  - Append the record next to `scheduler.Record` (1121).
  - In `DrawIndirectRecord` (1193-1199), classify only; its arguments are GPU-written.
- **Gate:** the VS is a UI hash and the color target is the UI target (`ui_color` during a UI phase, or the guest LDR
  target at 1080p).
- **Inputs:**
  - The T# of PS image 0: `prepared ? prepared->image_sharps[0] : desc.GetSharp(stage)`, as at 2235-2236. This
    does not depend on the texture-set memo. The memo's early return (2203-2216) skips the per-image loops,
    including the `FrameCapture::Sampled` site (2430-2431); capture mode turns the memo off (2532), but normal
    play cannot.
  - The VS constant V#s (2096-2097), read from guest memory after `memory->IsValidGpuMapping`, as the 864-byte
    check already does.
  - Viewport, blend and stencil registers.
  - The index range from `Motion::IndexedRange` (`motion_history.h:23`, used at 1070-1083).
- **Rect decode:** per VS, as in the §1.2 table. Batched VS produce one record per batch entry, so one record per
  sprite.
- **Texture identity:**
  - Fast key = (address, w, h, format).
  - Stable key = XXH3 of mip 0, computed once per fast key and invalidated when the texture cache reports a write.
  - Dynamic textures (glyph cache, video planes, render-to-texture) keep the address key and a `dynamic` flag.
  - Sprite key for atlases = (texture key, UV rectangle quantised to 1/256), from texgen·(position bbox) for
    `24042a9b`/`09957251` or the vertex UV range for `81d336ce`. Separate copies of a KG bitmap in different
    movies share one content hash.
- **Signature** = hash(vs, ps, sprite key, vertex count). Color and alpha are excluded because cursors pulse.
- **Output:** a fixed array of 1024 records × 40 B per frame, triple-buffered, published at the next display pass
  (959-973) with an atomic frame id. No allocations, no GPU readback.

```cpp
namespace Vulkan::UiTap {
enum class Class : u8 { Unknown, Hud, Marker, Menu, Cursor, KeyGuide, Fade, Fmv, Background };
struct Record {                  // 40 bytes
    u32 vs, ps;                  // low 32 bits of pgm_hash
    u64 tex_addr;                // T# base (0: no texture)
    u32 tex_wh;                  // w | h << 16
    u16 tex_fmt, batch;          // data|num format; batch entry or 0xffff
    s16 x0, y0, x1, y1;          // UI pixels (1920x1080), clamped
    u16 verts; u8 blend, flags;  // blend class; stencil write/test, fullscreen, dynamic texture
    u32 color;                   // add.a | mul.rgba quantised (fades, debugging)
    Class cls; u8 pad[3];
};
struct Frame { u64 id; u32 count; u8 scene, fmv, cutscene, pad; Record rec[1024]; };
}
```

- **Cost [S]:**
  - classification about 0.2 µs per draw; rect decode about 1 ns per vertex
  - HUD frames: about 100 draws and 5k vertices → up to 50 µs
  - text-heavy menus: about 600 draws and 25k vertices → about 150 µs on stage B, the bottleneck thread (1.4% of
    11.1 ms)
  - in normal play, decode rectangles only for candidate classes (cursor, marker, fade, full-screen); decode all of
    them in learn or dump mode
  - texture hashing: once per texture (up to 0.1 ms for a 1 MB atlas)
  - if stage B shows more than 0.3 ms, move the rectangles to a recompiler-emitted atomic min/max per draw slot
    (07's GPU variant); batched draws then give one rectangle per draw

### 3.3 Identifying the highlight

- **Equipment list:** the selected row's frame (§1.4) comes from the movie's `Cursor` clip. It moves about
  90 px per d-pad step while the row text stays put; only the help text and the right-hand stats change.
- **Title:** the gold bar (`Title_Cursor`, 07 §2) moves about 40 px.
- Both are found by the same rule: the signature that translates by one pitch along one axis, with its size
  unchanged, after a single d-pad pulse.
- These rectangles are the test fixtures. The detector must report them within ±8 px (§5.3).

### 3.4 Learn mode and data formats

- **A. Active learn** (developer chord or a `BB_UI_LEARN` trigger file):
  1. Record frame F0.
  2. Inject one d-pad pulse lasting 2 game frames, through the VR pad path (`bbgpu_vr_pad` → `sample_vr`,
     `runtime_pad.c:278`) or `BB_PAD_FILE`.
  3. Record frames F1..F12 and take Fk, the first frame that changed.
  4. Candidates have the same signature, the same size (±2 px), and a translation of 16-400 px along one axis.
     Prefer the largest area; on a tie, the rectangle that contains the most glyph records.
  5. Inject the opposite pulse and require the reverse motion.
  6. Store the signature, pitch, axis, wrap behaviour and context fingerprint.
- **B. Passive learn:** whenever the player presses a d-pad direction while `menu_open`, run the same match, and
  promote a candidate after 3 consistent observations. The renderer needs the last d-pad edge and its game frame,
  which a shared counter from `runtime_pad.c` can provide.
- **C. Offline labelling:** the frame dump writes `ui_NNN.json` next to the raws (§5.2). A small labeller
  (`scripts/ui_label.py`, or an HTML page) draws the records over the BMP and lets a person tag KG, cursor, marker
  and fade draws. It writes the profile.
- **Recommended order:** C first (fast, robust), then A, then B for coverage.

Profile `bbvr_ui_profile.json`, one per game build:

```json
{
  "format": "bbvr-ui-profile/1",
  "game": {"title_id": "CUSA03173", "app_ver": "01.09"},
  "ui_vs": ["34e8a281", "81d336ce", "09957251", "24042a9b", "a400228b"],
  "textures": {
    "t:5c1e9a7f03b2d811": {"w": 64, "h": 64, "fmt": "BC3", "label": "KG_OK"},
    "t:a03f77c2e91b4d05": {"w": 1024, "h": 1024, "fmt": "BC3", "label": "MENU_Common atlas"}
  },
  "signatures": {
    "s:kg_ok":        {"tex": "t:5c1e9a7f03b2d811", "class": "keyguide"},
    "s:equip_cursor": {"vs": "09957251", "ps": "3bf7d476", "tex": "t:a03f77c2e91b4d05",
                       "uv": [0.125, 0.5, 0.375, 0.5625], "verts": 36, "class": "cursor"},
    "s:fade_black":   {"vs": "81d336ce", "tex": null, "fullscreen": true, "rgb_max": 8, "class": "fade"},
    "s:lockon":       {"tex": "t:...", "class": "marker", "anchor": "centre", "depth": "min9x9"},
    "s:enemy_bar":    {"tex": "t:...", "class": "marker", "anchor": "below", "depth": "p25_below"}
  },
  "contexts": {
    "c:equipment_weapon_list": {"fingerprint": "f:0d17...", "cursor": ["s:equip_cursor"],
                                "kind": "list", "axis": "y", "pitch": 90, "wrap": false, "tabs": "L1/R1"},
    "c:title": {"fingerprint": "f:7e2a...", "cursor": ["s:title_cursor"], "kind": "list", "axis": "y", "pitch": 40}
  }
}
```

Per-frame record dump (`ui_NNN.json` from the frame dump, or one line per frame with `BB_UI_DRAW_LOG`):

```json
{"frame": 18342, "scene": true, "fmv": false, "ui_draws": 187, "state": "MENU",
 "menu_open": true, "cursor": [110, 264, 905, 356], "pitch": [0, 90], "fade": 0.0,
 "draws": [
  {"i": 0,  "vs": "34e8a281", "ps": "7cde8e5f", "tex": null, "rect": [0, 0, 1920, 1080],
   "verts": 4, "blend": "none", "flags": ["stencil_write"]},
  {"i": 57, "vs": "09957251", "ps": "3bf7d476", "tex": "t:a03f77c2e91b4d05", "taddr": "0x2f1c40000",
   "batch": 3, "uv": [0.125, 0.5, 0.375, 0.5625], "rect": [110, 264, 905, 356], "verts": 36,
   "blend": "over", "color": "mul ffffffcc add 00000000", "sig": "s:equip_cursor", "class": "cursor"}
 ]}
```

### 3.5 To the bridge

Published once per game frame through `Xr::SetUiState` → `api->SetUiState`:

```c
typedef struct BbXrUiRect { float x0, y0, x1, y1; } BbXrUiRect;   /* UI pixels, 1920x1080 space */
typedef struct BbXrUiMarker { BbXrUiRect rect; float depth_m; uint32_t kind; } BbXrUiMarker;
typedef struct BbXrUiState {
    uint64_t game_frame;          /* +1 per published game frame (pulse pacing) */
    uint32_t mode;                /* GAMEPLAY, MENU, FULLSCREEN_2D, THEATER */
    uint32_t flags;               /* scene, hud, menu_open, cursor_valid, fmv, cutscene, fade_above_hud */
    float fade_alpha;             /* 0..1 black over the world */
    BbXrUiRect cursor;            /* highlighted element when cursor_valid */
    float pitch_x, pitch_y;       /* learned step of one d-pad pulse, pixels */
    float game_fov_y, game_aspect;/* frustum the UI coordinates were projected with */
    BbXrPose ui_pose;             /* XR-space pose of that frustum (render pose or rig, per T0) */
    uint32_t marker_count;
    BbXrUiMarker markers[12];
} BbXrUiState;
```

- The pointer loop (07 §4 B2) runs in the bridge at the headset rate: laser hit → UI pixel via
  `xrscreen::RayToPixel` (`xr_screen_math.h:70-97`) on whichever panel shows the UI.
- It outputs d-pad and Cross bits through `BbXrInput.buttons` → `sample_vr`.
- Pulses are paced by `game_frame`: each lasts at least 2 game frames, then the loop waits for the cursor
  rectangle to change or for 150 ms.

## 4. Phased implementation plan

| Phase | What | Depends on | Effort [S] |
|---|---|---|---|
| **A** | UI-draw tap, record dumps, blend logging | nothing (works flat, cinema, VR) | 3-4 days |
| **B** | Menu state + cursor → bridge; pointer closed loop on the cinema screen | A | 3-5 days |
| **C** | HUD layer split: snapshot, transparent `ui_image`, alpha override, HUD swapchain, mirror | A; land with Phase 2 `BB_XR_MODE=mono` | 4-6 days |
| **D** | State machine: fades, FMV, cutscenes, full-screen 2D, menu panel placement | B, C | 3-4 days |
| **E** | World markers: classes, per-draw redirect, depth probe, per-marker quads | C, D, T0 | 5-8 days |
| **F** | Polish: cylinder, lazy follow, settings, profile coverage of all 103 movies | all | ongoing |

**Phase A: UI-draw tap.**
- New `renderer_vulkan/vk_ui_tap.{h,cpp}`. The decode and classification functions live in a header that tests
  can use without Vulkan (`ui_tap_decode.h`).
- Hooks:
  - `vk_rasterizer.cpp:978-984`: classify
  - `:1121`: append the record
  - `:959-973`: rotate and publish
  - `:1132-1140`: add alpha factors (`alpha_src/dst_factor`, `separate_alpha_blend`) and `depth_enable` to the
    `FrameCapture` note
  - `vk_presenter.cpp:361-472`: `BB_FRAME_DUMP_TRIGGER` also writes `ui_NNN.json`
- Switches: `BB_UI_TAP=1` (default on when `BB_VR=1`) and `BB_UI_DRAW_LOG=<trigger file>`.
- A/B bits in `bbport_toggles.h`: bits 56 and 58-63 appear free today.
- **Exit:** `ui-tap-test` passes, and the dumps of title, HUD and equipment frames overlay their BMPs correctly.

**Phase B: menu state and pointer.**
- Profile loader and classifier.
- `Xr::SetUiState` (in `vk_xr.h`) → `SetUiState` in the bridge ABI.
- Closed loop in the bridge (`xr_input.cpp` / `bbport_xr_bridge.cpp`), with learn method C, then A.
- **Exit:** the pause menu, equipment list, title and talk choices switch menu mode on and off within 2 game frames,
  and the loop reaches the row under the laser without overshoot. The sandbox test (§5.3) passes first.

**Phase C: HUD split.**
- Renderer:
  - §2.2 items 1-7: `ui_composition.h:13-18`, `vk_temporal_upscaler.cpp:1304-1344`, `1461-1526`, `1641-1661`,
    `1346-1401`, `1864-1890`
  - the pipeline key (`vk_pipeline_cache.cpp:618-619`)
  - the hand-off at `vk_presenter.cpp:359`
- Bridge: `AcquireLayer`/`ReleaseLayer` and the HUD quad or cylinder.
- **Exit:**
  - In the headset, with ASW/SSW on, the HUD stays sharp and does not smear while the head moves.
  - The mirror composite matches a non-VR display dump: PSNR ≥ 40 dB on the static title and on the pause menu with
    the camera still.
  - Semi-transparent bars look the same as flat over a grey wall.

**Phase D: state machine.**
- §2.1 states with hysteresis: fade suppression (§2.4), `fmv_active`/`cutscene_active` (§2.5), the menu panel
  placed like the cinema screen, coverage probe (16×9 samples).
- **Exit:** walk through title → FMV → load → Hunter's Dream → lamp travel (fade) → death (YOU DIED banner, fade)
  → pause menu → equipment. Each state is correct, nothing flickers, and no black rectangle appears during fades.

**Phase E: world markers.**
- §2.6 Tier 2: marker classes, `marker_image`, the depth-probe compute pass, marker quads in the bridge.
- **Exit:**
  - with stereo (Phase 3/4) on, the lock-on dot fuses on the enemy at 2-15 m
  - enemy bars sit above their enemies
  - no vergence conflict while circling a locked target

**Performance summary [S]:**

| Item | Cost |
|---|---|
| Tap, stage B | up to 50 µs per HUD frame, about 150 µs in text-heavy menus, 0 when off |
| Texture hashing | once per texture |
| Scene snapshot | replaces the existing background blit (Copy path) or removes it (Temporal path) |
| Clearing `ui_image` | negligible |
| Alpha override | none (one pipeline variant per UI pipeline) |
| HUD resolve pass | about 0.05 ms |
| Mirror composite | about 0.03 ms |
| Coverage and depth probes | negligible |
| Compositor | +1-10 quads, about 0.1-0.3 ms in the runtime |
| Memory | about 24 MB per 1080p 3-image swapchain (HUD, markers) |

## 5. Test procedure

1. **Unit tests (no GPU), the `tests/test_ui_composition.cpp` pattern** (`gpu/CMakeLists.txt:269-271`):
   - `ui-tap-test`: decode each of the five VS layouts from synthetic vertices and constants; NDC → pixels with
     a −540 yscale; batched split; signature stability under alpha pulsing; fade, full-screen and KG
     classification; profile JSON round trip.
   - Extend `ui-composition-test` with the new `layer` input of `Choose`.
2. **Frame dumps.**
   - With VR layer mode on, `BB_FRAME_DUMP_TRIGGER` also writes `scene_NNN.raw`, `hud_NNN.raw` (RGBA with alpha)
     and `ui_NNN.json`. Today it writes `display_NNN` only when not upscaled (`vk_presenter.cpp:437-444`).
   - `scripts/vr_frames.py` needs `--keep-alpha` for `hud_*` (it forces alpha 255 today) and copies the JSON
     sidecars.
   - Checks: `composite(scene, hud)` matches the display dump; the KG flag is set in every menu frame and clear in
     every HUD frame; the cursor rectangle matches §1.4 within ±8 px.
3. **Sandbox** (`gpu/xr_bridge/xr_sandbox.cpp`, `scripts/vr_sandbox.sh`). Extend it with
   `--image scene.bmp --hud hud.bmp --ui ui.json`:
   - show the scene on the cinema quad or as a backdrop, and the HUD as its own quad or cylinder with alpha
     (`LoadBmp` must keep alpha for the HUD; it forces 255 at line 324);
   - add a fade slider;
   - add a menu-panel mode that uses `cursor` and `pitch` from the sidecar, and simulate the game's reaction (one
     pulse moves the cursor by one pitch, reusing the synthetic menu model at 157-253).

   Fixtures from today's frames: `7_menu_choose_weapon.bmp` → cursor (110,264,905,356), pitch 90;
   `1_title.bmp` → about (800,660,1180,700), pitch 40. This tunes HUD distance, size, curvature, panel placement
   and the closed loop with no game and no GPU contention.
4. **In game** (when the headset and GPU are free): the Phase C, D and E exit checks above, run in order. Then
   **T0** (§2.6) and the blend-factor capture (`BB_CAPTURE_TRIGGER` in a HUD frame and a menu frame).
5. **Regression:** with `BB_VR` unset, the display dumps of the title and of a static Hunter's Dream frame must be
   byte-identical before and after each phase.

## 6. Risks and open questions

1. **Which camera projects the world markers (T0).** This decides Tier 1 anchoring. **[open]**
2. **Blend factors.** The alpha model is [L] from measurements and PS code; a capture settles it. If additive or
   multiply UI is common, use dual-target matting (§1.5).
3. **UI drawn into intermediate targets** (Scaleform filters, `cacheAsBitmap`). Those draws are not redirected and
   their results are sampled by later UI draws, so they are fine. Exclude them from records by target. [S]
4. **Non-UI draws with a UI VS.** The passthrough `34e8a281` could serve a post pass. The upscaler's gate (first
   draw, RGBA8, render size, not a display buffer) already guards this. The alpha override additionally requires
   blending to be on. [L]
5. **Vertex formats are unknown until captured.** Decode by the attribute formats from `GetVertexInputs`, and log
   unknown formats. [S]
6. **Stability of texture content hashes.** KG bitmaps are external images (`KG_OK.tga` references), most likely
   loaded once from TPF archives. Glyph caches are dynamic and must stay address-keyed. [L]
7. **Runtime support** for cylinder layers and the 16-layer budget on Pimax Play and SteamVR. Check
   `xrEnumerateInstanceExtensionProperties` at `Init`. [S]
8. **Gamma vs linear blending.** The game blends semi-transparent UI in gamma space, the compositor in linear, so
   panels look slightly lighter. Tune alpha in the resolve pass if needed. [S]
9. **Hand-off ordering:** `PrepareFrame` versus the two-stage draw pipe (§1.6). [L]

## Cross-links

- **PLAN.md Phase 5**: this file details "clear `ui_image` to transparent" and "verify Scaleform alpha". The
  answer to the second is "not as-is; override the alpha blend" (§1.5). Phase 5 should start together with
  Phase 2 (§0).
- **03 §3.5**: same boundary and redirect facts, now with line-level detail and the 1080p exception.
- **05 §5**: the HUD and fade movies are named here (`fe.gfx`, `fade.gfx`, `black.gfx`, `caption.gfx`,
  `movie.gfx`).
- **07 §4 B2 / §6**: the tap, learn mode and closed loop are specified here. The batch VS matrix layout is now
  [V], and KG bitmaps give a generic "menu open" signal without game RE.
- **08 §3, §7**: the head pose goes into render-side copies (H1/H2), which raises T0. The Remo slot at
  `SprjCamera+0x18` gives `cutscene_active`.
