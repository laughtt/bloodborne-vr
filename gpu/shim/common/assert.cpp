// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include "common/assert.h"
#include "common/logging/log.h"

/// bbport: set when the player closed the window (bbgpu.cpp). The exit tears down the OpenXR
/// session while the GPU threads still run; a runtime that loses the device then (run20:
/// "Failed to get master semaphore value: ErrorDeviceLost" after xrRequestExitSession) made a
/// clean close end as a crash (exit 23 and a report). During the exit an assertion parks its
/// thread instead: the window thread ends the process with 0 when the teardown is done (or after
/// its 2 s timeout). docs/vr/findings/43-pm4-race.md section 4.
std::atomic<bool> bb_process_exiting{false};

void assert_fail_impl() {
    std::fflush(stdout);
    if (bb_process_exiting.load()) {
        std::fputs("Exit: GPU library assertion during the exit (see GPU log above), ignored\n",
                   stderr);
        std::fflush(stderr);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
