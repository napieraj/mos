/*
 * test_adapter_oneshot.c — runs the REAL one-shot adapter TUs
 * (mos_scsi.c / mos_state.c / mos_dr.c) headless against the link-seam
 * fake of IOKit + DiscRecording (tests/fake/mos_fake_apple.c), fed
 * committed MMC fixture bytes. Phase 1 of
 * doc/research/2026-06-11-headless-adapter-emulation.md.
 *
 * This is a separate test program from mos_tests (which links mos_pure
 * only): it links the adapter object code + the fake + real
 * CoreFoundation, with NO IOKit / DiscRecording frameworks.
 */

#include "mos.h"
#include "mos_fake_apple.h"
#include "../src/mos_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standalone harness counters (this binary has its own main; it does
   not share test_main.c with the pure suite). */
int mos_tests_run = 0;
int mos_tests_failed = 0;
#include "test_harness.h"

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

TEST(adapter_open_index_query_ready)
{
    mos_fake_reset();

    /* Replay the committed DVD-ROM GET CONFIGURATION reply. The adapter's
       get_current_profile issues RT=2 and reads the 8-byte feature
       header; this fixture's header carries current profile 0x0010. */
    uint8_t cfg[64];
    size_t  cfg_len = load_fixture("getconfig_dvdrom_current.bin", cfg, sizeof cfg);
    mos_fake_set_getconfig_reply(0x00 /*GOOD*/, cfg, cfg_len);
    mos_fake_set_tur(0x00 /*GOOD*/, NULL);   /* ready */

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);
    EXPECT_EQ(MOS_OK, err);

    const mos_state_result *r = NULL;
    mos_error qerr = mos_query_state(h, &r);
    EXPECT_EQ(MOS_OK, qerr);
    EXPECT(r != NULL);
    EXPECT_EQ(MOS_STATE_READY, mos_state_result_state(r));
    EXPECT_EQ(0x0010, mos_state_result_current_profile(r));

    /* Identity flowed through the DR directory seam. Reading the string
       accessors under ASan is the O-3 lifetime check (seam contract):
       the pointers must reference handle-owned storage valid while the
       result lives — a dangle is an ASan abort here, not a lucky read. */
    EXPECT_EQ(4, mos_handle_bsd_unit(h));
    const char *vendor  = mos_state_result_vendor(r);
    const char *product = mos_state_result_product(r);
    EXPECT(vendor  && strcmp(vendor,  "HL-DT-ST") == 0);
    EXPECT(product && strcmp(product, "DVDROM")   == 0);

    mos_close(h);

    /* §5.5: the READY route takes no exclusive lock; balance stays 0. */
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_open_index_no_drive_is_no_device)
{
    mos_fake_reset();
    mos_fake_set_no_drive();

    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h == NULL);
    EXPECT_EQ(MOS_ERR_NO_DEVICE, err);
    return 0;
}

/* Fixed-format (0x70) 18-byte sense. */
static void make_sense(uint8_t out[18], uint8_t sk, uint8_t asc, uint8_t ascq)
{
    memset(out, 0, 18);
    out[0]  = 0x70;
    out[2]  = sk;
    out[7]  = 10;     /* additional sense length through byte 17 */
    out[12] = asc;
    out[13] = ascq;
}

/* 8-byte GESN media-event reply: Event Data Length 6 (excludes its own
   two bytes), NEA clear, class Media (4), supported-class bit Media,
   media status byte 5 (bit0 = DoorOpen). */
static void make_gesn(uint8_t out[8], bool door_open)
{
    memset(out, 0, 8);
    out[1] = 0x06;
    out[2] = 0x04;
    out[3] = 0x10;
    out[5] = door_open ? 0x01 : 0x00;
}

/* Drive a not-ready query: TUR CHECK CONDITION with (sk,asc,ascq) and a
   scripted GESN; returns the classified state via *state. */
static int query_not_ready(uint8_t sk, uint8_t asc, uint8_t ascq,
                           bool gesn_door_open, int32_t *state)
{
    uint8_t sense[18], gesn[8];
    make_sense(sense, sk, asc, ascq);
    make_gesn(gesn, gesn_door_open);
    mos_fake_set_bsd_unit(-1);                 /* no media: no IOMedia child */
    mos_fake_set_tur(0x02 /*CHECK CONDITION*/, sense);
    mos_fake_set_raw_reply(0x00 /*GOOD*/, gesn, 8, 8, NULL);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);
    EXPECT_EQ(-1, mos_handle_bsd_unit(h));     /* media absent ⇒ no unit */

    const mos_state_result *r = NULL;
    EXPECT_EQ(MOS_OK, mos_query_state(h, &r));
    *state = (int32_t)mos_state_result_state(r);

    /* O-1 shape: profile is suppressed on every non-READY state — a
       fresh query on an empty drive must report 0, never garbage. */
    EXPECT_EQ(0x0000, mos_state_result_current_profile(r));

    mos_close(h);
    return 0;
}

