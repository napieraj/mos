/*
 * mos_watch.c — Apple-side adapter for the pure watch state machine.
 *
 * The state machine lives in src/mos_watch_core.c (pure, no IOKit).
 * This file does three things:
 *
 *   1. Implements the public watch API (mos_watch_open_by_bsd_name,
 *      mos_watch_open_by_index, mos_watch_next_event, mos_watch_close).
 *
 *   2. Wires the mos_watch_ops_t vtable to real implementations:
 *      - probe()    → open mos_handle_t, query state, close handle
 *      - mono_ms()  → CLOCK_MONOTONIC in milliseconds (scheduling)
 *      - wall_ms()  → CLOCK_REALTIME in milliseconds (stream_open_ms / ts)
 *
 *   3. Registers kIOGeneralInterest notifications on the watched drive
 *      so device removal wakes the run loop and triggers a clean
 *      terminal event without waiting for the next scheduled poll.
 *
 * The per-probe open/close cycle is deliberate: a held handle keeps the
 * drive reserved for the whole watch, conflicting with DiskArbitration,
 * Finder, and other tools. A fresh handle per probe also tolerates a
 * transient driver detach without poisoning later polls. The retained
 * io_service_t we hold for the notification is just an IOKit reference,
 * not an active client connection.
 *
 * Threading: single-threaded by contract. The notification callback
 * fires on the run loop thread, which is the same thread calling
 * mos_watch_next_event. No locking needed.
 */

/* Must precede any system header so BSD extensions stay visible on
   Apple's SDK. The strlcpy call sites this originally served moved to
   mos_scsi.c during the string-copy normalization; the define stays
   because the amalgamation concatenates the adapter TUs into one
   feature-macro environment (scripts/amalgamate.sh adds a prologue copy of these (the per-TU defines stay as #ifndef no-ops) for its
   prologue), and dropping it here would make the standalone-TU and
   amalgamated builds see different SDK surfaces. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

/* CLOCK_MONOTONIC and clock_gettime require POSIX.1-2008. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mos_internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOBSD.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Private run-loop mode for the watch's IOKit and DA sources — never
   kCFRunLoopDefaultMode — so a host's default-mode work can't dispatch our
   callbacks and our CFRunLoopStop can't halt a run-loop invocation the host
   owns. The pump runs this same mode, so our sources fire only while
   mos_watch_next_event is waiting. (Caller-facing contract in mos.h.) */
#define MOS_WATCH_RUN_LOOP_MODE CFSTR("io.github.napieraj.mac-optical-state.watch")

/* ---- Public opaque type --------------------------------------------- */

struct mos_watch {
    /* Pure state machine. Owns the session identity / bsd_unit / seq state. */
    mos_watch_state core;

    /* What we're watching: the whole-disk unit (N in "diskN"), or -1 for
       an empty/open-tray drive. Tags emitted events and drives the Disk
       Arbitration wake filter — NOT the authority for which physical
       drive to probe. The actual probe identity is `registry_id` below. */
    int64_t bsd_unit;

    /* IORegistry entry ID of the physical drive this watch is bound to,
       captured at construction. watch_probe reopens the SAME drive every
       poll via mos_internal_open_by_registry_id, regardless of any BSD-name
       reassignment — so a session bound to drive A keeps probing A even if
       A's name is later recycled to a drive B on the same port. When A is
       terminated the reopen returns NO_DEVICE → terminal device_removed.
       registry_id (not the BSD name) is the single probe-identity authority. */
    uint64_t registry_id;

    /* Retained IOKit reference for the notification. Released in close. */
    io_service_t svc;

    /* Notification plumbing. The notification fires on the run loop
       this port is scheduled on (which is the caller's run loop, set
       up in watch_open). The token must be released in close. */
    IONotificationPortRef notify_port;
    io_object_t           notify_token;       /* kIOGeneralInterest */
    CFRunLoopSourceRef    notify_source;
    CFRunLoopRef          run_loop;

    /* Disk Arbitration session for media-change wake-up. DA fires
       description-changed callbacks when media is inserted, ejected,
       or its description otherwise changes — much faster than the
       2-second stable poll cycle. The DA callback calls
       mos_internal_watch_notify_wake() to pull the next poll forward
       and CFRunLoopStop() to break the pump's current sleep.
       NULL on poll-only fallback (DASessionCreate or callback
       DASessionCreate failed at open time —
       DARegisterDiskDescriptionChangedCallback returns void, so
       registration itself cannot fail detectably; fifth review F12d). */
    DASessionRef          da_session;

