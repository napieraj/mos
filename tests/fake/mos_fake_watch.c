/*
 * mos_fake_watch.c — phase-2 of the link-seam fake: the notification and
 * time symbols mos_watch.c imports, so the REAL watch adapter runs
 * headless with deterministic time. Control surface: mos_fake_watch.h.
 * Design record: doc/research/2026-06-11-headless-adapter-emulation.md §12.
 *
 * Two halves:
 *
 *   1. The eight Apple symbols (IONotificationPort* +
 *      IOServiceAddInterestNotification, DRNotificationCenter*).
 *      Run-loop sources are REAL version-0 CFRunLoopSources (CF is linked
 *      real); firing enqueues an event and signals the source, so the
 *      adapter's callbacks run inside its own CFRunLoopRunInMode wait, as
 *      on hardware.
 *
 *   2. The interposed time seam: clock_gettime / nanosleep /
 *      CFRunLoopRunInMode definitions here win cross-TU resolution over
 *      libSystem/CoreFoundation for every TU in this executable; the real
 *      functions stay reachable via dlsym(RTLD_NEXT) for pass-through.
 *      INVARIANT the design hangs on: while the clock is faked, BOTH
 *      sleep primitives advance it — a fake clock under a real wait
 *      deadlocks (the pump derives the interval from fake time, real CF
 *      waits real seconds, fake time never moves).
 *
 * Capacity: ONE notification port and ONE DR center at a time (one watch
 * per test); a double-create aborts naming the test bug.
 */

#include "mos_fake_watch.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <DiscRecording/DRCoreDevice.h>
#include <DiscRecording/DRCoreNotifications.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Must mirror MOS_WATCH_RUN_LOOP_MODE in src/mos_watch.c. Drift aborts:
   an interposed call in any other mode under the fake clock fails below. */
#define FAKE_WATCH_MODE CFSTR("io.github.napieraj.mos.watch")

/* DR device sentinel delivered as the notification `object`. A per-TU
   CFSTR is fine: the fake's DRDeviceCopyInfo/CopyStatus ignore their
   device argument, and the adapter resolves identity via the Info
   dict's registry path, never by pointer. */
#define FAKE_DEV_WATCH ((DRTypeRef)CFSTR("mos.fake.device"))

/* The center handle must be a REAL CF object: mos_watch.c CFReleases it
   at teardown. Immortal CFSTR sentinel, retained per Create. */
#define FAKE_CENTER ((DRNotificationCenterRef)CFSTR("mos.fake.drcenter"))

#define FAKE_NOTIFY_TOKEN ((io_object_t)11)
#define FAKE_EVQ_CAP   8
#define FAKE_OBS_CAP   8
#define FAKE_STEP_CAP  32

static void fake_abort(const char *msg)
{
    fprintf(stderr, "mos_fake_watch: %s\n", msg);
    abort();
}

/* ---- Real functions, for pass-through (probe C2) ------------------- */

typedef SInt32 (*real_run_in_mode_fn)(CFStringRef, CFTimeInterval, Boolean);
typedef int    (*real_clock_gettime_fn)(clockid_t, struct timespec *);
typedef int    (*real_nanosleep_fn)(const struct timespec *,
                                    struct timespec *);

static real_run_in_mode_fn real_run_in_mode(void)
{
    static real_run_in_mode_fn fn;
    if (!fn) fn = (real_run_in_mode_fn)dlsym(RTLD_NEXT, "CFRunLoopRunInMode");
    if (!fn) fake_abort("dlsym(CFRunLoopRunInMode) failed");
    return fn;
}

static real_clock_gettime_fn real_clock_gettime(void)
{
    static real_clock_gettime_fn fn;
    if (!fn) fn = (real_clock_gettime_fn)dlsym(RTLD_NEXT, "clock_gettime");
    if (!fn) fake_abort("dlsym(clock_gettime) failed");
    return fn;
}

