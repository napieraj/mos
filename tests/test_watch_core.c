/*
 * test_watch_core.c — Pure unit tests for the watch state machine.
 *
 * Drives mos_internal_watch_pump() with a fake mos_watch_ops_t whose
 * probe, mono_ms, and wall_ms callbacks are scripted by the fixture.
 * No IOKit, no real time, no real sleep — every scenario is a
 * deterministic sequence.
 *
 * Two-clock fixture: the fake separates the monotonic clock from the
 * wall clock. This is structural: the v0.3-dev tree had a bug where
 * the Apple-side adapter passed wall_clock_ms() as the scheduling
 * start_ms while wiring now_ms to monotonic uptime, producing a
 * first-poll deadline ~55 years in the future. The fix split the
 * vtable into mono_ms (scheduling) and wall_ms (timestamps); this
 * test fixture mirrors that split. The regression-prevention test
 * test_clock_domains_separate runs mono in the thousands and wall
 * in the trillions to pin the contract: the pump must never use
 * wall-clock values for scheduling decisions.
 *
 * Coverage targets:
 *   - First pump emits snapshot with correct prev_state=UNKNOWN
 *   - Two-clock separation: mono used for scheduling, wall for ts/sid
 *   - No-change pump returns SLEEP_UNTIL without emitting
 *   - State change emits state_changed with prev_state set correctly
 *   - Backoff: stable state schedules stable_poll_ms; transitional
 *     state schedules transition_poll_ms
 *   - notify_wake pulls next_poll_at_mono_ms forward to 0
 *   - notify_removed makes the next pump emit device_removed and
 *     the one after that TERMINAL without re-emitting
 *   - notify_removed BEFORE any snapshot still emits device_removed
 *     with prev_state=unknown (was a regression in v0.3-dev)
 *   - MOS_ERR_NO_DEVICE from a probe auto-terminates (was a regression:
 *     poll-only mode used to spin emitting error events forever)
 *   - Other probe errors emit error event without updating last_state,
 *     reschedule at transition rate
 *   - Sequence numbers are monotonic across event types
 *   - Stream ID is stable across the watch invocation
 *   - Latency is computed from probe entry to probe exit (mono)
 *   - RFC 3339 timestamp formatting works (sourced from wall_ms)
 */

#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ---- Fake clocks + probe ---------------------------------------------- *
 *
 * Two independent clock fields. Tests typically use the same value
 * for both, but the clock-domain regression test deliberately holds
 * them orders of magnitude apart. The auto_advance fields let
 * latency tests script elapsed probe time without breaking the
 * scheduling clock. */

typedef struct {
    /* Monotonic clock: scheduling and latency. */
    uint64_t  mono_clock_ms;
    uint64_t  mono_auto_advance_ms;
    int       mono_calls;

    /* Wall clock: stream_open_wall_ms and event ts. */
    uint64_t  wall_clock_ms;
    int       wall_calls;

    /* Probe responses, indexed by call. After probe_count, probe
       returns MOS_OK with state=READY (sentinel; tests assert before
       reaching this). */
    mos_state probe_state[8];
    mos_error      probe_err[8];
    uint64_t       probe_media_id[8];   /* whole-disk registry id per probe; 0 default */
    uint16_t       probe_profile[8];    /* current_profile per probe; 0 → fixture default */
    int            probe_count;
    int            probe_calls;

    /* When set, the probe drops the monotonic clock to 10 as a side
       effect — emulating a backward clock step between the pump's
       probe-start and probe-end reads (test_latency_saturates_on_
       backward_clock). Real CLOCK_MONOTONIC never does this; a buggy
       adapter's ops table could. */
    bool           flip_mono_on_probe;
} fake_watch_ctx;

static uint64_t fake_mono(void *vctx)
{
    fake_watch_ctx *c = (fake_watch_ctx *)vctx;
    uint64_t t = c->mono_clock_ms;
    c->mono_calls++;
    c->mono_clock_ms += c->mono_auto_advance_ms;
    return t;
}

static uint64_t fake_wall(void *vctx)
{
    fake_watch_ctx *c = (fake_watch_ctx *)vctx;
    c->wall_calls++;
    return c->wall_clock_ms;
}

static mos_error fake_probe(void *vctx, mos_state_result *out)
{
    fake_watch_ctx *c = (fake_watch_ctx *)vctx;
    if (c->flip_mono_on_probe) c->mono_clock_ms = 10;
    if (c->probe_count <= 0) {
        /* Fixture bug: a pump reached the probe with nothing scripted.
           Fail loudly rather than index probe_err[-1]. */
        fprintf(stderr, "fake_probe: probe_count == 0 (fixture misconfigured)\n");
        abort();
    }
    int idx = c->probe_calls < c->probe_count ? c->probe_calls : c->probe_count - 1;
    c->probe_calls++;

    if (c->probe_err[idx] != MOS_OK) {
        /* Seam contract E-1: out-params are UNDEFINED on error. Poison
           the result so any core read of an error-path field changes an
           assertion somewhere — the old leave-untouched behavior masked
           the obligation entirely (census B4). 0xEE fill makes state,
           unit, media_id, and profile all simultaneously garbage. */
        memset(out, 0xEE, sizeof(*out));
        return c->probe_err[idx];
    }

    memset(out, 0, sizeof(*out));
    out->state           = c->probe_state[idx];
    /* Realistic media identity: a drive with no media (open tray / empty)
       has no BSD disk node, so report -1; otherwise the media node is 4.
       The watch core now sources an event's bsd_unit from the probe, so
       this lets fixtures exercise media appearing and disappearing. */
    out->bsd_unit        = (out->state == MOS_STATE_OPEN ||
                            out->state == MOS_STATE_EMPTY) ? -1 : 4;
    out->media_id        = c->probe_media_id[idx];
    /* Profile: 0 in the fixture means "use the BD-ROM default"; the MMC
       reserved value 0xFFFF scripts a genuine 0 on the wire (transient
       enrichment failure), which 0 itself cannot express here. */
    out->current_profile = c->probe_profile[idx] == 0xFFFF ? 0
                         : c->probe_profile[idx] ? c->probe_profile[idx]
                         : 0x0040;
    return MOS_OK;
}

static const mos_watch_ops_t fake_ops = {
    .probe   = fake_probe,
    .mono_ms = fake_mono,
    .wall_ms = fake_wall,
};

/* Initialize the fake to "both clocks coincide" — convenient default
   for tests that don't care about the two-clock distinction. */
static void init_default(fake_watch_ctx *c, uint64_t t)
{
    memset(c, 0, sizeof(*c));
    c->mono_clock_ms = t;
    c->wall_clock_ms = t;
}

/* ---- Test cases ------------------------------------------------------- */