    /* Storage for the most recent event so mos_watch_next_event can
       return borrowed pointers that remain valid until the next call.
       Lifetime sources differ by field, and the difference is exactly
       what the pointer-lifetime audit rule (above watch_probe) governs:
       session identity (registry_id, stream_open_wall_ms) and bsd_unit
       are plain values with no pointer lifetime at all, while vendor /
       product / revision are borrowed from a short-lived mos_handle on
       each probe and must be re-homed into the buffers below before
       the handle is closed. The pure core does NOT own those three. */
    mos_watch_event last_event;

    /* Backing storage for the handle-borrowed identity strings. These
       are NOT owned by the pure core: they are copied out of a
       short-lived mos_handle on every probe and re-homed here so the
       event's pointers stay valid for the watch lifetime (until the
       next probe overwrites them, or mos_watch_close frees the watch).
       Per the pointer-lifetime audit rule above watch_probe, every
       handle-borrowed pointer field must be re-homed into one of these
       buffers (or set NULL) before the probe closes the handle.
         vendor[9]    INQUIRY VENDOR_IDENTIFICATION   ( 8 + NUL)
         product[17]  INQUIRY PRODUCT_IDENTIFICATION  (16 + NUL)
         revision[5]  INQUIRY PRODUCT_REVISION_LEVEL  ( 4 + NUL, SPC-4 §6.4.2) */
    char vendor[9];
    char product[17];
    char revision[5];
};

/* ---- Time --------------------------------------------------------- */

/* Monotonic milliseconds. CLOCK_MONOTONIC is available on macOS 10.12+;
   we're already floor 12.0 (Monterey) so this is unconditional. */
static uint64_t monotonic_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Wall-clock milliseconds for the session-open timestamp
   (stream_open_ms is documented as real epoch ms, not monotonic ms).
   Used only at watch open, via the monotonicized wrapper below. */
static uint64_t wall_clock_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Per-process monotonicized wall ms for the session-open timestamp
   (P4). Two watches opened on the same drive in the same wall-clock
   millisecond would otherwise share a (registry_id, stream_open_ms)
   pair, since per-drive uniqueness rides on the wall component.
   Bumping a same-or-earlier reading to last+1 keeps the value
   epoch-ms-shaped — rough cross-run orderability preserved — while
   guaranteeing per-process uniqueness even across NTP step-backs.
   Event `ts` is unaffected: it reads wall_clock_ms() fresh at every
   emit. */