static real_nanosleep_fn real_nanosleep(void)
{
    static real_nanosleep_fn fn;
    if (!fn) fn = (real_nanosleep_fn)dlsym(RTLD_NEXT, "nanosleep");
    if (!fn) fake_abort("dlsym(nanosleep) failed");
    return fn;
}

/* ---- State ---------------------------------------------------------- */

static struct {
    bool     enabled;
    uint64_t mono_ms;        /* current fake monotonic time */
    uint64_t wall_base_ms;   /* REALTIME = wall_base + mono  */
} g_clock;

static struct {
    uint64_t at_ms;
    void   (*action)(void *);
    void    *ctx;
} g_steps[FAKE_STEP_CAP];
static size_t g_step_count, g_step_next;

/* One IOKit notification port at a time (one single-target watch). */
static struct {
    bool                       alive;
    CFRunLoopSourceRef         src;     /* port-owned, per IOKit contract */
    IOServiceInterestCallback  cb;
    void                      *refcon;
    io_service_t               service;
    uint32_t                   pending[FAKE_EVQ_CAP]; /* messageType */
    size_t                     npending;
} g_io;

/* One DR notification center at a time. */
static struct {
    bool               alive;
    CFRunLoopSourceRef src;             /* our reference; adapter has its own */
    struct {
        const void            *observer;
        DRNotificationCallback cb;
        CFStringRef            name;
    }                  obs[FAKE_OBS_CAP];
    size_t             nobs;
    CFStringRef        pending[FAKE_EVQ_CAP]; /* notification name */
    size_t             npending;
} g_dr;

static bool g_io_fail, g_dr_fail;
static bool g_drained;   /* set by a drain; tripwire for undelivered signals */

/* ---- Drains (the run-loop source perform callbacks) ----------------- */

static void io_drain(void *info)
{
    (void)info;
    g_drained = true;
    while (g_io.npending) {
        uint32_t mt = g_io.pending[0];
        g_io.npending--;
        memmove(&g_io.pending[0], &g_io.pending[1],
                g_io.npending * sizeof g_io.pending[0]);
        if (g_io.cb) g_io.cb(g_io.refcon, g_io.service, mt, NULL);
    }
}

static void dr_drain(void *info)
{
    (void)info;
    g_drained = true;
    while (g_dr.npending) {
        CFStringRef name = g_dr.pending[0];
        g_dr.npending--;
        memmove(&g_dr.pending[0], &g_dr.pending[1],
                g_dr.npending * sizeof g_dr.pending[0]);
        /* A callback may mutate the observer list; the drain doesn't
           assume it won't. */
        for (size_t i = 0; i < g_dr.nobs; ++i) {
            if (CFEqual(g_dr.obs[i].name, name) && g_dr.obs[i].cb) {
                g_dr.obs[i].cb(FAKE_CENTER, (void *)g_dr.obs[i].observer,
                               name, FAKE_DEV_WATCH, NULL);
            }
        }
    }
}

static bool any_pending(void)
{
    return (g_io.alive && g_io.npending > 0) ||
           (g_dr.alive && g_dr.npending > 0);
}

/* ---- Control surface ------------------------------------------------ */

static void io_port_teardown(void)
{
    if (g_io.src) {
        CFRunLoopSourceInvalidate(g_io.src); /* before release: no stale
            signal may perform into a dead port in a later test */
        CFRelease(g_io.src);
    }
    memset(&g_io, 0, sizeof g_io);
}

static void dr_center_teardown(void)
{
    if (g_dr.src) {
        CFRunLoopSourceInvalidate(g_dr.src);
        CFRelease(g_dr.src);
    }
    memset(&g_dr, 0, sizeof g_dr);
}

void mos_fake_watch_reset(void)
{
    io_port_teardown();
    dr_center_teardown();
    memset(&g_clock, 0, sizeof g_clock);
    memset(g_steps, 0, sizeof g_steps);
    g_step_count = g_step_next = 0;
    g_io_fail = g_dr_fail = false;
    g_drained = false;
}

