# 41. Display size: choosing the render and eye image size before the game starts

Status: implemented in the launcher settings (`tools/release/zip/bbvr_settings.py`) and the dashboard Play tab
(`tools/release/dashboard/main.cpp`). Not yet checked in a headset. Tags: [T] tested, [S] from source, [U] untested
or estimated.

## 1. What already existed

There are two size knobs. Neither one was tied to the headset.

| Knob | What it sizes | How it is applied | Cost |
|---|---|---|---|
| `BB_RENDER_RES=WxH` (+ `BB_OUTPUT_RES`, `BB_DMEM_MB`) | The game's own render size (scene and display buffer) | A startup patch of the scene size immediates (`scripts/patches.py` `resolution_writes`, `render_size`). Above 1080p it adds "Increased Graphics Heap Sizes", and the game needs `BB_DMEM_MB=9152` or it faults at boot (findings/30 §5). `run.py` keeps an explicit value across in-game restarts. The renderer reads it in `vk_temporal_upscaler.cpp` (`scaled_session`) and `bbport_settings.cpp` (`FixedRenderSession`) | The real GPU cost: about 0.9 ms per Mpx (findings/23 4.4) |
| `BB_XR_EYE_RES=WxH\|factor` (iq key `res`) | The eye image: the FSR 1 / DLSS output, copied 1:1 into the eye swapchain | Read live (`vk_xr_eye_iq.cpp` `Apply`/`TargetSize`). The frame size is the IQ target, else the window (`xr_resize_logic.h` `Target`). With an explicit DLSS mode the eye is render x 1 / 1.5 / 1.72 / 2 / 3 (`xr_dlss_logic.h` `OutputSize`) | Small with FSR 1 (the whole chain is about 1 ms). With DLSS it scales with the output (3840x2160 ran at about 57 fps on a 4090) |

- In alt-eye mode the eye swapchain is the frame size (16:9), not the runtime's `recommendedImageRect`. The
  recommended size only sizes the stereo swapchains and the HUD (`bbport_xr_bridge.cpp` `AcquireStereo`, `ui_scale`).
  The bridge logs `VR: runtime ...`, `VR: headset <systemName>, ...` and
  `VR: eye images: recommended WxH and WxH, max ...`.
- **Aspect:** the 16:9 image is stretched over the eye's field of view, so pixels are not square: the eye aspect is
  about 0.93 on a Quest 3 and 1.25 on a Dream Air, while the union frustum is about 1.43 (findings/22 and 23).
  The patch accepts any WxH, but **a non-16:9 render size has never been tried with this game** (findings/23 P4.7,
  risk 10). Every size this feature produces is 16:9, the same mapping as today.
- `bb-gpu-capabilities` (`tools/gpu_capabilities.c`) prints the Vulkan device and, with `--live-resolution`, `1`/`0`
  for live flat preset changes. It knows nothing about headsets.

## 2. RestartResolutionTests (tests/test_run_settings.py)

At HEAD 3 tests failed on this Windows host. The render size path is not broken: all three failures came from the
Windows host.

- The Windows Python prints `--print-scaled` with CR LF, so `read` in `run.sh` kept `1280x720\r` in
  `BB_OUTPUT_RES`.
- `Path.write_text` writes `bbport.ini` with CR LF, so `^live_resolution=...$` never matched and
  `live_resolution=1` was ignored. The same bug hits a real CRLF `bbport.ini` on Linux, for example one edited on
  Windows and copied to a Steam Deck.
- The AppImage bare-PATH test symlinks MSYS coreutils, which do not run through a Windows symlink
  (`cd: null directory`).

Fixes:
- `run.sh` strips a trailing CR from both values.
- The bare-PATH test is skipped on Windows, because it emulates the Linux AppImage.

Result: test_run_settings 4 passed, 1 skipped.

## 3. Design

### What the player sees

The Play tab gets a **Display size** card under "This PC":
- a headset combo: Auto (with what was detected), Game default, presets, Custom;
- 70 / 85 / 100 / 125 % buttons;
- a line that shows what the game gets, for example `Render 1920x1080 (game's own) -> eye 3840x2160 (fsr1)`.

