# 21. "Out of order flip IRQ" crash (fixed, watch for it)

Status: fixed in 9079c3b; a soak (6 cold starts + a long world run) passed with 0 asserts and
0 warnings. Parked. If it shows up again, start here.

## Symptom

`Assertion Failed ... Out of order flip IRQ` (video_out.cpp), then an nvwgf2umx fault while the
process exits. Seen once (run vr16) at the title screen, with VR off and no hooks: not a VR bug.

## Cause (about 65% confidence)

A race on the video-out buffer label. The flip IRQ runs right after the command buffer writes
label=1, so finding 0 means a 0 landed in between. The only writer is `VideoOutDriver::Flip`
releasing the previously shown buffer, which it did *after* publishing the flip (status and
events), as upstream does. The guest present code (0x26d6e50) polls `sceVideoOutGetFlipStatus`
until `flip_arg >= frame+1-N`, then EOP-flips buffer `frame % N`. If the present thread is
preempted between the events and the release, the guest's new 1 lands before our late 0.
`SetCurrentThreadRealtime` does nothing on Windows, so preemption is easier there. The guest
gating is inferred from disassembly, not observed live. Upstream shadPS4 still has the assert.

## Fix

- `driver.cpp` `Flip()`: release the previous buffer's label before the status update and events.
- `video_out.cpp`: the assert is a rate-limited warning:
  `Flip IRQ #n: buffer B label is L, not 1 (released by the present thread N us ago); flipping it anyway`.
- `driver.h`: per-buffer release timestamp (diagnostics only).
- `platform.h`: all IRQ contexts are created up front (no unsynchronized map insert at first use).

## If it happens again

1. Grep the log: `grep -E 'Flip IRQ #|Out of order|Assertion Failed' session.log`.
   - Small "released N us ago": another late-release path, now absorbed by the warning.
   - N = -1 or large: an IRQ-order mismatch, a different bug; look at the IRQ/EOP ordering.
2. Re-run the soak (flat, no VR, saves protected, the game exits by itself):
   `bash tools/flip_soak.sh [world_minutes=30] [cold_starts=6]`
   (env: `BB_GAME_DIR`, default C:/games/CUSA03173; `BB_PROBE`; `BB_SOAK_OUT`, default out/flipsoak).
   Pass per run: `BB_EXIT_AFTER_SECONDS: exiting`, no assert, `Frame stats` to the end.