TEST(test_snapshot_emitted_on_first_pump)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242,
                            /*start_mono_ms=*/1000,
                            /*start_wall_ms=*/1000,
                            /*stable=*/2000, /*transition=*/200);

    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT,          d.event.kind);
    EXPECT_EQ(MOS_STATE_READY,             d.event.state);
    EXPECT_EQ(MOS_STATE_UNKNOWN,           d.event.prev_state);
    EXPECT_EQ(1,                           (int)d.event.seq);
    EXPECT_EQ(4242, (int)d.event.registry_id);
    EXPECT_EQ(1000, (int)d.event.stream_open_wall_ms);
    EXPECT_EQ(4,               d.event.bsd_unit);
    return 0;
}

TEST(test_zero_registry_id_passes_through)
{
    /* No adapter path produces token 0 (watch open fails closed if the
       registry ID can't be captured; xnu lazily assigns an ID to every
       attached entry, so a matchable service always has one). Direct
       pure-layer callers can pass 0, and it flows through as a plain
       value with no special case — unambiguous, because real registry
       IDs are >= 2^32+256 by kernel reservation. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_OPEN;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, -1, 0, 1000, 1000, 2000, 200);
    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(0,    (int)d.event.registry_id);
    EXPECT_EQ(1000, (int)d.event.stream_open_wall_ms);
    return 0;
}

TEST(test_empty_drive_yields_unit_minus_one_and_session_identity)
{
    /* Empty-drive contract in the pure watch core: initialized with
       bsd_unit == -1 (an empty/open-tray drive has no unit), the snapshot
       event's bsd_unit must be -1 — but session identity
       (registry_id + stream_open_wall_ms) is present and ordinary,
       because the registry-entry token exists precisely when the BSD
       name doesn't. (Watching an empty drive waiting for an insert is
       the primary watch use case; this was the original argument for
       retiring the BSD-derived composition.) The unit is event-time: it
       comes from the probe; session identity is open-time-stable. The
       companion test below pins that a later non-empty probe surfaces the
       fresh media unit rather than staying frozen at the open-time value. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_EMPTY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, -1, 4242,
                            /*start_mono_ms=*/1000,
                            /*start_wall_ms=*/1000,
                            /*stable=*/2000, /*transition=*/200);

    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT,          d.event.kind);
    EXPECT_EQ(MOS_STATE_EMPTY,             d.event.state);
    EXPECT_EQ(d.event.bsd_unit, -1);
    EXPECT_EQ(4242, (int)d.event.registry_id);
    EXPECT_EQ(1000, (int)d.event.stream_open_wall_ms);
    return 0;
}

TEST(test_media_appears_after_empty_open_uses_probe_unit)
{
    /* The reviews' release-blocking scenario, pinned in the pure core:
       a watch opened on an empty/open-tray drive (init unit -1) must
       surface the FRESH media unit once a disc appears, not stay frozen
       at the open-time -1. Session identity, by contrast, stays the stable
       open-time identity for the life of the stream. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_EMPTY;   /* empty at open -> unit -1 */
    c.probe_err[0]   = MOS_OK;
    c.probe_state[1] = MOS_STATE_READY;   /* disc inserted -> unit  4 */
    c.probe_err[1]   = MOS_OK;
    c.probe_count    = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, -1, 4242,
                            /*start_mono_ms=*/1000,
                            /*start_wall_ms=*/1000,
                            /*stable=*/2000, /*transition=*/200);

    /* Pump 1: empty snapshot — unit -1, watch-form stream id. */
    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);
    EXPECT_EQ(MOS_STATE_EMPTY,    d1.event.state);
    EXPECT_EQ(-1,                 (int)d1.event.bsd_unit);

    /* Media appears; advance well past the poll deadline and pump again. */
    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d2.kind);
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED,     d2.event.kind);
    EXPECT_EQ(MOS_STATE_READY,             d2.event.state);
    EXPECT_EQ(MOS_STATE_EMPTY,             d2.event.prev_state);
    /* The fix: the event carries the probe's fresh media unit, not the
       frozen open-time -1. */
    EXPECT_EQ(4, (int)d2.event.bsd_unit);
    /* Session identity remains stable at its open-time values. */
    EXPECT_EQ(4242, (int)d2.event.registry_id);
    EXPECT_EQ(1000, (int)d2.event.stream_open_wall_ms);
    return 0;
}

TEST(test_media_changed_on_same_state_ready_swap)
{
    /* Same-state swap: the drive stays READY across probes but the disc is physically
       replaced — the whole-disk IOMedia registry id changes. The core must
       emit media_changed (not state_changed, since state is unchanged), and
       a same-disc poll in between must emit nothing. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0x1111;
    c.probe_state[1] = MOS_STATE_READY; c.probe_media_id[1] = 0x1111; /* same disc */
    c.probe_state[2] = MOS_STATE_READY; c.probe_media_id[2] = 0x2222; /* swapped */
    c.probe_err[0] = c.probe_err[1] = c.probe_err[2] = MOS_OK;
    c.probe_count = 3;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);
    EXPECT_EQ(MOS_STATE_READY,    d1.event.state);

    c.mono_clock_ms = 100000;                 /* same disc, same state */
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d2.kind);

    c.mono_clock_ms = 200000;                 /* disc swapped, still READY */
    mos_watch_decision d3 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d3.kind);
    EXPECT_EQ(MOS_EVENT_MEDIA_CHANGED,     d3.event.kind);
    EXPECT_EQ(MOS_STATE_READY,             d3.event.state);
    EXPECT_EQ(MOS_STATE_READY,             d3.event.prev_state);
    return 0;
}

TEST(test_no_media_changed_when_id_unavailable)
{
    /* media_id == 0 is the "unavailable" sentinel (a bridge exposing no
       stable IOMedia registry id). The core must never infer a swap from
       an unknown id — no spurious media_changed. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0;
    c.probe_state[1] = MOS_STATE_READY; c.probe_media_id[1] = 0;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);

    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d2.kind);
    return 0;
}

TEST(test_media_changed_on_profile_class_swap_without_id)
{
    /* Finding 3: a bridge that never exposes a stable media_id (both 0) still
       reveals a cross-class swap through a changed non-zero current_profile.
       CD-ROM (0x08) → DVD-ROM (0x10), staying READY, must emit media_changed
       via the last_profile fallback fingerprint. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0; c.probe_profile[0] = 0x0008;
    c.probe_state[1] = MOS_STATE_READY; c.probe_media_id[1] = 0; c.probe_profile[1] = 0x0010;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);

    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d2.kind);
    EXPECT_EQ(MOS_EVENT_MEDIA_CHANGED,     d2.event.kind);
    EXPECT_EQ(MOS_STATE_READY,             d2.event.state);
    EXPECT_EQ(MOS_STATE_READY,             d2.event.prev_state);
    return 0;
}

TEST(test_media_changed_on_same_class_profile_swap_without_id)
{
    /* Same class, different profile (DVD-ROM 0x10 → DVD-R 0x11): fires. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0; c.probe_profile[0] = 0x0010;
    c.probe_state[1] = MOS_STATE_READY; c.probe_media_id[1] = 0; c.probe_profile[1] = 0x0011;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    (void)mos_internal_watch_pump(&w);                 /* snapshot */
    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d2.kind);
    EXPECT_EQ(MOS_EVENT_MEDIA_CHANGED,     d2.event.kind);
    return 0;
}

