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
#include <DiscRecording/DRCoreDevice.h>
#include <DiscRecording/DRCoreNotifications.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Private run-loop mode for the watch's IOKit and DR doorbell sources — never
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
       an empty/open-tray drive. Tags emitted events and feeds the
       mos_watch_bsd_unit accessor — NOT the authority for which physical
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

    /* DiscRecording doorbell for media/tray-change wake-up (Phase 2a of
       the DR pivot: replaced the DiskArbitration session — DR's
       StatusChanged is device-scoped, so it also wakes on tray-open /
       no-media drives where DA's media-scoped, bsd_unit-filtered wake
       matched nothing). The callback calls
       mos_internal_watch_notify_wake() to pull the next poll forward
       and CFRunLoopStop() to break the pump's current sleep. Both
       fields NULL on poll-only fallback (center or run-loop source
       creation failed at open time) — polling is the correctness
       floor, the doorbell is latency only. SINGLE-TARGET ONLY: in all
       mode arrival discovery rides the doorbell with no poll floor,
       so mos_watch_open_all fails instead of falling back. */
    DRNotificationCenterRef dr_center;
    CFRunLoopSourceRef      dr_source;

    /* Storage for the most recent event so mos_watch_next_event can
       return borrowed pointers that remain valid until the next call.
       Session identity (registry_id, stream_open_wall_ms) and bsd_unit
       are plain values with no pointer lifetime; vendor / product /
       revision point into the watch-owned buffers below. */
    mos_watch_event last_event;

    /* Device-static identity, captured ONCE from the validated open
       handle (whose strings come from the DR directory) and owned by
       the watch for its whole life. Events point here; per-probe
       handles never contribute identity (the per-probe re-home this
       replaced — and the v0.3.2 use-after-free class it existed to
       contain — retired with DR pivot Phase 2a). Widths are the SPC-4
       INQUIRY field widths the directory data is parsed from:
         vendor[9]    VENDOR_IDENTIFICATION   ( 8 + NUL)
         product[17]  PRODUCT_IDENTIFICATION  (16 + NUL)
         revision[5]  PRODUCT_REVISION_LEVEL  ( 4 + NUL, SPC-4 §6.4.2) */
    char vendor[9];
    char product[17];
    char revision[5];

    /* ---- Watch-all mode (DR pivot Phase 2b) ------------------------ *
     * all_mode selects the multiplexer path: `all` is the pure fan-in
     * over per-slot cores, `slots` is the adapter-side per-device probe
     * context (registry id + watch-static identity) each core's ctx
     * points at. Single-target fields above (core, svc, notify_*,
     * registry_id, identity buffers) are unused in all mode; bsd_unit
     * stays -1. Poll rates are kept for mid-stream joins. */
    bool                 all_mode;
    mos_watch_all_state  all;
    struct mos_watch_slot {
        uint64_t registry_id;
        char     vendor[9];
        char     product[17];
        char     revision[5];
    }                    slots[MOS_WATCH_ALL_CAP];
    uint32_t             stable_poll_ms;
    uint32_t             transition_poll_ms;
    /* The all-watch's ONE stream-open timestamp, minted once at
       mos_watch_open_all and given to every slot — drives present at
       open and later joiners alike — so stream_open_ms is constant
       across the stream as documented (mos.h, mos.event.v1). Per-event
       join/change time rides ts; (registry_id, stream_open_ms) stays
       unique because a replug re-mints the registry_id. 0 in
       single-target mode (those cores mint per-open as before). */
    uint64_t             all_stream_open_wall_ms;
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
   before any mos_close(h), every handle-borrowed pointer field of the
   escaping struct must be REPLACED — identity fields point at the
   watch-static buffers captured at open (w->vendor / w->product /
   w->revision; device-static data, so per-probe refresh carried no
   information) — or set NULL. The footgun is `*out = *qr;` — it copies
   every pointer verbatim, so "forgot one" is the default, not the
   exception (the v0.3.2 revision use-after-free was exactly this: it
   rode the struct copy un-replaced). Any NEW borrowed pointer added to
   mos_watch_event / mos_state_result needs a watch-lifetime backing
   store and a replacement below. (bsd_unit is a value, never replaced.) */
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
       the ADAPTER's copy (it feeds the mos_watch_bsd_unit accessor; the DR
       doorbell filters by registry ID, so no wake filter reads it); the pure
       core adopts the probe's unit itself on every successful pump
       (mos_watch_core.c), so the error/device_removed fallback no
       longer depends on this adapter
       — a layering obligation retired by the third review. media_id (the
       F1 swap fingerprint) needs no manual tracking — it rides the
       *out = *qr copy and the core reads it from the result. */
    w->bsd_unit = out->bsd_unit;
    /* Replace the three handle-borrowed identity pointers with the
       watch-static identity captured at open (the lifetime invariant
       above): they must not survive the mos_close(h) below. Identity is
       device-static directory data, so the per-probe handle's copy is
       byte-identical to the open-time capture — repointing loses
       nothing and removes the per-probe re-home entirely. */
    out->vendor   = w->vendor[0]   ? w->vendor   : NULL;
    out->product  = w->product[0]  ? w->product  : NULL;
    out->revision = w->revision[0] ? w->revision : NULL;

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