TEST(adapter_not_ready_gesn_closed_is_empty)
{
    mos_fake_reset();
    int32_t state = -1;
    int rc = query_not_ready(0x02, 0x3A, 0x00, /*door_open=*/false, &state);
    if (rc) return rc;
    EXPECT_EQ(MOS_STATE_EMPTY, state);

    /* §5.5, now LIVE: the GESN probe must have taken the exclusive lock
       exactly once and released it — acquired-and-released, not skipped. */
    EXPECT_EQ(1, mos_fake_lock_acquires());
    EXPECT_EQ(0, mos_fake_lock_balance());

    /* Pin the ONE raw CDB mos authors, byte for byte (ARCHITECTURE §4.2):
       GESN 0x4A, Polled, Media class, allocation length 8. */
    uint8_t cdb[16];
    size_t  len = mos_fake_last_cdb(cdb);
    static const uint8_t want[10] =
        { 0x4A, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00 };
    EXPECT_EQ(10, (int)len);
    EXPECT(memcmp(cdb, want, 10) == 0);
    return 0;
}

TEST(adapter_not_ready_gesn_open_is_open)
{
    mos_fake_reset();
    int32_t state = -1;
    int rc = query_not_ready(0x02, 0x3A, 0x02, /*door_open=*/true, &state);
    if (rc) return rc;
    EXPECT_EQ(MOS_STATE_OPEN, state);
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_becoming_ready_is_loading)
{
    mos_fake_reset();
    int32_t state = -1;
    /* 02/04/01 LOGICAL UNIT IS IN PROCESS OF BECOMING READY, tray closed. */
    int rc = query_not_ready(0x02, 0x04, 0x01, /*door_open=*/false, &state);
    if (rc) return rc;
    EXPECT_EQ(MOS_STATE_LOADING, state);
    return 0;
}

TEST(adapter_lock_denied_falls_back_to_sense)
{
    /* Another client holds the drive: ObtainExclusiveAccess fails, GESN
       yields no authoritative bit, and 3A/00 alone cannot place the
       tray — the honest answer is EMPTY_OR_OPEN. The lock must never
       have been acquired. */
    mos_fake_reset();
    mos_fake_set_exclusive_denied(true);

    uint8_t sense[18];
    make_sense(sense, 0x02, 0x3A, 0x00);
    mos_fake_set_bsd_unit(-1);
    mos_fake_set_tur(0x02, sense);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    const mos_state_result *r = NULL;
    EXPECT_EQ(MOS_OK, mos_query_state(h, &r));
    EXPECT_EQ(MOS_STATE_EMPTY_OR_OPEN, mos_state_result_state(r));

    mos_close(h);
    EXPECT_EQ(0, mos_fake_lock_acquires());
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_disc_info_replays_fixtures)
{
    /* The two committed READ DISC INFORMATION fixtures through the real
       convenience wrapper: blank CD-R and finalized CD-ROM. */
    uint8_t buf[64];
    size_t  n;

    mos_fake_reset();
    n = load_fixture("readdiscinfo_blank_cdr.bin", buf, sizeof buf);
    mos_fake_set_readdiscinfo_reply(0x00, buf, n);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    const mos_disc_info *di = NULL;
    EXPECT_EQ(MOS_OK, mos_query_disc_info(h, &di));
    EXPECT_EQ(MOS_DISC_BLANK, mos_disc_info_status(di));

    n = load_fixture("readdiscinfo_complete_cdrom.bin", buf, sizeof buf);
    mos_fake_set_readdiscinfo_reply(0x00, buf, n);
    EXPECT_EQ(MOS_OK, mos_query_disc_info(h, &di));
    EXPECT_EQ(MOS_DISC_COMPLETE, mos_disc_info_status(di));

    mos_close(h);
    return 0;
}