void mos_fake_clock_enable(uint64_t mono_start_ms, uint64_t wall_base_ms)
{
    g_clock.enabled      = true;
    g_clock.mono_ms      = mono_start_ms;
    g_clock.wall_base_ms = wall_base_ms;
}

uint64_t mos_fake_clock_now(void) { return g_clock.mono_ms; }

void mos_fake_step(uint64_t at_mono_ms, void (*action)(void *), void *ctx)
{
    if (g_step_count == FAKE_STEP_CAP) fake_abort("timeline full");
    if (g_step_count > 0 && g_steps[g_step_count - 1].at_ms > at_mono_ms) {
        fake_abort("timeline steps must be registered in time order");
    }
    g_steps[g_step_count].at_ms  = at_mono_ms;
    g_steps[g_step_count].action = action;
    g_steps[g_step_count].ctx    = ctx;
    g_step_count++;
}

static void fire_io(uint32_t message_type)
{
    if (!g_io.alive || !g_io.cb) return;  /* no observer: dropped */
    if (g_io.npending == FAKE_EVQ_CAP) fake_abort("io event queue full");
    g_io.pending[g_io.npending++] = message_type;
    if (g_io.src) CFRunLoopSourceSignal(g_io.src);
}

void mos_fake_fire_io_termination(void)
{
    fire_io(kIOMessageServiceIsTerminated);
}

void mos_fake_fire_io_property_change(void)
{
    fire_io(kIOMessageServicePropertyChange);
}

static void fire_dr(CFStringRef name)
{
    if (!g_dr.alive || g_dr.nobs == 0) return;  /* no observer: dropped */
    if (g_dr.npending == FAKE_EVQ_CAP) fake_abort("dr event queue full");
    g_dr.pending[g_dr.npending++] = name;
    if (g_dr.src) CFRunLoopSourceSignal(g_dr.src);
}

void mos_fake_fire_dr_status_changed(void)
{
    fire_dr(kDRDeviceStatusChangedNotification);
}

void mos_fake_fire_dr_appeared(void)
{
    fire_dr(kDRDeviceAppearedNotification);
}

void mos_fake_fire_dr_disappeared(void)
{
    fire_dr(kDRDeviceDisappearedNotification);
}

void mos_fake_set_io_notify_fail(bool fail) { g_io_fail = fail; }
void mos_fake_set_dr_center_fail(bool fail) { g_dr_fail = fail; }

int mos_fake_outstanding_notify_objects(void)
{
    return (g_io.alive ? 1 : 0) + (g_dr.alive ? 1 : 0) + (int)g_dr.nobs;
}

/* ---- IOKit notification symbols ------------------------------------- */

IONotificationPortRef IONotificationPortCreate(mach_port_t mainPort)
{
    (void)mainPort;
    if (g_io_fail) return NULL;
    if (g_io.alive) fake_abort("second IONotificationPortCreate while one "
                               "is live (one watch per test)");
    g_io.alive = true;
    return (IONotificationPortRef)&g_io;
}

CFRunLoopSourceRef IONotificationPortGetRunLoopSource(IONotificationPortRef notify)
{
    if ((void *)notify != (void *)&g_io || !g_io.alive) return NULL;
    if (!g_io.src) {
        CFRunLoopSourceContext ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.perform = io_drain;
        g_io.src = CFRunLoopSourceCreate(NULL, 0, &ctx);
    }
    /* Port-owned: caller must not release it (IOKit contract); it only
       removes it from the loop. */
    return g_io.src;
}

void IONotificationPortDestroy(IONotificationPortRef notify)
{
    if ((void *)notify != (void *)&g_io || !g_io.alive) {
        fake_abort("IONotificationPortDestroy on a dead/foreign port");
    }
    io_port_teardown();
}

kern_return_t IOServiceAddInterestNotification(IONotificationPortRef notifyPort,
                                               io_service_t service,
                                               const io_name_t interestType,
                                               IOServiceInterestCallback callback,
                                               void *refCon,
                                               io_object_t *notification)
{
    (void)interestType;
    if ((void *)notifyPort != (void *)&g_io || !g_io.alive || !callback ||
        !notification) {
        return KERN_FAILURE;
    }
    g_io.cb      = callback;
    g_io.refcon  = refCon;
    g_io.service = service;
    *notification = FAKE_NOTIFY_TOKEN;
    return KERN_SUCCESS;
}

