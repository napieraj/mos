/*
 * test_adapter_phase2.c — runs the REAL watch adapter (mos_watch.c,
 * with mos_scsi.c / mos_state.c / mos_dr.c beneath it) headless against
 * the link-seam fake, under deterministic fake time. Phase 2 of
 * doc/research/2026-06-11-headless-adapter-emulation.md: the watch
 * lifecycle — notification delivery into the parked run loop, doorbell
 * vs poll-floor timing, removal paths, degraded modes.
 *
 * Every scenario runs on the fake clock (tests/fake/mos_fake_watch.h):
 * timestamps and inter-event gaps are asserted EXACTLY, in fake
 * milliseconds — the pure-core oracle for the expected sequences is
 * tests/test_watch_core.c.
 */

#include "mos.h"
#include "mos_fake_apple.h"
#include "mos_fake_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standalone harness counters (separate binary with its own main). */
int mos_tests_run = 0;
int mos_tests_failed = 0;
#include "test_harness.h"

/* Fake wall base 2025-06-01T00:00:00Z exactly, so event `ts` strings
   are byte-deterministic: ts = base + fake-mono at emit time. */
#define WALL_BASE_MS 1748736000000ull

#define FAKE_DRIVE_ID 0x100000123ull

/* Default poll rates (0 = the core's documented defaults). */
#define STABLE_MS     2000
#define TRANSITION_MS 200

/* Load a committed fixture into `buf`; returns byte count or aborts. */
static size_t load_fixture(const char *name, uint8_t *buf, size_t cap)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", MOS_FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open fixture %s\n", path); exit(2); }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

/* Fixed-format (0x70) 18-byte sense. */
static void make_sense(uint8_t out[18], uint8_t sk, uint8_t asc, uint8_t ascq)
{
    memset(out, 0, 18);
    out[0]  = 0x70;
    out[2]  = sk;
    out[7]  = 10;
    out[12] = asc;
    out[13] = ascq;
}

/* 8-byte GESN media-event reply, door closed/open (test_adapter_phase1
   has the field map). */
static void make_gesn(uint8_t out[8], bool door_open)
{
    memset(out, 0, 8);
    out[1] = 0x06;
    out[2] = 0x04;
    out[3] = 0x10;
    out[5] = door_open ? 0x01 : 0x00;
}

/* Scenario base: both fakes reset, drive present and READY (committed
   DVD-ROM GET CONFIGURATION fixture, TUR GOOD), fake clock at mono 0. */
static void scenario_ready_at_t0(void)
{
    mos_fake_reset();
    mos_fake_watch_reset();
    uint8_t cfg[64];
    size_t  n = load_fixture("getconfig_dvdrom_current.bin", cfg, sizeof cfg);
    mos_fake_set_getconfig_reply(0x00, cfg, n);
    mos_fake_set_tur(0x00, NULL);
    mos_fake_clock_enable(0, WALL_BASE_MS);
}

/* Re-script the drive to not-ready EMPTY: media gone (no IOMedia
   child), TUR CHECK 02/3A/00, GESN door closed. */
static void script_empty(void)
{
    uint8_t sense[18], gesn[8];
    make_sense(sense, 0x02, 0x3A, 0x00);
    make_gesn(gesn, /*door_open=*/false);
    mos_fake_set_bsd_unit(-1);
    mos_fake_set_tur(0x02, sense);
    mos_fake_set_raw_reply(0x00, gesn, 8, 8, NULL);
}

/* 8-byte GET CONFIGURATION feature header, header-only reply: Data
   Length 4 (the reserved + current-profile bytes that follow it),
   current profile in [6..7]. */
static void make_config_header(uint8_t out[8], uint16_t profile)
{
    memset(out, 0, 8);
    out[3] = 0x04;
    out[6] = (uint8_t)(profile >> 8);
    out[7] = (uint8_t)(profile & 0xFF);
}

/* ---- Timeline step actions ----------------------------------------- */

static void act_empty_fire_dr(void *ctx)
{
    (void)ctx;
    script_empty();
    mos_fake_fire_dr_status_changed();
}

static void act_empty_fire_io_prop(void *ctx)
{
    (void)ctx;
    script_empty();
    mos_fake_fire_io_property_change();
}

static void act_empty_no_fire(void *ctx)
{
    (void)ctx;
    script_empty();
}

static void act_fire_io_termination(void *ctx)
{
    (void)ctx;
    mos_fake_fire_io_termination();
}

static void act_set_no_drive(void *ctx)
{
    (void)ctx;
    mos_fake_set_no_drive();
}

static void act_swap_media_fire_dr(void *ctx)
{
    (void)ctx;
    mos_fake_set_media_id(0x100000789ull);
    mos_fake_fire_dr_status_changed();
}