/* Per-slot probe for watch-all: identical contract to watch_probe, but
   ctx is the slot (its own registry id + watch-static identity). The
   same pointer-lifetime invariant applies: identity fields are
   repointed at slot-lifetime storage before the handle closes. */
static mos_error watch_slot_probe(void *ctx, mos_state_result *out)
{
    struct mos_watch_slot *s = (struct mos_watch_slot *)ctx;
    if (!s || !out) return MOS_ERR_INVALID_ARG;

    mos_error err = MOS_OK;
    mos_handle_t *h = mos_internal_open_by_registry_id(s->registry_id, &err);
    if (!h) return err != MOS_OK ? err : MOS_ERR_IO;

    const mos_state_result *qr = NULL;
    mos_error qerr = mos_query_state(h, &qr);
    if (qerr != MOS_OK || !qr) {
        mos_close(h);
        return qerr != MOS_OK ? qerr : MOS_ERR_IO;
    }

    *out = *qr;
    out->vendor   = s->vendor[0]   ? s->vendor   : NULL;
    out->product  = s->product[0]  ? s->product  : NULL;
    out->revision = s->revision[0] ? s->revision : NULL;

    mos_close(h);
    return MOS_OK;
}

static const mos_watch_ops_t apple_watch_slot_ops = {
    .probe   = watch_slot_probe,
    .mono_ms = watch_mono_ms,
    .wall_ms = watch_wall_ms,
};

/* Add one device from a DR snapshot. Dedupe by registry_id BEFORE
   touching slot storage; the slot is claimed by the same first-free
   scan add() uses (single-thread contract keeps the scans agreeing). */
static void watch_all_add_device(mos_watch_t *w,
                                 const mos_internal_dr_snapshot *snap,
                                 bool mid_stream)
{
    if (!w || !snap || snap->registry_id == 0) return;

    if (mos_internal_watch_all_find(&w->all, snap->registry_id) >= 0) {
        return; /* duplicate Appeared — already streaming in a slot */
    }
    int i = mos_internal_watch_all_free_slot(&w->all);
    if (i < 0) {
        return; /* full and genuinely new — documented drop until a slot frees */
    }
    /* Width-agreement pins: source and destination both carry the
       SPC-4 identity widths, so these copies can never truncate.
       Successor of the retired INQUIRY path's per-site asserts. */
    _Static_assert(sizeof w->slots[i].vendor   == sizeof snap->vendor,
                   "slot vendor width must match the DR snapshot's");
    _Static_assert(sizeof w->slots[i].product  == sizeof snap->product,
                   "slot product width must match the DR snapshot's");
    _Static_assert(sizeof w->slots[i].revision == sizeof snap->revision,
                   "slot revision width must match the DR snapshot's");
    w->slots[i].registry_id = snap->registry_id;
    strlcpy(w->slots[i].vendor,   snap->vendor,   sizeof w->slots[i].vendor);
    strlcpy(w->slots[i].product,  snap->product,  sizeof w->slots[i].product);
    strlcpy(w->slots[i].revision, snap->revision, sizeof w->slots[i].revision);

    (void)mos_internal_watch_all_add(&w->all, &apple_watch_slot_ops,
                                     &w->slots[i],
                                     snap->bsd_unit, snap->registry_id,
                                     monotonic_ms(),
                                     w->all_stream_open_wall_ms,
                                     w->stable_poll_ms, w->transition_poll_ms,
                                     mid_stream);
}

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

