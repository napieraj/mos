/*
 * test_watch_core.c — Pure unit tests for the watch state machine.
 *
 * Drives mos_internal_watch_pump() with a fake mos_watch_ops_t whose
 * probe, mono_ms, and wall_ms callbacks are scripted by the fixture.
 * No IOKit, no real time, no real sleep — every scenario is
 * deterministic.
 *
 * Two-clock fixture: the fake separates the monotonic clock (scheduling,
 * latency) from the wall clock (timestamps, session identity). The split
 * is structural — an adapter passing wall ms as the scheduling start_ms
 * while wiring now_ms to monotonic uptime gets a first-poll deadline ~55
 * years out. test_clock_domains_separate runs mono in the thousands and
 * wall in the trillions to pin that scheduling never reads the wall clock.
 */

#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ---- Fake clocks + probe ---------------------------------------------- *
 *
 * Two independent clock fields, usually equal but held orders of
 * magnitude apart by the clock-domain test. The auto_advance fields let
 * latency tests script elapsed probe time without moving scheduling. */

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
    /* Drive serial per probe (NULL = not grabbed yet, the null-until-free
       case the adapter produces before its first free poll). Borrowed string
       literal; the core forwards the pointer verbatim into the event. */
    const char    *probe_serial[8];
    int            probe_count;
    int            probe_calls;

    /* When set, the probe drops the monotonic clock to 10 — emulating a
       backward step between the pump's probe-start and probe-end reads.
       Real CLOCK_MONOTONIC never does this; a buggy ops table could. */
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
        /* Pump reached the probe with nothing scripted; fail loudly
           rather than index probe_err[-1]. */
        fprintf(stderr, "fake_probe: probe_count == 0 (fixture misconfigured)\n");
        abort();
    }
    int idx = c->probe_calls < c->probe_count ? c->probe_calls : c->probe_count - 1;
    c->probe_calls++;

    if (c->probe_err[idx] != MOS_OK) {
        /* Seam contract E-1: out-params are UNDEFINED on error. Poison
           with 0xEE so any core read of an error-path field (state,
           unit, media_id, profile) breaks an assertion; a
           leave-untouched fake would mask the obligation. */
        memset(out, 0xEE, sizeof(*out));
        return c->probe_err[idx];
    }

    memset(out, 0, sizeof(*out));
    out->state           = c->probe_state[idx];
    /* No-media states (open/empty) have no BSD disk node -> -1, else 4.
       The core sources event bsd_unit from the probe, so this lets
       fixtures exercise media appearing and disappearing. */
    out->bsd_unit        = (out->state == MOS_STATE_OPEN ||
                            out->state == MOS_STATE_EMPTY) ? -1 : 4;
    out->media_id        = c->probe_media_id[idx];
    /* Fixture profile 0 means "BD-ROM default"; 0xFFFF scripts a genuine
       wire 0 (transient enrichment failure) that 0 itself can't express. */
    out->current_profile = c->probe_profile[idx] == 0xFFFF ? 0
                         : c->probe_profile[idx] ? c->probe_profile[idx]
                         : 0x0040;
    /* Serial: NULL by the memset above unless the fixture scripts one. The
       core copies r->serial verbatim into the event (fill_event_state_fields),
       mirroring vendor/product/revision. */
    out->serial          = c->probe_serial[idx];
    return MOS_OK;
}

static const mos_watch_ops_t fake_ops = {
    .probe   = fake_probe,
    .mono_ms = fake_mono,
    .wall_ms = fake_wall,
};

/* Both clocks coincide — default for tests that don't care about the
   two-clock split. */
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

TEST(test_event_carries_serial_through_fill_state_fields)
{
    /* The serial rides the probe result into the event exactly as
       vendor/product/revision do (fill_event_state_fields). A probe with no
       serial yet (the null-until-free-poll case the adapter starts in) leaves
       the event's serial NULL; a probe that has grabbed it carries the
       string. Two pumps over two probes pin both. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0]  = MOS_STATE_READY;
    c.probe_err[0]    = MOS_OK;
    c.probe_serial[0] = NULL;              /* not grabbed yet */
    c.probe_state[1]  = MOS_STATE_EMPTY;   /* state delta forces a 2nd event */
    c.probe_err[1]    = MOS_OK;
    c.probe_serial[1] = "KL2G7942618WL";   /* grabbed on a free poll */
    c.probe_count     = 2;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242,
                            /*start_mono_ms=*/1000,
                            /*start_wall_ms=*/1000,
                            /*stable=*/2000, /*transition=*/200);

    /* First probe: snapshot, serial still NULL. */
    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT,          d.event.kind);
    EXPECT(d.event.serial == NULL);

    /* Advance to the next poll and probe again: state_changed carries the
       now-grabbed serial. */
    c.mono_clock_ms = w.next_poll_at_mono_ms;
    c.wall_clock_ms = c.mono_clock_ms;
    d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED,     d.event.kind);
    EXPECT(d.event.serial != NULL);
    EXPECT_STREQ(d.event.serial, "KL2G7942618WL");
    return 0;
}

