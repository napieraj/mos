/*
 * mos_watch_core.c — pure watch state machine.
 *
 * Drives the poll loop:
 *
 *   - When to probe (next_poll_at_mono_ms, with two backoff rates by
 *     whether the last state was "transitional" or "stable").
 *   - What to emit (snapshot on first probe; state_changed when state
 *     differs from last; media_changed on a same-state ready disc swap;
 *     error on transient probe failure; device_removed when notify_removed
 *     fires OR a probe returns MOS_ERR_NO_DEVICE).
 *   - How to label it (monotonic seq; RFC 3339 ts from ops->wall_ms;
 *     registry_id + stream_open_wall_ms values; bsd_unit int).
 *   - When to stop (terminated flag from notify_removed, or auto-set on
 *     probe → NO_DEVICE).
 *
 * Two time domains, separated at the type level so a mixup cannot be
 * introduced silently — the signatures distinguish them at every callsite:
 *
 *   - ops->mono_ms() — MONOTONIC. Poll scheduling and latency only; never
 *     human-readable output.
 *   - ops->wall_ms() — WALL-CLOCK ms since Unix epoch. stream_open_wall_ms
 *     and event ts only; can jump backward on NTP, so never used for
 *     scheduling.
 *
 * The caller's pump loop owns blocking — this core never sleeps or calls
 * the OS. It returns a decision: EMIT_EVENT (write and re-pump),
 * SLEEP_UNTIL (block, then re-pump), TERMINAL (close). mos_watch.c maps
 * SLEEP_UNTIL to CFRunLoopRunInMode so a notification can wake early; the
 * test driver maps it to advancing a fake clock. Mirrors the
 * mos_state_core.c pattern: every transition testable without IOKit. See
 * tests/test_watch_core.c — including the fixture that pins the two-clock
 * contract by running mono_ms in the thousands and wall_ms in the trillions.
 */

/* Precautionary, mirroring mos_watch.c: this file uses only POSIX time
   interfaces today, but the define keeps BSD extensions visible if a
   BSD-only helper is added later. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

/* gmtime_r requires POSIX.1-2008 (200809L) on glibc; Apple's time.h
   exposes it unconditionally. Define before any header inclusion. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mos_pure.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- Defaults ----------------------------------------------------- */

/* The two backoff rates. "stable" (open / empty / ready) is not a
   transition window — changes arrive via OS notifications (insert,
   eject). "transition" (loading / busy / unknown) is mid-transition, so
   short polls catch the resolution faster than waiting for one. Defaults
   are conservative (2s / 200ms): stable still notices an insert within a
   couple of seconds without notifications, transition observes ready
   resolution with no perceptible delay. Both configurable per watch. */
#define MOS_WATCH_DEFAULT_STABLE_MS     2000U
#define MOS_WATCH_DEFAULT_TRANSITION_MS  200U

/* ---- Time formatting --------------------------------------------- */

/* Format milliseconds-since-epoch as RFC 3339 UTC in YYYY-MM-DDTHH:MM:SSZ.
   Writes 21 bytes including NUL into the buffer (size must be >= 21).
   The seconds component is integer; sub-second precision is not
   surfaced in events. Input is WALL-CLOCK ms — feeding monotonic ms
   would produce nonsense like 1970-01-01T00:00:12Z.

   SATURATING: the schema pattern requires a 4-digit year, and the clock
   is an INPUT to this pure layer (the fourth review's point: the
   hostile-input discipline applies to ops->wall_ms exactly as it does
   to drive-controlled bytes — an insane host clock, NTP step, or fuzzed
   ops table must not produce a schema-invalid line). Values past
   9999-12-31T23:59:59Z clamp to that instant; a 5-digit year from
   strftime (21 chars) and an empty string from a gmtime_r failure are
   both schema violations, so neither can escape. Post-clamp, gmtime_r
   and strftime cannot fail for any uint64 input; the fallback writes
   the clamp constant anyway so the contract holds unconditionally. */
