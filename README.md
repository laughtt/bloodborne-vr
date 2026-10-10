# Bloodborne VR (alpha)

Play your own copy of **Bloodborne** (PS4, CUSA03173, version 1.09) in VR on Windows. The game's
own PS4 x86-64 code runs natively on a shadPS4-derived Vulkan renderer; an OpenXR bridge adds
stereo rendering, head tracking, first-person view, controller-driven arms and weapons, and VR
menus.

> **Alpha, single-player only.** Expect bugs and comfort issues. Online and co-op features are
> not part of this release.
>
> **No game files are included.** You need your own legally obtained, decrypted dump. This is an
> unofficial fan project, not affiliated with, endorsed or sponsored by Sony Interactive
> Entertainment, FromSoftware or any trademark holder. PlayStation, PS4 and Bloodborne are
> trademarks of their owners.

## Play (testers)

Download the latest `BloodborneVR-<version>.zip` from Releases, unzip it, double-click
`BloodborneVR.bat` and follow the first-run questions. The zip's `README.txt` has the install
steps, controls, known issues and how to report bugs.

## What works (see `docs/vr/`)

First-person camera with head tracking, true stereo (alternate-eye and renderer stereo), arm IK
and weapons that follow the controllers, comfort dash and turning, stamina and swing feedback,
hit-shape scaling, VR menus (quick wheel, inventory, weapon wheel), hand-thrown goods in the
sandbox. Research notes and design documents are in `docs/vr/findings/`.

## Build from source (Windows)

1. Install MSYS2 and open the **CLANG64** shell. Packages: `packaging/windows/README.md`.
2. `git clone --recursive <this repo>`, then `bash build.sh`
   (outputs in `out/`: `bb-probe.exe`, `bb-gpu-capabilities.exe`, `bbport_xr.dll`).
   `build.sh` also applies the patch series in `gpu/patches/fsr-vulkan/` to the `gpu/third_party/fsr-vulkan`
   submodule (needed; the submodules are pinned in this repository). The optional DLSS bridge (`bbport_dlss.dll`,
   needs NVIDIA's DLSS SDK) is not part of this repository yet.
3. Run with your own dump:
   `BB_GAME_DIR=/c/games/CUSA03173 python run.py` (flat) or see `tools/release/zip/bbvr_launcher.py`
   for the VR environment variables.
4. Package: `python tools/release/make_release.py` (needs the build output; see
   `docs/RELEASE_CHECKLIST.md` if you are publishing).

Unit tests are `EXCLUDE_FROM_ALL` targets in `gpu/CMakeLists.txt` and `gpu/xr_bridge/`.

## Help and bugs

Use the issue templates. Attach `logs/bbvr-latest.log` and never post saves or game files.
Crash reports are opt-in: `docs/CRASH_REPORTS.md`. Contributing: `CONTRIBUTING.md`.

## Licence

GPL-2.0-or-later (`LICENSE`); binary releases are GPLv3 because they bundle Apache-2.0 and
GPLv3 libraries (`COPYING.GPL-3.0`, `THIRD_PARTY_NOTICES.md`).

## Credits

Built on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28, the Windows port
by [Supermedo](https://github.com/Supermedo), the [shadPS4](https://github.com/shadps4-emu/shadPS4)
emulator, community patches by illusion, Kyo and others, FSR-Vulkan (FireBurn), Dear ImGui,
LibAtrac9, and the OpenXR / Khronos projects.
