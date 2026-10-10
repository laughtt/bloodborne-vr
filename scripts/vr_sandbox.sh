#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: MIT
#
# Builds the OpenXR bridge + sandbox and runs bbport_xr_sandbox.exe (wear the headset).
# Run from the MSYS2 CLANG64 shell: scripts/vr_sandbox.sh [--image shot.bmp] [--seconds N]
# [--headlock] [--stick N]
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ ! -f out/xr-bridge/build.ninja ]]; then
    cmake -S gpu/xr_bridge -B out/xr-bridge -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
ninja -C out/xr-bridge
cp out/xr-bridge/bbport_xr.dll out/xr-bridge/bbport_xr_sandbox.exe \
    "$MSYSTEM_PREFIX/bin/libopenxr_loader.dll" out/
cd out
exec ./bbport_xr_sandbox.exe "$@"