#define MOS_TS_MAX_SECS 253402300799ULL   /* 9999-12-31T23:59:59Z */
static void format_rfc3339(uint64_t wall_ms, char *out, size_t cap)
{
    if (cap < 21) {
        if (cap > 0) out[0] = 0;
        return;
    }
    uint64_t s64 = wall_ms / 1000U;
    if (s64 > MOS_TS_MAX_SECS) s64 = MOS_TS_MAX_SECS;
    time_t secs = (time_t)s64;
    struct tm tm;
    /* gmtime_r: POSIX.1-2008, present on every platform this project
       compiles on (macOS targets, Linux pure-test CI). A _WIN32/gmtime_s
       branch used to live here; removed as dead — Windows is neither a
       build nor a test target, and untestable code is unverifiable. */
    if (gmtime_r(&secs, &tm) != NULL) {
        /* All-numeric strftime specifiers (%Y %m %d %H %M %S) are
           POSIX-defined as locale-independent — locale only affects textual
           ones (%A, %B, %p, %c/%x/%X) we don't use. strftime also sidesteps a
           -Wformat-truncation false positive that a hand-rolled snprintf
           hits under -O2 (GCC sees tm_year as unbounded int). */
        if (strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 20) {
            return;
        }
    }
    /* Unreachable post-clamp on a conforming libc; the contract holds
       even if a libc misbehaves. */
    memcpy(out, "9999-12-31T23:59:59Z", 21);
}
/* ---- Public-via-mos_pure.h init / pump --------------------------- */

void mos_internal_watch_init(mos_watch_state *w,
                             const mos_watch_ops_t *ops, void *ctx,
                             int64_t bsd_unit,
                             uint64_t registry_id,
                             uint64_t start_mono_ms,
                             uint64_t start_wall_ms,
                             uint32_t stable_poll_ms,
                             uint32_t transition_poll_ms)
{
    memset(w, 0, sizeof(*w));
    w->ops                    = ops;
    w->ctx                    = ctx;
    w->stable_poll_ms         = stable_poll_ms     ? stable_poll_ms
                                                   : MOS_WATCH_DEFAULT_STABLE_MS;
    w->transition_poll_ms     = transition_poll_ms ? transition_poll_ms
                                                   : MOS_WATCH_DEFAULT_TRANSITION_MS;
    w->last_state             = MOS_STATE_UNKNOWN;
    w->have_last_state        = false;
    w->last_media_id          = 0;
    w->last_profile           = 0;
    w->next_seq               = 1;
    /* Schedule the first probe at start_mono_ms (i.e. immediately).
       This is a MONOTONIC value; the pump compares against ops->mono_ms. */
    w->next_poll_at_mono_ms   = start_mono_ms;
    w->terminated             = false;
    w->removed_event_emitted  = false;
    w->bsd_unit               = bsd_unit;   /* -1 == no media */

    /* Session identity: two plain values, no composite token. The
       registry_id is the attachment identity (xnu mints real IDs from a
       never-reused monotone counter >= 2^32+256; 0 is only reachable
       from direct pure-layer callers). start_wall_ms is recorded as
       stream_open_wall_ms on every event; the adapter monotonicizes it
       per process so the (registry_id, stream_open_wall_ms) pair is
       unique per session even for same-millisecond reopens of the same
       drive. Consumers needing a single correlation key derive one —
       the data layer stays normalized. */
    w->registry_id         = registry_id;
    w->stream_open_wall_ms = start_wall_ms;
}

void mos_internal_watch_notify_removed(mos_watch_state *w)
{
    if (!w) return;
    w->terminated = true;
}

void mos_internal_watch_notify_wake(mos_watch_state *w)
{
    if (!w) return;
    /* Pull the next poll forward to "right now" without inspecting
       the clock here — the caller's pump call will compare against
       ops->mono_ms() and probe immediately. Setting to 0 (zero
       monotonic ms) guarantees `now >= next_poll_at_mono_ms` on the
       next pump regardless of how the monotonic clock started. */
    w->next_poll_at_mono_ms = 0;
}

/* Whether the state is a "transition" state for backoff purposes.
   Loading / busy / unknown are transitional; the others are stable. */
