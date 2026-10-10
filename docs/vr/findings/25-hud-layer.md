# 25: HUD layer: the game's UI as its own quad, loading screens to the cinema screen

Date: 2026-10-09. Branch `vr`. Status: **built and unit-tested, not yet seen in the headset.** Nothing
was launched: a game was running in the headset, so no flat run was done either.

## Problem

From the headset (Pimax Dream Air): "I cannot see the subtitles. They are way too low." In alt and mono
modes the game's 1920x1080 frame, HUD included, is stretched over the eye frustum (about 126 x 98 deg per
eye with the overscan from `runtime_vr_head.c`). Subtitles, bars and item slots therefore land at the
bottom and corner edges of the lenses, where they are blurry or not visible at all. The HUD also sits at
infinite depth inside the eye image.

Second report: in-game loading screens (warp, death, entering an area; the screen with the tips) look
"zoomed in" in the headset. Those frames still have a matching scene camera, so `xrstereo::Pairing`
classifies them as eye frames and they get stretched like the 3D scene. The log analysis of
`out/vr_all_alt*.log` shows this:
- The loading windows run at 280-600 draws/frame with `L n R n, mono 0`.
- Only no-camera frames go to the mono/cinema path.
- So the L/R vs mono tag cannot tell a loading screen from gameplay.

## How the UI is separated (evidence)

The 3D/UI boundary is the one findings/10 §1.1 verified in code, and the scaled presets have shipped on it:
- The UI starts with the first draw of a frame whose vertex shader is one of the five Scaleform shaders
  (`34e8a281`, `81d336ce`, `09957251`, `24042a9b`, `a400228b`; `UiComposition::MovieShader`).
- That draw goes into the finished frame: the last render-size RGBA8 target before the UI, which has the
  scene depth's size (`TemporalUpscaler::OnColorTarget` → `ldr_target`).
- Everything after it in that frame is 2D: the HUD, subtitles (`caption.gfx`), menus, fades and the FMV
  host. findings/23's census lists the same boundary (draws 924 and later, after tonemap and post).

Before this change, the 1080p output (what VR uses: `Output resolution: 1920x1080` in every
`vr_all_alt*.log`, `BB_UPSCALER=off`) redirected nothing. Only the scaled presets drew the UI into
`ui_image`. Now, when `XrHud::Separating()`, the 1080p path does it too (`OnDrawHud`/`StartHud` in
`vk_temporal_upscaler.cpp`):

1. At the first Scaleform draw into `ldr_target` (only in frames with a scene camera depth), `ui_image` is
   cleared to transparent black. The scene depth is copied in and stencil is cleared, as the scaled path's
   `PrepareUiDepth` does. All later draws into that target are redirected there by the existing
   `RedirectColor`/`RedirectDepth`, with scale 1.
2. The game's target keeps the scene without the HUD. The next display pass is **not** redirected, so it
   copies the HUD-free scene into the display buffer. The eye and flat images carry no HUD.
3. **Coverage alpha** (findings/10 §1.5): Scaleform blends alpha with its color factors, which gives
   `A = As²` over an empty target. With separation on, `vk_pipeline_cache.cpp` gives pipelines with a
   Scaleform VS and an "over" color blend the alpha blend `ONE, ONE_MINUS_SRC_ALPHA`; additive blends get
   `ZERO, ONE` (`UiComposition::HudAlpha`, unit-tested). The HUD image is premultiplied with true
   coverage. The modified blend is part of the pipeline key, so nothing changes when separation is off.
4. At the next display pass, the complete UI of the previous frame is copied to `hud_snapshot`. That is
   exactly the frame whose display buffer is about to be flipped.
5. `Presenter::PrepareFrame` copies the snapshot into the frame's own `hud_image`. This uses the same
   command stream and the frame's existing semaphore, so there is no cross-thread race.
6. `Present` hands it to the bridge: `AcquireHud`, then a raw copy into an sRGB swapchain with alpha kept,
   then `ReleaseHud`. A frame without a separated UI (title, menus without a scene) calls `HideHud`.