TEST(test_no_media_changed_on_same_profile_without_id)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0; c.probe_profile[0] = 0x0010;
    c.probe_state[1] = MOS_STATE_READY; c.probe_media_id[1] = 0; c.probe_profile[1] = 0x0010;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    (void)mos_internal_watch_pump(&w);                 /* snapshot */
    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d2.kind); /* no event */
    return 0;
}

TEST(test_media_changed_after_late_media_id)
{
    /* REGRESSION FIXTURE (fingerprint staleness): the whole-disk IOMedia
       child often registers a beat after TUR goes GOOD, so the snapshot can
       land READY with media_id == 0 and the id arrives on the next probe
       with no event in between. Pre-fix, the no-change pump never refreshed
       the fingerprint, so last_media_id stayed 0 for the session: id_changed
       requires both ids non-zero and the profile fallback requires
       r.media_id == 0 — a later physical swap was undetectable. The id
       arriving late must be adopted silently (no fabricated event), and the
       swap after it must emit media_changed. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0;       /* id not yet registered */
    c.probe_state[1] = MOS_STATE_READY; c.probe_media_id[1] = 0x1111;  /* id arrives, same disc */
    c.probe_state[2] = MOS_STATE_READY; c.probe_media_id[2] = 0x2222;  /* physical swap */
    c.probe_err[0] = c.probe_err[1] = c.probe_err[2] = MOS_OK;
    c.probe_count = 3;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);

    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d2.kind);  /* adopt id, no event */

    c.mono_clock_ms = 200000;
    mos_watch_decision d3 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d3.kind);
    EXPECT_EQ(MOS_EVENT_MEDIA_CHANGED,     d3.event.kind);
    EXPECT_EQ(MOS_STATE_READY,             d3.event.state);
    EXPECT_EQ(MOS_STATE_READY,             d3.event.prev_state);
    return 0;
}

TEST(test_media_changed_across_transient_zero_id)
{
    /* A known identity is KEPT across an unavailability gap (zero never
       overwrites non-zero). A swap whose new id first appears after a
       transient id == 0 probe must still compare against the pre-gap
       identity and emit media_changed — overwriting the fingerprint with 0
       would silently miss the swap forever. The gap probe itself must not
       fabricate an event. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0x1111;  /* disc A */
    c.probe_state[1] = MOS_STATE_READY; c.probe_media_id[1] = 0;       /* id transiently gone */
    c.probe_state[2] = MOS_STATE_READY; c.probe_media_id[2] = 0x2222;  /* disc B visible */
    c.probe_err[0] = c.probe_err[1] = c.probe_err[2] = MOS_OK;
    c.probe_count = 3;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);

    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d2.kind);  /* gap: keep 0x1111 */

    c.mono_clock_ms = 200000;
    mos_watch_decision d3 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d3.kind);
    EXPECT_EQ(MOS_EVENT_MEDIA_CHANGED,     d3.event.kind);
    return 0;
}

TEST(test_profile_fallback_after_late_profile)
{
    /* The profile-fallback twin of the late-id case, on a bridge that never
       exposes a media_id: enrichment fails transiently at snapshot time
       (profile 0 on the wire), the real profile arrives on the next probe
       (adopted silently), and a later cross-class swap must fire the
       last_profile fallback. Pre-fix, last_profile stayed 0 and the fallback
       (which requires both profiles non-zero) was disarmed for the session. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0; c.probe_profile[0] = 0xFFFF; /* wire 0 */
    c.probe_state[1] = MOS_STATE_READY; c.probe_media_id[1] = 0; c.probe_profile[1] = 0x0008;
    c.probe_state[2] = MOS_STATE_READY; c.probe_media_id[2] = 0; c.probe_profile[2] = 0x0010;
    c.probe_err[0] = c.probe_err[1] = c.probe_err[2] = MOS_OK;
    c.probe_count = 3;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);

    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d2.kind);  /* adopt 0x0008, no event */

    c.mono_clock_ms = 200000;
    mos_watch_decision d3 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d3.kind);
    EXPECT_EQ(MOS_EVENT_MEDIA_CHANGED,     d3.event.kind);
    return 0;
}

TEST(test_poll_class_pinned_for_every_state)
{
    /* Finding 2: pin the poll class of EVERY mos_state value, so a new
       state can't silently inherit "stable." The snapshot pump schedules the
       next poll by the observed state's class; with the mono clock not
       auto-advancing, the deadline is start + (transition|stable)_poll_ms. */
    const uint64_t STABLE = 2000, TRANS = 200, START = 1000;
    struct { mos_state s; bool transitional; } cases[] = {
        { MOS_STATE_OPEN,             false },
        { MOS_STATE_EMPTY,            false },
        { MOS_STATE_LOADING,          true  },
        { MOS_STATE_READY,            false },
        { MOS_STATE_BUSY,             true  },
        { MOS_STATE_FORMATTING,       true  },
        { MOS_STATE_MEDIA_UNREADABLE, false },
        { MOS_STATE_DEVICE_FAULT,     false },
        { MOS_STATE_EMPTY_OR_OPEN,    true  },
        { MOS_STATE_UNKNOWN,          true  },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fake_watch_ctx c;
        init_default(&c, START);          /* mono_auto_advance_ms = 0 */
        c.probe_state[0] = cases[i].s;
        c.probe_err[0]   = MOS_OK;
        c.probe_count    = 1;

        mos_watch_state w;
        mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, START, START, STABLE, TRANS);
        (void)mos_internal_watch_pump(&w);   /* snapshot schedules by state */

        uint64_t want = START + (cases[i].transitional ? TRANS : STABLE);
        EXPECT_EQ(w.next_poll_at_mono_ms, want);
    }
    return 0;
}

