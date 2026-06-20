/*
 * mos_watch.c — Apple-side adapter for the pure watch state machine
 * (src/mos_watch_core.c). Single-threaded by contract: notification
 * callbacks fire on the same run-loop thread that calls
 * mos_watch_next_event, so no locking.
 */

/* Before any system header so BSD extensions stay visible: strlcpy (the
   identity/serial re-home below) needs _DARWIN_C_SOURCE, and the amalgamation
   merges adapter TUs into one feature-macro environment, so this also keeps
   standalone and amalgamated builds from diverging. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

/* CLOCK_MONOTONIC / clock_gettime require POSIX.1-2008. */
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

/* Private run-loop mode for the watch's sources — never
   kCFRunLoopDefaultMode — so the host's default-mode work can't dispatch
   our callbacks and our CFRunLoopStop can't halt a host-owned run-loop
   invocation. The pump runs this mode, so our sources fire only while
   mos_watch_next_event waits. */
#define MOS_WATCH_RUN_LOOP_MODE CFSTR("io.github.napieraj.mos.watch")

/* ---- Public opaque type --------------------------------------------- */

struct mos_watch {
    /* Pure state machine; owns session identity / bsd_unit / seq. */
    mos_watch_state core;

    /* Whole-disk unit (N in "diskN"), or -1 for an empty/open-tray drive.
       Tags events and feeds mos_watch_bsd_unit — NOT the probe authority;
       that's registry_id below. */
    int64_t bsd_unit;

    /* IORegistry entry ID of the bound drive, captured at construction.
       watch_probe reopens the SAME drive each poll, immune to BSD-name
       reassignment: bound to drive A, keeps probing A even if A's name is
       recycled. When A terminates the reopen returns NO_DEVICE → terminal
       device_removed. The single probe-identity authority. */
    uint64_t registry_id;

    /* Retained IOKit reference for the notification; released in close. */
    io_service_t svc;

    /* Notification plumbing; fires on the run loop this port is scheduled
       on (the caller's, set in watch_open). Token released in close. */
    IONotificationPortRef notify_port;
    io_object_t           notify_token;       /* kIOGeneralInterest */
    CFRunLoopSourceRef    notify_source;
    CFRunLoopRef          run_loop;

    /* DiscRecording doorbell for media/tray-change wake-up. DR's
       StatusChanged is device-scoped, so it also wakes tray-open/no-media
       drives. The callback pulls the next poll forward and CFRunLoopStop()s
       the pump. Both NULL on poll-only fallback (creation failed) — polling
       is the correctness floor, the doorbell latency only. SINGLE-TARGET
       ONLY: in all mode discovery rides the doorbell with no poll floor, so
       mos_watch_open_all fails instead of falling back. */
    DRNotificationCenterRef dr_center;
    CFRunLoopSourceRef      dr_source;

    /* Holds the most recent event so mos_watch_next_event can return
       borrowed pointers valid until the next call. Identity values are
       plain; vendor/product/revision point into the buffers below. */
    mos_watch_event last_event;

    /* Device-static identity, captured ONCE from the validated open handle
       (DR-directory strings), owned for the watch's life. Events point
       here; per-probe handles never contribute identity. Widths are the
       SPC-4 INQUIRY field widths the directory data parses from:
         vendor[9]    VENDOR_IDENTIFICATION   ( 8 + NUL)
         product[17]  PRODUCT_IDENTIFICATION  (16 + NUL)
         revision[5]  PRODUCT_REVISION_LEVEL  ( 4 + NUL, SPC-4 §6.4.2) */
    char vendor[9];
    char product[17];
    char revision[5];

    /* Drive Unit Serial Number (INQUIRY VPD 0x80), grabbed ONCE per session
       on a probe handle and cached for the watch's life. serial_grabbed flips
       true on the first successful read so later probes stop trying. serial[64]
       matches mos_handle's serial_str width (SPC max is 255; 64 truncates
       safely — the chosen buffer everywhere). Empty (serial[0]==0) until the
       first free/not-ready poll lands the read; events carry NULL until then,
       the cached string after. */
    char serial[64];
    bool serial_grabbed;