/* ---- DiscRecording notification symbols ----------------------------- */

DRNotificationCenterRef DRNotificationCenterCreate(void)
{
    if (g_dr_fail) return NULL;
    if (g_dr.alive) fake_abort("second DRNotificationCenterCreate while one "
                               "is live (one watch per test)");
    g_dr.alive = true;
    /* Retained: balances the adapter's CFRelease at teardown. */
    return (DRNotificationCenterRef)CFRetain(FAKE_CENTER);
}

CFRunLoopSourceRef DRNotificationCenterCreateRunLoopSource(DRNotificationCenterRef center)
{
    (void)center;
    if (!g_dr.alive) return NULL;
    if (g_dr.src) fake_abort("second DRNotificationCenterCreateRunLoopSource");
    CFRunLoopSourceContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.perform = dr_drain;
    g_dr.src = CFRunLoopSourceCreate(NULL, 0, &ctx);
    if (!g_dr.src) return NULL;
    /* Create-rule: caller owns and releases one reference at teardown;
       g_dr.src above is OUR reference. */
    return (CFRunLoopSourceRef)CFRetain(g_dr.src);
}

void DRNotificationCenterAddObserver(DRNotificationCenterRef center,
                                     const void *observer,
                                     DRNotificationCallback callback,
                                     CFStringRef name, DRTypeRef object)
{
    (void)center; (void)object; /* adapter always observes all devices */
    if (!g_dr.alive) fake_abort("AddObserver on a dead center");
    if (g_dr.nobs == FAKE_OBS_CAP) fake_abort("observer table full");
    g_dr.obs[g_dr.nobs].observer = observer;
    g_dr.obs[g_dr.nobs].cb       = callback;
    g_dr.obs[g_dr.nobs].name     = name;
    g_dr.nobs++;
}

void DRNotificationCenterRemoveObserver(DRNotificationCenterRef center,
                                        const void *observer,
                                        CFStringRef name, DRTypeRef object)
{
    (void)center; (void)object;
    if (!g_dr.alive) fake_abort("RemoveObserver on a dead center");
    for (size_t i = 0; i < g_dr.nobs; ++i) {
        if (g_dr.obs[i].observer == observer &&
            CFEqual(g_dr.obs[i].name, name)) {
            g_dr.nobs--;
            memmove(&g_dr.obs[i], &g_dr.obs[i + 1],
                    (g_dr.nobs - i) * sizeof g_dr.obs[0]);
            break;
        }
    }
    /* The adapter removes observers only at teardown, so the count
       reaching zero is the observable "center died" moment (its
       CFRelease lands on the immortal sentinel, invisible). The source
       stays valid for the adapter's own RemoveSource/CFRelease, which
       hold their own retain. */
    if (g_dr.nobs == 0) dr_center_teardown();
}

/* ---- Interposed time seam ------------------------------------------- *
 *
 * These definitions win cross-TU resolution for mos_watch.c's calls.
 * When the fake clock is disabled they pass through to the real
 * functions, so startup, CF internals, and non-clock tests are normal.
 */

int clock_gettime(clockid_t clock_id, struct timespec *ts)
{
    if (!g_clock.enabled) return real_clock_gettime()(clock_id, ts);
    if (!ts) return -1;
    uint64_t ms;
    if (clock_id == CLOCK_MONOTONIC) {
        ms = g_clock.mono_ms;
    } else if (clock_id == CLOCK_REALTIME) {
        ms = g_clock.wall_base_ms + g_clock.mono_ms;
    } else {
        return real_clock_gettime()(clock_id, ts);
    }
    ts->tv_sec  = (time_t)(ms / 1000);
    ts->tv_nsec = (long)(ms % 1000) * 1000000L;
    return 0;
}