Frames without a scene (title, main menu, loading without a camera) are untouched: their UI stays in the
image and they keep going to the flat cinema screen.

### Loading screens and full-screen menus

At each snapshot the UI's alpha is sampled at 32x18 points (a blit plus an async readback through
`DeferPriorityOperation`, with no stall). When at least 90% of the probes have alpha >= 0.9, the UI covers
the screen (`UiComposition::FullScreenUi`). A latch switches on after 2 such frames and off after 4
(`FullScreenLatch`). While it is on:
- the frame's HUD image (the loading screen itself, opaque) goes to the **flat cinema screen** through
  `AcquireFlat`/`ReleaseFlat` instead of the eye;
- the HUD quad is hidden;
- the alternate eyes come back once both have newer images, as after menus.

Log lines: `VR HUD: the UI covers the screen (...): frames go to the flat cinema screen` / `... no longer
covers ...`.

## What the user sees (expected)

- **Gameplay, alt or mono:** the world without the HUD. The HUD (bars, item slots, souls, subtitles,
  prompts, boss bar, pickup list) is a flat quad 2.5 m away, 55 deg wide (2.60 x 1.46 m), centered 5 deg
  below straight ahead, and head-locked. Subtitles land about 18 deg below the view axis, inside the sharp
  part of the lenses. Both eyes see the same image (no stereo HUD).
- **Pause and small menus over the scene:** on the same quad.
- **In-game loading screens and opaque full-screen menus over a scene:** on the flat cinema screen, like
  the title menus. There may be about 2 frames of the old routing when one starts.
- **Title, main menu, loading without a scene:** unchanged, on the cinema screen.
- **Desktop mirror window:** shows the world **without** the HUD while separation is on.

## Configuration

| Env | Default | Meaning |
|---|---|---|
| `BB_XR_HUD` | `layer` with `BB_XR_MODE=alt`/`mono`, else `off` | `layer`: HUD quad; `off`: HUD stays in the image (old behaviour); `force`: separate even without a headset (flat tests; frame dumps get a `hud_*.raw`) |
| `BB_XR_HUD_DIST` | 2.5 (m) | quad distance, 0.5-20 |
| `BB_XR_HUD_WIDTH_DEG` | 55 | horizontal angle, 15-140 |
| `BB_XR_HUD_PITCH_DEG` | -5 | center above (+) or below (-) the view axis, -60..60 |
| `BB_XR_HUD_FOLLOW` | `lock` | `lazy`: stays in the world until the head turns more than 20 deg away, then eases back (time constant 0.25 s) |

The settings are logged at session start by the bridge (`VR: HUD layer on (BB_XR_HUD=layer): the game's UI
as a quad ...`) and by the port at the first draw (`VR HUD: layer: ...`). The first separated frame is
logged (`VR HUD: first frame with its UI drawn into its own 1920x1080 image`), and so is the first HUD image
on the headset (`VR: HUD layer: first image on the headset ...`).

Safety:
- The port separates only when the bridge has ABI 7 and a session.
- If the bridge's `AcquireHud` returns -1 (HUD off, or the runtime refused the layer), the port logs
  `VR HUD: off from now on` and puts the UI back into the game image from the next frame.
- The bridge drops the HUD layer first when the runtime rejects frames (`DropOptionalLayer`).

## Bridge (ABI 7)

The new functions are `AcquireHud`/`ReleaseHud`/`HideHud` (`bbport_xr_bridge.h`). The HUD is an
`XrCompositionLayerQuad` with `BLEND_TEXTURE_SOURCE_ALPHA` (premultiplied). It sits after the projection
or cinema layer and before the laser and hands.
- Head-locked: placed in VIEW space.
- Lazy: placed in the world space at the head position (`xrhud::LazyFollow`).

It is shown only while an image released within 250 ms has not been followed by `HideHud`. Placement and
parsing are pure (`gpu/xr_bridge/xr_hud.h`) and tested in `bbport_xr_math_test`.

## Interim vs final