TEST(test_state_change_takes_precedence_over_media_changed)
{
    /* When both the state and the media id change, it is a state
       transition, not a same-state swap: the state-change branch is
       evaluated first. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY; c.probe_media_id[0] = 0x1111;
    c.probe_state[1] = MOS_STATE_BUSY;  c.probe_media_id[1] = 0x2222;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);

    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED, d2.event.kind);
    EXPECT_EQ(MOS_STATE_BUSY,          d2.event.state);
    return 0;
}

/* REGRESSION FIXTURE: the two clocks are in different numeric
   domains by 10+ orders of magnitude. Pre-fix, this would have
   put next_poll_at_ms in the trillions (from wall_clock_ms()) but
   now_ms in the thousands (from monotonic_ms()), and the first
   pump would return SLEEP_UNTIL with a deadline ~55 years out.
   With the fix, the scheduling start_mono_ms == ops->mono_ms so
   the first poll fires immediately. */
TEST(test_clock_domains_separate)
{
    fake_watch_ctx c;
    memset(&c, 0, sizeof(c));
    /* Monotonic: uptime ms, ~ thousands. */
    c.mono_clock_ms = 12345;
    /* Wall: Unix-epoch ms, ~ trillions (2025-05-06). */
    c.wall_clock_ms = 1746526503000ULL;
    c.probe_state[0] = MOS_STATE_EMPTY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    /* The init call exercises the exact production shape:
       start_mono_ms from monotonic_ms(), start_wall_ms from
       wall_clock_ms(). The numbers are deliberately implausible
       as a unified clock to keep the contract obvious. */
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242,
                            /*start_mono_ms=*/12345,
                            /*start_wall_ms=*/1746526503000ULL,
                            2000, 200);

    mos_watch_decision d = mos_internal_watch_pump(&w);
    /* MUST be EMIT_EVENT with a snapshot. If this is SLEEP_UNTIL,
       the time-domain regression has come back: scheduling is
       reading wall-clock values somewhere it shouldn't. */
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT,          d.event.kind);
    EXPECT_EQ(MOS_STATE_EMPTY,             d.event.state);
    /* stream_open MUST tag the wall-clock epoch ms, not the
       monotonic value. */
    EXPECT_EQ((long long)1746526503000LL,
              (long long)d.event.stream_open_wall_ms);
    /* ts MUST be derived from wall_clock_ms, not monotonic.
       1746526503000 ms → 2025-05-06T10:15:03Z. */
    EXPECT_STREQ("2025-05-06T10:15:03Z", d.event.ts);
    /* next_poll deadline MUST be in the monotonic domain. The
       fake didn't advance mono past 12345, so the next poll
       is 12345 + stable_poll_ms (2000) = 14345. If this comes
       back as a trillion-shaped number, scheduling is reading
       the wrong clock. */
    EXPECT_EQ(14345, (int)w.next_poll_at_mono_ms);
    return 0;
}

TEST(test_no_change_pump_returns_sleep_no_event)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_state[1] = MOS_STATE_READY;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count    = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    /* Pump 1: snapshot */
    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d1.kind);

    /* Advance mono clock to next poll deadline */
    c.mono_clock_ms = 3000; /* 1000 + 2000 stable_poll */

    /* Pump 2: same state → no event, sleep until next */
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d2.kind);
    EXPECT_EQ(5000, (int)d2.next_poll_at_mono_ms); /* 3000 + 2000 stable */
    return 0;
}

TEST(test_state_change_emits_delta_with_prev_state)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_EMPTY;
    c.probe_state[1] = MOS_STATE_LOADING;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count    = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    /* Pump 1: snapshot empty */
    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);
    EXPECT_EQ(MOS_STATE_EMPTY,    d1.event.state);

    /* Advance mono past stable poll (empty is stable: 2000ms) */
    c.mono_clock_ms = 3001;

    /* Pump 2: state changed empty → loading */
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d2.kind);
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED,     d2.event.kind);
    EXPECT_EQ(MOS_STATE_LOADING,           d2.event.state);
    EXPECT_EQ(MOS_STATE_EMPTY,             d2.event.prev_state);
    EXPECT_EQ(2, (int)d2.event.seq);
    return 0;
}

TEST(test_transitional_state_uses_short_backoff)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_LOADING;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    /* Next poll at probe_end + transition_poll_ms.
       probe_end == 1000 (fake mono didn't advance), so 1200. */
    EXPECT_EQ(1200, (int)w.next_poll_at_mono_ms);
    return 0;
}

TEST(test_stable_state_uses_long_backoff)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    /* Next poll at probe_end + stable_poll_ms = 1000 + 2000 = 3000 */
    EXPECT_EQ(3000, (int)w.next_poll_at_mono_ms);
    return 0;
}

TEST(test_notify_wake_pulls_next_poll_forward)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_state[1] = MOS_STATE_OPEN;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count    = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    /* Pump 1: snapshot ready, schedules next poll for 3000 */
    mos_internal_watch_pump(&w);
    EXPECT_EQ(3000, (int)w.next_poll_at_mono_ms);

    /* Notify wake at mono=1500 (well before 3000) */
    c.mono_clock_ms = 1500;
    mos_internal_watch_notify_wake(&w);
    EXPECT_EQ(0, (int)w.next_poll_at_mono_ms);

    /* Pump 2: probe should run immediately (1500 >= 0), emit state_changed */
    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED,     d.event.kind);
    EXPECT_EQ(MOS_STATE_OPEN,              d.event.state);
    return 0;
}

TEST(test_notify_removed_emits_device_removed_then_terminal)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    /* Pump 1: snapshot */
    mos_internal_watch_pump(&w);

    /* Notify removed */
    mos_internal_watch_notify_removed(&w);

    /* Pump 2: emit device_removed with prev_state=ready */
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d2.kind);
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED,    d2.event.kind);
    EXPECT_EQ(MOS_STATE_READY,             d2.event.prev_state);
    EXPECT_EQ(2,                           (int)d2.event.seq);

    /* Pump 3: terminal, no event */
    mos_watch_decision d3 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_TERMINAL, d3.kind);

    /* Pump 4: still terminal, no event (idempotent) */
    mos_watch_decision d4 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_TERMINAL, d4.kind);
    return 0;
}

/* REGRESSION FIXTURE: termination before any successful snapshot.
   Pre-v0.3-dev, the device_removed event was gated on have_last_state
   being true, so a removal that fired before the first probe was
   silently dropped — TERMINAL with no terminal event. The schema
   contract promises a device_removed event closes every stream. */
TEST(test_notify_removed_before_snapshot_still_emits_removed)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    /* No probe configured; we never call probe. */
    c.probe_count = 0;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    /* Removal fires immediately, before any pump probes. */
    mos_internal_watch_notify_removed(&w);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d1.kind);
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED,    d1.event.kind);
    /* prev_state is unknown because no successful probe ever happened. */
    EXPECT_EQ(MOS_STATE_UNKNOWN,           d1.event.prev_state);
    EXPECT_EQ(1,                           (int)d1.event.seq);

    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_TERMINAL, d2.kind);
    return 0;
}