static uint64_t stream_epoch_wall_ms(void)
{
    static _Atomic uint64_t last = 0;
    uint64_t now  = wall_clock_ms();
    uint64_t prev = atomic_load_explicit(&last, memory_order_relaxed);
    for (;;) {
        uint64_t next = (now > prev) ? now : prev + 1;
        if (atomic_compare_exchange_weak_explicit(&last, &prev, next,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            return next;
        }
        /* prev was reloaded by the failed CAS; recompute and retry. */
    }
}

/* ---- vtable callbacks -------------------------------------------- */

/* probe: reopen a fresh handle by the watch's registry id (not BSD name —
   see the registry_id field), query state, close. Handle-per-probe lets a
   transient driver detach recover on the next poll.

   POINTER-LIFETIME INVARIANT (adapter-scoped, and it must be — the pure
   layer forwards `const char *` fields verbatim and is structurally blind
   to the fact that one is borrowed from a handle this adapter is about to
   close; the pure tests/fuzzers therefore cannot catch a violation):
   before any mos_close(h), every borrowed pointer field of the escaping
   struct must be re-homed into watch-lifetime storage (w->vendor /
   w->product / w->revision) or set NULL. The footgun is `*out = *qr;` —
   it copies every pointer verbatim, so "forgot one" is the default, not
   the exception (the v0.3.2 revision use-after-free was exactly this: it
   rode the struct copy un-rehomed). Any NEW borrowed pointer added to
   mos_watch_event / mos_state_result needs a backing buffer in struct
   mos_watch and a re-home below. (bsd_unit is a value, never re-homed.) */
static mos_error watch_probe(void *ctx, mos_state_result *out)
{
    mos_watch_t *w = (mos_watch_t *)ctx;
    if (!w || !out) return MOS_ERR_INVALID_ARG;

    /* Reopen by registry ID, not BSD name (see registry_id field): the
       original entry still exists and we get the SAME drive back, or it has
       been terminated and we get NO_DEVICE — which the core treats as
       terminal removal. */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_internal_open_by_registry_id(w->registry_id, &err);
    if (!h) {
        /* Contract is NULL iff err != MOS_OK; force non-OK if ever violated,
           so the core never reads garbage state on a NULL handle. */
        return err != MOS_OK ? err : MOS_ERR_IO;
    }

    const mos_state_result *qr = NULL;
    mos_error qerr = mos_query_state(h, &qr);
    if (qerr != MOS_OK || !qr) {
        mos_close(h);
        return qerr != MOS_OK ? qerr : MOS_ERR_IO;
    }

    /* Copy the handle-owned result into the caller's struct so its
       identity strings can be re-homed below and survive mos_close(h). */
    *out = *qr;

    /* The drive is pinned by registry ID, but the media's BSD unit is not
       stable (-1 when empty at open; changes across eject/reinsert). Refresh
       the ADAPTER's copy for the DA wake filter; the pure core adopts the
       probe's unit itself on every successful pump (mos_watch_core.c), so
       the error/device_removed fallback no longer depends on this adapter
       — a layering obligation retired by the third review. media_id (the
       F1 swap fingerprint) needs no manual tracking — it rides the
       *out = *qr copy and the core reads it from the result. */
    w->bsd_unit = out->bsd_unit;
    /* Re-home the three borrowed identity strings into watch-owned buffers
       and repoint out at them, so they survive the mos_close(h) below (the
       lifetime invariant above). The helper is pure (mos_pure.c) so the
       no-dangle property is ASan-gated headlessly. */
    mos_internal_rehome_identity_strings(out,
        w->vendor,   sizeof w->vendor,
        w->product,  sizeof w->product,
        w->revision, sizeof w->revision);

    mos_close(h);
    return MOS_OK;
}

/* Monotonic ms callback for the watch core. Used for poll deadline
   scheduling and latency measurement. CLOCK_MONOTONIC only. */
static uint64_t watch_mono_ms(void *ctx)
{
    (void)ctx;
    return monotonic_ms();
}

/* Wall-clock ms callback for the watch core. Used only for event ts
   formatting. CLOCK_REALTIME (Unix epoch ms). MUST NOT be used for
   scheduling — clock can jump backward. */
static uint64_t watch_wall_ms(void *ctx)
{
    (void)ctx;
    return wall_clock_ms();
}

static const mos_watch_ops_t apple_watch_ops = {
    .probe   = watch_probe,
    .mono_ms = watch_mono_ms,
    .wall_ms = watch_wall_ms,
};

/* ---- Notification handler ---------------------------------------- *
 *
 * Fires on the run loop thread for kIOGeneralInterest messages on the
 * matched io_service_t. Do NOT also subscribe to kIOBusyInterest: issuing
 * a probe changes the drive's busy state, which would fire the notification
 * and schedule another probe — a live loop.
 *
 * Message handling:
 *   1. kIOMessageServiceIsTerminated → TERMINAL: notify_removed; pump emits
 *      device_removed.
 *   2. kIOMessageServicePropertyChange → WAKE: notify_wake; pump re-probes.
 *      Tracks drive state, not client state, so it does NOT fire on our own
 *      per-probe MMC user-client open/close — safe to wake on.
 *   3. Everything else IGNORED — including IsAttemptingOpen / WasClosed /
 *      BusyStateChange, which fire on ANY user-client open/close (our own
 *      probes included) and would self-trigger. Whether they can be used
 *      safely is deferred to v0.4 pending the empirical probe.
 *
 * messageType is natural_t here vs uint32_t in the SDK's
 * IOServiceInterestCallback typedef; both are `unsigned int`, so the
 * function-pointer types are compatible and the -Werror check is satisfied. */
static void watch_interest_callback(void *refcon,
                                    io_service_t service,
                                    natural_t messageType,
                                    void *messageArgument)
{
    (void)service;
    (void)messageArgument;
    mos_watch_t *w = (mos_watch_t *)refcon;
    if (!w) return;

    switch (messageType) {
    case kIOMessageServiceIsTerminated:
        /* TERMINAL. Drive went away. */
        mos_internal_watch_notify_removed(&w->core);
        break;

    case kIOMessageServicePropertyChange:
        /* WAKE. A registry property changed — possibly a media-state
           transition from the kernel's MMC stack. Pull the next-poll
           deadline forward so the pump probes now. False wakes are cheap
           (one probe, no state mutation if unchanged). */
        mos_internal_watch_notify_wake(&w->core);
        break;

    default:
        /* IGNORED. Other messages (IsRequestingClose, power-state
           transitions, system sleep notifications, AND the
           IsAttemptingOpen/WasClosed/BusyStateChange family that
           self-trigger on our own probe handles — see header
           comment for the deferred-v0.4 plan). */
        return;
    }

    /* For both TERMINAL and WAKE: break the pump's CFRunLoopRunInMode
       sleep so the next mos_watch_next_event call returns promptly. */
    if (w->run_loop) {
        CFRunLoopStop(w->run_loop);
    }
}

/* ---- Wake source: Disk Arbitration -------------------------------- *
 *
 * DA fires when a disk's description changes (media inserted/ejected, size,
 * mount state, ...), collapsing worst-case insert→event latency from
 * stable_poll_ms (default 2s) to however long DA takes — typically <100ms.
 * Polling is the correctness floor; DA is just the optimization, so if
 * DASessionCreate fails the watch falls back to poll-only
 * (w->da_session stays NULL; close treats NULL as a no-op).
 *
 * Applies only to watches with a known unit: the callback filters by
 * mos_internal_bsd_unit_matches against w->bsd_unit, which is -1 for an
 * empty/open-tray drive — so such a watch matches nothing until media loads
 * and a unit appears, relying on poll + kIOGeneralInterest until then.
 *
 * Registration uses match=NULL / watch=NULL (all disks, all keys) to avoid
 * depending on specific DA key constants; the callback filters in-process by
 * unit. The pump re-probes from scratch on any wake, so the filter need not
 * be precise about which entry fired.
 */

static void disk_description_changed_callback(DADiskRef disk,
                                              CFArrayRef keys,
                                              void *context)
{
    /* DA fires this on the run loop the session is scheduled on
       (the caller's run loop, same as our IONotificationPort). */
    (void)keys;

    mos_watch_t *w = (mos_watch_t *)context;
    if (!w || !disk) return;

    /* Filter to events involving our drive. DADiskGetBSDName returns
       NULL for DA's internal representations of unmounted media —
       skip those. */
    const char *bsd = DADiskGetBSDName(disk);
    if (!bsd) return;

    /* Match "disk4" or a partition child "disk4s1", rejecting "disk40"-style
       prefix collisions. The tested helper, not inline strncmp — the strncmp
       form has been miswritten as a prefix-only match before. Pinned by
       tests/test_bsd_name.c. */
    if (!mos_internal_bsd_unit_matches(bsd, w->bsd_unit)) {
        return;
    }

    mos_internal_watch_notify_wake(&w->core);
    /* Break the pump's CFRunLoopRunInMode sleep — same pattern as
       watch_interest_callback's termination handling. The next
       mos_watch_next_event call re-probes immediately. */
    if (w->run_loop) {
        CFRunLoopStop(w->run_loop);
    }
}

/* Set up an IOKit interest notification (kIOGeneralInterest) for
   service termination. Called from watch_open_common after the pure
   watch core is initialized. Each step that fails tears down what
   it created and returns leaving every field NULL — caller falls
   back to poll-only for this mechanism (the DA path below is
   independent). The invariant this maintains: after this function
   returns, w->notify_port is non-NULL iff w->notify_source is also
   non-NULL AND a kIOGeneralInterest notification is registered.
   That invariant is what the pump's run-loop gate depends on. */
static void setup_iokit_interest_wake(mos_watch_t *w)
{
    if (!w || !w->run_loop || w->svc == IO_OBJECT_NULL) return;

    w->notify_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!w->notify_port) return;

    w->notify_source = IONotificationPortGetRunLoopSource(w->notify_port);
    if (!w->notify_source) {
        /* Port created but cannot acquire its run-loop source — tear
           the port down immediately. Leaving w->notify_port set
           without a live source would make the pump's run-loop gate
           enter CFRunLoopRunInMode in a mode with no sources, which
           returns instantly and tight-loops until the caller's
           timeout fires. */
        IONotificationPortDestroy(w->notify_port);
        w->notify_port = NULL;
        return;
    }

    CFRunLoopAddSource(w->run_loop, w->notify_source,
                       MOS_WATCH_RUN_LOOP_MODE);

    kern_return_t kr = IOServiceAddInterestNotification(
            w->notify_port, w->svc, kIOGeneralInterest,
            watch_interest_callback, w, &w->notify_token);
    if (kr != KERN_SUCCESS) {
        /* Notification registration failed; remove the source and
           tear down the port. DA below is unaffected. */
        CFRunLoopRemoveSource(w->run_loop, w->notify_source,
                              MOS_WATCH_RUN_LOOP_MODE);
        w->notify_source = NULL;
        IONotificationPortDestroy(w->notify_port);
        w->notify_port   = NULL;
        return;
    }

    /* kIOBusyInterest deliberately NOT registered: BusyStateChange fires on
       every user-client open/close — including our own per-probe MMC
       user-clients — so dispatching it would self-trigger a tight probe
       loop. Revisiting it is a v0.4 question for the empirical probe. */
}