This is the **final architecture** for HUD, subtitles and menus: separate image, own layer, coverage
alpha. These parts are still interim or known limits:
- **World markers** (lock-on reticle, enemy HP bars, damage numbers, co-op tags) are in the HUD image.
  The game projects them for the full eye frustum, so on a 55 deg quad they are shifted toward the center
  and no longer sit on the enemy. The final fix is findings/10 §2.6 Tier 2: classify marker draws and put
  them on per-marker quads at scene depth. Workaround: `BB_XR_HUD_WIDTH_DEG` about 120 keeps them aligned,
  but brings back the edge problem.
- **Fades** (`fade.gfx`, "YOU DIED" blackout) darken the HUD quad, not the world. They need the FADE
  classification from findings/10 §2.4.
- **Mono mode**: full-screen UI frames still go to the mono projection layer, because mono has no cinema
  path for game frames.
- **Desktop mirror**: no HUD. The fix is a composite pass before the window blit.
- **Colours**: the HUD skips the game's display pass and the presenter's post pass, so a non-default
  in-game brightness or `pp` gamma applies to the world and not to the HUD.
- **Scaled output presets** (1440p/2160p) keep their old composition, with the HUD in the image. VR runs at
  1080p.

## Tests

- `ui-composition-test` (new: HUD alpha, full-screen coverage, latch)
- `bbport_xr_math_test` (2704 checks, new HUD parse, quad and lazy follow)
- `bbport_xr_stereo_test`, `vr-stereo-test`, `vr-stereo-hooks-test`, `vr-head-test`

All pass. Built: `out/gpu/bb-probe-hud.exe`, `out/xr-bridge/bbport_xr.dll` (ABI 7; not copied to
`out/bbport_xr.dll`).

**First headset check:** run with the new DLL next to the exe and look for the four log lines above.
Then check:
1. Subtitles readable.
2. Bars top-left of the quad.
3. Alpha on the semi-transparent bars looks right (not too see-through).
4. A warp loading screen lands on the cinema screen.

Flat check without a headset: `BB_VR=0 BB_XR_HUD=force` plus `BB_FRAME_DUMP_TRIGGER`. The `display_*`
dump has no HUD and `hud_*` holds it on transparent black.

## Risks

- A Scaleform draw with blending off that writes alpha 0 over opaque color would vanish from the quad. No
  capture yet shows one.
- The full-screen threshold is an estimate (90% of probes at alpha >= 0.9). A dimmed menu background
  (partial alpha) stays on the quad.
- The Pimax runtime "blends by alpha whatever the flags say" (bridge comment). Whether it treats the HUD as
  premultiplied is untested. If edges look dark or haloed, try the straight-alpha variant.
- The 1080p path redirects at every frame with a scene. A pass the port cannot fully redirect logs
  `Upscaler: pass not redirected` and draws into the game image; the HUD then shows in the eye as before.

## Dark box on the quad (Pimax, first headset look)

Report: "I see the dark box clearly". Static findings: layer flags were `BLEND_TEXTURE_SOURCE_ALPHA` (correct
for the premultiplied image, no `UNPREMULTIPLIED`), the HUD image is cleared to (0,0,0,0) in `StartHud`, the
copy keeps alpha (`CopyToFlat(..., opaque=false)`), swapchain format 43 (R8G8B8A8_SRGB) has alpha. So the
fault is in the alpha the image really holds or in the runtime's blend; it needs a measurement:
- `BB_XR_HUD_DEBUG=1` logs `VR HUD debug #n: RGBA corner ...` (32x18 probes from the UI image; corners must be
  A=0) and the runtime's swapchain format list.
- `BB_XR_HUD_ALPHA=premul|straight|opaque` (default premul) picks the layer flags; live: write the word into
  `bb_xr_hud_alpha.txt` in the game's working directory (polled each second, logged as `HUD blend: ...`).
  If the box vanishes with `straight`, Pimax reads the flag the other way round. `BB_XR_HUD=off` puts the HUD back
  in the image.
