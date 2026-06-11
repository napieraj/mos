/*
 * probe0_caller.c — the CALLING side of the phase-2 interpose probe.
 *
 * These calls live in a separate TU from the interposing definitions
 * (test_adapter_probe0.c) because the property under test is ld's
 * cross-TU resolution: a reference to clock_gettime / nanosleep /
 * CFRunLoopRunInMode emitted in one object file must bind to a
 * definition in ANOTHER object file of the same executable, ahead of
 * libSystem / CoreFoundation. That is the seam src/mos_watch.c's
 * deterministic-time scenarios stand on (design record §12,
 * doc/research/2026-06-11-headless-adapter-emulation.md).
 *
 * Both TUs include the system headers so any Darwin asm-label
 * renaming ($NOCANCEL / $UNIX2003 variants) applies identically to
 * the reference and the definition — the same condition mos_watch.c
 * compiles under.
 */

#include <time.h>
#include <CoreFoundation/CoreFoundation.h>

#include "probe0_caller.h"

int probe0_call_clock_gettime(struct timespec *ts)
{
    return clock_gettime(CLOCK_MONOTONIC, ts);
}

int probe0_call_nanosleep(void)
{
    struct timespec req = {1, 0}; /* a real nanosleep would take 1 s */
    return nanosleep(&req, NULL);
}

int32_t probe0_call_run_loop(const void *mode)
{
    return (int32_t)CFRunLoopRunInMode((CFStringRef)mode, 0.001, false);
}