static void act_profile_to_dvd_fire_dr(void *ctx)
{
    (void)ctx;
    uint8_t hdr[8];
    make_config_header(hdr, 0x0010);
    mos_fake_set_getconfig_reply(0x00, hdr, 8);
    mos_fake_fire_dr_status_changed();
}

static void act_plugin_fail(void *ctx)
{
    (void)ctx;
    mos_fake_set_plugin_fail(true);
}

/* Open a watch on the default drive and consume the snapshot event,
   asserting its full shape; returns via *so the stream_open_ms the rest
   of the stream must keep. */
static int open_and_take_snapshot(mos_watch_t **w_out, uint64_t *so)
{
    mos_error err = MOS_ERR_IO;
    mos_watch_t *w = mos_watch_open_by_index(1, STABLE_MS, TRANSITION_MS,
                                             &err);
    EXPECT(w != NULL);
    EXPECT_EQ(MOS_OK, err);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 1000));
    EXPECT(e != NULL);
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, mos_watch_event_kind(e));
    EXPECT_EQ(1, mos_watch_event_seq(e));
    EXPECT_EQ(MOS_STATE_READY, mos_watch_event_state(e));
    EXPECT_EQ(MOS_STATE_UNKNOWN, mos_watch_event_prev_state(e));
    EXPECT_EQ(FAKE_DRIVE_ID, mos_watch_event_registry_id(e));
    EXPECT_EQ(0, mos_fake_clock_now());      /* first poll is immediate */
    EXPECT_STREQ("2025-06-01T00:00:00Z", mos_watch_event_ts(e));

    *so    = mos_watch_event_stream_open_ms(e);
    *w_out = w;
    return 0;
}

/* ---- Scenarios ------------------------------------------------------ */

TEST(watch_snapshot_then_doorbell_state_change)
{
    scenario_ready_at_t0();

    mos_watch_t *w = NULL;
    uint64_t so = 0;
    int rc = open_and_take_snapshot(&w, &so);
    if (rc) return rc;

    /* The adapter registered exactly: one IOKit port, one DR center,
       one StatusChanged observer. */
    EXPECT_EQ(3, mos_fake_outstanding_notify_objects());

    /* t=500: media ejected; the DR doorbell rings. The state change is
       observed AT t=500 — the doorbell beat the 2000 ms poll floor. */
    mos_fake_step(500, act_empty_fire_dr, NULL);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 1500));
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED, mos_watch_event_kind(e));
    EXPECT_EQ(2, mos_watch_event_seq(e));
    EXPECT_EQ(MOS_STATE_EMPTY, mos_watch_event_state(e));
    EXPECT_EQ(MOS_STATE_READY, mos_watch_event_prev_state(e));
    EXPECT_EQ(500, mos_fake_clock_now());
    EXPECT_STREQ("2025-06-01T00:00:00Z", mos_watch_event_ts(e));
    EXPECT_EQ(so, mos_watch_event_stream_open_ms(e));

    mos_watch_close(w);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(watch_property_change_wake)
{
    /* Same shape through the OTHER wake path: a kIOGeneralInterest
       property-change message on the drive service. */
    scenario_ready_at_t0();

    mos_watch_t *w = NULL;
    uint64_t so = 0;
    int rc = open_and_take_snapshot(&w, &so);
    if (rc) return rc;

    mos_fake_step(500, act_empty_fire_io_prop, NULL);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 1500));
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED, mos_watch_event_kind(e));
    EXPECT_EQ(MOS_STATE_EMPTY, mos_watch_event_state(e));
    EXPECT_EQ(500, mos_fake_clock_now());
    EXPECT_EQ(so, mos_watch_event_stream_open_ms(e));

    mos_watch_close(w);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    return 0;
}

TEST(watch_removed_via_interest_termination)
{
    scenario_ready_at_t0();

    mos_watch_t *w = NULL;
    uint64_t so = 0;
    int rc = open_and_take_snapshot(&w, &so);
    if (rc) return rc;

    /* t=700: the kernel terminates the service. The removal event is
       notification-driven: no probe, latency 0, arrives at t=700. */
    mos_fake_step(700, act_fire_io_termination, NULL);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 1500));
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED, mos_watch_event_kind(e));
    EXPECT_EQ(MOS_STATE_READY, mos_watch_event_prev_state(e));
    EXPECT_EQ(0, mos_watch_event_latency_ms(e));
    EXPECT_EQ(700, mos_fake_clock_now());
    EXPECT_EQ(so, mos_watch_event_stream_open_ms(e));

    /* Single-target removal is terminal. */
    EXPECT_EQ(MOS_ERR_NO_DEVICE, mos_watch_next_event(w, &e, 100));

    mos_watch_close(w);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    return 0;
}