static bool watch_state_is_transitional(mos_state_enum s)
{
    switch (s) {
        /* In-progress or degraded observations — poll fast to converge. */
        case MOS_STATE_LOADING:
        case MOS_STATE_BUSY:
        case MOS_STATE_FORMATTING:      /* long op, still progressing to ready/empty */
        case MOS_STATE_EMPTY_OR_OPEN:   /* degraded: GESN was unavailable; re-probe to resolve */
        case MOS_STATE_UNKNOWN:
            return true;
        /* Settled physical states and stable error conditions. */
        case MOS_STATE_OPEN:
        case MOS_STATE_EMPTY:
        case MOS_STATE_READY:
        case MOS_STATE_MEDIA_UNREADABLE:  /* bad disc; not self-resolving */
        case MOS_STATE_DEVICE_FAULT:      /* drive fault; not self-resolving */
            return false;
    }
    /* No default: a new mos_state_enum value trips -Wswitch under -Werror, so
       its poll class must be chosen deliberately rather than silently
       inheriting "stable." This trailing return only handles an out-of-range
       value (the enum is int32-wide). */
    return false;
}

/* Build a base event: session identity, seq, ts, bsd_unit. The ts is read
   fresh from ops->wall_ms each time, *not* derived from the
   monotonic clock used for scheduling. The caller fills in
   kind-specific fields. */
static void fill_event_base(mos_watch_state *w, mos_watch_event *e)
{
    memset(e, 0, sizeof(*e));
    e->seq                 = w->next_seq++;
    e->registry_id         = w->registry_id;
    e->stream_open_wall_ms = w->stream_open_wall_ms;
    e->bsd_unit            = w->bsd_unit; /* -1 == no media; copied verbatim */

    uint64_t wall = w->ops->wall_ms ? w->ops->wall_ms(w->ctx) : 0;
    format_rfc3339(wall, e->ts, sizeof(e->ts));

    e->error     = MOS_OK;
}

/* Copy probe-result fields (everything except prev_state) into an event.
   Shared by the snapshot, state_changed, and media_changed branches, so a
   field added to mos_watch_event has one assignment site, not three. */
static void fill_event_state_fields(mos_watch_event *e,
                                    const mos_state_result *r,
                                    uint32_t latency_ms)
{
    e->state           = r->state;
    /* Event-time media identity: take the BSD unit from THIS probe rather
       than the watch's open-time value, so a disc appearing in a drive
       that was empty at open (or a unit that changed across eject/reinsert)
       is reflected in the event. fill_event_base seeds w->bsd_unit as the
       fallback for events with no fresh probe (error / device_removed). */
    e->bsd_unit        = r->bsd_unit;
    e->current_profile = r->current_profile;
    e->vendor          = r->vendor;
    e->product         = r->product;
    e->revision        = r->revision;
    e->sense_key       = r->sense_key;
    e->asc             = r->asc;
    e->ascq            = r->ascq;
    e->latency_ms      = latency_ms;
}

/* Saturating monotonic delta in milliseconds. Guards against a
   non-monotonic clock source (end < start -> 0) and against a probe that
   somehow spans more than ~49.7 days (delta > UINT32_MAX -> clamped),
   either of which would otherwise underflow or truncate on the cast. */
static uint32_t mos_watch_latency_ms(uint64_t start, uint64_t end)
{
    uint64_t delta = end >= start ? end - start : 0;
    return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}

/* Per-state poll interval: transitional states re-probe at the faster
   transition_poll_ms, stable states at stable_poll_ms. One site so the
   policy is audited in one place. */
static uint32_t poll_ms_for_state(const mos_watch_state *w,
                                  mos_state_enum state)
{
    return watch_state_is_transitional(state)
        ? w->transition_poll_ms
        : w->stable_poll_ms;
}

