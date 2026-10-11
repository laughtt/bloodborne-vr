#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: MIT
#
# Builds the OpenXR bridge + sandbox3d and runs bbport_xr_sandbox3d.exe (wear the headset) from a
# fresh run folder out/sandbox3d/run-<time>/: a running game or sandbox keeps its own copy of
# bbport_xr.dll locked, and out/ itself is never written. Run from the MSYS2 CLANG64 shell:
#
#   scripts/vr_sandbox3d.sh [--extract] [--offline] [--seconds N] [--size N]
#     --extract   (re)extract the hunter and weapons, and the menu art (out/vr_assets/ui), from
#                 the dump first (also done when assets.json or ui/icons.json is missing); the
#                 dump is BB_GAME_DIR, default C:/games/CUSA03173, and is only read; the dodge,
#                 backstep and jump clips (out/vr_assets/dash) and the enemy (enemy_c2632)
#                 likewise
#     --offline   no headset: render the scripted poses to out/vr_assets/preview/sandbox3d_*.png
#                 and plot the comfort dash against the raw moves to preview/dash_*.png; the
#                 scripted weapon swings against the props go to preview/collision_*.png, the
#                 swing/stamina HUD after scripted swings to preview/combat_*.png
#     other arguments go to bbport_xr_sandbox3d.exe
#
# Tune live by editing out/vr_assets/sandbox3d.json while it runs (see
# gpu/xr_bridge/sandbox3d/README.md). BB_BUILD_LOCK=<file> serializes the build (flock).
set -euo pipefail
cd "$(dirname "$0")/.."

# From Git Bash (not the MSYS2 shell) MSYSTEM_PREFIX is unset: default to the CLANG64 tree.
prefix="${MSYSTEM_PREFIX:-/c/msys64/clang64}"
if ! command -v ninja >/dev/null 2>&1; then
    export PATH="$prefix/bin:/c/msys64/usr/bin:$PATH"
fi

extract=0
pass=()
for arg in "$@"; do
    case "$arg" in
    --extract) extract=1 ;;
    *) pass+=("$arg") ;;
    esac
done

if [[ ! -f out/xr-bridge/build.ninja ]]; then
    cmake -S gpu/xr_bridge -B out/xr-bridge -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
if [[ -n "${BB_BUILD_LOCK:-}" ]]; then
    flock "$BB_BUILD_LOCK" ninja -C out/xr-bridge
else
    ninja -C out/xr-bridge
fi
out/xr-bridge/bbport_xr_math_test.exe
out/xr-bridge/bbport_xr_dash_test.exe
out/xr-bridge/sandbox3d/bbport_xr_sandbox3d_test.exe out/vr_assets
out/xr-bridge/sandbox3d/bbport_xr_sandbox3d_ui_test.exe out/vr_assets
# Swing classification and stamina (docs/vr/findings/20): the shared logic, then the sandbox's use
# of it (scripted swings, also through the weapon physics and the locomotion's move gate).
out/xr-bridge/bbport_xr_swing_test.exe
out/xr-bridge/sandbox3d/bbport_xr_sandbox3d_combat_test.exe out/vr_assets
# Co-op pose sync (docs/vr/findings/34-coop-pose-sync.md): the packet codec and the interpolation
# buffer in virtual time, then two peers over UDP on localhost (~9 s) under 100 ms / 5% loss.
if [[ -x out/xr-bridge/bbport_xr_pose_net_test.exe ]]; then # absent with -DBBPORT_ONLINE=OFF
    out/xr-bridge/bbport_xr_pose_net_test.exe
    out/xr-bridge/sandbox3d/bbport_xr_sandbox3d_coop_test.exe
fi

python=python3
command -v "$python" >/dev/null 2>&1 || python=python
if [[ $extract == 1 || ! -f out/vr_assets/assets.json ]]; then
    "$python" scripts/vr_assets/extract.py --game "${BB_GAME_DIR:-C:/games/CUSA03173}"
fi
# The VR menus' item pictures, names and font (out/vr_assets/ui, gpu/xr_bridge/sandbox3d/README.md).
if [[ $extract == 1 || ! -f out/vr_assets/ui/icons.json ]]; then
    "$python" scripts/vr_assets/ui_extract.py --game "${BB_GAME_DIR:-C:/games/CUSA03173}"
fi
# The dodge/backstep/jump clips for the comfort dash (findings/19); without them the sandbox
# plays synthetic moves.
if [[ $extract == 1 || ! -f out/vr_assets/dash/dodges.json ]]; then
    "$python" scripts/vr_assets/dodge_extract.py --game "${BB_GAME_DIR:-C:/games/CUSA03173}" ||
        echo "dodge_extract.py failed: the sandbox plays synthetic moves"
fi
# The enemy the solid weapons collide with and hit (c2632, the Central Yharnam Huntsman; README
# "Solid weapons"); without it only its hurtbox capsules stand there.
if [[ $extract == 1 || ! -f out/vr_assets/enemy_c2632.json ]]; then
    "$python" scripts/vr_assets/extract_enemy.py --game "${BB_GAME_DIR:-C:/games/CUSA03173}" ||
        echo "extract_enemy.py failed: the enemy shows as its hurtbox capsules only"
fi
out/xr-bridge/sandbox3d/bbport_xr_sandbox3d_dash_test.exe out/vr_assets

run="out/sandbox3d/run-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$run"
cp out/xr-bridge/bbport_xr.dll out/xr-bridge/sandbox3d/bbport_xr_sandbox3d.exe \
    "$prefix/bin/libopenxr_loader.dll" "$run/"
# Keep the three newest run folders (a folder still in use stays: its files are locked).
ls -1d out/sandbox3d/run-* 2>/dev/null | sort | head -n -3 | while read -r old; do
    rm -rf "$old" 2>/dev/null || true
done

assets="$(cygpath -w "$(pwd)/out/vr_assets")"
cd "$run"
echo "running $(pwd)/bbport_xr_sandbox3d.exe --assets $assets ${pass[*]:-}"
exec ./bbport_xr_sandbox3d.exe --assets "$assets" "${pass[@]}"