/* ---- Wake source: DiscRecording doorbell --------------------------- *
 *
 * kDRDeviceStatusChangedNotification fires when a device's status dict
 * changes (media in/out, tray, busy), collapsing worst-case
 * insert→event latency from stable_poll_ms (default 2s) to however
 * long DR takes. Polling is the correctness floor; the doorbell is
 * latency only, so any setup failure falls back to poll-only
 * (dr_center/dr_source stay NULL; close treats NULL as a no-op).
 *
 * DR's notification is DEVICE-scoped (not media-scoped), so this
 * doorbell also rings for tray-open / no-media drives (DR pivot
 * Phase 2a, doc/research/2026-06-10-dr-pivot-implementation-plan.md).
 *
 * The callback filters by registry ID — a parameter, not a structural
 * assumption, so a future watch-all mode widens the filter rather than
 * rewiring the pump (plan, Phase 2b). Filtering is fail-OPEN: if the
 * event's device can't be resolved to an ID, wake anyway — a false
 * wake is one cheap probe, a missed wake is stable_poll_ms of latency.
 * DR data never decides state; the wake only schedules the MMC probe.
 */

static void dr_status_changed_callback(DRNotificationCenterRef center,
                                       void *observer, CFStringRef name,
                                       DRTypeRef object,
                                       CFDictionaryRef info)
{
    /* Fires on the run loop the DR source is scheduled on (the caller's
       run loop, same as our IONotificationPort source). */
    (void)center; (void)name; (void)info;

    mos_watch_t *w = (mos_watch_t *)observer;
    if (!w) return;

    /* Per-device filter by registry ID (the watch's one identity
       authority). object is the DRDeviceRef that changed; resolve its
       registry path → entry ID and compare. Any resolution failure
       wakes anyway (fail-open, see design block). In all mode the
       filter routes instead of rejects: wake the matching slot, or
       every slot when unresolved. */
    uint64_t id = 0;
    if (object) {
        CFDictionaryRef dev_info = DRDeviceCopyInfo((DRDeviceRef)object);
        if (dev_info) {
            id = mos_internal_dr_id_for_path_value(
                CFDictionaryGetValue(dev_info,
                                     kDRDeviceIORegistryEntryPathKey));
            CFRelease(dev_info);
        }
    }

    if (w->all_mode) {
        int slot = (id != 0) ? mos_internal_watch_all_find(&w->all, id) : -1;
        if (slot >= 0) {
            mos_internal_watch_notify_wake(&w->all.cores[slot]);
        } else if (id == 0) {
            for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
                if (w->all.active[i]) {
                    mos_internal_watch_notify_wake(&w->all.cores[i]);
                }
            }
        }
        /* id resolved but unknown: a device we are not watching (cap
           overflow) or one Appeared hasn't delivered yet — the
           Appeared handler owns joins; nothing to wake. */
        if (w->run_loop) CFRunLoopStop(w->run_loop);
        return;
    }

    if (id != 0 && w->registry_id != 0 && id != w->registry_id) {
        return; /* another drive */
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
   back to poll-only for this mechanism (the DR doorbell below is
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
           tear down the port. The DR doorbell below is unaffected. */
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

/* All-mode lifecycle (Phase 2b): Appeared joins a device to the
   stream (its first event is relabeled device_appeared by the pure
   multiplexer); Disappeared wakes the matching slot so its reopen can
   confirm removal. All-mode only — single-target watches keep
   kIOGeneralInterest as their terminal-removal source. */
static void dr_device_appeared_callback(DRNotificationCenterRef center,
                                        void *observer, CFStringRef name,
                                        DRTypeRef object,
                                        CFDictionaryRef info)
{
    (void)center; (void)name; (void)info;
    mos_watch_t *w = (mos_watch_t *)observer;
    if (!w || !w->all_mode || !object) return;

    mos_internal_dr_snapshot snap;
    if (mos_internal_dr_device_snapshot((CFTypeRef)object, &snap)) {
        watch_all_add_device(w, &snap, /*mid_stream=*/true);
    }
    if (w->run_loop) CFRunLoopStop(w->run_loop);
}

static void dr_device_disappeared_callback(DRNotificationCenterRef center,
                                           void *observer, CFStringRef name,
                                           DRTypeRef object,
                                           CFDictionaryRef info)
{
    (void)center; (void)name; (void)info;
    mos_watch_t *w = (mos_watch_t *)observer;
    if (!w || !w->all_mode || !object) return;

    uint64_t id = 0;
    CFDictionaryRef dev_info = DRDeviceCopyInfo((DRDeviceRef)object);
    if (dev_info) {
        id = mos_internal_dr_id_for_path_value(
            CFDictionaryGetValue(dev_info, kDRDeviceIORegistryEntryPathKey));
        CFRelease(dev_info);
    }
    int slot = (id != 0) ? mos_internal_watch_all_find(&w->all, id) : -1;
    if (slot >= 0) {
        /* Wake, not removal authority: the woken reopen confirms
           (NO_DEVICE → terminal) at the same latency, and a spurious
           Disappeared costs one probe instead of a permanent eviction
           (C1 — DR data never decides state). */
        mos_internal_watch_notify_wake(&w->all.cores[slot]);
    }
    /* Unresolved id: the probe floor catches it — the slot's next
       reopen returns NO_DEVICE, which the core treats as removal. */
    if (w->run_loop) CFRunLoopStop(w->run_loop);
}

/* Set up the DR notification center and register the StatusChanged
   observer. Independent of the IOKit interest wake — either or both
   may fail soft to poll-only. Stores center + source on success;
   leaves both NULL on any failure. */
static void setup_dr_doorbell_wake(mos_watch_t *w)
{
    if (!w || !w->run_loop) return;

    DRNotificationCenterRef center = DRNotificationCenterCreate();
    if (!center) return;

    CFRunLoopSourceRef source = DRNotificationCenterCreateRunLoopSource(center);
    if (!source) {
        CFRelease(center);
        return;
    }
    CFRunLoopAddSource(w->run_loop, source, MOS_WATCH_RUN_LOOP_MODE);

    /* Register LAST: once observed, callbacks can fire, so every prior
       step must already be safe to be live. object=NULL observes all
       devices; the callback filters by registry ID (fail-open). In all
       mode the Appeared/Disappeared lifecycle observers join here —
       they are what makes the bus stream live. */
    DRNotificationCenterAddObserver(center, w, dr_status_changed_callback,
                                    kDRDeviceStatusChangedNotification,
                                    NULL);
    if (w->all_mode) {
        DRNotificationCenterAddObserver(center, w,
                                        dr_device_appeared_callback,
                                        kDRDeviceAppearedNotification, NULL);
        DRNotificationCenterAddObserver(center, w,
                                        dr_device_disappeared_callback,
                                        kDRDeviceDisappearedNotification,
                                        NULL);
    }

    w->dr_center = center;
    w->dr_source = source;
}

/* Tear down the DR doorbell in reverse order: remove the observer (no
   more callbacks), remove the source from the run loop, release both.
   Safe to call with NULL/poll-only state. Called from mos_watch_close. */
static void teardown_dr_doorbell_wake(mos_watch_t *w)
{
    if (!w || !w->dr_center) return;

    DRNotificationCenterRemoveObserver(w->dr_center, w,
                                       kDRDeviceStatusChangedNotification,
                                       NULL);
    if (w->all_mode) {
        DRNotificationCenterRemoveObserver(w->dr_center, w,
                                           kDRDeviceAppearedNotification,
                                           NULL);
        DRNotificationCenterRemoveObserver(w->dr_center, w,
                                           kDRDeviceDisappearedNotification,
                                           NULL);
    }
    if (w->run_loop && w->dr_source) {
        CFRunLoopRemoveSource(w->run_loop, w->dr_source,
                              MOS_WATCH_RUN_LOOP_MODE);
    }
    if (w->dr_source) {
        CFRelease(w->dr_source);
        w->dr_source = NULL;
    }
    CFRelease(w->dr_center);
    w->dr_center = NULL;
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
       below, and the DR doorbell is device-scoped, so a nameless drive
       gets the same wake coverage as a named one. */
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

    /* Capture the device-static identity strings ONCE, before the
       validation handle closes — they came from the DR directory at
       open. Events point at these watch-owned buffers for the watch's
       whole life; per-probe handles never contribute identity (see the
       buffer comment in struct mos_watch). strlcpy truncation cannot
       trigger — pinned at build time: */
    _Static_assert(sizeof w->vendor   == sizeof h->vendor_str,
                   "watch vendor width must match the handle's");
    _Static_assert(sizeof w->product  == sizeof h->product_str,
                   "watch product width must match the handle's");
    _Static_assert(sizeof w->revision == sizeof h->revision_str,
                   "watch revision width must match the handle's");
    strlcpy(w->vendor,   h->vendor_str,   sizeof w->vendor);
    strlcpy(w->product,  h->product_str,  sizeof w->product);
    strlcpy(w->revision, h->revision_str, sizeof w->revision);

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

    /* Capture the caller's run loop once so the IOKit and DiscRecording
       wake sources can be scheduled independently. Both are best-effort;
       either can succeed without the other, and both can fail to
       poll-only without affecting correctness. */
    w->run_loop = CFRunLoopGetCurrent();

    setup_iokit_interest_wake(w);
    setup_dr_doorbell_wake(w);

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

mos_watch_t *mos_watch_open_by_registry_id(uint64_t registry_id,
                                           uint32_t stable_poll_ms,
                                           uint32_t transition_poll_ms,
                                           mos_error *err_out)
{
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_registry_id(registry_id, &err);
    if (!h) {
        if (err_out) *err_out = (err != MOS_OK) ? err : MOS_ERR_IO;
        return NULL;
    }
    return watch_open_from_validated_handle(h, stable_poll_ms,
                                            transition_poll_ms, err_out);
}

mos_watch_t *mos_watch_open_all(uint32_t stable_poll_ms,
                                uint32_t transition_poll_ms,
                                mos_error *err_out)
{
    mos_watch_t *w = (mos_watch_t *)calloc(1, sizeof(*w));
    if (!w) {
        if (err_out) *err_out = MOS_ERR_OOM;
        return NULL;
    }
    w->all_mode           = true;
    w->bsd_unit           = -1;   /* no single unit; accessor contract */
    /* Raw caller values (possibly 0); each per-core watch_init
       substitutes the documented defaults, so these fields are NOT
       effective rates. */
    w->stable_poll_ms     = stable_poll_ms;
    w->transition_poll_ms = transition_poll_ms;
    /* One stream, one open time: minted before any slot exists so
       every event — open-time snapshot or hot-plug join — carries the
       same stream_open_ms (struct field comment has the contract). */
    w->all_stream_open_wall_ms = stream_epoch_wall_ms();
    mos_internal_watch_all_init(&w->all);

    /* Observers BEFORE the snapshot: a device arriving in the gap is
       caught by the queued Appeared (its callback runs only inside the
       pump), and one landing in both dedupes by registry_id. The
       reverse order left an unwatchable window — all-mode discovery
       has no poll floor (C2, doc/research/2026-06-11-review-triage.md). */
    w->run_loop = CFRunLoopGetCurrent();
    setup_dr_doorbell_wake(w);
    /* No kIOGeneralInterest in all mode: removal rides DR Disappeared
       (wake) + per-probe NO_DEVICE (floor). */

    /* UNLIKE single mode, the doorbell is NOT latency-only here, so
       its failure cannot soft-fail to polling: arrivals are discovered
       ONLY by the DR Appeared observer — the pump probes known slots
       and never re-scans the directory, so all-mode's poll floor
       covers state changes and removals but discovery has no floor at
       all. A doorbell-less all-watch would "succeed" while unable to
       honor the hot-plug-joins / empty-stream-waits contract (the
       headline of this function's doc block), and the caller has no
       way to detect the degradation. Fail honestly instead. A slow
       directory-rescan fallback that would restore soft-fail is a
       v0.next decision contingent on this failure being observed in
       practice (ROADMAP). */
    if (!w->dr_center) {
        mos_watch_close(w);
        if (err_out) *err_out = MOS_ERR_IO;
        return NULL;
    }

    /* Initial population from ONE directory snapshot — no per-device
       opens (the first probe validates each device; a vanished one
       yields its device_removed through the normal path). Zero devices
       is a valid empty stream. */
    mos_internal_dr_snapshot snap[MOS_WATCH_ALL_CAP];
    size_t n = mos_internal_dr_copy_snapshot(snap, MOS_WATCH_ALL_CAP);
    for (size_t i = 0; i < n; ++i) {
        watch_all_add_device(w, &snap[i], /*mid_stream=*/false);
    }

    if (err_out) *err_out = MOS_OK;
    return w;
}

/* Safe to call on NULL; do not call twice (mos_close convention). Order is
   load-bearing — stop callbacks before releasing the memory they reference:
   DR doorbell, then IOKit interest wake, then the retained io_service_t.
   (Each teardown helper enforces its own internal stop-before-free order.) */
void mos_watch_close(mos_watch_t *w)
{
    if (!w) return;

    teardown_dr_doorbell_wake(w);
    teardown_iokit_interest_wake(w);

    if (w->svc != IO_OBJECT_NULL) {
        IOObjectRelease(w->svc);
    }
    free(w);
}

int64_t mos_watch_bsd_unit(const mos_watch_t *w)
{
    /* -1 for NULL, for a media-less single-target watch, and always
       for an all-watch (no single unit; demux per event instead). */
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
        mos_watch_decision d = w->all_mode
            ? mos_internal_watch_all_pump(&w->all)
            : mos_internal_watch_pump(&w->core);

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
           (notify_source or dr_source) — an empty mode returns instantly
           and tight-loops — AND we are on the thread that owns it. The
           documented contract is open/pump/close on one thread; if a
           caller violates it anyway, CFRunLoopRunInMode here would run
           the WRONG thread's loop, where our private mode has no sources,
           returning instantly and burning CPU until timeout_ms. The
           misuse stays misuse (notification wakes can't reach a foreign
           thread's loop), but it degrades to honest nanosleep polling
           instead of a busy-spin. */
        if (w->run_loop && (w->notify_source || w->dr_source) &&
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