    /* ---- Watch-all mode -------------------------------------------- *
     * all_mode selects the multiplexer: `all` is the pure fan-in over
     * per-slot cores, `slots` is the per-device probe context (registry id
     * + identity) each core's ctx points at. The single-target fields above
     * are unused in all mode; bsd_unit stays -1. Poll rates kept for
     * mid-stream joins. */
    bool                 all_mode;
    mos_watch_all_state  all;
    struct mos_watch_slot {
        uint64_t registry_id;
        char     vendor[9];
        char     product[17];
        char     revision[5];
        /* Per-slot serial: same grab-once-per-session contract as the
           single-target fields above. Slots are RECYCLED — a removed device
           frees its slot and watch_all_add_device reclaims any inactive one —
           so these are NOT fresh per device by themselves; that function
           memsets the slot on claim, which is what resets serial_grabbed and
           prevents the prior device's serial from carrying over. */
        char     serial[64];
        bool     serial_grabbed;
    }                    slots[MOS_WATCH_ALL_CAP];
    uint32_t             stable_poll_ms;
    uint32_t             transition_poll_ms;
    /* The all-watch's ONE stream-open timestamp, minted once and given to
       every slot (open-time drives and later joiners alike), so
       stream_open_ms is constant across the stream (mos.h, mos.event.v1).
       Per-event time rides ts; (registry_id, stream_open_ms) stays unique
       because a replug re-mints registry_id. 0 in single-target mode. */
    uint64_t             all_stream_open_wall_ms;

    /* One-shot reconciliation flag (all-mode). A DR Appeared whose snapshot
       fails — transient: DRDeviceCopyInfo returned NULL, or the IORegistry
       path was not yet resolvable at that instant — cannot join its device,
       and Appeared is edge-triggered, so it never re-fires; all-mode also has
       no discovery poll floor, so the device would stay invisible for the
       session. A failed Appeared therefore ARMS this flag, and the next pump
       entry clears it and re-copies the directory ONCE, joining anything not
       already active (watch_all_add_device dedupes). Bounded: a single flag,
       not a periodic rescan, so it recovers the drop WITHOUT reintroducing the
       poll floor the all-watch deliberately omits. Set/read only on the single
       run-loop thread (the watch's threading contract), so no synchronization.
       Residual (strictly narrower than the bug it closes): if the device is
       STILL unresolvable when the reconciliation runs, the flag is already
       cleared and it is missed — two consecutive resolution failures, vs. the
       one the bug needed. */
    bool                 all_rescan_pending;
};

/* ---- Time --------------------------------------------------------- */

/* Monotonic ms. CLOCK_MONOTONIC is unconditional at our 12.0 floor. */
static uint64_t monotonic_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Wall-clock ms for the session-open timestamp (stream_open_ms is real
   epoch ms). Used only at open, via the monotonicized wrapper below. */
static uint64_t wall_clock_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Per-process monotonicized wall ms for the session-open timestamp. Two
   watches opened on the same drive in the same wall ms would otherwise
   share a (registry_id, stream_open_ms) pair. Bumping a same-or-earlier
   reading to last+1 keeps it epoch-ms-shaped while guaranteeing per-process
   uniqueness across NTP step-backs. Event ts is unaffected — it reads
   wall_clock_ms() fresh per emit. */
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
        /* failed CAS reloaded prev; recompute and retry. */
    }
}

/* ---- vtable callbacks -------------------------------------------- */

/* probe: reopen a fresh handle by the watch's registry id, query state,
   close. Handle-per-probe lets a transient driver detach recover next poll.

   POINTER-LIFETIME INVARIANT (adapter-scoped — the pure layer forwards
   `const char *` fields verbatim, blind to one being borrowed from a handle
   we're about to close, so its tests can't catch a violation): before any
   mos_close(h), every handle-borrowed pointer field of the escaping struct
   must be REPLACED with a watch-static buffer (w->vendor / w->product /
   w->revision / w->serial) or NULLed. The footgun is `*out = *qr;` — it copies every
   pointer, so "forgot one" is the default. A new borrowed pointer on
   mos_watch_event / mos_state_result needs watch-lifetime backing and a
   replacement below. (bsd_unit is a value, never replaced.) */