mos_watch_decision mos_internal_watch_pump(mos_watch_state *w)
{
    mos_watch_decision d;
    memset(&d, 0, sizeof(d));

    if (!w || !w->ops) {
        d.kind = MOS_WATCH_DECIDE_TERMINAL;
        return d;
    }

    /* Terminal: caller was told the device went away (either via
       notify_removed or via a probe that returned NO_DEVICE). Emit a
       final device_removed event then refuse further pumps. The
       removed_event_emitted sentinel ensures we emit exactly once,
       even when termination happens before any successful observation
       (in which case prev_state is reported as unknown). */
    if (w->terminated) {
        if (!w->removed_event_emitted) {
            fill_event_base(w, &d.event);
            d.event.kind       = MOS_EVENT_DEVICE_REMOVED;
            d.event.state      = MOS_STATE_UNKNOWN;
            d.event.prev_state = w->have_last_state ? w->last_state
                                                    : MOS_STATE_UNKNOWN;
            w->removed_event_emitted = true;
            d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
            return d;
        }
        d.kind = MOS_WATCH_DECIDE_TERMINAL;
        return d;
    }

    if (!w->ops->probe || !w->ops->mono_ms) {
        d.kind = MOS_WATCH_DECIDE_TERMINAL;
        return d;
    }

    uint64_t now_mono = w->ops->mono_ms(w->ctx);

    /* Not yet time to probe → tell the caller to sleep. */
    if (now_mono < w->next_poll_at_mono_ms) {
        d.kind                 = MOS_WATCH_DECIDE_SLEEP_UNTIL;
        d.next_poll_at_mono_ms = w->next_poll_at_mono_ms;
        return d;
    }

    mos_state_result r;
    memset(&r, 0, sizeof(r));
    uint64_t  probe_start_mono = now_mono;
    mos_error perr             = w->ops->probe(w->ctx, &r);
    uint64_t  probe_end_mono   = w->ops->mono_ms(w->ctx);

    /* MOS_ERR_NO_DEVICE from a probe means the device went away
       underneath the watch. Treat as terminal removal: flip the
       termination flag and let the next pump emit the device_removed
       event through the terminated path above. This handles the case
       where notifications didn't register (or aren't supported on
       the OS) — poll-only mode used to spin emitting error events
       forever when the drive was unplugged; now it terminates. */
    if (perr == MOS_ERR_NO_DEVICE) {
        w->terminated = true;
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_DEVICE_REMOVED;
        d.event.state      = MOS_STATE_UNKNOWN;
        d.event.prev_state = w->have_last_state ? w->last_state
                                                : MOS_STATE_UNKNOWN;
        d.event.latency_ms = mos_watch_latency_ms(probe_start_mono, probe_end_mono);
        w->removed_event_emitted = true;
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    if (perr != MOS_OK) {
        /* Other error: we couldn't observe state this round. Treat
           as non-terminal, emit the error event, and reschedule. */
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_ERROR;
        d.event.error      = perr;
        d.event.state      = MOS_STATE_UNKNOWN;
        d.event.prev_state = w->have_last_state ? w->last_state : MOS_STATE_UNKNOWN;
        d.event.latency_ms = mos_watch_latency_ms(probe_start_mono, probe_end_mono);

        /* Don't update last_state on an error — we don't have an
           observation, just an absence of one.

           Retry cadence: the first error (or a different error code)
           reschedules at transition rate for a prompt retry; each further
           consecutive identical error doubles the interval, capped at
           stable_poll_ms. A persistent failure thus converges to the
           stable cadence instead of flooding at the transition rate,
           while a notify_wake still pulls the next poll forward
           immediately on a real event. */
        if (perr == (mos_error)w->last_probe_err && w->consec_probe_errs > 0) {
            if (w->consec_probe_errs < UINT32_MAX) w->consec_probe_errs++;
        } else {
            w->last_probe_err    = (int32_t)perr;
            w->consec_probe_errs = 1;
        }
        {
            uint64_t interval = w->transition_poll_ms;
            uint32_t doublings = w->consec_probe_errs - 1u;
            while (doublings-- > 0u && interval < w->stable_poll_ms) {
                interval <<= 1;
            }
            if (interval > w->stable_poll_ms) interval = w->stable_poll_ms;
            w->next_poll_at_mono_ms = probe_end_mono + interval;
        }
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    /* Successful observation: any error streak is over. */
    w->last_probe_err    = (int32_t)MOS_OK;
    w->consec_probe_errs = 0;
    /* Adopt the probe's unit as the core's own. The media's BSD unit is
       not stable (-1 when empty at open; changes across eject/reinsert);
       events with a fresh probe carry r.bsd_unit directly, but the
       error/device_removed fallback in fill_event_base reads w->bsd_unit
       — which must therefore track the last OBSERVED unit, not the
       open-time one. This used to be the Apple adapter's job (a direct
       w->core.bsd_unit write), which meant any second adapter had to
       rediscover the obligation and the pure-only behavior was wrong;
       the core owns it now, where the data already is. */
    w->bsd_unit = r.bsd_unit;


    /* First successful probe → snapshot. */
    if (!w->have_last_state) {
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_SNAPSHOT;
        d.event.prev_state = MOS_STATE_UNKNOWN;
        fill_event_state_fields(&d.event, &r,
            mos_watch_latency_ms(probe_start_mono, probe_end_mono));

        w->last_state           = r.state;
        w->have_last_state      = true;
        w->last_media_id        = r.media_id;
        w->last_profile         = r.current_profile;
        w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    /* State change → emit delta. */
    if (r.state != w->last_state) {
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_STATE_CHANGED;
        d.event.prev_state = w->last_state;
        fill_event_state_fields(&d.event, &r,
            mos_watch_latency_ms(probe_start_mono, probe_end_mono));

        w->last_state           = r.state;
        w->last_media_id        = r.media_id;
        w->last_profile         = r.current_profile;
        w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    /* Same-state media swap (F1) → media_changed, while the drive stays READY
       across two probes (a fast slot-load swap, or eject/reinsert that fell
       entirely between polls). Two independent signals:

       1. Registry-identity change — the whole-disk IOMedia registry entry ID
          re-mints on a physical swap even when the MMC profile is unchanged
          (the same-profile DVD→DVD case a profile compare would miss). This is
          the strong signal; both ids must be non-zero (0 = identity
          unavailable, never inferred from).

       2. Profile-class change with NO usable identity — some USB-ATAPI bridges
          never expose a media_id (both 0). There a changed *non-zero* current
          profile (e.g. CD-ROM 0x08 → DVD-ROM 0x10) is the only evidence a swap
          happened, so we use last_profile as the fallback fingerprint. This
          still cannot see a same-class swap (DVD-R → DVD-R) on such bridges —
          no signal exists there — but it catches cross-class swaps that would
          otherwise be silent. */
    bool id_changed =
        r.media_id != 0 && w->last_media_id != 0 &&
        r.media_id != w->last_media_id;
    bool profile_class_changed_without_id =
        r.media_id == 0 && w->last_media_id == 0 &&
        r.current_profile != 0 && w->last_profile != 0 &&
        r.current_profile != w->last_profile;

    if (r.state == MOS_STATE_READY && w->last_state == MOS_STATE_READY &&
        (id_changed || profile_class_changed_without_id)) {
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_MEDIA_CHANGED;
        d.event.prev_state = w->last_state;   /* READY → READY */
        fill_event_state_fields(&d.event, &r,
            mos_watch_latency_ms(probe_start_mono, probe_end_mono));

        w->last_media_id        = r.media_id;
        w->last_profile         = r.current_profile;
        w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    /* No change → reschedule and sleep, no event. The poll-rate
       choice still uses the (unchanged) current state.

       Adopt any *informative* identity the probe carried before sleeping.
       media_id / current_profile that were unavailable (0) at the last
       event often arrive one probe later — the whole-disk IOMedia child
       registers a beat after TUR goes GOOD, and profile enrichment can
       fail transiently. Zero means "unavailable", never an observation:
       a non-zero value is adopted, a zero never overwrites one, so the
       fingerprint survives an unavailability gap and an identity change
       observed across the gap is still detected as a swap. Without this,
       a 0→non-zero arrival would pin the snapshot-era fingerprint for the
       whole session and permanently disarm same-state swap detection. */
    if (r.media_id != 0)        w->last_media_id = r.media_id;
    if (r.current_profile != 0) w->last_profile  = r.current_profile;
    w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
    d.kind                 = MOS_WATCH_DECIDE_SLEEP_UNTIL;
    d.next_poll_at_mono_ms = w->next_poll_at_mono_ms;
    return d;
}

/* ---- Watch-all multiplexer (DR pivot Phase 2b) --------------------- *
 *
 * See mos_pure.h for the contract. Iteration order is ascending
 * registry_id over active slots on EVERY entry, so same-tick event
 * interleave is deterministic and independent of slot assignment
 * history — the property the fixture tests pin. */

void mos_internal_watch_all_init(mos_watch_all_state *a)
{
    if (!a) return;
    memset(a, 0, sizeof *a);
}

int mos_internal_watch_all_free_slot(const mos_watch_all_state *a)
{
    if (!a) return -1;
    for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
        if (!a->active[i]) return i;
    }
    return -1;
}

int mos_internal_watch_all_find(const mos_watch_all_state *a,
                                uint64_t registry_id)
{
    if (!a || registry_id == 0) return -1;
    for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
        if (a->active[i] && a->cores[i].registry_id == registry_id) return i;
    }
    return -1;
}

int mos_internal_watch_all_add(mos_watch_all_state *a,
                               const mos_watch_ops_t *ops, void *ctx,
                               int64_t bsd_unit, uint64_t registry_id,
                               uint64_t start_mono_ms,
                               uint64_t start_wall_ms,
                               uint32_t stable_poll_ms,
                               uint32_t transition_poll_ms,
                               bool mid_stream)
{
    if (!a || registry_id == 0) return -1;

    /* Dedupe by attachment identity: the DR Appeared notification can
       announce a device the open-time snapshot already carried (or
       fire twice across a bus rescan). Same id ⇒ same plug session ⇒
       same slot; a REPLUG has a fresh id by xnu construction and lands
       in a new slot. */
    int existing = mos_internal_watch_all_find(a, registry_id);
    if (existing >= 0) return existing;

    int i = mos_internal_watch_all_free_slot(a);
    if (i < 0) return -1;

    mos_internal_watch_init(&a->cores[i], ops, ctx, bsd_unit, registry_id,
                            start_mono_ms, start_wall_ms,
                            stable_poll_ms, transition_poll_ms);
    a->active[i]       = true;
    a->join_pending[i] = mid_stream;
    return i;
}

mos_watch_decision mos_internal_watch_all_pump(mos_watch_all_state *a)
{
    mos_watch_decision out;
    memset(&out, 0, sizeof out);
    out.kind                 = MOS_WATCH_DECIDE_SLEEP_UNTIL;
    out.next_poll_at_mono_ms = UINT64_MAX;
    if (!a) return out;

    /* Visit active slots in ascending registry_id (selection scan; CAP
       is 16, an index sort would be ceremony). Returning on the first
       EMIT keeps per-call work bounded; the next call re-enters at the
       lowest id, so same-tick events drain in id order. */
    uint64_t visited = 0; /* bitmask of slots already pumped this call */
    for (;;) {
        int best = -1;
        for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
            if (!a->active[i] || (visited & (1ull << i))) continue;
            if (best < 0 ||
                a->cores[i].registry_id < a->cores[best].registry_id) {
                best = i;
            }
        }
        if (best < 0) break;
        visited |= (1ull << best);

        mos_watch_decision d = mos_internal_watch_pump(&a->cores[best]);

        if (d.kind == MOS_WATCH_DECIDE_EMIT_EVENT) {
            d.event.seq = ++a->seq;            /* stream-global numbering */
            /* Relabel the join's SNAPSHOT — and only the snapshot. A
               mid-stream device whose first pumps yield ERROR events
               (probe failing right after hot-plug) keeps its pending
               join, so the eventual first successful probe still
               announces it as device_appeared; clearing on any first
               event would silently demote it to a mid-stream snapshot
               (contract: every joining drive emits device_appeared). */
            if (a->join_pending[best] &&
                d.event.kind == MOS_EVENT_SNAPSHOT) {
                d.event.kind = MOS_EVENT_DEVICE_APPEARED;
                a->join_pending[best] = false;
            }
            if (d.event.kind == MOS_EVENT_DEVICE_REMOVED) {
                /* Per-slot, not stream-terminal: free the slot AFTER
                   taking the event. A replug arrives as a new id. */
                a->active[best] = false;
            }
            return d;
        }
        if (d.kind == MOS_WATCH_DECIDE_TERMINAL) {
            /* The core's device_removed was emitted on an earlier call
               and the slot somehow pumped again (external notify after
               emission). Nothing to emit — just free the slot. */
            a->active[best] = false;
            continue;
        }
        /* SLEEP_UNTIL: fold the earliest deadline. */
        if (d.next_poll_at_mono_ms < out.next_poll_at_mono_ms) {
            out.next_poll_at_mono_ms = d.next_poll_at_mono_ms;
        }
    }
    return out;
}
