# 38. Start-up settings menu

`tools/release/zip/bbvr_launcher.py` (the tester launcher) shows a settings page before it starts the game.
Modules, all standard library (they run under the embeddable Python, no tkinter):

| File | Job |
|---|---|
| `bbvr_settings.py` | option schema (`SCHEMA`), profiles, launch builder (`build_launch`), key-preserving control-file writer, `ui_layouts.json` overrides, GPU / DLSS / OpenXR detection |
| `bbvr_menu.py` | local web server (127.0.0.1, random port, per-run token, Host / Origin checks, JSON-only POST), the page, console fallback |
| `bbvr_launcher.py` | `--menu`, `--no-menu`, `--profile NAME`, `--reset-settings`, `--console`, `--no-browser`, `--skip-checks`, `--dry-run` |

Dev use from this repo: `sh out/vr_menu.sh` (or `python tools/release/zip/bbvr_launcher.py --skip-checks --menu`).
The Default profile reproduces the `out/vr_all.sh` environment, BB_HOOKS and BB_PATCHES exactly
(`tests/test_bbvr_menu.py`, which also compares against `out/vr_all.sh` when present) and leaves
`out/fp.txt` / `out/stereo.txt` byte for byte as they are.

## Where an option goes
`env` (start only), `hook` / `patch` (BB_HOOKS / BB_PATCHES, start only), `fp` (`out/fp.txt`), `stereo` (`out/stereo.txt`),
`iq` (`out/iq.txt`, `BB_XR_IQ_CONTROL`), `ui` (`out/vr_assets/ui_layouts.json`, "game"). The last four are live: the menu keeps
running while the game does and rewrites the file at once. Files are edited per key (other tokens and spacing stay; `.bak` of the
original once; a number is not rewritten when it already has the same value). `ui_layouts.json`: pristine copy in
`ui_layouts.base.json`, our overrides in `ui_layouts.user.json`, and the merged result is `ui_layouts.json`; a hand edit made
since our last write becomes the new base.
Settings: `user/bbvr_settings.json` (profile, Custom values, show-menu flag).

## DLSS tab
Needs an NVIDIA RTX GPU (registry display adapter name) and `bbport_dlss.dll` + `nvngx_dlss.dll` next to an executable that has the
VR DLSS path (its strings contain `BB_XR_DLSS_QUALITY`). Candidates in order: the main executable (`bin\`, `out\bb-probe-all<N>.exe` newest, `out\bb-probe.exe`),
then `out\dlss-vr\bb-probe.exe` (pre-rebuild fallback, as `out/dlss-vr/vr_dlss.sh`). Otherwise the choice is greyed with the reason and
the launch falls back to FSR 1 with a note. Keys: `upscale`, `dlss_quality`, `dlss_preset`, `dlss_sharpen`, `res` (eye resolution),
`BB_XR_DLSS_JITTER_SIGN=-1` (restart). Measured: 3840x2160 per eye about 57 fps against the 72 cap on an RTX 4090; Quality or
Performance should reach 72 (findings/37).

## Runtimes
Active runtime and `AvailableRuntimes` from `HKLM\SOFTWARE\Khronos\OpenXR\1`, SteamVR through `openvrpaths.vrpath`, Pimax by its install folder.
The choice only sets `XR_RUNTIME_JSON` for the game process. SteamVR with a Pimax runtime installed shows a "double images" warning.

## Public build
Options flagged `online` are left out of the page data and of the launch. `tools/release/export_public.py` sets `PUBLIC_BUILD = True` in the exported
`bbvr_settings.py` (`BB_PUBLIC_ALPHA=1` in the environment does the same); `make_release.py` ships the three modules in `launcher/`.
