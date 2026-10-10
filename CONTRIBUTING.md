# Contributing

Thanks for helping. This project is a single-player VR mod of a game you must own; keep it
legal and keep it focused.

## Ground rules

* **No game material, ever**: no eboot, dumps, extracted models/textures/audio/params, save
  files, screenshots of dumps, or links to places that host them. Issues and PRs containing
  them are closed and deleted. Short byte patterns in `patches/Bloodborne.xml` (a few bytes at
  an address, with the bytes they replace) are the only game-derived data we keep.
* **No online or co-op work** in this repository (alpha scope: single-player VR). No server
  hosts, accounts, keys or protocol notes.
* **No secrets or personal data**: no tokens, e-mail addresses, user names in paths (`C:\Users\...`),
  IP addresses. Use `C:\games\CUSA03173`-style placeholders in docs.
* Licence: contributions are GPL-2.0-or-later (files under `gpu/xr_bridge/` are MIT). Add the
  SPDX header used in neighbouring files to new files.

## Workflow

1. Open an issue first for anything larger than a fix.
2. Branch from `main`, keep commits small, say why in the message.
3. Build with `bash build.sh` (MSYS2 CLANG64). Run the unit tests that touch your change
   (the `*-test` targets, `tests/*.py`); they need no game and no headset.
4. If you changed hooks or `patches/Bloodborne.xml`, state how you verified the `Expect` bytes
   (the first-run helper checks them against the executable).
5. Line endings: new files use LF; do not reformat files you only touch.
6. Open the PR with: what changed, how you tested it (headset model, runtime, GPU), and the
   log if relevant.

## Reporting bugs

Use the "Bug report" issue form. Attach `logs/bbvr-latest.log`, your GPU and driver, headset and
OpenXR runtime, and the crash id if you opted in to crash reports (`docs/CRASH_REPORTS.md`).
