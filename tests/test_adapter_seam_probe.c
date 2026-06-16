/*
 * test_adapter_seam_probe.c — adapter-fake mechanism probe.
 *
 * Validates the mechanisms the adapter fake stands on, before any
 * scenario builds on them:
 *
 *   A. time seam — a clock_gettime / nanosleep definition in the
 *      executable's own objects wins cross-TU resolution over
 *      libSystem (A1, A2);
 *   C. CF interpose — same for CFRunLoopRunInMode (C1), and
 *      dlsym(RTLD_NEXT, ...) still reaches the REAL definitions for
 *      pass-through (C2);
 *   B. run-loop delivery — through the REAL CFRunLoopRunInMode, a
 *      signalled version-0 CFRunLoopSource performs in a private mode
 *      (B1) and a perform-callback's CFRunLoopStop returns the loop
 *      promptly (B2) — the adapter's exact wake shape.
 *
 * Each probe prints "PROBE <id>: PASS|FAIL" so a red CI run names the
 * failed mechanism from the log alone. The binary stays in the
 * adapter-fake job as a canary for toolchain/runtime drift.
 */

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <CoreFoundation/CoreFoundation.h>

#include "seam_probe_caller.h"

/* ---- interposing definitions (called from seam_probe_caller.c) ---- */

#define SEAM_PROBE_RUN_LOOP_SENTINEL 0x7E57

static int g_nanosleep_calls;
static int g_run_loop_calls;

int clock_gettime(clockid_t clock_id, struct timespec *ts)
{
    (void)clock_id;
    if (ts) { ts->tv_sec = 42; ts->tv_nsec = 0; }
    return 0;
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    (void)rqtp; (void)rmtp;
    g_nanosleep_calls++;
    return 0;
}

SInt32 CFRunLoopRunInMode(CFStringRef mode, CFTimeInterval seconds,
                          Boolean return_after_source_handled)
{
    (void)mode; (void)seconds; (void)return_after_source_handled;
    g_run_loop_calls++;
    return SEAM_PROBE_RUN_LOOP_SENTINEL;
}

/* ---- harness ------------------------------------------------------ */

static int g_failures;

static void report(const char *id, bool ok)
{
    printf("PROBE %s: %s\n", id, ok ? "PASS" : "FAIL");
    if (!ok) g_failures++;
}

/* ---- B: real-run-loop delivery ------------------------------------ */

typedef SInt32 (*run_in_mode_fn)(CFStringRef, CFTimeInterval, Boolean);

static bool g_perform_ran;
static bool g_perform_stops_loop;

static void seam_probe_perform(void *info)
{
    (void)info;
    g_perform_ran = true;
    if (g_perform_stops_loop) CFRunLoopStop(CFRunLoopGetCurrent());
}

static void run_b_probes(run_in_mode_fn real_run_in_mode)
{
    CFStringRef mode = CFSTR("mos.seam_probe.private");
    CFRunLoopSourceContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.perform = seam_probe_perform;

    CFRunLoopSourceRef src = CFRunLoopSourceCreate(NULL, 0, &ctx);
    if (!src || !real_run_in_mode) {
        report("B1", false);
        report("B2", false);
        if (src) CFRelease(src);
        return;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, mode);

    /* B1: signalled source performs; returnAfterSourceHandled=true. */
    g_perform_ran = false;
    g_perform_stops_loop = false;
    CFRunLoopSourceSignal(src);
    CFRunLoopWakeUp(CFRunLoopGetCurrent());
    SInt32 r = real_run_in_mode(mode, 0.5, true);
    report("B1", g_perform_ran && r == kCFRunLoopRunHandledSource);

    /* B2: perform calls CFRunLoopStop; returnAfterSourceHandled=false
       (the adapter's shape) — the loop must return Stopped promptly,
       not after the full 5 s interval. */
    g_perform_ran = false;
    g_perform_stops_loop = true;
    CFRunLoopSourceSignal(src);
    CFRunLoopWakeUp(CFRunLoopGetCurrent());
    CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
    r = real_run_in_mode(mode, 5.0, false);
    CFAbsoluteTime elapsed = CFAbsoluteTimeGetCurrent() - t0;
    report("B2", g_perform_ran && r == kCFRunLoopRunStopped &&
                 elapsed < 2.0);

    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), src, mode);
    CFRunLoopSourceInvalidate(src);
    CFRelease(src);
}

int main(void)
{
    /* A1: cross-TU clock_gettime resolves to our definition. */
    struct timespec ts = {0, 0};
    int rc = seam_probe_call_clock_gettime(&ts);
    report("A1", rc == 0 && ts.tv_sec == 42);

    /* A2: cross-TU nanosleep resolves to ours (a miss would both
       leave the counter at 0 and stall this binary for 1 s). */
    rc = seam_probe_call_nanosleep();
    report("A2", rc == 0 && g_nanosleep_calls == 1);

    /* C1: cross-TU CFRunLoopRunInMode resolves to ours. */
    int32_t r = seam_probe_call_run_loop(CFSTR("mos.seam_probe.unused"));
    report("C1", r == SEAM_PROBE_RUN_LOOP_SENTINEL && g_run_loop_calls == 1);

    /* C2: dlsym(RTLD_NEXT, ...) reaches the REAL definitions, distinct
       from ours — the pass-through path the fake needs. */
    void *real_rim = dlsym(RTLD_NEXT, "CFRunLoopRunInMode");
    void *real_cg  = dlsym(RTLD_NEXT, "clock_gettime");
    report("C2", real_rim && real_cg &&
                 real_rim != (void *)&CFRunLoopRunInMode &&
                 real_cg != (void *)&clock_gettime);

    /* B1/B2: delivery through the real run loop. */
    run_b_probes((run_in_mode_fn)real_rim);

    printf("seam probe: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