static mos_error watch_probe(void *ctx, mos_state_result *out)
{
    mos_watch_t *w = (mos_watch_t *)ctx;
    if (!w || !out) return MOS_ERR_INVALID_ARG;

    /* Reopen by registry ID: either the SAME drive back, or NO_DEVICE if it
       terminated (core treats that as terminal removal). */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_internal_open_by_registry_id(w->registry_id, &err);
    if (!h) {
        /* Contract: NULL iff err != MOS_OK; force non-OK if violated. */
        return err != MOS_OK ? err : MOS_ERR_IO;
    }

    const mos_state_result *qr = NULL;
    mos_error qerr = mos_query_state(h, &qr);
    if (qerr != MOS_OK || !qr) {
        mos_close(h);
        return qerr != MOS_OK ? qerr : MOS_ERR_IO;
    }

    /* Copy the handle-owned result so its identity strings can be re-homed
       below and survive mos_close(h). */
    *out = *qr;

    /* The drive is pinned, but the media's BSD unit isn't (-1 empty,
       changes across eject/reinsert). Refresh the adapter's copy (feeds
       mos_watch_bsd_unit). The media_id fingerprint rides the copy. */
    w->bsd_unit = out->bsd_unit;
    /* Re-home the three identity pointers to watch-static buffers (lifetime
       invariant above) so they don't survive the mos_close below. Identity
       is device-static, so this loses nothing. */
    out->vendor   = w->vendor[0]   ? w->vendor   : NULL;
    out->product  = w->product[0]  ? w->product  : NULL;
    out->revision = w->revision[0] ? w->revision : NULL;

    /* Grab the serial ONCE per session, piggybacked on this same handle (no
       extra open). mos_query_serial self-gates on exclusive access, so a
       mounted/ready disc makes it BUSY and the CDB never issues — leave it
       ungrabbed and retry next poll; the first empty/not-ready poll lands it
       (the walk's lock is already free then and the serial needs no disc).
       Re-home into watch-static storage like the identity strings (the
       returned pointer borrows the handle we're about to close). */
    if (!w->serial_grabbed) {
        const char *sn = NULL;
        if (mos_query_serial(h, &sn) == MOS_OK && sn && sn[0]) {
            strlcpy(w->serial, sn, sizeof w->serial);
            w->serial_grabbed = true;
        }
    }
    out->serial = w->serial[0] ? w->serial : NULL;

    mos_close(h);
    return MOS_OK;
}

/* Monotonic-ms callback: poll scheduling and latency only. */
static uint64_t watch_mono_ms(void *ctx)
{
    (void)ctx;
    return monotonic_ms();
}

/* Wall-clock ms callback: event ts formatting only. Never scheduling —
   the clock can jump backward. */
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

/* Per-slot probe for watch-all: same contract as watch_probe, but ctx is
   the slot (its own registry id + identity). Same pointer-lifetime
   invariant — identity AND serial repointed at slot storage before close. */
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

    /* Grab the serial once per slot (per session), same contract as
       watch_probe above — piggyback the open handle, BUSY-back-off, re-home
       into slot storage before close. */
    if (!s->serial_grabbed) {
        const char *sn = NULL;
        if (mos_query_serial(h, &sn) == MOS_OK && sn && sn[0]) {
            strlcpy(s->serial, sn, sizeof s->serial);
            s->serial_grabbed = true;
        }
    }
    out->serial = s->serial[0] ? s->serial : NULL;

    mos_close(h);
    return MOS_OK;
}

static const mos_watch_ops_t apple_watch_slot_ops = {
    .probe   = watch_slot_probe,
    .mono_ms = watch_mono_ms,
    .wall_ms = watch_wall_ms,
};