/* REGRESSION FIXTURE: probe → MOS_ERR_NO_DEVICE is terminal.
   Pre-v0.3-dev, this was treated as a transient error and the loop
   spun emitting MOS_EVENT_ERROR every transition_poll_ms when the
   drive was unplugged in poll-only mode (no kIOGeneralInterest
   notification registered). The fix routes NO_DEVICE through the
   terminal path with a device_removed event. */
TEST(test_probe_no_device_terminates_with_removed_event)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_state[1] = MOS_STATE_UNKNOWN;
    c.probe_err[0]   = MOS_OK;
    c.probe_err[1]   = MOS_ERR_NO_DEVICE;
    c.probe_count    = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    /* Pump 1: snapshot ready. */
    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);

    /* Advance past stable poll. */
    c.mono_clock_ms = 3001;

    /* Pump 2: probe returns NO_DEVICE → must produce device_removed,
       not a recurring error event. */
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d2.kind);
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED,    d2.event.kind);
    EXPECT_EQ(MOS_STATE_READY,             d2.event.prev_state);

    /* Pump 3: terminal, no further events. */
    mos_watch_decision d3 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_TERMINAL, d3.kind);
    return 0;
}

TEST(test_probe_error_emits_error_event_no_state_update)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_state[1] = MOS_STATE_UNKNOWN;
    c.probe_err[0]   = MOS_OK;
    c.probe_err[1]   = MOS_ERR_IO;  /* IO is non-terminal */
    c.probe_count    = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    /* Pump 1: snapshot ready */
    mos_internal_watch_pump(&w);

    /* Advance, pump 2: probe fails with MOS_ERR_IO */
    c.mono_clock_ms = 3001;
    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_ERROR,             d.event.kind);
    EXPECT_EQ(MOS_ERR_IO,                  (int)d.event.error);
    EXPECT_EQ(MOS_STATE_READY,             d.event.prev_state);
    /* last_state should NOT be updated on error */
    EXPECT_EQ(MOS_STATE_READY,             w.last_state);
    /* Should reschedule at transition rate (200) not stable (2000) */
    EXPECT_EQ(3201, (int)w.next_poll_at_mono_ms);
    return 0;
}

TEST(test_error_event_unit_reflects_last_observed_not_open_time)
{
    /* Third review, finding 3 (layering): error/device_removed events have
       no fresh probe, so fill_event_base falls back to the core's own
       bsd_unit — which must be the last OBSERVED unit, adopted by the core
       itself on successful probes. Before the fix, only the Apple adapter
       kept it current (a direct w->core.bsd_unit write), so in a pure-only
       context — i.e., for any second adapter — this sequence reported the
       open-time -1 on the error event. Sequence: open empty (unit -1),
       media appears (unit 4), then a probe error. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_EMPTY;  c.probe_err[0] = MOS_OK;
    c.probe_state[1] = MOS_STATE_READY;  c.probe_err[1] = MOS_OK;
    c.probe_state[2] = MOS_STATE_UNKNOWN; c.probe_err[2] = MOS_ERR_IO;
    c.probe_count = 3;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, -1, 4242, 1000, 1000, 2000, 200);

    c.mono_clock_ms = 1000;
    mos_watch_decision d1 = mos_internal_watch_pump(&w);   /* snapshot, -1 */
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d1.event.kind);
    EXPECT_EQ(-1, (int)d1.event.bsd_unit);

    c.mono_clock_ms = 100000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);   /* ready, 4 */
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED, d2.event.kind);
    EXPECT_EQ(4, (int)d2.event.bsd_unit);

    c.mono_clock_ms = 200000;
    mos_watch_decision d3 = mos_internal_watch_pump(&w);   /* error */
    EXPECT_EQ(MOS_EVENT_ERROR, d3.event.kind);
    /* THE assertion: the fallback unit is the last observed (4), not the
       open-time -1. Fails against the pre-fix core. */
    EXPECT_EQ(4, (int)d3.event.bsd_unit);
    return 0;
}

TEST(test_error_backoff_escalates_and_caps)
{
    /* consecutive identical probe errors double the retry interval
       from transition_poll_ms (200) toward stable_poll_ms (2000):
       200, 400, 800, 1600, 2000 (capped), 2000, ... Every pump still
       emits an error event — only the spacing grows. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_UNKNOWN;
    c.probe_err[0]   = MOS_ERR_BUSY;      /* repeats: fake clamps to last */
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    static const uint64_t want[6] = { 200, 400, 800, 1600, 2000, 2000 };
    uint64_t now = 1000;
    for (int i = 0; i < 6; i++) {
        c.mono_clock_ms = now;
        mos_watch_decision d = mos_internal_watch_pump(&w);
        EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
        EXPECT_EQ(MOS_EVENT_ERROR,             d.event.kind);
        EXPECT_EQ((int)(i + 1),                (int)d.event.seq);
        EXPECT_EQ((long long)(now + want[i]),
                  (long long)w.next_poll_at_mono_ms);
        now += 100000;                    /* well past any deadline */
    }
    return 0;
}

TEST(test_error_backoff_resets_on_success)
{
    /* A successful observation ends the streak: the next error after a
       snapshot reschedules at the prompt transition rate again. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_UNKNOWN; c.probe_err[0] = MOS_ERR_BUSY;
    c.probe_state[1] = MOS_STATE_UNKNOWN; c.probe_err[1] = MOS_ERR_BUSY;
    c.probe_state[2] = MOS_STATE_READY;   c.probe_err[2] = MOS_OK;
    c.probe_state[3] = MOS_STATE_UNKNOWN; c.probe_err[3] = MOS_ERR_BUSY;
    c.probe_count = 4;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    c.mono_clock_ms = 1000;
    (void)mos_internal_watch_pump(&w);                          /* err #1 */
    EXPECT_EQ(1200, (int)w.next_poll_at_mono_ms);
    c.mono_clock_ms = 100000;
    (void)mos_internal_watch_pump(&w);                          /* err #2 */
    EXPECT_EQ(100400, (int)w.next_poll_at_mono_ms);             /* doubled */
    c.mono_clock_ms = 200000;
    mos_watch_decision d3 = mos_internal_watch_pump(&w);        /* snapshot */
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d3.event.kind);
    c.mono_clock_ms = 300000;
    mos_watch_decision d4 = mos_internal_watch_pump(&w);        /* err again */
    EXPECT_EQ(MOS_EVENT_ERROR, d4.event.kind);
    EXPECT_EQ(300200, (int)w.next_poll_at_mono_ms);             /* reset to 200 */
    return 0;
}