TEST(adapter_toc_round_trip_and_fail_closed)
{
    /* Synthetic two-track audio TOC, format 0000b: header (len=26,
       first=1, last=2), tracks 1 (lba 0) and 2 (lba 18000), lead-out
       0xAA (lba 210895). Same shape the pure parser's fixtures pin. */
    static const uint8_t toc[] = {
        0x00, 0x1A, 0x01, 0x02,
        0x00, 0x10, 0x01, 0x00,  0x00, 0x00, 0x00, 0x00,
        0x00, 0x10, 0x02, 0x00,  0x00, 0x00, 0x46, 0x50,
        0x00, 0x10, 0xAA, 0x00,  0x00, 0x03, 0x37, 0xCF,
    };
    mos_fake_reset();
    mos_fake_set_toc_reply(0x00, toc, sizeof toc);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    const mos_toc *t = NULL;
    EXPECT_EQ(MOS_OK, mos_query_toc(h, &t));
    EXPECT_EQ(1, mos_toc_first_track(t));
    EXPECT_EQ(2, mos_toc_last_track(t));
    EXPECT_EQ(2, (int)mos_toc_track_count(t));
    EXPECT(mos_toc_have_leadout(t));
    EXPECT_EQ(210895u, mos_toc_leadout_lba(t));
    EXPECT_EQ(18000u, mos_toc_track_start_lba(t, 1));
    EXPECT(!(mos_toc_track_control(t, 0) & 0x4));   /* audio */

    /* Hostile: non-ascending tracks refuse the whole TOC (*out NULL). */
    static const uint8_t bad[] = {
        0x00, 0x12, 0x01, 0x02,
        0x00, 0x10, 0x02, 0x00,  0x00, 0x00, 0x00, 0x00,
        0x00, 0x10, 0x01, 0x00,  0x00, 0x00, 0x46, 0x50,
    };
    mos_fake_set_toc_reply(0x00, bad, sizeof bad);
    EXPECT_EQ(MOS_ERR_IO, mos_query_toc(h, &t));
    EXPECT(t == NULL);

    /* Transport injection maps the IOReturn, not MOS_ERR_IO. */
    mos_fake_set_method_ioreturn(MOS_FAKE_METHOD_READTOC, 0xE00002D6u);
    EXPECT_EQ(MOS_ERR_TIMEOUT, mos_query_toc(h, &t));

    mos_close(h);
    return 0;
}

TEST(adapter_da_volume_lookup_modalities)
{
    char name[256], path[1024];

    /* Unknown disk: no description, not mounted. */
    mos_fake_reset();
    EXPECT(!mos_internal_da_volume("disk4", name, sizeof name,
                                   path, sizeof path));
    EXPECT(name[0] == 0 && path[0] == 0);

    /* Mounted with a name. */
    mos_fake_set_da_volume("ARRIVAL_4K_UHD", "/Volumes/ARRIVAL_4K_UHD");
    EXPECT(mos_internal_da_volume("disk4", name, sizeof name,
                                  path, sizeof path));
    EXPECT(strcmp(name, "ARRIVAL_4K_UHD") == 0);
    EXPECT(strcmp(path, "/Volumes/ARRIVAL_4K_UHD") == 0);

    /* Present but unmounted (description without VolumePath):
       false, both empty — DA describes unmounted media too. */
    mos_fake_set_da_volume("GHOST", NULL);
    EXPECT(!mos_internal_da_volume("disk4", name, sizeof name,
                                   path, sizeof path));
    EXPECT(name[0] == 0 && path[0] == 0);

    /* Hostile/degenerate args stay in bounds and report unmounted. */
    EXPECT(!mos_internal_da_volume(NULL, name, sizeof name,
                                   path, sizeof path));
    EXPECT(!mos_internal_da_volume("", name, sizeof name,
                                   path, sizeof path));
    return 0;
}

int main(void)
{
    printf("adapter one-shot (headless, link-seam fake):\n");
    RUN(adapter_open_index_query_ready);
    RUN(adapter_open_index_no_drive_is_no_device);
    RUN(adapter_not_ready_gesn_closed_is_empty);
    RUN(adapter_not_ready_gesn_open_is_open);
    RUN(adapter_becoming_ready_is_loading);
    RUN(adapter_lock_denied_falls_back_to_sense);
    RUN(adapter_disc_info_replays_fixtures);
    RUN(adapter_toc_round_trip_and_fail_closed);
    RUN(adapter_da_volume_lookup_modalities);
    printf("\n%d run, %d passed, %d failed\n",
           mos_tests_run, mos_tests_run - mos_tests_failed, mos_tests_failed);
    return mos_tests_failed ? 1 : 0;
}