/* Add one device from a DR snapshot. Dedupe by registry_id before touching
   slot storage; the slot is claimed by the same first-free scan add() uses
   (single-thread contract keeps the scans agreeing). */
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
        return; /* full and genuinely new — dropped for this plug session. A
                   later slot free does NOT reconsider it (no rescan on free);
                   recovery is a replug, which re-fires Appeared (mos.h). */
    }
    /* free_slot returns ANY inactive slot, so this one may be RECYCLED from a
       device that was removed (the core freed it: mos_watch_core.c active[best]
       = false on DEVICE_REMOVED). Reset every cached per-device field before
       claiming it. The identity strings below are unconditionally overwritten,
       but the grab-once serial cache (serial / serial_grabbed) is NOT — a
       recycled slot with serial_grabbed still true would skip the re-query and
       emit the PRIOR device's serial (durable-identity corruption). memset
       clears all of it, and stays correct if another cached slot field is
       added later. (Initial-snapshot slots are already zero from the calloc'd
       handle, so this is a no-op there.) */
    memset(&w->slots[i], 0, sizeof w->slots[i]);
    /* Source and destination share the SPC-4 identity widths, so these
       copies can't truncate. */
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
 * Fires on the run-loop thread for kIOGeneralInterest on the matched
 * io_service_t. Do NOT also subscribe to kIOBusyInterest: a probe changes
 * the drive's busy state, which would fire and schedule another probe — a
 * live loop.
 *
 * Message handling:
 *   1. kIOMessageServiceIsTerminated → TERMINAL (notify_removed).
 *   2. kIOMessageServicePropertyChange → WAKE (notify_wake). Tracks drive,
 *      not client, state, so it does NOT fire on our own probe
 *      open/close — safe to wake on.
 *   3. Everything else IGNORED — including IsAttemptingOpen / WasClosed /
 *      BusyStateChange, which fire on any user-client open/close (our probes
 *      included) and would self-trigger.
 *
 * messageType is natural_t here vs uint32_t in the SDK typedef; both are
 * `unsigned int`, so the function-pointer types stay compatible under
 * -Werror. */
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
        /* TERMINAL: drive went away. */
        mos_internal_watch_notify_removed(&w->core);
        break;

    case kIOMessageServicePropertyChange:
        /* WAKE: a registry property changed (maybe a media-state
           transition). Pull the next poll forward; false wakes are cheap. */
        mos_internal_watch_notify_wake(&w->core);
        break;

    default:
        /* IGNORED (see header): power/sleep messages and the
           IsAttemptingOpen/WasClosed/BusyStateChange family that
           self-trigger on our own probe handles. */
        return;
    }

    /* TERMINAL and WAKE: break the pump's CFRunLoopRunInMode sleep so the
       next mos_watch_next_event returns promptly. */
    if (w->run_loop) {
        CFRunLoopStop(w->run_loop);
    }
}

/* ---- Wake source: DiscRecording doorbell --------------------------- *
 *
 * kDRDeviceStatusChangedNotification fires on a device status-dict change
 * (media in/out, tray, busy), collapsing worst-case insert→event latency
 * from stable_poll_ms to DR's delivery time. Polling is the correctness
 * floor; the doorbell is latency only, so any setup failure falls back to
 * poll-only (dr_center/dr_source NULL; close treats NULL as no-op).
 *
 * DR's notification is DEVICE-scoped, so it also rings for tray-open /
 * no-media drives.
 *
 * The callback filters by registry ID — a parameter, so watch-all widens
 * the filter rather than rewiring the pump. Fail-OPEN: an unresolvable
 * device wakes anyway (a false wake is one cheap probe; a missed wake is
 * stable_poll_ms of latency). DR data never decides state — the wake only
 * schedules the MMC probe.
 */