TEST(test_error_backoff_resets_on_different_error)
{
    /* A different error code is a new situation, not a continuation:
       the escalation restarts at the transition rate. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_UNKNOWN; c.probe_err[0] = MOS_ERR_BUSY;
    c.probe_state[1] = MOS_STATE_UNKNOWN; c.probe_err[1] = MOS_ERR_BUSY;
    c.probe_state[2] = MOS_STATE_UNKNOWN; c.probe_err[2] = MOS_ERR_IO;
    c.probe_count = 3;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    c.mono_clock_ms = 1000;
    (void)mos_internal_watch_pump(&w);                          /* BUSY #1 */
    EXPECT_EQ(1200, (int)w.next_poll_at_mono_ms);
    c.mono_clock_ms = 100000;
    (void)mos_internal_watch_pump(&w);                          /* BUSY #2 */
    EXPECT_EQ(100400, (int)w.next_poll_at_mono_ms);
    c.mono_clock_ms = 200000;
    mos_watch_decision d3 = mos_internal_watch_pump(&w);        /* IO */
    EXPECT_EQ(MOS_ERR_IO, (int)d3.event.error);
    EXPECT_EQ(200200, (int)w.next_poll_at_mono_ms);             /* reset */
    return 0;
}

TEST(test_seq_monotonic_across_event_types)
{
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_EMPTY;
    c.probe_state[1] = MOS_STATE_UNKNOWN; /* probe will fail */
    c.probe_state[2] = MOS_STATE_LOADING;
    c.probe_err[0]   = MOS_OK;
    c.probe_err[1]   = MOS_ERR_TIMEOUT;
    c.probe_err[2]   = MOS_OK;
    c.probe_count    = 3;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    EXPECT_EQ(1, (int)d1.event.seq);

    c.mono_clock_ms = 3001;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_ERROR, d2.event.kind);
    EXPECT_EQ(2,               (int)d2.event.seq);

    c.mono_clock_ms = 4000;
    mos_watch_decision d3 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED, d3.event.kind);
    EXPECT_EQ(3,                       (int)d3.event.seq);

    mos_internal_watch_notify_removed(&w);
    mos_watch_decision d4 = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED, d4.event.kind);
    EXPECT_EQ(4,                        (int)d4.event.seq);
    return 0;
}

TEST(test_latency_ms_measures_probe_duration)
{
    fake_watch_ctx c;
    init_default(&c, 5000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 5000, 5000, 2000, 200);

    /* Auto-advance the MONO clock by 50 ms on each fake_mono call.
       Wall clock stays constant. Sequence during a snapshot pump:
         1. Pre-probe time check (returns 5000, advances to 5050)
         2. probe_start was just captured
         3. probe_end after probe runs (returns 5050, advances to 5100)
       So latency = 5050 - 5000 = 50. */
    c.mono_auto_advance_ms = 50;

    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d.event.kind);
    EXPECT_EQ(50, (int)d.event.latency_ms);
    return 0;
}

TEST(test_ts_saturates_at_year_9999_boundary)
{
    /* Fourth review, finding 1 (the real defect): the clock is an input,
       and the schema requires a 4-digit year. Boundary-valid, boundary+1s,
       and absurd values must all yield schema-shaped ts — the last two by
       saturating to 9999-12-31T23:59:59Z, never a 5-digit year (strftime
       returns 21 chars for those) and never "". */
    static const struct { uint64_t wall; const char *want; } cases[] = {
        { 253402300799000ULL, "9999-12-31T23:59:59Z" },  /* exactly max   */
        { 253402300800000ULL, "9999-12-31T23:59:59Z" },  /* +1s, clamped  */
        { UINT64_MAX / 2,     "9999-12-31T23:59:59Z" },  /* absurd        */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fake_watch_ctx c;
        init_default(&c, 1000);
        c.wall_clock_ms  = cases[i].wall;
        c.probe_state[0] = MOS_STATE_EMPTY;
        c.probe_err[0]   = MOS_OK;
        c.probe_count    = 1;
        mos_watch_state w;
        mos_internal_watch_init(&w, &fake_ops, &c, -1, 4242,
                                1000, cases[i].wall, 2000, 200);
        mos_watch_decision d = mos_internal_watch_pump(&w);
        EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
        EXPECT_STREQ(cases[i].want, d.event.ts);
    }
    return 0;
}

TEST(test_latency_saturates_on_backward_clock)
{
    /* Fourth review, finding 8 (donated regression test, adapted to house
       style): the latency clamp (end < start -> 0) was live but
       unexercised — the mutation campaign deleted it and the suite stayed
       green, because every latency fixture used a forward clock. A
       backward step between probe-start and probe-end must clamp to 0,
       not underflow to a ~49-day wrapped value. Verified to kill the
       raw-subtraction mutant. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;
    /* First mono read (probe start) = 1000 via init_default; step the
       clock BACKWARD before the pump's end-read by pre-positioning: the
       fake returns mono_clock_ms on every call, so set it low and init
       the watch with a HIGHER start... the pump reads start and end
       around the probe from the same fake value. Instead, drive the
       backward step with the auto-advance: negative steps aren't
       expressible, so emulate by giving the pump a start deadline in the
       past and flipping the clock between reads via probe side effect. */
    c.flip_mono_on_probe    = true;   /* probe drops the clock to 10 */
    c.mono_clock_ms         = 1000;
    /* Seam audit C-3: guard the flip itself. With a forward auto-advance,
       a silently no-op flip yields latency == advance (non-zero) and the
       0-assertion below FAILS — so the backward-step mechanism cannot rot
       into a static clock that passes for the wrong reason. */
    c.mono_auto_advance_ms  = 7;
    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242, 1000, 1000, 2000, 200);
    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d.event.kind);
    EXPECT_EQ(0, (int)d.event.latency_ms);
    return 0;
}

TEST(test_session_identity_stable_across_pumps_uses_wall_ms)
{
    fake_watch_ctx c;
    memset(&c, 0, sizeof(c));
    c.mono_clock_ms = 1000;
    /* Wall ms recorded as stream_open — deliberately a real-looking
       epoch value, distinct from the mono value, to verify the
       stream_open comes from the wall-clock source. */
    c.wall_clock_ms = 1746526503000ULL;
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_state[1] = MOS_STATE_OPEN;
    c.probe_err[0] = c.probe_err[1] = MOS_OK;
    c.probe_count    = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242,
                            /*start_mono_ms=*/1000,
                            /*start_wall_ms=*/1746526503000ULL,
                            2000, 200);

    mos_watch_decision d1 = mos_internal_watch_pump(&w);
    c.mono_clock_ms += 3000;
    mos_watch_decision d2 = mos_internal_watch_pump(&w);

    /* Session identity is constant across events of one stream... */
    EXPECT_EQ((long long)d1.event.registry_id,
              (long long)d2.event.registry_id);
    EXPECT_EQ((long long)d1.event.stream_open_wall_ms,
              (long long)d2.event.stream_open_wall_ms);
    /* ...and stream_open MUST carry the wall-clock value, not the
       monotonic 1000. */
    EXPECT_EQ((long long)1746526503000LL,
              (long long)d1.event.stream_open_wall_ms);
    EXPECT_EQ(4242, (int)d1.event.registry_id);
    return 0;
}