/* Advance fake time to `target_ms`, running due timeline steps in order
   as the clock passes them. Shared by nanosleep and the run-loop
   interpose's timeout arm. */
static void advance_to(uint64_t target_ms)
{
    while (g_step_next < g_step_count &&
           g_steps[g_step_next].at_ms <= target_ms) {
        size_t i = g_step_next++;
        if (g_steps[i].at_ms > g_clock.mono_ms) {
            g_clock.mono_ms = g_steps[i].at_ms;
        }
        if (g_steps[i].action) g_steps[i].action(g_steps[i].ctx);
    }
    if (target_ms > g_clock.mono_ms) g_clock.mono_ms = target_ms;
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    if (!g_clock.enabled) return real_nanosleep()(rqtp, rmtp);
    if (!rqtp) return -1;
    uint64_t req_ms = (uint64_t)rqtp->tv_sec * 1000ULL +
                      (uint64_t)(rqtp->tv_nsec / 1000000L);
    advance_to(g_clock.mono_ms + req_ms);
    if (rmtp) { rmtp->tv_sec = 0; rmtp->tv_nsec = 0; }
    return 0;
}

SInt32 CFRunLoopRunInMode(CFStringRef mode, CFTimeInterval seconds,
                          Boolean returnAfterSourceHandled)
{
    if (!g_clock.enabled) {
        return real_run_in_mode()(mode, seconds, returnAfterSourceHandled);
    }
    /* The only interposed caller is mos_watch.c's pump, always in its
       private mode. Any other mode means the mode string drifted from
       FAKE_WATCH_MODE. */
    if (!CFEqual(mode, FAKE_WATCH_MODE)) {
        fake_abort("CFRunLoopRunInMode in an unexpected mode under the "
                   "fake clock (mode-constant drift?)");
    }

    /* Reconstruct the pump's absolute deadline from the interval it
       derived from our fake clock (ms-scale doubles round-trip
       losslessly). Saturate an unbounded timeout's huge interval. */
    uint64_t wait_end;
    if (seconds >= 9.0e15) {  /* ~285 My: anything near UINT64_MAX/1000 */
        wait_end = UINT64_MAX;
    } else {
        uint64_t add = (uint64_t)llround(seconds * 1000.0);
        wait_end = (g_clock.mono_ms > UINT64_MAX - add)
                       ? UINT64_MAX
                       : g_clock.mono_ms + add;
    }

    for (unsigned iter = 0;; ++iter) {
        if (iter > 10000) fake_abort("runaway run-loop interpose (scenario "
                                     "emits nothing and never times out)");

        /* 1. Pending notifications deliver before time moves: delegate
           ONE batch to the real run loop, whose perform invokes the
           adapter's callbacks (they CFRunLoopStop, as on hardware).
           Return after the batch; the pump re-pumps. */
        if (any_pending()) {
            g_drained = false;
            SInt32 r = real_run_in_mode()(mode, 0.05,
                                          /*returnAfterSourceHandled=*/true);
            if (!g_drained) {
                fake_abort("signalled source did not perform — source not "
                           "scheduled in the watch mode?");
            }
            return r;
        }

        /* 2. Next scripted step within this wait: advance to it, run it,
           loop (it may fire notifications or move state). */
        if (g_step_next < g_step_count &&
            g_steps[g_step_next].at_ms <= wait_end) {
            size_t i = g_step_next++;
            if (g_steps[i].at_ms > g_clock.mono_ms) {
                g_clock.mono_ms = g_steps[i].at_ms;
            }
            if (g_steps[i].action) g_steps[i].action(g_steps[i].ctx);
            continue;
        }

        /* 3. Nothing left in this wait: time out at the pump's deadline.
           An unbounded wait with an exhausted timeline never ends — a
           scenario-authoring bug; name it. */
        if (wait_end == UINT64_MAX) {
            fake_abort("unbounded run-loop wait with an empty timeline");
        }
        if (wait_end > g_clock.mono_ms) g_clock.mono_ms = wait_end;
        return kCFRunLoopRunTimedOut;
    }
}
