/*
 * mos_watch_core.c — pure watch state machine.
 *
 * Drives the poll loop:
 *
 *   - When to probe (next_poll_at_mono_ms, two backoff rates by whether the
 *     last state was transitional or stable).
 *   - What to emit (snapshot on first probe; state_changed on a state
 *     delta; media_changed on a same-state READY disc swap; error on
 *     transient probe failure; device_removed on notify_removed or a probe
 *     returning MOS_ERR_NO_DEVICE).
 *   - How to label it (monotonic seq; RFC 3339 ts from ops->wall_ms;
 *     registry_id + stream_open_wall_ms; bsd_unit).
 *   - When to stop (terminated flag, set by notify_removed or probe →
 *     NO_DEVICE).
 *
 * Two time domains, distinct at the type level so a mixup can't slip in:
 *   - ops->mono_ms() — MONOTONIC. Scheduling/latency only.
 *   - ops->wall_ms() — WALL-CLOCK ms since epoch. stream_open_wall_ms and
 *     event ts only; can jump backward, never used for scheduling.
 *
 * The caller's pump owns blocking — this core never sleeps. It returns a
 * decision: EMIT_EVENT (write and re-pump), SLEEP_UNTIL (block then
 * re-pump), TERMINAL (close). mos_watch.c maps SLEEP_UNTIL to
 * CFRunLoopRunInMode; the test driver advances a fake clock. Every
 * transition is testable without IOKit.
 */

/* Mirrors mos_watch.c: no BSD-only helper here yet, but the define keeps
   the amalgamated feature-macro environment consistent. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

/* gmtime_r needs POSIX.1-2008 on glibc (Apple exposes it unconditionally).
   Define before any header. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mos_pure.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- Defaults ----------------------------------------------------- */

/* Two backoff rates. "stable" (open/empty/ready) isn't a transition window
   — changes arrive via OS notifications. "transition" (loading/busy/
   unknown) polls fast to catch the resolution. Defaults (2s / 200ms) are
   conservative: stable still notices an insert within a couple seconds
   without notifications. Both configurable per watch. */
#define MOS_WATCH_DEFAULT_STABLE_MS     2000U
#define MOS_WATCH_DEFAULT_TRANSITION_MS  200U

/* ---- Time formatting --------------------------------------------- */

/* Format wall-clock ms-since-epoch as RFC 3339 UTC (YYYY-MM-DDTHH:MM:SSZ),
   21 bytes incl NUL (cap must be >= 21). Integer seconds; no sub-second
   precision. Feeding monotonic ms produces nonsense like
   1970-01-01T00:00:12Z.

   SATURATING: the clock is hostile INPUT to this pure layer, so an insane
   host clock, NTP step, or fuzzed ops table must not emit a schema-invalid
   line (the schema requires a 4-digit year). Values past
   9999-12-31T23:59:59Z clamp to that instant; post-clamp gmtime_r/strftime
   cannot fail, and the fallback writes the clamp constant anyway, so the
   contract holds unconditionally. */
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
    /* gmtime_r: POSIX.1-2008, present on every platform we build/test on.
       No _WIN32/gmtime_s branch — Windows isn't a target. */
    if (gmtime_r(&secs, &tm) != NULL) {
        /* The numeric strftime specifiers used here are POSIX locale-
           independent. strftime also sidesteps a -Wformat-truncation false
           positive a hand-rolled snprintf hits under -O2 (GCC sees tm_year
           as unbounded). */
        if (strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 20) {
            return;
        }
    }
    /* Unreachable post-clamp on a conforming libc; holds the contract if
       one misbehaves. */
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
    /* First probe at start_mono_ms (immediately). Monotonic value. */
    w->next_poll_at_mono_ms   = start_mono_ms;
    w->terminated             = false;
    w->removed_event_emitted  = false;
    w->bsd_unit               = bsd_unit;   /* -1 == no media */

    /* Session identity: two plain values. registry_id is the attachment
       identity (xnu mints non-reused IDs >= 2^32+256; 0 only from direct
       pure-layer callers). start_wall_ms rides every event as
       stream_open_wall_ms; the adapter monotonicizes it per process so
       (registry_id, stream_open_wall_ms) is unique per session even for
       same-ms reopens of the same drive. */
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
    /* Pull the next poll to "now" without reading the clock here: setting
       0 guarantees now >= next_poll_at_mono_ms on the next pump, whatever
       the monotonic clock's origin. */
    w->next_poll_at_mono_ms = 0;
}