Options > Display / VR has the full options: `display_size`, `display_scale` (50 to 150 %) and `display_custom`
(WxH per eye).

- **Display size is a hardware choice.** `display_size` and `display_custom` are "sticky": they are stored in
  `settings['hw']` and applied over every profile, so picking a headset never turns the profile into Custom, and
  switching profiles keeps it. `display_scale` is a normal option: Performance sets 70 %. The tier default is 100 %
  with an NVIDIA RTX GPU and 85 % otherwise. A GPU list that is empty (unknown) gives 100 %.
- **Eye resolution** (DLSS tab) applies only with Display size = Game default.
- `extra_env` still overrides everything (for example `BB_RENDER_RES=...`).

### One rule: the headset's size, capped at a Quest 3 eye, times the %

```
ref   = the headset's recommended per-eye size (Auto: last session / runtime; preset; custom)
s     = Display resolution / 100;    A = s^2 * min(ref_w * ref_h, 2064 * 2208)
DLSS        eye = 16:9 of area A;  render = eye / mode ratio (auto 1.5, quality 1.5, balanced 1.72,
            performance 2, ultra 3, dlaa 1); an explicit mode makes the game size the eye from the render
FSR 1 / off render = 16:9 of area A / 1.5^2;   eye height = s * min(max(ref_h, ref_w * 9/16), 2160)
clamp       eye 720..2880 tall, render 540..1440 tall, render <= eye; even sizes
native      a render height within 4% of 1080 stays the game's own frame (no patch, no BB_RENDER_RES)
emit        BB_XR_EYE_RES = iq res = eye;  if not native: BB_RENDER_RES = BB_OUTPUT_RES = render,
            and BB_DMEM_MB = 9152 when the render is larger than 1080p
```

Why this rule:
- A Quest-3-class or larger headset at 100 % gets exactly today's sizes: 1080p to 2880x1620 with DLSS quality,
  1080p to 3840x2160 with FSR 1. The default never costs more than today's measured setup.
- A smaller headset renders less. 125 % and 150 % are the opt-in way up.
- Pixel count (area) drives the render size, because that is the cost. With FSR 1 the vertical size drives the eye
  image, because it is cheap and the 16:9 stretch makes vertical density the limit.
- Output = render (the combination findings/30 measured at 1440p). The UI is then drawn at the render size, which
  is slightly softer below 1080p.

| Choice | Variables set |
|---|---|
| Game default | none (`eye_res` as before) |
| Auto, headset unknown (no runtime, no log) | none: today's session (the golden test) |
| Auto / preset / custom, render within 4% of 1080p | `BB_XR_EYE_RES` (+ iq `res`) |
| ... render other than 1080p | + `BB_RENDER_RES`, `BB_OUTPUT_RES` |
| ... render above 1080p | + `BB_DMEM_MB=9152` |

Sizes from the rule (render -> eye), with the native frame marked *:

| Headset (ref) | DLSS quality 100% | FSR 1 100% | FSR 1 85% | FSR 1 70% |
|---|---|---|---|---|
| Quest 1 (1440x1600) | 1348x758 -> 2022x1137 | 1348x758 -> 2844x1600 | 1148x646 -> 2418x1360 | 960x540 -> 1992x1120 |
| Quest 2 / 3S (1832x1920) | 1668x938 -> 2502x1407 | 1668x938 -> 3414x1920 | 1418x798 -> 2902x1632 | 1166x656 -> 2390x1344 |
| Quest 3, Index, Frame, G2, Pimax, Varjo, Beyond | 1920x1080* -> 2880x1620 | 1920x1080* -> 3840x2160 | 1614x908 -> 3264x1836 | 1330x748 -> 2688x1512 |

### Auto-detection (no game, no OpenXR session)

`detect_all` adds `caps['headset']` (`detect_headset`):
1. The active runtime: `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` and its JSON name, already read by
   `detect_runtimes`. `classify_runtime` now also returns `vdxr` (Virtual Desktop) and `varjo`.