TEST(test_rfc3339_format_uses_wall_ms)
{
    fake_watch_ctx c;
    memset(&c, 0, sizeof(c));
    c.mono_clock_ms = 1000;
    /* 2026-05-01T00:00:00Z = 1777593600 unix seconds = 1777593600000 ms.
       Distinct from mono so the test pins that ts comes from wall_ms. */
    c.wall_clock_ms  = 1777593600000ULL;
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242,
                            1000, 1777593600000ULL, 2000, 200);

    mos_watch_decision d = mos_internal_watch_pump(&w);
    /* ts MUST be formatted from the wall clock, not the monotonic
       1000 (which would render as 1970-01-01T00:00:01Z). */
    EXPECT_STREQ("2026-05-01T00:00:00Z", d.event.ts);
    return 0;
}

/* ---- Suite registration ----------------------------------------------- */


/* ---- Watch-all multiplexer (DR pivot Phase 2b) --------------------- *
 *
 * The multiplexer is pure fan-in: these tests pin the four properties
 * the adapter relies on — deterministic ascending-registry_id
 * interleave, stream-global seq, mid-stream join relabeling
 * (snapshot → device_appeared), and per-slot (non-terminal) removal.
 * Each slot core gets its own fake ctx/clock; the multiplexer itself
 * never touches a clock. */

TEST(all_empty_stream_sleeps_unbounded)
{
    mos_watch_all_state a;
    mos_internal_watch_all_init(&a);

    mos_watch_decision d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d.kind);
    EXPECT(d.next_poll_at_mono_ms == UINT64_MAX);
    return 0;
}

TEST(all_interleaves_ascending_registry_id_with_global_seq)
{
    fake_watch_ctx c1, c2;
    init_default(&c1, 1000);
    init_default(&c2, 1000);
    c1.probe_state[0] = MOS_STATE_READY; c1.probe_err[0] = MOS_OK; c1.probe_count = 1;
    c2.probe_state[0] = MOS_STATE_EMPTY; c2.probe_err[0] = MOS_OK; c2.probe_count = 1;

    mos_watch_all_state a;
    mos_internal_watch_all_init(&a);
    /* Add in DESCENDING id order: emission order must still ascend. */
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c2, -1, 200,
                                      1000, 1000, 2000, 200, false) >= 0);
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c1, 4, 100,
                                      1000, 1000, 2000, 200, false) >= 0);

    mos_watch_decision d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d.event.kind);
    EXPECT_EQ(100, (int)d.event.registry_id);
    EXPECT_EQ(1,   (int)d.event.seq);

    d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d.event.kind);
    EXPECT_EQ(200, (int)d.event.registry_id);
    EXPECT_EQ(2,   (int)d.event.seq);

    /* Drained: both stable → sleep until the earlier core deadline. */
    d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d.kind);
    EXPECT(d.next_poll_at_mono_ms != UINT64_MAX);
    return 0;
}

TEST(all_mid_stream_join_relabels_first_event_only)
{
    fake_watch_ctx c1, c2;
    init_default(&c1, 1000);
    c1.probe_state[0] = MOS_STATE_READY; c1.probe_err[0] = MOS_OK; c1.probe_count = 1;

    mos_watch_all_state a;
    mos_internal_watch_all_init(&a);
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c1, 4, 100,
                                      1000, 1000, 2000, 200, false) >= 0);

    mos_watch_decision d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d.event.kind);

    /* Hot-plug at t=1500: the join's first event is device_appeared,
       carrying the snapshot payload. */
    init_default(&c2, 1500);
    c2.probe_state[0] = MOS_STATE_OPEN;  c2.probe_err[0] = MOS_OK;
    c2.probe_state[1] = MOS_STATE_EMPTY; c2.probe_err[1] = MOS_OK;
    c2.probe_count = 2;
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c2, -1, 200,
                                      1500, 1500, 2000, 200, true) >= 0);

    d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_DEVICE_APPEARED, d.event.kind);
    EXPECT_EQ(200, (int)d.event.registry_id);
    EXPECT_EQ(MOS_STATE_OPEN, d.event.state);
    EXPECT_EQ(2, (int)d.event.seq);

    /* Relabel is first-event-only: the join's next transition is a
       normal state_changed. */
    mos_internal_watch_notify_wake(&a.cores[
        mos_internal_watch_all_find(&a, 200)]);
    d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED, d.event.kind);
    EXPECT_EQ(200, (int)d.event.registry_id);
    EXPECT_EQ(3, (int)d.event.seq);
    return 0;
}

TEST(all_device_removed_is_per_slot_not_terminal)
{
    fake_watch_ctx c1, c2;
    init_default(&c1, 1000);
    init_default(&c2, 1000);
    /* Core 100's first probe says the device is gone. */
    c1.probe_err[0]   = MOS_ERR_NO_DEVICE;
    c1.probe_state[0] = MOS_STATE_UNKNOWN;
    c1.probe_count    = 1;
    c2.probe_state[0] = MOS_STATE_READY; c2.probe_err[0] = MOS_OK;
    c2.probe_count    = 1;

    mos_watch_all_state a;
    mos_internal_watch_all_init(&a);
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c1, 4, 100,
                                      1000, 1000, 2000, 200, false) >= 0);
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c2, 5, 200,
                                      1000, 1000, 2000, 200, false) >= 0);

    mos_watch_decision d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED, d.event.kind);
    EXPECT_EQ(100, (int)d.event.registry_id);
    EXPECT_EQ(1, (int)d.event.seq);

    /* The slot is freed, the stream continues with the other drive —
       never TERMINAL. */
    EXPECT_EQ(-1, mos_internal_watch_all_find(&a, 100));
    d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d.event.kind);
    EXPECT_EQ(200, (int)d.event.registry_id);
    EXPECT_EQ(2, (int)d.event.seq);
    return 0;
}

TEST(all_add_dedupes_and_caps)
{
    fake_watch_ctx c;
    init_default(&c, 1000);

    mos_watch_all_state a;
    mos_internal_watch_all_init(&a);

    int first = mos_internal_watch_all_add(&a, &fake_ops, &c, -1, 100,
                                           1000, 1000, 2000, 200, false);
    EXPECT(first >= 0);
    /* Same registry id (same plug session) dedupes to the same slot —
       the Appeared notification can announce an open-time device. */
    EXPECT_EQ(first, mos_internal_watch_all_add(&a, &fake_ops, &c, -1, 100,
                                                1000, 1000, 2000, 200, true));

    /* Fill the rest; the slot past CAP is refused. */
    for (uint64_t id = 101; id < 100 + MOS_WATCH_ALL_CAP; ++id) {
        EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c, -1, id,
                                          1000, 1000, 2000, 200, false) >= 0);
    }
    EXPECT_EQ(-1, mos_internal_watch_all_add(&a, &fake_ops, &c, -1, 999,
                                             1000, 1000, 2000, 200, false));
    /* registry_id 0 is never addable (no attachment identity). */
    EXPECT_EQ(-1, mos_internal_watch_all_add(&a, &fake_ops, &c, -1, 0,
                                             1000, 1000, 2000, 200, false));
    return 0;
}