/* Transition vs stable, for backoff selection. */
static bool watch_state_is_transitional(mos_state s)
{
    switch (s) {
        /* In-progress or degraded — poll fast to converge. */
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
    /* No default: a new mos_state value must trip -Wswitch so its poll
       class is chosen deliberately. This trailing return only covers an
       out-of-range value (the enum is int32-wide). */
    return false;
}

/* Build a base event: session identity, seq, ts (read fresh from
   ops->wall_ms, not the scheduling clock), bsd_unit. Caller fills the
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

/* Copy probe-result fields (all but prev_state) into an event. Shared by
   snapshot/state_changed/media_changed so a new field has one assignment
   site, not three. */
static void fill_event_state_fields(mos_watch_event *e,
                                    const mos_state_result *r,
                                    uint32_t latency_ms)
{
    e->state           = r->state;
    /* BSD unit from THIS probe, not the open-time value, so a disc in a
       drive empty at open (or a unit changed across eject/reinsert) shows
       in the event. fill_event_base seeds w->bsd_unit for events with no
       fresh probe (error / device_removed). */
    e->bsd_unit        = r->bsd_unit;
    e->current_profile = r->current_profile;
    e->vendor          = r->vendor;
    e->product         = r->product;
    e->revision        = r->revision;
    e->serial          = r->serial;   /* NULL until a free poll grabs it (mos_watch.c) */
    e->media_type      = r->media_type;  /* static token storage or NULL — no re-home */
    e->writable        = r->writable;    /* tri-state -1/0/1, plain scalar */
    e->sense_key       = r->sense_key;
    e->asc             = r->asc;
    e->ascq            = r->ascq;
    e->latency_ms      = latency_ms;
}

/* Saturating monotonic delta in ms: clamps end < start to 0 and a >49.7-day
   span to UINT32_MAX, either of which would otherwise underflow/truncate. */
static uint32_t mos_watch_latency_ms(uint64_t start, uint64_t end)
{
    uint64_t delta = end >= start ? end - start : 0;
    return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}

/* Per-state poll interval: transition_poll_ms for transitional states,
   stable_poll_ms otherwise. One site for the policy. */
static uint32_t poll_ms_for_state(const mos_watch_state *w,
                                  mos_state state)
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

    /* Terminal (notify_removed or a probe → NO_DEVICE): emit one final
       device_removed, then refuse further pumps. The removed_event_emitted
       sentinel guarantees exactly once, even if termination precedes any
       observation (prev_state then unknown). */
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

    /* Not yet time to probe → sleep. */
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

    /* NO_DEVICE means the drive went away under the watch: terminal
       removal. Set the flag and let the terminated path above emit
       device_removed. Without this, poll-only mode (no notifications) would
       spin emitting error events forever for an unplugged drive. */
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
        /* Other error: no observation this round. Non-terminal — emit error
           and reschedule. */
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_ERROR;
        d.event.error      = perr;
        d.event.state      = MOS_STATE_UNKNOWN;
        d.event.prev_state = w->have_last_state ? w->last_state : MOS_STATE_UNKNOWN;
        d.event.latency_ms = mos_watch_latency_ms(probe_start_mono, probe_end_mono);

        /* Don't update last_state — there's no observation, just its
           absence.

           Retry cadence: a first (or changed) error retries at transition
           rate; each further identical error doubles the interval, capped
           at stable_poll_ms. A persistent failure converges to the stable
           cadence instead of flooding; a notify_wake still pulls the next
           poll forward on a real event. */
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
    /* Adopt the probe's unit. Fresh-probe events carry r.bsd_unit directly,
       but the error/device_removed fallback reads w->bsd_unit, which must
       track the last OBSERVED unit (not open-time). The core owns this so
       pure-only behavior is correct without each adapter rediscovering it. */
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

    /* Same-state media swap → media_changed while the drive stays READY
       across two probes (a fast slot-load swap, or eject/reinsert between
       polls). Two independent signals:

       1. Registry-identity change — the whole-disk IOMedia entry ID re-mints
          on a physical swap even when the profile is unchanged (the
          same-profile DVD→DVD case). Strong signal; both ids must be
          non-zero (0 = unavailable, never inferred from).

       2. Profile change with NO usable identity — some USB-ATAPI bridges
          never expose a media_id (both 0). Any non-zero profile change
          fires: a different profile with no identity means a different disc.
          A same-PROFILE swap (DVD-R → DVD-R) is invisible here. */
    bool id_changed =
        r.media_id != 0 && w->last_media_id != 0 &&
        r.media_id != w->last_media_id;
    bool profile_changed_without_id =
        r.media_id == 0 && w->last_media_id == 0 &&
        r.current_profile != 0 && w->last_profile != 0 &&
        r.current_profile != w->last_profile;

    if (r.state == MOS_STATE_READY && w->last_state == MOS_STATE_READY &&
        (id_changed || profile_changed_without_id)) {
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

    /* No change → reschedule and sleep, no event.

       Adopt any *informative* identity first. media_id / current_profile
       unavailable (0) at the last event often arrive a probe later (the
       IOMedia child registers a beat after TUR goes GOOD; profile
       enrichment can fail transiently). A non-zero value is adopted, a zero
       never overwrites one, so the fingerprint survives the gap and a swap
       across it is still detected. Without this, a 0→non-zero arrival would
       pin the snapshot fingerprint and disarm swap detection for the
       session. */
    if (r.media_id != 0)        w->last_media_id = r.media_id;
    if (r.current_profile != 0) w->last_profile  = r.current_profile;
    w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
    d.kind                 = MOS_WATCH_DECIDE_SLEEP_UNTIL;
    d.next_poll_at_mono_ms = w->next_poll_at_mono_ms;
    return d;
}

/* ---- Watch-all multiplexer ----------------------------------------- *
 *
 * Contract in mos_pure.h. Slots are visited in ascending registry_id on
 * every entry, so same-tick interleave is deterministic and independent of
 * slot-assignment history. */

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

    /* Dedupe by attachment identity: DR Appeared can re-announce a device
       the snapshot already had (or fire twice on a bus rescan). Same id ⇒
       same slot; a replug gets a fresh id and a new slot. */
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

    /* Visit active slots in ascending registry_id (selection scan; the CAP is
       small, a sort would be ceremony). Returning on the first EMIT bounds per-call
       work; the next call re-enters at the lowest id, draining same-tick
       events in id order. */
    _Static_assert(MOS_WATCH_ALL_CAP <= 64, "visited bitmask is 64-wide");
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
            /* Relabel only the join's SNAPSHOT. A mid-stream device whose
               first pumps yield ERROR keeps its pending join, so its first
               successful probe still announces device_appeared; clearing on
               any first event would demote it (contract: every joining
               drive emits device_appeared). */
            if (a->join_pending[best] &&
                d.event.kind == MOS_EVENT_SNAPSHOT) {
                d.event.kind = MOS_EVENT_DEVICE_APPEARED;
                a->join_pending[best] = false;
            }
            if (d.event.kind == MOS_EVENT_DEVICE_REMOVED) {
                /* Per-slot, not stream-terminal: free after taking the
                   event. A replug arrives as a new id. */
                a->active[best] = false;
            }
            return d;
        }
        if (d.kind == MOS_WATCH_DECIDE_TERMINAL) {
            /* device_removed already emitted on an earlier call; this slot
               pumped again (external notify after emission). Just free it. */
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