static void dr_status_changed_callback(DRNotificationCenterRef center,
                                       void *observer, CFStringRef name,
                                       DRTypeRef object,
                                       CFDictionaryRef info)
{
    /* Fires on the run loop the DR source is scheduled on (the caller's). */
    (void)center; (void)name; (void)info;

    mos_watch_t *w = (mos_watch_t *)observer;
    if (!w) return;

    /* Filter by registry ID: resolve the changed DRDeviceRef's path → entry
       ID and compare. Any resolution failure wakes anyway (fail-open). In
       all mode the filter routes instead of rejects: wake the matching
       slot, or every slot when unresolved. */
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
        bool woke = false;
        int slot = (id != 0) ? mos_internal_watch_all_find(&w->all, id) : -1;
        if (slot >= 0) {
            mos_internal_watch_notify_wake(&w->all.cores[slot]);
            woke = true;
        } else if (id == 0) {
            for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
                if (w->all.active[i]) {
                    mos_internal_watch_notify_wake(&w->all.cores[i]);
                    woke = true;
                }
            }
        }
        /* id resolved but unknown: a device we're not watching (cap
           overflow) or one whose Appeared hasn't delivered — the Appeared
           handler owns joins, nothing to wake. Only stop the pump when a
           poll was actually pulled forward. */
        if (woke && w->run_loop) CFRunLoopStop(w->run_loop);
        return;
    }

    if (id != 0 && w->registry_id != 0 && id != w->registry_id) {
        return; /* another drive */
    }

    mos_internal_watch_notify_wake(&w->core);
    /* Break the pump's sleep so the next call re-probes immediately. */
    if (w->run_loop) {
        CFRunLoopStop(w->run_loop);
    }
}

/* Set up the kIOGeneralInterest notification for service termination. Each
   failing step tears down what it created and leaves every field NULL —
   caller falls back to poll-only for this mechanism (the DR doorbell is
   independent). Invariant the pump's run-loop gate depends on: on return,
   w->notify_port is non-NULL iff w->notify_source is too AND a notification
   is registered. */
static void setup_iokit_interest_wake(mos_watch_t *w)
{
    if (!w || !w->run_loop || w->svc == IO_OBJECT_NULL) return;

    w->notify_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!w->notify_port) return;

    w->notify_source = IONotificationPortGetRunLoopSource(w->notify_port);
    if (!w->notify_source) {
        /* No source: tear the port down. A set notify_port without a live
           source would make the pump's gate run CFRunLoopRunInMode in a
           source-less mode, returning instantly and tight-looping to
           timeout. */
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
        /* Registration failed: remove the source and tear down the port. */
        CFRunLoopRemoveSource(w->run_loop, w->notify_source,
                              MOS_WATCH_RUN_LOOP_MODE);
        w->notify_source = NULL;
        IONotificationPortDestroy(w->notify_port);
        w->notify_port   = NULL;
        return;
    }

    /* kIOBusyInterest deliberately NOT registered: BusyStateChange fires on
       every user-client open/close (our own probes included), so it would
       self-trigger a tight probe loop. */
}

/* Tear down the IOKit interest notification in reverse order: remove
   source, release token, destroy port. Safe on NULL/poll-only state. */
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

/* All-mode lifecycle: Appeared joins a device (its first event is relabeled
   device_appeared by the multiplexer); Disappeared wakes the matching slot
   so its reopen confirms removal. All-mode only — single-target watches use
   kIOGeneralInterest for terminal removal. */
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
    } else {
        /* Not resolvable at this Appeared edge (transient). Appeared won't
           re-fire and all-mode has no discovery floor, so arm the one-shot
           reconciliation (struct field doc): the next pump re-copies the
           directory and joins anything now resolvable. */
        w->all_rescan_pending = true;
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
        /* Wake, not removal authority: the woken reopen confirms (NO_DEVICE
           → terminal), and a spurious Disappeared costs one probe instead
           of a permanent eviction (DR never decides state). */
        mos_internal_watch_notify_wake(&w->all.cores[slot]);
    }
    /* Unresolved id: the probe floor catches it — the next reopen returns
       NO_DEVICE. */
    if (w->run_loop) CFRunLoopStop(w->run_loop);
}

