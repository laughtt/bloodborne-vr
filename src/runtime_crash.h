#ifndef BB_RUNTIME_CRASH_H
#define BB_RUNTIME_CRASH_H
#ifdef _WIN32
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
/* Crash and hang capture (Windows). Writes <user dir>/crashes/<timestamp>-<kind>/{crash.dmp,crash.json}.
 * Local only: run.py decides (with the player's consent) whether a report is ever sent.
 *   BB_CRASH_CAPTURE=0   no files are written
 *   BB_HANG_SECONDS=<n>  hang watchdog: the same bundle once when the frame heartbeat stalls n s (off by default)
 *   BB_TEST_CRASH=1|hang deliberate fault (or stall) 8 s after startup, for testing the pipeline */
void bb_crash_init(const char *user_dir, unsigned char *const *guest_image);
/* Writes a bundle; info may be NULL (hang). Safe to call from the fault handler. */
void bb_crash_write(EXCEPTION_POINTERS *info, const char *kind);
#endif
#endif
