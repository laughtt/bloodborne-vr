# 43. "Unimplemented PM4 type 0" at the world load, and silent exits at startup

Status: fixed for the PM4 crash (command buffers copied at submit, invalid packets reported and
skipped). The silent exits are not explained by the logs; every exit now names its caller.
Build: `out/stereo_ab/run17/bb-probe-pm4fix1.exe` (branch `pm4-race`).

## 1. The crash

```
GPU [Debug] <Critical> liverpool.cpp:744 ProcessGraphics: Unreachable code!
Unimplemented PM4 type 0, base reg: 41162, size: 4372
STOP: GPU library assertion failed (see GPU log above)
Fault: read access to ffffffffffffffff at nvwgf2umx.dll+0x693e9b
  rax=dededededededede ...
```

About 3 in 15 headset launches on 2026-10-12, and also in flat runs. It always comes in the first
seconds of the world, after the loading screen. A retry works.

| log | mode | header the parser read |
|---|---|---|
| out/coop_test/flat3_all74_crash1.log (10-11) | flat, co-op | `00000002` (type 0, base 2, 1 word) |
| out/stereo_ab/run8/chain1.session.log | flat (BB_VR=0), before the cache changes | `00000000` |
| out/stereo_ab/run8/k1.session.log | flat, about 10 s into the world | type 1 ("Wrong PM4 type 1") |
| out/vr_all_stereo_run10crash.log | VR stereo | `1113a0ca` (base 0xa0ca, 4372 words) |
| out/vr_all_stereo_run12f_crash.log | VR stereo | `00000002` |
| findings/23 section 4.5 (Phase 4 prototype) | flat, BB_UPSCALER=off | type 0, "not attributed" |

So it is not stereo, not the new caches, not VR: those only make it more likely.

### 1.1 Evidence

- **Every one of these logs has the game's own line `== Stall during rendering at flush 1` shortly
  before.** The string is not in the port; it is FromSoftware's GX code noticing that the GPU did
  not finish a flush in time. "flush 1" at the world load is the usual case (191 of 193 stall lines
  in out/), when the GPU thread compiles the world's pipelines and falls behind.
- The words read as headers are zeros or data: freshly zeroed memory (`direct_release` zeroes
  released pages, runtime_memory.c `host_discard`) or a buffer the guest is writing again.
- The parser read guest memory late. `SubmitGfx` only creates the `ProcessGraphics` coroutine; the
  GPU thread parses the buffer when it gets to it: after earlier submissions, pipeline compiles,
  waits on the draw pipe, and in VR the presenter (`BB_FRAMES_AHEAD`), which is why VR sees it
  more often. Nothing kept the guest from reusing the memory in the meantime: the guest decides
  that from labels and its own timeouts, not from when the port parses.
- The draw preparation workers already parse a copy of each buffer taken at submit
  (`DrawPreparation::Build`). The GPU thread was the only reader of the live memory, and it relies
  on the copy and the live memory being the same (prepared draws are matched by count).
- Bloodborne's graphics buffers call no nested indirect buffers (docs/parallel_gpu.md), so the
  top-level DCB and CCB are what got read late.
- The `nvwgf2umx.dll` fault with `rax=dededede...` is secondary: the assertion ends the process
  with `_Exit(23)` while the Vulkan threads still run, and the driver touches freed objects during
  DLL detach. It also replaced the real error in the crash bundle (kind "fault", driver frame).

What the guest does exactly after the stall (free the chunk, clear it, or reuse it after a timeout)
is inferred, not observed. The fix does not depend on it, and the new log line (2.1) shows the
rewrite when it happens.

Confidence: high that the parser decoded memory the guest had changed after submitting it;
medium on the exact guest action.

## 2. The fix (gpu/shadps4/video_core/amdgpu/pm4_snapshot.h, liverpool.cpp)

1. **Copy at submit.** `SubmitGfx` copies the DCB and CCB (`Pm4::TakeSnapshot`) before queueing,
   and `ProcessGraphics` / `ProcessCeUpdate` parse the copy. The coroutine owns the copy until it
   is done. 64 zero dwords after each copy keep the parser's look-ahead (the NOP hints after render
   target setup) inside the allocation. Indirect buffers are copied too with `BB_PM4_SNAPSHOT_IB=1`
   (only those whose memory is mapped, at most 4 levels and 64 MiB per submission). `BB_PM4_SNAPSHOT=0`
   parses guest memory as before. This also replaces `BB_COPY_GPU_BUFFERS` (whose copy area is
   reset at `sceGnmSubmitDone` while the GPU thread may still parse it).