TEST(watch_removed_via_reopen_failure)
{
    /* The poll-floor removal path: the drive vanishes WITHOUT any
       notification (t=300); the scheduled poll at t=2000 reopens by
       registry ID, gets NO_DEVICE, and the core converts it to a
       terminal device_removed. Pins the by-ID-reopen authority. */
    scenario_ready_at_t0();

    mos_watch_t *w = NULL;
    uint64_t so = 0;
    int rc = open_and_take_snapshot(&w, &so);
    if (rc) return rc;

    mos_fake_step(300, act_set_no_drive, NULL);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 3000));
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED, mos_watch_event_kind(e));
    EXPECT_EQ(2000, mos_fake_clock_now());   /* found by the poll floor */
    EXPECT_EQ(so, mos_watch_event_stream_open_ms(e));

    EXPECT_EQ(MOS_ERR_NO_DEVICE, mos_watch_next_event(w, &e, 100));

    mos_watch_close(w);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    return 0;
}

TEST(watch_poll_only_degraded)
{
    /* Both notification setups fail at open. The single-target contract
       is soft-fail: the watch opens, polling is the correctness floor.
       A state change at t=900 is observed at the t=2000 poll, via the
       nanosleep path (no sources exist, the run-loop gate is closed). */
    scenario_ready_at_t0();
    mos_fake_set_io_notify_fail(true);
    mos_fake_set_dr_center_fail(true);

    mos_watch_t *w = NULL;
    uint64_t so = 0;
    int rc = open_and_take_snapshot(&w, &so);
    if (rc) return rc;

    /* Nothing was registered — poll-only confirmed structurally. */
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());

    mos_fake_step(900, act_empty_no_fire, NULL);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 3000));
    EXPECT_EQ(MOS_EVENT_STATE_CHANGED, mos_watch_event_kind(e));
    EXPECT_EQ(MOS_STATE_EMPTY, mos_watch_event_state(e));
    EXPECT_EQ(2000, mos_fake_clock_now());
    EXPECT_STREQ("2025-06-01T00:00:02Z", mos_watch_event_ts(e));
    EXPECT_EQ(so, mos_watch_event_stream_open_ms(e));

    mos_watch_close(w);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    return 0;
}

TEST(watch_media_swap_emits_media_changed)
{
    /* F1: same-state swap. The whole-disk IOMedia registry ID re-mints
       (0x100000456 → 0x100000789) while the drive stays READY across
       two probes; the id flows through the real chain —
       IORegistryEntryGetRegistryEntryID → handle media_id → probe
       result → core fingerprint — and emits media_changed. */
    scenario_ready_at_t0();

    mos_watch_t *w = NULL;
    uint64_t so = 0;
    int rc = open_and_take_snapshot(&w, &so);
    if (rc) return rc;

    mos_fake_step(500, act_swap_media_fire_dr, NULL);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 1500));
    EXPECT_EQ(MOS_EVENT_MEDIA_CHANGED, mos_watch_event_kind(e));
    EXPECT_EQ(MOS_STATE_READY, mos_watch_event_state(e));
    EXPECT_EQ(MOS_STATE_READY, mos_watch_event_prev_state(e));
    EXPECT_EQ(500, mos_fake_clock_now());
    EXPECT_EQ(so, mos_watch_event_stream_open_ms(e));
    /* Identity strings on a live event: the watch-static buffers, read
       under ASan (the watch-lifetime analogue of seam-contract O-3). */
    EXPECT_STREQ("HL-DT-ST", mos_watch_event_vendor(e));
    EXPECT_STREQ("DVDROM",   mos_watch_event_product(e));

    mos_watch_close(w);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    return 0;
}

TEST(watch_media_swap_profile_fallback)
{
    /* The bridge-without-identity arm: media_id is 0 on both sides, so
       the only swap evidence is the current profile's class changing
       (CD-ROM 0x08 → DVD-ROM 0x10) — the core's documented fallback
       fingerprint. */
    mos_fake_reset();
    mos_fake_watch_reset();
    uint8_t hdr[8];
    make_config_header(hdr, 0x0008);
    mos_fake_set_getconfig_reply(0x00, hdr, 8);
    mos_fake_set_tur(0x00, NULL);
    mos_fake_set_media_id(0);            /* identity never available */
    mos_fake_clock_enable(0, WALL_BASE_MS);

    mos_error err = MOS_ERR_IO;
    mos_watch_t *w = mos_watch_open_by_index(1, STABLE_MS, TRANSITION_MS,
                                             &err);
    EXPECT(w != NULL);
    EXPECT_EQ(MOS_OK, err);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 1000));
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, mos_watch_event_kind(e));
    EXPECT_EQ(MOS_STATE_READY, mos_watch_event_state(e));
    EXPECT_EQ(0x0008, mos_watch_event_current_profile(e));
    uint64_t so = mos_watch_event_stream_open_ms(e);

    mos_fake_step(500, act_profile_to_dvd_fire_dr, NULL);

    EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 1500));
    EXPECT_EQ(MOS_EVENT_MEDIA_CHANGED, mos_watch_event_kind(e));
    EXPECT_EQ(0x0010, mos_watch_event_current_profile(e));
    EXPECT_EQ(500, mos_fake_clock_now());
    EXPECT_EQ(so, mos_watch_event_stream_open_ms(e));

    mos_watch_close(w);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    return 0;
}

