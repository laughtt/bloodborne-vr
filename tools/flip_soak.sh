#!/usr/bin/env bash
# Flip IRQ soak for the "Out of order flip IRQ" fix (video_out.cpp, driver.cpp, platform.h).
# Flat, no hooks, default FPS preset (uncap: vblank 480 Hz, frame limit 60), out/gpu/bb-probe.exe,
# saves protected (each run plays on a copy of user/). The game always ends itself
# (BB_EXIT_AFTER_SECONDS); this script never kills it.
#   bash tools/flip_soak.sh [world_minutes=30] [cold_starts=6]
# Part 1: cold starts to the title screen (vr16 crashed there). Part 2: one long run in the world,
# the camera turned now and then (like tools/soak.sh). Logs: $OUT/<run>/session.log.
set -u
PORT=<repo>
OUT=${BB_SOAK_OUT:-$PORT/out/flipsoak}
MINUTES=${1:-30}
STARTS=${2:-6}

press() { echo "$2" > "$1"; sleep "${3:-0.3}"; : > "$1"; }

run() { # name seconds enter_world
    local dir="$OUT/$1"
    rm -rf "$dir" && mkdir -p "$dir" && cp -r "$PORT/user" "$dir/user" && : > "$dir/pad"
    (cd "$PORT" && env -u BB_VR -u BB_XR_MODE -u BB_HOOKS -u BB_VR_HEAD -u BB_VR_FAKE_HEAD \
        -u BB_FPS -u BB_VBLANK_HZ \
        BB_GAME_DIR="${BB_GAME_DIR:-C:/games/CUSA03173}" BB_PROBE="${BB_PROBE:-$PORT/out/gpu/bb-probe.exe}" BB_USER_DIR="$dir/user" BB_PAD_FILE="$dir/pad" \
        BB_FRAME_STATS=1 BB_EXIT_AFTER_SECONDS="$2" python run.py > "$dir/session.log" 2>&1) &
    local game=$!
    if [ "$3" = 1 ]; then
        local waited=0
        until [ "$(grep -c '^Frame stats' "$dir/session.log" 2>/dev/null)" -ge 3 ] &&
              grep -q 'pad opened' "$dir/session.log" 2>/dev/null; do
            sleep 2; waited=$((waited + 2))
            kill -0 "$game" 2>/dev/null || break
            [ "$waited" -ge 240 ] && break
        done
        sleep 20
        press "$dir/pad" cross; sleep 6; press "$dir/pad" cross   # continue the save
        while kill -0 "$game" 2>/dev/null; do                     # turn the camera now and then
            sleep 14; press "$dir/pad" rx=60 2; sleep 14; press "$dir/pad" rx=195 2
        done
    fi
    wait "$game"
    local log="$dir/session.log"
    # PASS: survived to its deadline (self-exit 1), no assert, Frame stats until the end.
    printf '%-7s self-exit=%s asserts=%s flip-irq-reports=%s frame-stats=%s last: %s\n' "$1" \
        "$(grep -c 'BB_EXIT_AFTER_SECONDS: exiting' "$log")" \
        "$(grep -c -E 'Assertion Failed|Out of order flip IRQ|STOP: GPU' "$log")" \
        "$(grep -c 'Flip IRQ #' "$log")" "$(grep -c '^Frame stats' "$log")" \
        "$(tail -1 "$log" | cut -c1-60)"
    grep -m3 -E 'Flip IRQ #|Assertion Failed|Out of order|STOP: GPU|^Fault' "$log"
}

mkdir -p "$OUT"
for i in $(seq 1 "$STARTS"); do run "start$i" 100 0; done
run world $((MINUTES * 60 + 120)) 1