TEST(test_zero_registry_id_passes_through)
{
    /* No adapter path produces token 0 (open fails closed if the
       registry ID can't be captured, and xnu assigns one to every
       attached entry). A direct pure-layer 0 flows through unspecial-
       cased — unambiguous, since real IDs are >= 2^32+256 by reservation. */
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
    /* Empty/open-tray drive (init unit -1): the snapshot's bsd_unit must
       be -1, but session identity (registry_id + stream_open_wall_ms) is
       present and ordinary — the registry token exists precisely when the
       BSD name doesn't. Unit is event-time (from the probe); identity is
       open-time-stable. The companion test below pins that a later
       non-empty probe surfaces the fresh unit. */
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
    /* A watch opened empty (init unit -1) must surface the fresh media
       unit once a disc appears, not stay frozen at -1. Session identity
       stays open-time-stable for the stream's life. */
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

    /* Pump 1: empty snapshot, unit -1. */
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
    /* Fresh probe unit, not the frozen open-time -1. */
    EXPECT_EQ(4, (int)d2.event.bsd_unit);
    /* Session identity stable at open-time values. */
    EXPECT_EQ(4242, (int)d2.event.registry_id);
    EXPECT_EQ(1000, (int)d2.event.stream_open_wall_ms);
    return 0;
}

TEST(test_media_changed_on_same_state_ready_swap)
{
    /* Same-state swap: stays READY but the disc is physically replaced
       (IOMedia registry id changes). The core emits media_changed (not
       state_changed), and a same-disc poll between emits nothing. */
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
    /* media_id == 0 means "unavailable" (a bridge with no stable IOMedia
       id). The core must never infer a swap from an unknown id. */
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
    /* With no stable media_id (both 0), a cross-class swap still shows in
       a changed current_profile. CD-ROM (0x08) -> DVD-ROM (0x10), staying
       READY, must emit media_changed via the last_profile fallback. */
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
    /* REGRESSION (fingerprint staleness): the IOMedia child often
       registers a beat after TUR goes GOOD, so the snapshot lands READY
       with media_id 0 and the id arrives on the next probe. If a
       no-change pump never refreshes the fingerprint, last_media_id stays
       0 all session and a later swap is undetectable. The late id must be
       adopted silently (no event), and the swap after it must emit
       media_changed. */
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
    /* A known identity is KEPT across an unavailability gap (0 never
       overwrites non-zero). A swap whose new id appears after a transient
       0 probe must still compare against the pre-gap identity and emit
       media_changed; overwriting with 0 would miss the swap forever. The
       gap probe itself fabricates no event. */
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
    /* Profile-fallback twin of the late-id case, on a bridge with no
       media_id: enrichment fails at snapshot (wire profile 0), the real
       profile arrives next probe (adopted silently), and a later
       cross-class swap fires the last_profile fallback. If last_profile
       stayed 0, the fallback (needs both non-zero) is disarmed all
       session. */
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
    /* Pin the poll class of EVERY mos_state, so a new state can't
       silently inherit "stable." With mono not auto-advancing, the
       snapshot deadline is start + (transition|stable)_poll_ms. */
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
    /* State + media id both change: it's a transition, not a swap — the
       state-change branch is evaluated first. */
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

/* REGRESSION: the two clocks differ by 10+ orders of magnitude.
   Crossing them puts next_poll in the trillions (wall) while now is in
   the thousands (mono), so the first pump would SLEEP_UNTIL ~55 years
   out. With start_mono_ms == ops->mono_ms the first poll fires now. */
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
    /* Production shape: start_mono_ms from mono, start_wall_ms from wall,
       deliberately implausible as a unified clock. */
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242,
                            /*start_mono_ms=*/12345,
                            /*start_wall_ms=*/1746526503000ULL,
                            2000, 200);

    mos_watch_decision d = mos_internal_watch_pump(&w);
    /* SLEEP_UNTIL here means the regression is back: scheduling read a
       wall-clock value. */
    EXPECT_EQ(MOS_WATCH_DECIDE_EMIT_EVENT, d.kind);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT,          d.event.kind);
    EXPECT_EQ(MOS_STATE_EMPTY,             d.event.state);
    /* stream_open tags the wall epoch ms, not the mono value. */
    EXPECT_EQ((long long)1746526503000LL,
              (long long)d.event.stream_open_wall_ms);
    /* ts from wall: 1746526503000 ms -> 2025-05-06T10:15:03Z. */
    EXPECT_STREQ("2025-05-06T10:15:03Z", d.event.ts);
    /* next_poll is mono: mono didn't advance past 12345, so
       12345 + 2000 = 14345. A trillion-shaped value = wrong clock. */
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

