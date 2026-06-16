/*
 * seam_probe_caller.c — the CALLING side of the interpose probe.
 *
 * These calls live in a separate TU from the interposing definitions
 * (test_adapter_seam_probe.c) because the property under test is ld's
 * cross-TU resolution: a reference to clock_gettime / nanosleep /
 * CFRunLoopRunInMode in one object file must bind to a definition in
 * ANOTHER object file of the same executable, ahead of libSystem /
 * CoreFoundation — the seam the deterministic-time scenarios stand on.
 *
 * Both TUs include the system headers so any Darwin asm-label renaming
 * ($NOCANCEL / $UNIX2003 variants) applies identically to reference and
 * definition.
 */

#include <time.h>
#include <CoreFoundation/CoreFoundation.h>

#include "seam_probe_caller.h"

int seam_probe_call_clock_gettime(struct timespec *ts)
{
    return clock_gettime(CLOCK_MONOTONIC, ts);
}

int seam_probe_call_nanosleep(void)
{
    struct timespec req = {1, 0}; /* a real nanosleep would take 1 s */
    return nanosleep(&req, NULL);
}

int32_t seam_probe_call_run_loop(const void *mode)
{
    return (int32_t)CFRunLoopRunInMode((CFStringRef)mode, 0.001, false);
}