2. **Last session:** the newest `logs/bbvr-*.log` (first 4 MB of up to 6 files) that has
   `VR: eye images: recommended WxH`, together with its `VR: runtime` and `VR: headset` lines. It is used when that
   session ran on the same kind of runtime as the active one. The headset name is matched to a preset for display.
3. Otherwise a **typical headset of the runtime**: Oculus/Meta and VDXR -> Quest 2 / 3S (the conservative common
   case), SteamVR -> Index, Pimax -> Crystal, WMR -> Reverb G2, Varjo -> Aero.
4. Otherwise unknown: game default sizes.

Checked against the dev machine's real logs: `Pimax OpenXR`, `Pimax Dream Air`, recommended 3828x3074 (source
log).

**No OpenXR probe.** A probe (`xrCreateInstance` + `xrGetSystem` + `xrEnumerateViewConfigurationViews`) would need no
session, but creating an instance starts SteamVR, or the Pimax and Oculus services, on a menu open. That is not
safe to do every time the dashboard opens. A "Detect now" button that runs such a probe is a possible follow-up.

**Runtime sliders.** Meta's resolution slider (Oculus app / Debug Tool), SteamVR's per-app or global
supersampling, and Virtual Desktop's quality preset all change the recommended size. Auto reads it from the last
session, so the slider counts and the % multiplies on top. A named preset uses that headset's standard size and
ignores the slider. The help text says this.

### Reference numbers

The table lists the recommended size per eye at the runtime's 100 % / 1.0x (`ref`), the panel per eye, and the
confidence in `ref`.

| Preset | ref | panel | confidence |
|---|---|---|---|
| Meta Quest 1 | 1440x1600 | 1440x1600 | low (Link's 1.0x not verified) |
| Meta Quest 2 / Quest 3S | 1832x1920 | 1832x1920 | medium (Link 1.0x at 72 Hz = 3664x1920) |
| Meta Quest 3 | 2064x2208 | 2064x2208 | medium |
| Meta Quest Pro | 1800x1920 | 1800x1920 | medium |
| Steam Frame | 2160x2160 | about 2160x2160 | low (SteamVR 100% unknown) |
| Valve Index | 2016x2240 | 1440x1600 | high (SteamVR 100%) |
| HTC Vive / Vive Pro | 1512x1680 / 2016x2240 | 1080x1200 / 1440x1600 | high |
| Oculus Rift S | 1648x1776 | 1280x1440 | medium |
| HP Reverb G2 (WMR) | 2160x2160 | 2160x2160 | medium (SteamVR 100% is about 3160x3092) |
| Pimax Crystal | 2880x2880 | 2880x2880 | low |
| Pimax Dream Air | 3832x3076 | about 3840x3552 | high (measured: Pimax runtime log; SteamVR logged 3960x3268) |
| Varjo Aero / Bigscreen Beyond | 2880x2720 / 2560x2560 | same | low |

Because of the cap, every headset at or above a Quest 3 eye gives the same sizes at the same %. So the uncertain
numbers only matter for the small headsets.

## 4. To check in a headset [U]

- Render sizes below 1080p in VR (Quest 2 at 100 % = 1668x938, Performance 70 % on a Quest 3 = 1330x748). This
  includes the UI and HUD drawn at that size, and the flat 720p path's behaviour in VR.
- 125 % and 150 % above 1080p (up to 1440 tall) with `BB_DMEM_MB=9152` and DLSS.
- The fps gain of 85 % and 70 % on a non-RTX GPU, compared with today's 1080p.
- The in-game "Apply and restart" keeps the sizes (`run.py` keeps an explicit `BB_RENDER_RES`). The flat settings
  overlay then shows its "BB_RENDER_RES fixes the scene size" hint (`bbport_overlay.cpp`).
- Later: a non-16:9 render size at the eye's aspect (findings/23 P4.7) would remove the stretch. The rule would then
  use `ref` directly instead of its 16:9 fit.