/* REGRESSION: removal before any successful snapshot. Gating
   device_removed on have_last_state would drop it (TERMINAL with no
   event); the schema promises device_removed closes every stream. */
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

/* REGRESSION: probe -> MOS_ERR_NO_DEVICE is terminal. Treated as
   transient, the loop would spin emitting ERROR every transition_poll_ms
   when the drive is unplugged in poll-only mode. NO_DEVICE must route
   through the terminal path with a device_removed event. */
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
    /* Error/device_removed events have no fresh probe, so the base falls
       back to the core's own bsd_unit, which must be the last OBSERVED
       unit (adopted by the core, not just an adapter — else a pure-only
       context reports the open-time -1). Sequence: open empty (-1), media
       appears (4), then a probe error. */
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
       open-time -1. */
    EXPECT_EQ(4, (int)d3.event.bsd_unit);
    return 0;
}

TEST(test_error_backoff_escalates_and_caps)
{
    /* Consecutive identical errors double the retry interval from
       transition (200) toward stable (2000): 200, 400, 800, 1600, 2000
       (capped), 2000. Every pump still emits an error; only spacing grows. */
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

    /* Mono auto-advances 50 ms per read; wall stays put. Across a
       snapshot pump the probe-start read returns 5000 and probe-end
       returns 5050, so latency = 50. */
    c.mono_auto_advance_ms = 50;

    mos_watch_decision d = mos_internal_watch_pump(&w);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, d.event.kind);
    EXPECT_EQ(50, (int)d.event.latency_ms);
    return 0;
}

TEST(test_ts_saturates_at_year_9999_boundary)
{
    /* The schema requires a 4-digit year. Max, max+1s, and absurd wall
       values must all yield schema-shaped ts — the last two saturating
       to 9999-12-31T23:59:59Z, never a 5-digit year and never "". */
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
    /* The latency clamp (end < start -> 0) goes unexercised by any
       forward-clock fixture: a deleted clamp would still pass. A backward
       step between probe-start and probe-end must clamp to 0, not
       underflow to a ~49-day wrapped value. */
    fake_watch_ctx c;
    init_default(&c, 1000);
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;
    /* The backward step is driven by the probe side effect: it flips the
       clock to 10 between the pump's probe-start and probe-end reads
       (negative auto-advance isn't expressible). */
    c.flip_mono_on_probe    = true;   /* probe drops the clock to 10 */
    c.mono_clock_ms         = 1000;
    /* Seam contract C-3: guard the flip itself. A forward auto-advance
       means a no-op flip yields non-zero latency and the 0-assertion
       below fails — so the backward step can't rot into a static clock
       that passes for the wrong reason. */
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
    /* Wall ms distinct from mono, so stream_open is shown to come from
       the wall source. */
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
    /* 1777593600000 ms = 2026-05-01T00:00:00Z, distinct from mono so ts
       is pinned to wall_ms. */
    c.wall_clock_ms  = 1777593600000ULL;
    c.probe_state[0] = MOS_STATE_READY;
    c.probe_err[0]   = MOS_OK;
    c.probe_count    = 1;

    mos_watch_state w;
    mos_internal_watch_init(&w, &fake_ops, &c, 4, 4242,
                            1000, 1777593600000ULL, 2000, 200);

    mos_watch_decision d = mos_internal_watch_pump(&w);
    /* ts from wall, not mono 1000 (which renders 1970-01-01T00:00:01Z). */
    EXPECT_STREQ("2026-05-01T00:00:00Z", d.event.ts);
    return 0;
}

/* ---- Suite registration ----------------------------------------------- */


/* ---- Watch-all multiplexer ----------------------------------------- *
 *
 * Pure fan-in. These tests pin the four properties the adapter relies
 * on: ascending-registry_id interleave, stream-global seq, mid-stream
 * join relabeling (snapshot -> device_appeared), and per-slot
 * (non-terminal) removal. Each slot has its own fake ctx/clock; the
 * multiplexer touches no clock. */

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
    RUN(test_event_carries_serial_through_fill_state_fields);
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