/* Tear down the IOKit interest notification in reverse order:
   remove source from run loop (no more callbacks), release the
   notification token, destroy the port. Safe to call with NULL /
   poll-only state. Called from mos_watch_close. */
static void teardown_iokit_interest_wake(mos_watch_t *w)
{
    if (!w) return;

    if (w->run_loop && w->notify_source) {
        CFRunLoopRemoveSource(w->run_loop, w->notify_source,
                              MOS_WATCH_RUN_LOOP_MODE);
        w->notify_source = NULL;
    }
    if (w->notify_token != IO_OBJECT_NULL) {
        IOObjectRelease(w->notify_token);
        w->notify_token = IO_OBJECT_NULL;
    }
    if (w->notify_port) {
        IONotificationPortDestroy(w->notify_port);
        w->notify_port = NULL;
    }
}

/* Set up a Disk Arbitration session and register the
   description-changed callback. Independent of the IOKit interest
   wake — either or both may fail soft to poll-only. Stores the
   session in w->da_session on success; leaves it NULL on any
   failure. */
static void setup_disk_arbitration_wake(mos_watch_t *w)
{
    if (!w || !w->run_loop) return;

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return;

    /* Register for all-disk description changes. Filtering happens
       in the callback by BSD-name comparison — see the design block
       above for why this is cleaner than constructing a match
       dictionary against a specific DA description key. */
    DARegisterDiskDescriptionChangedCallback(
        session,
        /*match=*/NULL,    /* all disks */
        /*watch=*/NULL,    /* all keys */
        disk_description_changed_callback,
        w);

    /* Schedule LAST: once on the run loop, callbacks can fire, so every
       prior step must already be safe to be live. */
    DASessionScheduleWithRunLoop(session, w->run_loop,
                                  MOS_WATCH_RUN_LOOP_MODE);

    w->da_session = session;
}