TEST(all_error_first_join_still_announces_device_appeared)
{
    /* A hot-plugged drive whose probe fails right after Appeared must
       still announce itself: ERROR events do not consume the pending
       join — the first successful probe's snapshot is relabeled. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_err[0]   = MOS_ERR_IO;       /* first probe fails */
    c.probe_state[0] = MOS_STATE_UNKNOWN;
    c.probe_err[1]   = MOS_OK;           /* then the drive answers */
    c.probe_state[1] = MOS_STATE_OPEN;
    c.probe_count    = 2;

    mos_watch_all_state a;
    mos_internal_watch_all_init(&a);
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c, -1, 300,
                                      1000, 1000, 2000, 200, true) >= 0);

    mos_watch_decision d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_ERROR, d.event.kind);
    EXPECT_EQ(1, (int)d.event.seq);

    mos_internal_watch_notify_wake(
        &a.cores[mos_internal_watch_all_find(&a, 300)]);
    d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_DEVICE_APPEARED, d.event.kind);
    EXPECT_EQ(MOS_STATE_OPEN, d.event.state);
    EXPECT_EQ(2, (int)d.event.seq);
    return 0;
}

TEST(all_last_device_removed_stream_stays_open)
{
    /* Removing the only device must NOT terminate the stream: the slot
       frees, the next pump sleeps unbounded (an empty bus waits for
       arrivals), and the id is no longer findable. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_err[0]   = MOS_ERR_NO_DEVICE;
    c.probe_state[0] = MOS_STATE_UNKNOWN;
    c.probe_count    = 1;

    mos_watch_all_state a;
    mos_internal_watch_all_init(&a);
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c, 4, 100,
                                      1000, 1000, 2000, 200, false) >= 0);

    mos_watch_decision d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED, d.event.kind);

    EXPECT_EQ(-1, mos_internal_watch_all_find(&a, 100));
    d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d.kind);
    EXPECT(d.next_poll_at_mono_ms == UINT64_MAX);
    return 0;
}

TEST(all_sleep_folds_earliest_deadline_across_rates)
{
    /* Two stable devices with DIFFERENT stable rates: the folded
       deadline must be the earlier one (min), not the later. */
    fake_watch_ctx c1, c2;
    init_default(&c1, 1000);
    init_default(&c2, 1000);
    c1.probe_state[0] = MOS_STATE_READY; c1.probe_err[0] = MOS_OK; c1.probe_count = 1;
    c2.probe_state[0] = MOS_STATE_READY; c2.probe_err[0] = MOS_OK; c2.probe_count = 1;

    mos_watch_all_state a;
    mos_internal_watch_all_init(&a);
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c1, 4, 100,
                                      1000, 1000, /*stable=*/2000, 200,
                                      false) >= 0);
    EXPECT(mos_internal_watch_all_add(&a, &fake_ops, &c2, 5, 200,
                                      1000, 1000, /*stable=*/500, 200,
                                      false) >= 0);

    /* Drain both snapshots, then the fold: min(1000+500, 1000+2000). */
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT,
              mos_internal_watch_all_pump(&a).kind);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT,
              mos_internal_watch_all_pump(&a).kind);
    mos_watch_decision d = mos_internal_watch_all_pump(&a);
    EXPECT_EQ(MOS_WATCH_DECIDE_SLEEP_UNTIL, d.kind);
    EXPECT_EQ(1500, (int)d.next_poll_at_mono_ms);
    return 0;
}

void register_watch_core_tests(void);
void register_watch_core_tests(void)
{
    RUN(all_empty_stream_sleeps_unbounded);
    RUN(all_interleaves_ascending_registry_id_with_global_seq);
    RUN(all_mid_stream_join_relabels_first_event_only);
    RUN(all_device_removed_is_per_slot_not_terminal);
    RUN(all_add_dedupes_and_caps);
    RUN(all_error_first_join_still_announces_device_appeared);
    RUN(all_last_device_removed_stream_stays_open);
    RUN(all_sleep_folds_earliest_deadline_across_rates);
    RUN(test_snapshot_emitted_on_first_pump);
    RUN(test_zero_registry_id_passes_through);
    RUN(test_empty_drive_yields_unit_minus_one_and_session_identity);
    RUN(test_media_appears_after_empty_open_uses_probe_unit);
    RUN(test_media_changed_on_same_state_ready_swap);
    RUN(test_no_media_changed_when_id_unavailable);
    RUN(test_media_changed_on_profile_class_swap_without_id);
    RUN(test_media_changed_on_same_class_profile_swap_without_id);
    RUN(test_no_media_changed_on_same_profile_without_id);
    RUN(test_media_changed_after_late_media_id);
    RUN(test_media_changed_across_transient_zero_id);
    RUN(test_profile_fallback_after_late_profile);
    RUN(test_poll_class_pinned_for_every_state);
    RUN(test_state_change_takes_precedence_over_media_changed);
    RUN(test_clock_domains_separate);
    RUN(test_no_change_pump_returns_sleep_no_event);
    RUN(test_state_change_emits_delta_with_prev_state);
    RUN(test_transitional_state_uses_short_backoff);
    RUN(test_stable_state_uses_long_backoff);
    RUN(test_notify_wake_pulls_next_poll_forward);
    RUN(test_notify_removed_emits_device_removed_then_terminal);
    RUN(test_notify_removed_before_snapshot_still_emits_removed);
    RUN(test_probe_no_device_terminates_with_removed_event);
    RUN(test_probe_error_emits_error_event_no_state_update);
    RUN(test_error_event_unit_reflects_last_observed_not_open_time);
    RUN(test_error_backoff_escalates_and_caps);
    RUN(test_error_backoff_resets_on_success);
    RUN(test_error_backoff_resets_on_different_error);
    RUN(test_seq_monotonic_across_event_types);
    RUN(test_latency_ms_measures_probe_duration);
    RUN(test_ts_saturates_at_year_9999_boundary);
    RUN(test_latency_saturates_on_backward_clock);
    RUN(test_session_identity_stable_across_pumps_uses_wall_ms);
    RUN(test_rfc3339_format_uses_wall_ms);
}