/* Set up the DR notification center and register the StatusChanged
   observer. Independent of the IOKit wake — either may fail soft to
   poll-only. Stores center + source on success, both NULL on failure. */
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

    /* Register LAST: once observed, callbacks can fire, so every prior step
       must already be safe. object=NULL observes all devices; the callback
       filters by registry ID (fail-open). All mode adds the
       Appeared/Disappeared observers that make the bus stream live. */
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

    /* AddObserver is void with no documented failure channel; the fallible
       setup is center/source creation (checked above), and all-watch open fails
       honestly (MOS_ERR_IO) if either fails. The Appeared observer catches
       FUTURE arrivals ("a device has become available", DRCoreDevice.h);
       devices already present at registration are caught by the explicit initial
       DRCopyDeviceArray snapshot (mos_watch_open_all) + registry-ID dedup, NOT by
       an SDK already-connected guarantee — the SDK documents none. */
    w->dr_center = center;
    w->dr_source = source;
}

/* Tear down the DR doorbell in reverse order: remove observer, remove
   source, release both. Safe on NULL/poll-only state. */
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

/* Takes ownership of `h`; builds a watch or closes h on failure. The single
   funnel all public open entry points go through — none re-resolve by name.
   bsd_unit comes from the handle's resolved unit, not a caller string, so
   unit and service identity stay consistent. */
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

    /* An empty drive has no unit (-1). Not a failure — identity is the
       registry_id below, and the device-scoped doorbell covers a nameless
       drive like a named one. */
    const int64_t bsd_unit = mos_handle_bsd_unit(h);   /* -1 if empty */

    mos_watch_t *w = (mos_watch_t *)calloc(1, sizeof(*w));
    if (!w) {
        mos_close(h);
        if (err_out) *err_out = MOS_ERR_OOM;
        return NULL;
    }

    w->bsd_unit = bsd_unit;   /* -1 (empty) carries through */

    /* Capture the validated io_service_t before mos_close — the identity
       the watch preserves (registry_id below comes from it). handle_get_
       service returns it without a retain, so retain here (mos_close drops
       the handle's own) and release in mos_watch_close. On retain failure
       leave w->svc NULL and fall back to poll-only. */
    io_service_t validated_svc = mos_internal_handle_get_service(h);
    if (validated_svc != IO_OBJECT_NULL &&
        IOObjectRetain(validated_svc) == KERN_SUCCESS) {
        w->svc = validated_svc;
    }

    /* Capture the registry-entry ID — the authority watch_probe reopens by.
       Fail closed if absent: a BSD-name fallback would reintroduce the
       two-identity bug this path closes. */
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

    /* Capture device-static identity ONCE before the handle closes (DR
       directory). Events point at these buffers for the watch's life;
       per-probe handles never contribute identity. strlcpy can't truncate —
       pinned at build time: */
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

    /* Init the pure state machine BEFORE registering callbacks that mutate
       it: init can't fail and depends on nothing but the clocks, so doing
       it first makes early callback delivery harmless. */
    mos_internal_watch_init(&w->core, &apple_watch_ops, w,
                            w->bsd_unit,
                            /*registry_id=*/w->registry_id,
                            /*start_mono_ms=*/monotonic_ms(),
                            /*start_wall_ms=*/stream_epoch_wall_ms(),
                            stable_poll_ms,
                            transition_poll_ms);

    /* Capture the caller's run loop once for both wake sources (best-effort,
       independent, either can fail to poll-only).

       CFRunLoopGetCurrent() is a BORROWED ref (Get rule), and a thread's run
       loop is freed when the thread exits. If the handle outlives its origin
       thread (embedder behind a dispatch queue / thread pool / actor), the
       unconditional derefs in callbacks, teardown, and the pump gate become
       use-after-free. Retain here / release in mos_watch_close so it lives
       for the handle's life. The retain makes off-thread misuse memory-safe,
       but it stays a CONTRACT VIOLATION degraded to polling/sleep: the wakes
       (CFRunLoopStop) target the ORIGIN loop, so driven off-thread they never
       break the current thread's sleep and the pump falls back to its poll
       cadence. It does NOT make cross-thread use functional; the single-thread
       contract stands. */
    w->run_loop = CFRunLoopGetCurrent();
    CFRetain(w->run_loop);

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
    /* Resolve once via BSD name (this entry point's contract). The
       validated handle's service identity is then preserved by the funnel —
       no second BSD-name resolution. */
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
       IORegistryEntryIDMatching, so the handle is the exact peripheral
       selected at enumeration time; the funnel preserves that identity. */
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
    w->bsd_unit           = -1;   /* no single unit */
    /* Raw caller values (possibly 0); each per-core watch_init substitutes
       defaults, so these are NOT effective rates. */
    w->stable_poll_ms     = stable_poll_ms;
    w->transition_poll_ms = transition_poll_ms;
    /* One stream, one open time, minted before any slot exists so every
       event carries the same stream_open_ms (contract in the struct). */
    w->all_stream_open_wall_ms = stream_epoch_wall_ms();
    mos_internal_watch_all_init(&w->all);

    /* Observers BEFORE the snapshot: a device arriving in the gap is caught
       by the queued Appeared (callback runs only inside the pump), and one
       landing in both dedupes by registry_id. The reverse order leaves an
       unwatchable window — all-mode discovery has no poll floor. */
    w->run_loop = CFRunLoopGetCurrent();
    CFRetain(w->run_loop);   /* borrowed Get-rule ref; released in close */
    setup_dr_doorbell_wake(w);
    /* No kIOGeneralInterest in all mode: removal rides DR Disappeared
       (wake) + per-probe NO_DEVICE (floor). */

    /* Unlike single mode the doorbell is NOT latency-only: arrivals are
       discovered by the Appeared observer, with no periodic poll floor (the
       pump re-scans the directory ONLY when a failed Appeared snapshot arms
       the bounded reconciliation — never on a timer). A doorbell-less all-watch
       would "succeed" while unable to honor the hot-plug-joins contract,
       undetectably. Fail honestly instead. */
    if (!w->dr_center) {
        mos_watch_close(w);
        if (err_out) *err_out = MOS_ERR_IO;
        return NULL;
    }

    /* Initial population from ONE directory snapshot — no per-device opens
       (the first probe validates each; a vanished one yields device_removed
       normally). Zero devices is a valid empty stream. */
    mos_internal_dr_snapshot snap[MOS_WATCH_ALL_CAP];
    size_t n = mos_internal_dr_copy_snapshot(snap, MOS_WATCH_ALL_CAP);
    for (size_t i = 0; i < n; ++i) {
        watch_all_add_device(w, &snap[i], /*mid_stream=*/false);
    }

    if (err_out) *err_out = MOS_OK;
    return w;
}