/* Tear down the DA session in reverse order: unschedule (no more
   callbacks), unregister, release. Safe to call with NULL/poll-only
   state. Called from mos_watch_close. */
static void teardown_disk_arbitration_wake(mos_watch_t *w)
{
    if (!w || !w->da_session) return;

    if (w->run_loop) {
        DASessionUnscheduleFromRunLoop(w->da_session, w->run_loop,
                                        MOS_WATCH_RUN_LOOP_MODE);
    }
    DAUnregisterCallback(w->da_session,
                          (void *)disk_description_changed_callback,
                          w);
    CFRelease(w->da_session);
    w->da_session = NULL;
}

/* ---- Open / close ------------------------------------------------ */

/* Takes ownership of `h` and either builds a watch around it or closes it
   on failure. The single funnel both public open entry points go through —
   neither re-resolves the drive by name internally. The watch's bsd_unit
   comes from the handle's resolved unit (mos_handle_bsd_unit), not a
   caller-supplied string, so unit and service identity stay consistent. */
static mos_watch_t *watch_open_from_validated_handle(
        mos_handle_t *h,
        uint32_t stable_poll_ms,
        uint32_t transition_poll_ms,
        mos_error *err_out)
{
    if (!h) {
        if (err_out) *err_out = MOS_ERR_INVALID_ARG;
        return NULL;
    }

    /* An empty/open-tray drive has no unit (mos_handle_bsd_unit returns -1).
       Not a failure for the watch — identity is the registry_id captured
       below. The DA filter returns false for unit < 0, so a nameless drive
       relies on the kIOGeneralInterest wake plus polling until media loads. */
    const int64_t bsd_unit = mos_handle_bsd_unit(h);   /* -1 if empty */

    mos_watch_t *w = (mos_watch_t *)calloc(1, sizeof(*w));
    if (!w) {
        mos_close(h);
        if (err_out) *err_out = MOS_ERR_OOM;
        return NULL;
    }

    /* Plain value copy; -1 (empty drive) carries through unchanged. */
    w->bsd_unit = bsd_unit;

    /* Capture the validated io_service_t before mos_close — this is the
       identity the watch preserves (the registry_id below is taken from it,
       and per-poll reopen targets that, immune to BSD-name reassignment).
       Refcount: handle_get_service returns it without a retain, so retain
       here (mos_close drops the handle's own) and release in
       mos_watch_close. If the retain fails, leave w->svc NULL and fall back
       to poll-only — safer than holding a service we don't own. */
    io_service_t validated_svc = mos_internal_handle_get_service(h);
    if (validated_svc != IO_OBJECT_NULL &&
        IOObjectRetain(validated_svc) == KERN_SUCCESS) {
        w->svc = validated_svc;
    }

    /* Capture the registry-entry ID — the identity authority watch_probe
       reopens by each poll. Fail closed if it can't be captured: a BSD-name
       fallback would reintroduce the two-identity bug this path closes. */
    uint64_t entry_id = 0;
    if (validated_svc == IO_OBJECT_NULL ||
        IORegistryEntryGetRegistryEntryID(validated_svc, &entry_id) != KERN_SUCCESS ||
        entry_id == 0) {
        if (w->svc != IO_OBJECT_NULL) IOObjectRelease(w->svc);
        free(w);
        mos_close(h);
        if (err_out) *err_out = MOS_ERR_IO;
        return NULL;
    }
    w->registry_id = entry_id;

    mos_close(h);

    /* Initialize the pure state machine BEFORE registering any
       callbacks that mutate it. The pure init has no failure path
       and depends on nothing beyond monotonic_ms() / wall_clock_ms(),
       so doing it first costs nothing and makes early callback
       delivery harmless by construction. */
    mos_internal_watch_init(&w->core, &apple_watch_ops, w,
                            w->bsd_unit,
                            /*registry_id=*/w->registry_id,
                            /*start_mono_ms=*/monotonic_ms(),
                            /*start_wall_ms=*/stream_epoch_wall_ms(),
                            stable_poll_ms,
                            transition_poll_ms);

    /* Capture the caller's run loop once so IOKit and DiskArbitration
       wake sources can be scheduled independently. Both are best-effort;
       either can succeed without the other, and both can fail to
       poll-only without affecting correctness. */
    w->run_loop = CFRunLoopGetCurrent();

    setup_iokit_interest_wake(w);
    setup_disk_arbitration_wake(w);

    if (err_out) *err_out = MOS_OK;
    return w;
}