TEST(watch_replug_reminted_drive_id)
{
    /* Replug: terminal removal of stream 1, then the "same" drive back
       under a re-minted registry ID (xnu's never-reused counter). The
       new stream carries the NEW id and a DIFFERENT stream_open_ms —
       (registry_id, stream_open_ms) stays a unique session key. */
    scenario_ready_at_t0();

    mos_watch_t *w1 = NULL;
    uint64_t so1 = 0;
    int rc = open_and_take_snapshot(&w1, &so1);
    if (rc) return rc;

    mos_fake_step(400, act_fire_io_termination, NULL);

    const mos_watch_event *e = NULL;
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w1, &e, 1500));
    EXPECT_EQ(MOS_EVENT_DEVICE_REMOVED, mos_watch_event_kind(e));
    EXPECT_EQ(MOS_ERR_NO_DEVICE, mos_watch_next_event(w1, &e, 100));
    mos_watch_close(w1);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());

    mos_fake_set_drive_id(0x100000AAAull);

    mos_error err = MOS_ERR_IO;
    mos_watch_t *w2 = mos_watch_open_by_index(1, STABLE_MS, TRANSITION_MS,
                                              &err);
    EXPECT(w2 != NULL);
    EXPECT_EQ(MOS_OK, err);
    EXPECT_EQ(MOS_OK, mos_watch_next_event(w2, &e, 1000));
    EXPECT_EQ(MOS_EVENT_SNAPSHOT, mos_watch_event_kind(e));
    EXPECT_EQ(0x100000AAAull, mos_watch_event_registry_id(e));
    EXPECT(mos_watch_event_stream_open_ms(e) != so1);

    mos_watch_close(w2);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    return 0;
}

TEST(watch_error_backoff_escalates_deterministically)
{
    /* Consecutive identical probe errors escalate the retry interval
       200 → 400 → 800 → 1600 → 2000 (capped) — pinned at the pure
       layer by test_watch_core.c, asserted HERE as exact fake-clock
       positions through the real adapter: the per-probe reopen fails
       with MOS_ERR_DRIVER_REJECTED (plugin factory declines) from the
       t=2000 poll onward. */
    scenario_ready_at_t0();

    mos_watch_t *w = NULL;
    uint64_t so = 0;
    int rc = open_and_take_snapshot(&w, &so);
    if (rc) return rc;

    mos_fake_step(100, act_plugin_fail, NULL);

    static const uint64_t at_ms[6] = {2000, 2200, 2600, 3400, 5000, 7000};
    for (int i = 0; i < 6; ++i) {
        const mos_watch_event *e = NULL;
        EXPECT_EQ(MOS_OK, mos_watch_next_event(w, &e, 3000));
        EXPECT_EQ(MOS_EVENT_ERROR, mos_watch_event_kind(e));
        EXPECT_EQ(MOS_ERR_DRIVER_REJECTED, mos_watch_event_error(e));
        EXPECT_EQ((long long)at_ms[i], (long long)mos_fake_clock_now());
        EXPECT_EQ(2 + i, mos_watch_event_seq(e));
        EXPECT_EQ(so, mos_watch_event_stream_open_ms(e));
    }

    mos_watch_close(w);
    EXPECT_EQ(0, mos_fake_outstanding_notify_objects());
    return 0;
}

int main(void)
{
    printf("adapter phase-2 (watch lifecycle, fake clock):\n");
    RUN(watch_snapshot_then_doorbell_state_change);
    RUN(watch_property_change_wake);
    RUN(watch_removed_via_interest_termination);
    RUN(watch_removed_via_reopen_failure);
    RUN(watch_poll_only_degraded);
    RUN(watch_media_swap_emits_media_changed);
    RUN(watch_media_swap_profile_fallback);
    RUN(watch_replug_reminted_drive_id);
    RUN(watch_error_backoff_escalates_deterministically);
    printf("\n%d run, %d passed, %d failed\n",
           mos_tests_run, mos_tests_run - mos_tests_failed, mos_tests_failed);
    return mos_tests_failed ? 1 : 0;
}