/* Safe on NULL; not idempotent (mos_close convention). Order is
   load-bearing — stop callbacks before freeing what they reference: DR
   doorbell, IOKit wake, then the retained io_service_t. */
void mos_watch_close(mos_watch_t *w)
{
    if (!w) return;

    teardown_dr_doorbell_wake(w);
    teardown_iokit_interest_wake(w);

    /* Release the run loop AFTER both teardowns — they call
       CFRunLoopRemoveSource(w->run_loop, …), so it must still be alive.
       NULL only on a watch that never reached the capture sites. */
    if (w->run_loop) {
        CFRelease(w->run_loop);
    }

    if (w->svc != IO_OBJECT_NULL) {
        IOObjectRelease(w->svc);
    }
    free(w);
}

int64_t mos_watch_bsd_unit(const mos_watch_t *w)
{
    /* -1 for NULL, a media-less single-target watch, and always for an
       all-watch (no single unit — demux per event). */
    if (!w) return -1;
    return w->bsd_unit;
}

/* ---- Pump --------------------------------------------------------- */

mos_error mos_watch_next_event(mos_watch_t *w, const mos_watch_event **out,
                               int timeout_ms)
{
    if (out) *out = NULL;
    if (!w || !out) return MOS_ERR_INVALID_ARG;

    /* The pump may spin a few times (pump → SLEEP_UNTIL → wait → pump →
       EMIT) within one call, bounded by timeout_ms. On exhaustion return
       MOS_ERR_TIMEOUT; the caller decides whether to call again. */
    uint64_t start = monotonic_ms();
    uint64_t deadline = (timeout_ms < 0)
        ? UINT64_MAX
        : start + (uint64_t)timeout_ms;
    bool drained = false;   /* at most one non-blocking source drain per call */

    for (;;) {
        /* Bounded reconciliation: a DR Appeared that couldn't snapshot its
           device armed all_rescan_pending (struct field doc). Drain it BEFORE
           pumping so a re-resolved device is joined and its slot is included in
           this pump's fan-in (its first event is relabeled device_appeared).
           One directory re-copy per armed flag — watch_all_add_device dedupes
           the already-active devices, so this only ADDS the missed one(s). */
        if (w->all_mode && w->all_rescan_pending) {
            w->all_rescan_pending = false;
            mos_internal_dr_snapshot snap[MOS_WATCH_ALL_CAP];
            size_t rn = mos_internal_dr_copy_snapshot(snap, MOS_WATCH_ALL_CAP);
            for (size_t i = 0; i < rn; ++i)
                watch_all_add_device(w, &snap[i], /*mid_stream=*/true);
        }

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

        /* SLEEP_UNTIL: block until next_poll_at_mono_ms or a notification
           fires (CFRunLoopStop). The deadline is a monotonic value,
           compared like the pump does. */
        uint64_t now = monotonic_ms();
        if (now >= deadline) {
            /* Contract (mos.h): timeout_ms == 0 must DRAIN a ready event, not
               merely poll the pure core. The deadline is already reached, but a
               signalled DR/IOKit source may be queued with no poll core to pump
               it — the empty all-watch + queued Appeared case, which has no
               discovery floor and would otherwise stay invisible across every
               zero-timeout call. Service ready sources ONCE with a non-blocking
               run of the private mode, then re-pump (a handled Appeared adds a
               slot or arms the rescan the loop top drains). Guarded to one drain
               per call so a positive timeout cannot spin here; only on the owning
               thread with a source scheduled (else the run would be a no-op). */
            if (!drained && w->run_loop && (w->notify_source || w->dr_source) &&
                CFRunLoopGetCurrent() == w->run_loop) {
                drained = true;
                CFRunLoopRunInMode(MOS_WATCH_RUN_LOOP_MODE, 0, false);
                continue;
            }
            return MOS_ERR_TIMEOUT;   /* caller timeout; pump again next call */
        }

        uint64_t sleep_until_ms = d.next_poll_at_mono_ms;
        if (sleep_until_ms > deadline) sleep_until_ms = deadline;
        if (sleep_until_ms < now) sleep_until_ms = now;
        double interval_sec = (double)(sleep_until_ms - now) / 1000.0;

        /* Wait on the run loop only if a source is scheduled (an empty mode
           returns instantly and tight-loops) AND we're on the owning thread.
           Off the owning thread, CFRunLoopRunInMode would run a source-less
           mode on the wrong loop and burn CPU to timeout; the thread check
           degrades that misuse to honest nanosleep instead of a busy-spin
           (it does not make cross-thread wakes work). */
        if (w->run_loop && (w->notify_source || w->dr_source) &&
            CFRunLoopGetCurrent() == w->run_loop) {
            /* Returns on timer, CFRunLoopStop, or a handled source — any is
               a wake; loop back to pump. Private mode keeps us off the
               host's default-mode sources. */
            CFRunLoopRunInMode(MOS_WATCH_RUN_LOOP_MODE, interval_sec, false);
        } else {
            /* No source scheduled: nanosleep. Taken in poll-only mode (no
               validated io_service_t) or when both registration paths
               failed at open. */
            struct timespec ts;
            ts.tv_sec  = (time_t)(sleep_until_ms - now) / 1000;
            ts.tv_nsec = (long)((sleep_until_ms - now) % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
        /* Loop and re-pump. */
    }
}