mos_watch_t *mos_watch_open_by_bsd_name(const char *bsd_name,
                                        uint32_t stable_poll_ms,
                                        uint32_t transition_poll_ms,
                                        mos_error *err_out)
{
    if (!bsd_name || !*bsd_name) {
        if (err_out) *err_out = MOS_ERR_INVALID_ARG;
        return NULL;
    }
    /* Resolve once via BSD name (this entry point's contract is "find
       me whatever drive currently has this BSD name"). The handle that
       comes back is validated against the storage-device class
       hierarchy; watch_open_from_validated_handle then preserves THAT
       service identity — no second BSD-name resolution. */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_bsd_name(bsd_name, &err);
    if (!h) {
        if (err_out) *err_out = (err != MOS_OK) ? err : MOS_ERR_IO;
        return NULL;
    }
    return watch_open_from_validated_handle(h, stable_poll_ms,
                                            transition_poll_ms, err_out);
}

mos_watch_t *mos_watch_open_by_index(int one_based,
                                     uint32_t stable_poll_ms,
                                     uint32_t transition_poll_ms,
                                     mos_error *err_out)
{
    /* Resolve once via index. mos_open_by_index uses
       IORegistryEntryIDMatching internally, so the handle carries the exact
       peripheral selected at enumeration time, not whatever currently holds
       its BSD name; watch_open_from_validated_handle preserves that identity
       (no second resolution). */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_index(one_based, &err);
    if (!h) {
        if (err_out) *err_out = (err != MOS_OK) ? err : MOS_ERR_IO;
        return NULL;
    }
    return watch_open_from_validated_handle(h, stable_poll_ms,
                                            transition_poll_ms, err_out);
}