2. **Rewind.** The guest sets a Rewind packet's valid bit after submitting; the parser waits on the
   guest's word and copies the rest of the buffer again once it is set.
3. **Detector.** Ahead of the parser, the copy is compared with guest memory in windows of 512
   dwords (one fault-guarded read each, `runtime_fault_recover`). A difference is a word the guest
   changed before it was parsed, i.e. what used to crash:
   `PM4: the guest rewrote a DCB command buffer after submitting it, before the GPU thread parsed
   it: buffer ... (N dwords, submitted X ms ago), word +W (parser at +P): submitted ..., now ...`
   (once per buffer; the first 16, then at powers of two). `BB_PM4_CHECK=0` turns it off.
4. **Guard.** A header that is no packet (type 0 or 1), a packet that runs past its buffer, or an
   unknown type-3 opcode no longer asserts: one line with the engine, the guest address, the offset,
   the words around it and what guest memory holds there now, then the rest of that buffer is
   skipped (no way to find the next packet). `BB_PM4_STRICT=1` keeps the old stop. The constant
   engine accepts type-2 padding like the draw engine.

Tests: `tests/test_pm4_snapshot.cpp` (`ninja -C out/gpu pm4-snapshot-test`): the crash headers are
rejected, the copy keeps the submitted words after the "guest" zeroes its buffer, the detector's
comparison finds the rewritten word, indirect buffers are copied by offset with address/size
checks, unmapped targets and the budget are respected, a scan stops at garbage.

## 3. Silent exits at startup

Three launches ended with exit code 1 and no error line, about 7 s after start, right after the VR
session started (`VR: screen placed ...`), before the pipeline cache warm-up line that follows in a
good launch: out/vr_all_stereo_run12b_crash1.log (15:46), run16 (16:24, its log was overwritten),
out/vr_all_stereo_run12g_crash1.log (16:35, report 20261012-163518-520e). A retry worked each time.
(The exit-1 reports of 12:01-12:03 are a different case: they carry the shader cache errors.)

What the logs rule out:
- Not the PM4 assertion: that exits with 23 and prints `STOP:`; faults go through the vectored
  handler (exit 3, `Fault:` line and a bundle). None of these appear.
- Every `exit(1)` / `_exit(1)` / `fail()` in src/ and gpu/ prints a line first (stdout is
  unbuffered, probe.c). A guest `exit`/`_exit` prints `Runtime: guest requested ...`.
- So something exited with 1 without printing: a library calling `ExitProcess`/`exit` (the OpenXR
  runtime, NGX, the driver) or a C runtime path. The logs cannot tell which.

Change (src/runtime_crash.c): `ntdll!RtlExitUserProcess`, which every exit but `TerminateProcess`
goes through, is patched at startup. A nonzero exit prints
`Exit: the process is exiting with code N on thread T, called from:` with a module+offset
backtrace, writes a bundle (`<user>/crashes/<time>-exitN`, minidump with all threads), then exits
with the same code. `BB_EXIT_TRACE=0` turns it off. The next silent exit names its caller. It also
gives the PM4-type assertion (and every other `STOP:`) a bundle from the failing thread, before
DLL detach can fault in the driver.

## 4. Device lost at shutdown (run20)

out/vr_all_stereo_run20_devlost.log: the player closed the window; `VR: shutdown: waiting for the
GPU to go idle`, `xrRequestExitSession`, then `vk_semaphore.cpp:38 Refresh: Assertion Failed!
Failed to get master semaphore value: ErrorDeviceLost` and a crash report.

The window thread tears the OpenXR session down (`Xr::ShutdownForExit`, 2 s limit) and then ends
the process with `TerminateProcess(0)`. The game and the GPU threads keep running meanwhile:
the bridge's `vkDeviceWaitIdle` does not stop them (and is not externally synchronized with their
queue submissions, which Vulkan requires), and the runtime lost the device around
`xrRequestExitSession`. The next semaphore read on a GPU thread asserted, and `_Exit(23)` turned a
clean close into a crash.

Change (gpu/shim/common/assert.cpp, bbgpu.cpp): once the window is closed (`bb_process_exiting`),
an assertion prints `Exit: GPU library assertion during the exit ..., ignored` and parks its
thread; the window thread finishes the teardown and exits with 0 as before. Not done: stopping
the GPU threads before the bridge's wait-idle (the clean order), a larger change.

## 5. Open

- Confirm with the detector line whether the guest rewrites submitted buffers at the world load
  (expected right after `== Stall during rendering at flush 1`).
- Read the first `Exit:` backtrace of a silent exit.