/* Safe to call on NULL; do not call twice (mos_close convention). Order is
   load-bearing — stop callbacks before releasing the memory they reference:
   DA session, then IOKit interest wake, then the retained io_service_t.
   (Each teardown helper enforces its own internal stop-before-free order.) */
void mos_watch_close(mos_watch_t *w)
{
    if (!w) return;

    teardown_disk_arbitration_wake(w);
    teardown_iokit_interest_wake(w);

    if (w->svc != IO_OBJECT_NULL) {
        IOObjectRelease(w->svc);
    }
    free(w);
}

int64_t mos_watch_bsd_unit(const mos_watch_t *w)
{
    if (!w) return -1;
    return w->bsd_unit;
}

/* ---- Pump --------------------------------------------------------- */

mos_error mos_watch_next_event(mos_watch_t *w, const mos_watch_event **out,
                               int timeout_ms)
{
    if (out) *out = NULL;
    if (!w || !out) return MOS_ERR_INVALID_ARG;

    /* The pump may need to spin a couple of times — pump → SLEEP_UNTIL →
       wait → pump → EMIT_EVENT — within a single user-visible call.
       We bound that with the user's timeout_ms; if we exhaust it without
       producing an event, return MOS_ERR_TIMEOUT and the caller's outer
       loop decides whether to call again. */
    uint64_t start = monotonic_ms();
    uint64_t deadline = (timeout_ms < 0)
        ? UINT64_MAX
        : start + (uint64_t)timeout_ms;

    for (;;) {
        mos_watch_decision d = mos_internal_watch_pump(&w->core);

        if (d.kind == MOS_WATCH_DECIDE_EMIT_EVENT) {
            w->last_event = d.event;
            *out = &w->last_event;
            return MOS_OK;
        }

        if (d.kind == MOS_WATCH_DECIDE_TERMINAL) {
            return MOS_ERR_NO_DEVICE;
        }

        /* SLEEP_UNTIL: block until next_poll_at_mono_ms or until a
           notification fires (which calls CFRunLoopStop). Use
           CFRunLoopRunInMode with a bounded interval. The deadline
           is a monotonic-clock value, compared against monotonic_ms()
           the same way the pump compares. */
        uint64_t now = monotonic_ms();
        if (now >= deadline) {
            /* Caller-side timeout elapsed without producing an event.
               Pump again next time the caller asks. */
            return MOS_ERR_TIMEOUT;
        }

        uint64_t sleep_until_ms = d.next_poll_at_mono_ms;
        if (sleep_until_ms > deadline) sleep_until_ms = deadline;
        if (sleep_until_ms < now) sleep_until_ms = now;
        double interval_sec = (double)(sleep_until_ms - now) / 1000.0;

        /* Only wait on the run loop if a source is actually scheduled
           (notify_source or da_session) — an empty mode returns instantly
           and tight-loops — AND we are on the thread that owns it. The
           documented contract is open/pump/close on one thread; if a
           caller violates it anyway, CFRunLoopRunInMode here would run
           the WRONG thread's loop, where our private mode has no sources,
           returning instantly and burning CPU until timeout_ms. The
           misuse stays misuse (notification wakes can't reach a foreign
           thread's loop), but it degrades to honest nanosleep polling
           instead of a busy-spin. */
        if (w->run_loop && (w->notify_source || w->da_session) &&
            CFRunLoopGetCurrent() == w->run_loop) {
            /* Returns on timer, CFRunLoopStop (a notification callback), or
               a handled source — any is a wake; loop back to pump. Private
               mode keeps us off host-app default-mode sources. */
            CFRunLoopRunInMode(MOS_WATCH_RUN_LOOP_MODE, interval_sec, false);
        } else {
            /* No notification source set up; fall back to nanosleep.
               This path is taken when the handle didn't surface a
               validated io_service_t (poll-only mode) or when BOTH
               notification registration paths failed at open time. */
            struct timespec ts;
            ts.tv_sec  = (time_t)(sleep_until_ms - now) / 1000;
            ts.tv_nsec = (long)((sleep_until_ms - now) % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
        /* Loop and re-pump. */
    }
}
