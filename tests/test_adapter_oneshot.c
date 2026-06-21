/*
 * test_adapter_oneshot.c — runs the REAL one-shot adapter TUs (mos_scsi.c /
 * mos_state.c / mos_dr.c) headless against the link-seam fake of IOKit +
 * DiscRecording (tests/fake/mos_fake_apple.c), fed committed MMC fixtures.
 * Phase 1 of doc/research/2026-06-11-headless-adapter-emulation.md.
 *
 * Separate program from mos_tests (mos_pure only): links the adapter object
 * code + the fake + real CoreFoundation, NO IOKit/DiscRecording frameworks.
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

    /* Committed DVD-ROM GET CONFIGURATION reply. get_current_profile issues
       RT=2 and reads the 8-byte header; this fixture carries profile 0x0010. */
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
       accessors under ASan is seam contract O-3: the pointers must
       reference handle-owned storage valid while the result lives — a
       dangle aborts ASan here, not a lucky read. */
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

/* 8-byte GESN media-event reply: Event Data Length 6 (excludes its own two
   bytes), NEA clear, class Media (4), supported-class bit Media, media
   status byte 5 (bit0 = DoorOpen). The field map other tests cite. */
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

    /* O-1 shape: profile suppressed on every non-READY state — an empty
       drive reports 0, never garbage. */
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

    /* §5.5 LIVE: the GESN probe took the exclusive lock exactly once and
       released it — acquired-and-released, not skipped. */
    EXPECT_EQ(1, mos_fake_lock_acquires());
    EXPECT_EQ(0, mos_fake_lock_balance());

    /* Pin the GESN raw CDB mos authors on the not-ready branch, byte for
       byte (ARCHITECTURE §4.2): GESN 0x4A, Polled, Media class, allocation
       length 8. */
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
       yields no bit, and 3A/00 alone can't place the tray → EMPTY_OR_OPEN.
       The lock must never have been acquired. */
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

/* R3 mos_state.c audit (2026-06-20): a media swap/removal between the query's
   identity capture and its TUR/GESN must not publish a temporally mixed result.
   The S1/S2 coherence retry re-observes once, then refuses (BUSY) on churn. */

TEST(adapter_media_swap_to_absent_retries_coherent)   /* R3 sequence A */
{
    mos_fake_reset();                        /* media present, unit 4 */
    uint8_t sense[18], gesn[8];
    make_sense(sense, 0x02, 0x3A, 0x00);     /* not ready, no medium */
    make_gesn(gesn, /*door_open=*/false);
    mos_fake_set_tur(0x02 /*CHECK CONDITION*/, sense);
    mos_fake_set_raw_reply(0x00 /*GOOD*/, gesn, 8, 8, NULL);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* The medium vanishes between the query's S1 capture and its S2 confirm. */
    mos_fake_set_media_swap_after_first_capture(-1, 0);

    const mos_state_result *r = NULL;
    EXPECT_EQ(MOS_OK, mos_query_state(h, &r));
    EXPECT(r != NULL);
    EXPECT_EQ(MOS_STATE_EMPTY, mos_state_result_state(r));
    /* No stale identity: the published unit is the coherent post-swap -1, not
       S1's 4 (the EMPTY-with-nonzero-bsd_unit incoherence R3 demonstrated). */
    EXPECT_EQ(-1, mos_handle_bsd_unit(h));
    EXPECT_EQ(4u, mos_fake_capture_walks());  /* S1,S2 then a retry S1',S2' */
    mos_close(h);
    return 0;
}

TEST(adapter_media_swap_between_discs_retries_coherent)   /* R3 sequence B */
{
    mos_fake_reset();                        /* media present, unit 4 */
    uint8_t cfg[64];
    size_t  cfg_len = load_fixture("getconfig_dvdrom_current.bin", cfg, sizeof cfg);
    mos_fake_set_getconfig_reply(0x00, cfg, cfg_len);
    mos_fake_set_tur(0x00 /*GOOD*/, NULL);   /* ready */

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* Disc A at S1, disc B (same diskN, fresh IOMedia id) by S2 — only media_id
       reveals the swap; bsd_unit stays 4, so it cannot. The retry must fire on
       the id alone and republish the coherent post-swap generation. */
    mos_fake_set_media_swap_after_first_capture(4, 0x100000999ull);

    const mos_state_result *r = NULL;
    EXPECT_EQ(MOS_OK, mos_query_state(h, &r));
    EXPECT(r != NULL);
    EXPECT_EQ(MOS_STATE_READY, mos_state_result_state(r));
    EXPECT_EQ(4u, mos_fake_capture_walks());  /* media_id change forced a retry */
    mos_close(h);
    return 0;
}

TEST(adapter_media_churn_refuses_mixed_observation)
{
    mos_fake_reset();
    uint8_t cfg[64];
    size_t  cfg_len = load_fixture("getconfig_dvdrom_current.bin", cfg, sizeof cfg);
    mos_fake_set_getconfig_reply(0x00, cfg, cfg_len);
    mos_fake_set_tur(0x00 /*GOOD*/, NULL);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* Every capture mints a fresh media_id: no two observations agree, so the
       query exhausts its one retry and refuses rather than publish a mix. */
    mos_fake_set_media_churn(true);

    const mos_state_result *r = NULL;
    mos_error q = mos_query_state(h, &r);
    EXPECT_EQ(MOS_ERR_BUSY, q);
    EXPECT(r == NULL);
    mos_close(h);
    return 0;
}

/* Compound-query coherence (2026-06-20): mos_query_drive_perf and
   mos_query_physical_structure each issue TWO media-dependent reads, so a
   media swap between them could splice disc A's first read with disc B's
   second. They carry the same S1/S2 generation-coherence retry as
   mos_query_state / mos_query_capacity: re-observe once on a swap, refuse
   (BUSY) on churn. */

/* One Nominal Performance Descriptor reporting 5540 kB/s (Start == End perf);
   24 bytes = 8-byte header (Data Length = 20) + one 16-byte descriptor. */
static const uint8_t k_perf_5540[] = {
    0,0,0,20,        /* Performance Data Length (BE) = bytes after byte 3   */
    0,0,0,0,         /* [4] write/except echo  [5..7] reserved              */
    0,0,0,0,         /* desc Start LBA                                       */
    0,0,0x15,0xA4,   /* desc Start Performance = 5540 kB/s                   */
    0,0,0,0,         /* desc End LBA                                         */
    0,0,0x15,0xA4,   /* desc End Performance   = 5540 kB/s                   */
};

/* Minimal Physical Format Information reply: 21 bytes = 4-byte header
   (declared length 19 => trusted end 21, the PHYS_MIN_LEN floor) + base[0..16].
   base[0] = 0x01 => book_type 0 (DVD-ROM), part_version 1. */
static const uint8_t k_phys_dvdrom[] = {
    0x00,0x13, 0x00,0x00,   /* Disc Structure Data Length = 19, reserved     */
    0x01,                   /* base[0]  book_type 0 | part_version 1         */
    0x00,0x00,0x00,         /* base[1..3] size/rate, layers/path/type, dens. */
    0x00,                   /* base[4]  reserved                             */
    0x00,0x00,0x00,         /* base[5..7]  starting PSN                       */
    0x00,                   /* base[8]  reserved                             */
    0x00,0x00,0x00,         /* base[9..11] end PSN                            */
    0x00,                   /* base[12] reserved                             */
    0x00,0x00,0x00,         /* base[13..15] end PSN layer 0                   */
    0x00,                   /* base[16] bca                                   */
};

TEST(adapter_perf_swap_between_discs_retries_coherent)
{
    mos_fake_reset();
    mos_fake_set_perf_reply(0x00 /*GOOD*/, k_perf_5540, sizeof k_perf_5540);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* Disc B (fresh media_id, same diskN) arrives between the two GET
       PERFORMANCE reads: S1 != S2 once, then the retry observes B twice. */
    mos_fake_set_media_swap_after_first_capture(4, 0x100000999ull);

    const mos_drive_perf *p = NULL;
    EXPECT_EQ(MOS_OK, mos_query_drive_perf(h, &p));
    EXPECT(p != NULL);
    EXPECT(mos_drive_perf_have(p));
    EXPECT_EQ(5540u, mos_drive_perf_max_read_kbps(p));
    EXPECT_EQ(4u, mos_fake_capture_walks());   /* S1,S2 then retry S1',S2' */
    mos_close(h);
    return 0;
}

TEST(adapter_perf_churn_refuses_mixed_observation)
{
    mos_fake_reset();
    mos_fake_set_perf_reply(0x00 /*GOOD*/, k_perf_5540, sizeof k_perf_5540);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* Every capture mints a fresh media_id: the one retry exhausts, so the
       query refuses rather than splice read perf from one disc with write
       perf from another. */
    mos_fake_set_media_churn(true);

    const mos_drive_perf *p = NULL;
    EXPECT_EQ(MOS_ERR_BUSY, mos_query_drive_perf(h, &p));
    EXPECT(p == NULL);
    mos_close(h);
    return 0;
}

TEST(adapter_physical_swap_between_discs_retries_coherent)
{
    mos_fake_reset();
    mos_fake_set_disc_structure_reply(0x00 /*GOOD*/, k_phys_dvdrom,
                                      sizeof k_phys_dvdrom);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* Swap between the FORMAT 0x00 (physical) and 0x01 (copyright) reads. */
    mos_fake_set_media_swap_after_first_capture(4, 0x100000999ull);

    const mos_physical_structure *d = NULL;
    EXPECT_EQ(MOS_OK, mos_query_physical_structure(h, &d));
    EXPECT(d != NULL);
    EXPECT(mos_physical_structure_have_physical(d));
    EXPECT_EQ(0u, mos_physical_structure_book_type(d));     /* DVD-ROM       */
    EXPECT_EQ(1u, mos_physical_structure_part_version(d));
    EXPECT_EQ(4u, mos_fake_capture_walks());
    mos_close(h);
    return 0;
}

TEST(adapter_physical_churn_refuses_mixed_observation)
{
    mos_fake_reset();
    mos_fake_set_disc_structure_reply(0x00 /*GOOD*/, k_phys_dvdrom,
                                      sizeof k_phys_dvdrom);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    mos_fake_set_media_churn(true);

    const mos_physical_structure *d = NULL;
    EXPECT_EQ(MOS_ERR_BUSY, mos_query_physical_structure(h, &d));
    EXPECT(d == NULL);
    mos_close(h);
    return 0;
}

TEST(adapter_disc_info_replays_fixtures)
{
    /* Two committed READ DISC INFORMATION fixtures through the real
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
    /* Synthetic two-track audio TOC, format 0000b: header (len=26, first=1,
       last=2), tracks 1 (lba 0) / 2 (lba 18000), lead-out 0xAA (lba 210895).
       Same shape the pure parser's fixtures pin. */
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
    const uint64_t MID = 0x100000456ull;   /* the fake's default media id */

    /* Unknown disk: the IOMedia resolves but DA has no description → not
       mounted. */
    mos_fake_reset();
    EXPECT(!mos_internal_da_volume(MID, name, sizeof name, path, sizeof path));
    EXPECT(name[0] == 0 && path[0] == 0);

    /* Mounted with a name. */
    mos_fake_set_da_volume("ARRIVAL", "/Volumes/ARRIVAL");
    EXPECT(mos_internal_da_volume(MID, name, sizeof name, path, sizeof path));
    EXPECT(strcmp(name, "ARRIVAL") == 0);
    EXPECT(strcmp(path, "/Volumes/ARRIVAL") == 0);

    /* Present but unmounted (description without VolumePath): false, both
       empty — DA describes unmounted media too. */
    mos_fake_set_da_volume("GHOST", NULL);
    EXPECT(!mos_internal_da_volume(MID, name, sizeof name, path, sizeof path));
    EXPECT(name[0] == 0 && path[0] == 0);

    /* Vanished disc: a stale / wrong media id resolves to NO IOMedia (its
       registry entry is gone, never reused), so DA is never consulted and
       nothing is misattributed — even with a mount present. */
    mos_fake_set_da_volume("ARRIVAL", "/Volumes/ARRIVAL");
    EXPECT(!mos_internal_da_volume(0xDEADBEEFull, name, sizeof name,
                                   path, sizeof path));
    EXPECT(name[0] == 0 && path[0] == 0);

    /* Zero media id (identity unknown / bridge fallback): fail closed. */
    EXPECT(!mos_internal_da_volume(0, name, sizeof name, path, sizeof path));
    EXPECT(name[0] == 0 && path[0] == 0);

    /* ENDPOINT IDENTITY GUARD (A2): the media id resolves and DA reports a
       mount, but the name-backed DADiskRef now resolves to a DIFFERENT IOMedia
       (a diskN reused mid-lookup). DADiskCreateFromIOMedia is name-delegated, so
       DADiskCopyDescription could return the wrong disc's volume; the post-read
       DADiskCopyIOMedia re-check sees id != media_id and REFUSES rather than
       attribute another disc's mount path. */
    mos_fake_set_da_volume("OTHERDISC", "/Volumes/OTHERDISC");
    mos_fake_set_da_media_id(0x100000999ull);  /* != the default media id */
    EXPECT(!mos_internal_da_volume(MID, name, sizeof name, path, sizeof path));
    EXPECT(name[0] == 0 && path[0] == 0);

    return 0;
}

TEST(adapter_query_volume_gates_on_nub)
{
    char name[256], path[1024];
    bool mounted = true;

    /* Media absent (bsd_unit -1): MOS_OK, unmounted, DA never asked — the
       fake (da_volume set) would have answered if consulted. */
    mos_fake_reset();
    mos_fake_set_bsd_unit(-1);
    mos_fake_set_da_volume("SHOULD_NOT_SURFACE", "/Volumes/NO");

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);
    EXPECT_EQ(MOS_OK, mos_query_volume(h, &mounted, name, sizeof name,
                                       path, sizeof path));
    EXPECT(!mounted && name[0] == 0 && path[0] == 0);
    mos_close(h);

    /* Nub present and mounted: fields surface through the public API. */
    mos_fake_set_bsd_unit(4);
    h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);
    EXPECT_EQ(MOS_OK, mos_query_volume(h, &mounted, name, sizeof name,
                                       path, sizeof path));
    EXPECT(mounted);
    EXPECT(strcmp(name, "SHOULD_NOT_SURFACE") == 0);
    EXPECT(strcmp(path, "/Volumes/NO") == 0);

    EXPECT_EQ(MOS_ERR_INVALID_ARG,
              mos_query_volume(NULL, &mounted, name, sizeof name,
                               path, sizeof path));
    mos_close(h);
    return 0;
}

TEST(adapter_drive_caps_roundtrip_and_absent)
{
    /* RT=0 reply: header (profile 0x0040) + Profile List + AACS feature
       (BEC set, AACS version 68). */
    static const uint8_t cfg_aacs[] = {
        0,0,0,16,  0,0, 0x00,0x40,
        0x00,0x00, 0x03, 0x00,
        0x01,0x0D, 0x09, 0x04,  0x03, 0x00, 0x01, 68,
    };
    mos_fake_reset();
    mos_fake_set_getconfig_reply(0x00, cfg_aacs, sizeof cfg_aacs);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    const mos_drive_caps *c = NULL;
    EXPECT_EQ(MOS_OK, mos_query_drive_caps(h, &c));
    EXPECT(mos_drive_caps_aacs(c));
    EXPECT(mos_drive_caps_bus_encryption(c));
    EXPECT_EQ(68, mos_drive_caps_aacs_version(c));
    /* Current Profile from the RT=0 header (0x0040 = BD) flows to the
       accessor — the loaded-medium class used for speed 1x scaling. */
    EXPECT_EQ(0x0040, mos_drive_caps_current_profile(c));

    /* Non-BD drive: feature list without 0x010D — aacs=false is data. */
    static const uint8_t cfg_plain[] = {
        0,0,0,8,  0,0, 0x00,0x10,
        0x00,0x00, 0x03, 0x00,
    };
    mos_fake_set_getconfig_reply(0x00, cfg_plain, sizeof cfg_plain);
    EXPECT_EQ(MOS_OK, mos_query_drive_caps(h, &c));
    EXPECT(!mos_drive_caps_aacs(c));
    EXPECT(!mos_drive_caps_bus_encryption(c));
    EXPECT_EQ(0, mos_drive_caps_aacs_version(c));
    EXPECT_EQ(0x0010, mos_drive_caps_current_profile(c));   /* DVD */

    /* Transport injection maps the IOReturn. */
    mos_fake_set_method_ioreturn(MOS_FAKE_METHOD_GETCONFIG, 0xE00002D6u);
    EXPECT_EQ(MOS_ERR_TIMEOUT, mos_query_drive_caps(h, &c));
    EXPECT(c == NULL);

    mos_close(h);
    return 0;
}

typedef struct { int n; uint16_t codes[8]; bool cur[8]; int stop_after; } feat_ctx;
static bool feat_cb(const mos_feature_info_t *f, void *vctx)
{
    feat_ctx *c = (feat_ctx *)vctx;
    if (c->n < 8) {
        c->codes[c->n] = mos_feature_info_code(f);
        c->cur[c->n]   = mos_feature_info_current(f);
    }
    c->n++;
    return c->stop_after == 0 || c->n < c->stop_after;
}

TEST(adapter_feature_enumeration_order_and_stop)
{
    /* Profile List + Core + AACS, reply order. */
    static const uint8_t cfg[] = {
        0,0,0,20,  0,0, 0x00,0x40,
        0x00,0x00, 0x03, 0x00,
        0x00,0x01, 0x01, 0x00,
        0x01,0x0D, 0x09, 0x04,  0x03, 0x00, 0x01, 68,
    };
    mos_fake_reset();
    mos_fake_set_getconfig_reply(0x00, cfg, sizeof cfg);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    feat_ctx c = {0};
    EXPECT_EQ(MOS_OK, mos_enumerate_features(h, feat_cb, &c));
    EXPECT_EQ(3, c.n);
    EXPECT_EQ(0x0000, c.codes[0]);
    EXPECT_EQ(0x0001, c.codes[1]);
    EXPECT_EQ(0x010D, c.codes[2]);
    EXPECT(c.cur[0] && c.cur[1] && c.cur[2]);

    /* Early stop is honored and is not an error. */
    feat_ctx c2 = {0}; c2.stop_after = 1;
    EXPECT_EQ(MOS_OK, mos_enumerate_features(h, feat_cb, &c2));
    EXPECT_EQ(1, c2.n);

    EXPECT_EQ(MOS_ERR_INVALID_ARG, mos_enumerate_features(h, NULL, NULL));
    mos_close(h);
    return 0;
}

TEST(adapter_disc_id_decodes_and_fails_closed)
{
    /* One-DI-unit BD reply: disc type BDR, MILLEN/MR1, rev 0. */
    static const uint8_t di[116] = {
        0x00, 114, 0x00, 0x00,  'D','I', 0x00,0x00,
        [12]='B',[13]='D',[14]='R',
        [104]='M',[105]='I',[106]='L',[107]='L',[108]='E',[109]='N',
        [110]='M',[111]='R',[112]='1',
        [115]='0',
    };
    mos_fake_reset();
    mos_fake_set_disc_structure_reply(0x00, di, sizeof di);

    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    const mos_disc_id *id = NULL;
    EXPECT_EQ(MOS_OK, mos_query_disc_id(h, &id));
    EXPECT(strcmp(mos_disc_id_disc_type(id), "BDR") == 0);
    EXPECT(strcmp(mos_disc_id_manufacturer(id), "MILLEN") == 0);
    EXPECT(strcmp(mos_disc_id_media_type(id), "MR1") == 0);
    EXPECT(strcmp(mos_disc_id_revision(id), "0") == 0);

    /* Non-BD media (no 'DI' structure): GOOD status but the decode refuses
       → MOS_ERR_IO, *out NULL. */
    static const uint8_t notdi[116] = { 0x00, 114, 0,0, 'X','X' };
    mos_fake_set_disc_structure_reply(0x00, notdi, sizeof notdi);
    EXPECT_EQ(MOS_ERR_IO, mos_query_disc_id(h, &id));
    EXPECT(id == NULL);

    /* Transport injection maps the IOReturn. */
    mos_fake_set_method_ioreturn(MOS_FAKE_METHOD_READDISCSTRUCT, 0xE00002D6u);
    EXPECT_EQ(MOS_ERR_TIMEOUT, mos_query_disc_id(h, &id));

    EXPECT_EQ(MOS_ERR_INVALID_ARG, mos_query_disc_id(h, NULL));
    mos_close(h);
    return 0;
}

/* Pin one tray verb: script a raw reply, issue it, assert the authored CDB
   bytes, the balanced lock (acquire-on-call / release-on-return), and the
   classified outcome. */
static int pin_tray_cdb(mos_handle_t *h, mos_error (*call)(mos_handle_t *,
                        mos_tray_outcome *), const uint8_t want[6],
                        mos_tray_outcome want_outcome)
{
    mos_fake_set_raw_reply(0x00 /*GOOD*/, NULL, 0, 0, NULL);
    mos_tray_outcome out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, call(h, &out));
    EXPECT_EQ(want_outcome, out);
    EXPECT_EQ(0, mos_fake_lock_balance());
    uint8_t cdb[16];
    size_t len = mos_fake_last_cdb(cdb);
    EXPECT_EQ(6, (int)len);
    EXPECT(memcmp(cdb, want, 6) == 0);
    return 0;
}

/* Thin adapters so pin_tray_cdb can take a uniform (handle, outcome*)
   signature for the close/lock/unlock variants. */
static mos_error call_close(mos_handle_t *h, mos_tray_outcome *o)
{ return mos_tray_close(h, o, NULL); }
static mos_error call_lock(mos_handle_t *h, mos_tray_outcome *o)
{ return mos_tray_lock(h, o, NULL); }
static mos_error call_unlock(mos_handle_t *h, mos_tray_outcome *o)
{ return mos_tray_unlock(h, o, NULL); }
static mos_error call_eject(mos_handle_t *h, mos_tray_outcome *o)
{ return mos_tray_eject(h, false, o, NULL); }

TEST(adapter_tray_cdbs_pinned_byte_for_byte)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* START STOP UNIT 0x1B: eject LoEj1 START0 -> 0x02, close -> 0x03. */
    static const uint8_t eject[6]  = { 0x1B, 0, 0, 0, 0x02, 0 };
    static const uint8_t close_[6] = { 0x1B, 0, 0, 0, 0x03, 0 };
    /* PREVENT ALLOW 0x1E byte4 {PERSISTENT,PREVENT} (04-349r1 Table 8). lock
       sets the BASIC Prevent (0x01 — the hard removal block; the Persistent
       Prevent 0x03 is retired, it does not block the button on macOS); unlock
       clears BOTH (0x00 then 0x02), so its LAST CDB — the one mos_fake_last_cdb
       returns — is the persistent ALLOW 0x02 (unlock-issues-both is pinned
       separately). */
    static const uint8_t lock_basic[6]  = { 0x1E, 0, 0, 0, 0x01, 0 };
    static const uint8_t unlock_last[6] = { 0x1E, 0, 0, 0, 0x02, 0 };

    int rc = 0;
    rc |= pin_tray_cdb(h, call_eject,    eject,       MOS_TRAY_DONE);
    rc |= pin_tray_cdb(h, call_close,    close_,      MOS_TRAY_DONE);
    rc |= pin_tray_cdb(h, call_lock,     lock_basic,  MOS_TRAY_DONE);
    rc |= pin_tray_cdb(h, call_unlock,   unlock_last, MOS_TRAY_DONE);
    mos_close(h);
    return rc;
}

TEST(adapter_tray_eject_graceful_unmounts_and_ejects)
{
    /* `tray eject` GRACEFULLY unmounts a mounted (idle) disc, then ejects — BOTH
       the default and --force (force only adds LOCK clearing, never fs forcing).
       The mount makes the first eject's ObtainExclusiveAccess BUSY; the graceful
       DADiskUnmount clears the idle mount; the re-eject succeeds → DONE. */
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* Default (no force) now unmounts gracefully (matching `drutil eject`). */
    mos_fake_set_mounted_busy(true);
    mos_tray_outcome out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_eject(h, /*force=*/false, &out, NULL));
    EXPECT_EQ(MOS_TRAY_DONE, out);
    EXPECT_EQ(0, mos_fake_lock_balance());

    /* --force takes the SAME graceful path on a mount (force is for LOCKS). */
    mos_fake_set_mounted_busy(true);
    out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_eject(h, /*force=*/true, &out, NULL));
    EXPECT_EQ(MOS_TRAY_DONE, out);
    EXPECT_EQ(0, mos_fake_lock_balance());

    mos_close(h);
    return 0;
}

TEST(adapter_tray_eject_busy_fs_surfaces_busy_never_forces)
{
    /* A BUSY filesystem (open handles): the graceful DADiskUnmount DISSENTS, the
       mount stays, and mos surfaces MOS_ERR_BUSY — it NEVER forces. True for BOTH
       the default and --force (--force clears LOCKS, never fights the fs). This is
       the whole point: mos has no data-loss path. */
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    mos_fake_set_mounted_busy(true);
    mos_fake_set_unmount_refused(true);   /* graceful unmount dissents (busy fs) */

    mos_tray_outcome out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_ERR_BUSY, mos_tray_eject(h, /*force=*/false, &out, NULL));
    EXPECT_EQ(0, mos_fake_lock_balance());

    /* --force does not change it: still BUSY, the mount untouched (no force). */
    out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_ERR_BUSY, mos_tray_eject(h, /*force=*/true, &out, NULL));
    EXPECT_EQ(0, mos_fake_lock_balance());

    mos_close(h);
    return 0;
}

TEST(adapter_tray_eject_default_clears_os_mountlock)
{
    /* macOS arms a tray PREVENT when it MOUNTS a disc; that lock survives mos's
       graceful unmount, so the re-eject answers 5/53/02. A DEFAULT eject (no
       --force) of a mounted disc must clear that OS mount-lock after its own
       unmount and succeed — Finder/`drutil` semantics — because the lock it hit
       followed mos's unmount and is the OS's, not a deliberate one. */
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    mos_fake_set_mounted_busy(true);    /* a Finder/system mount */
    mos_fake_set_prevent_locked(true);  /* armed when macOS mounted it */

    mos_tray_outcome out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_eject(h, /*force=*/false, &out, NULL));
    EXPECT_EQ(MOS_TRAY_DONE, out);      /* unmount → clear OS lock → eject */
    EXPECT_EQ(0, mos_fake_lock_balance());
    mos_close(h);
    return 0;
}

TEST(adapter_tray_eject_cold_lock_needs_force)
{
    /* A COLD Prevent lock — a deliberately-locked idle drive (a robot's `mos
       tray lock`), no mount in play, so no graceful unmount precedes the eject.
       The default eject leaves it REFUSED_LOCKED (mos does not clear a lock it
       did not just cause); --force clears both Prevent states and ejects. */
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    mos_fake_set_prevent_locked(true);  /* locked, but NOT mounted */

    mos_tray_outcome out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_eject(h, /*force=*/false, &out, NULL));
    EXPECT_EQ(MOS_TRAY_REFUSED_LOCKED, out);   /* default leaves it locked */
    EXPECT_EQ(0, mos_fake_lock_balance());

    /* --force clears the cold lock and ejects. */
    out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_eject(h, /*force=*/true, &out, NULL));
    EXPECT_EQ(MOS_TRAY_DONE, out);
    EXPECT_EQ(0, mos_fake_lock_balance());
    mos_close(h);
    return 0;
}

TEST(adapter_tray_locked_eject_classifies_refused_locked)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* CHECK CONDITION + 5/53/02 MEDIA REMOVAL PREVENTED. */
    uint8_t sense[18] = {0};
    sense[0] = 0x70; sense[2] = 0x05; sense[12] = 0x53; sense[13] = 0x02;
    mos_fake_set_raw_reply(0x02 /*CHECK CONDITION*/, NULL, 0, 0, sense);

    mos_tray_outcome out = (mos_tray_outcome)-1;
    uint8_t triple[3] = { 0xFF, 0xFF, 0xFF };
    EXPECT_EQ(MOS_OK, mos_tray_eject(h, false, &out, triple)); /* answered = MOS_OK */
    EXPECT_EQ(MOS_TRAY_REFUSED_LOCKED, out);
    /* The sense out-param carries the real triple back (Plan A). */
    EXPECT_EQ(0x05, triple[0]);
    EXPECT_EQ(0x53, triple[1]);
    EXPECT_EQ(0x02, triple[2]);
    EXPECT_EQ(0, mos_fake_lock_balance());
    mos_close(h);
    return 0;
}

TEST(adapter_tray_refused_other_carries_its_sense)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* A drive that rejects the lock's basic Prevent 0x01 with 5/24/00 (INVALID
       FIELD IN CDB) — refused_other, and the sense triple must reach the caller
       (the gap Plan A closed). */
    uint8_t sense[18] = {0};
    sense[0] = 0x70; sense[2] = 0x05; sense[12] = 0x24; sense[13] = 0x00;
    mos_fake_set_raw_reply(0x02 /*CHECK CONDITION*/, NULL, 0, 0, sense);

    mos_tray_outcome out = (mos_tray_outcome)-1;
    uint8_t triple[3] = {0};
    EXPECT_EQ(MOS_OK, mos_tray_lock(h, &out, triple));
    EXPECT_EQ(MOS_TRAY_REFUSED_OTHER, out);
    EXPECT_EQ(0x05, triple[0]);
    EXPECT_EQ(0x24, triple[1]);
    EXPECT_EQ(0x00, triple[2]);
    mos_close(h);
    return 0;
}

TEST(adapter_da_unmount_is_name_based)
{
    /* The GRACEFUL unmount is BY NAME (diskutil semantics): mos_internal_da_unmount
       unmounts the disc currently named "diskN". With a graceful unmount the
       wrong-target name-reuse is harmless (no force, no data loss), so no identity
       bind is needed. Here we pin the library contract: a non-empty name attempts
       the unmount (fake succeeds on an idle disc), degenerate names fail closed. */
    mos_fake_reset();
    EXPECT(mos_internal_da_unmount("disk4"));   /* succeeds by name */
    EXPECT(!mos_internal_da_unmount(NULL));      /* no name: false */
    EXPECT(!mos_internal_da_unmount(""));        /* empty name: false */
    return 0;
}

TEST(adapter_da_unmount_bounded_when_callback_never_fires)
{
    /* F1 regression: when the unmount callback never arrives (a void
       DASessionScheduleWithRunLoop failure / wedged daemon), the bounded
       run-loop wait in mos_internal_da_unmount must return false rather than
       hang forever. The fake schedules no real source, so CFRunLoopRunInMode's
       private mode is empty and returns kCFRunLoopRunFinished at once — the
       fast-false path. That this test RETURNS at all is the assertion the old
       DISPATCH_TIME_FOREVER wait would have failed. */
    mos_fake_reset();
    mos_fake_set_unmount_never_completes(true);
    EXPECT(!mos_internal_da_unmount("disk4"));   /* bounded false, no hang */
    return 0;
}

TEST(adapter_tray_exclusive_denied_is_negative_error)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* Another client holds the drive: ObtainExclusiveAccess fails, so the
       verb is a transport failure (negative), not an answered refusal. A peer
       client (EXCLUSIVE_ACCESS) is NOT translated to already_locked — only a
       mount (BUSY) is, since only a mount implies the OS removal lock. */
    mos_fake_set_exclusive_denied(true);
    mos_tray_outcome out = MOS_TRAY_DONE;
    uint8_t triple[3] = { 0xFF, 0xFF, 0xFF };
    mos_error e = mos_tray_lock(h, &out, triple);
    EXPECT(e != MOS_OK);
    EXPECT_EQ(MOS_ERR_EXCLUSIVE_ACCESS, e);
    /* Transport failure zeroes the sense out-param (no command answered). */
    EXPECT_EQ(0, triple[0]);
    EXPECT_EQ(0, triple[1]);
    EXPECT_EQ(0, triple[2]);
    EXPECT_EQ(0, mos_fake_lock_balance());
    mos_close(h);
    return 0;
}

TEST(adapter_tray_lock_on_mounted_is_already_locked)
{
    /* `lock` on a MOUNTED disc: ObtainExclusiveAccess returns BUSY (media still
       mounted), but a mounted disc is already removal-locked by macOS — the
       requested state already holds, so report ALREADY_LOCKED (a success), not
       MOS_ERR_BUSY. */
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    mos_fake_set_mounted_busy(true);
    mos_tray_outcome out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_lock(h, &out, NULL));
    EXPECT_EQ(MOS_TRAY_ALREADY_LOCKED, out);
    EXPECT_EQ(0, mos_fake_lock_balance());
    mos_close(h);
    return 0;
}

TEST(adapter_tray_unlock_clears_both_states)
{
    /* unlock issues basic ALLOW 0x00 then persistent ALLOW 0x02, so the tray
       ends unlocked whichever state held it. Model a locked drive, unlock, then
       confirm an eject is no longer refused. */
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    mos_fake_set_prevent_locked(true);
    mos_tray_outcome out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_unlock(h, &out, NULL));
    EXPECT_EQ(MOS_TRAY_DONE, out);

    /* The lock is cleared: an eject now succeeds instead of refused_locked. */
    out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_eject(h, /*force=*/false, &out, NULL));
    EXPECT_EQ(MOS_TRAY_DONE, out);
    EXPECT_EQ(0, mos_fake_lock_balance());
    mos_close(h);
    return 0;
}

TEST(adapter_raw_cdb_release_failure_poisons_handle)
{
    /* A ReleaseExclusiveAccess the kernel does not confirm (non-success) must
       NOT be reported as success. mos_internal_raw_cdb returns the unlock error
       (precedence over a GOOD execute), leaves the scalar outputs at their zero
       init, and POISONS the handle: the next raw call fails closed WITHOUT
       issuing a task, and mos_close makes a final release retry. (Apple defines
       a non-success release as "release not confirmed" — the in-kernel
       logical-unit driver may stay quiesced, so the flag must stay true.) */
    mos_fake_reset();
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL && err == MOS_OK);

    mos_fake_set_release_fail(true);

    /* START STOP UNIT (eject): a non-data CDB whose execute SUCCEEDS, so the
       only failure is the unlock. Outputs pre-set to non-zero to prove the
       error path does not copy task results over the zero init. */
    const uint8_t cdb[6] = { 0x1B, 0x00, 0x00, 0x00, 0x02, 0x00 };
    uint32_t st   = 0xABCD;
    uint64_t xfer = 99;
    uint8_t  sense[18];
    memset(sense, 0xAB, sizeof sense);

    mos_error e = mos_internal_raw_cdb(h, cdb, sizeof cdb, NULL, 0,
                                       MOS_XFER_NONE, 5000, &st, sense, &xfer);

    /* (1) Not MOS_OK — the unlock failure takes precedence over the GOOD execute. */
    EXPECT(e != MOS_OK);
    /* (2) Scalar outputs stay at raw_cdb's zero init (no copy on the error). */
    EXPECT(st == 0);
    EXPECT(xfer == 0);
    EXPECT(sense[0] == 0);
    /* The model kernel still holds the lock (release did not decrement). */
    EXPECT(mos_fake_lock_balance() == 1);

    /* (3) Poisoned: a second raw call fails closed and issues NO task. */
    int creates  = mos_fake_task_creates();
    int executes = mos_fake_execute_calls();
    e = mos_internal_raw_cdb(h, cdb, sizeof cdb, NULL, 0,
                             MOS_XFER_NONE, 5000, &st, sense, &xfer);
    EXPECT(e != MOS_OK);
    EXPECT(mos_fake_task_creates()  == creates);   /* no new task created     */
    EXPECT(mos_fake_execute_calls() == executes);  /* no new task executed    */
    EXPECT(mos_fake_lock_balance()  == 1);         /* not re-acquired          */

    /* (4) mos_close retries the release. Clear the injection so the retry
       confirms; verify a release was attempted and the lock cleared. */
    int rel = mos_fake_release_calls();
    mos_fake_set_release_fail(false);
    mos_close(h);
    EXPECT(mos_fake_release_calls() > rel);   /* close attempted the release   */
    EXPECT(mos_fake_lock_balance() == 0);     /* retry confirmed: lock cleared */
    return 0;
}

/* kIOReturnTimeout — the raw transport IOReturn the fake injects; the adapter
   maps it to MOS_ERR_TIMEOUT (mapping static-asserted in mos_scsi.c). */
#define FAKE_IORETURN_TIMEOUT 0xE00002D6u

static bool features_cb_noop(const mos_feature_info_t *f, void *ctx)
{
    (void)f; if (ctx) (*(int *)ctx)++; return true;
}

/* The query verbs share one error contract: a TRANSPORT failure (negative
   IOReturn) maps through mos_internal_ioreturn_to_mos_error; a command that
   reached the drive but answered non-GOOD (or unparseably) is MOS_ERR_IO.
   These pin the error-return arms the success-path scenarios never reach. */

TEST(adapter_disc_info_transport_failure_maps_timeout)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL && err == MOS_OK);

    mos_fake_set_method_ioreturn(MOS_FAKE_METHOD_READDISCINFO,
                                 FAKE_IORETURN_TIMEOUT);
    const mos_disc_info *d = (const mos_disc_info *)0x1;
    EXPECT_EQ(MOS_ERR_TIMEOUT, mos_query_disc_info(h, &d));
    EXPECT(d == NULL);                 /* out cleared on the error path */

    mos_close(h);
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_cdtext_transport_failure_maps_timeout)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL && err == MOS_OK);

    /* CD-TEXT issues READ TOC format 0101b; the default DVD fake has no cached
       IOCDMedia TOC, so the read is the issued one the injection fails. */
    mos_fake_set_method_ioreturn(MOS_FAKE_METHOD_READTOC,
                                 FAKE_IORETURN_TIMEOUT);
    const mos_cdtext *c = (const mos_cdtext *)0x1;
    EXPECT_EQ(MOS_ERR_TIMEOUT, mos_query_cdtext(h, &c));
    EXPECT(c == NULL);

    mos_close(h);
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_feature_enumeration_transport_failure_maps_timeout)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL && err == MOS_OK);

    mos_fake_set_method_ioreturn(MOS_FAKE_METHOD_GETCONFIG,
                                 FAKE_IORETURN_TIMEOUT);
    int seen = 0;
    EXPECT_EQ(MOS_ERR_TIMEOUT,
              mos_enumerate_features(h, features_cb_noop, &seen));
    EXPECT_EQ(0, seen);                /* callback never fired on the failure */

    mos_close(h);
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_physical_structure_transport_failure_maps_timeout)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL && err == MOS_OK);

    /* A negative IOReturn on the first READ DISC STRUCTURE compromises the
       compound (format 0 + format 1) observation and is surfaced, not folded
       into a half result. */
    mos_fake_set_method_ioreturn(MOS_FAKE_METHOD_READDISCSTRUCT,
                                 FAKE_IORETURN_TIMEOUT);
    const mos_physical_structure *p = (const mos_physical_structure *)0x1;
    EXPECT_EQ(MOS_ERR_TIMEOUT, mos_query_physical_structure(h, &p));
    EXPECT(p == NULL);

    mos_close(h);
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_physical_structure_neither_format_is_io_error)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL && err == MOS_OK);

    /* No disc_structure reply set: both formats answer empty, neither parses,
       so the compound read yields nothing → MOS_ERR_IO (non-DVD / refused). */
    const mos_physical_structure *p = (const mos_physical_structure *)0x1;
    EXPECT_EQ(MOS_ERR_IO, mos_query_physical_structure(h, &p));
    EXPECT(p == NULL);

    mos_close(h);
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_track_info_non_good_status_is_io_error)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL && err == MOS_OK);

    /* A command that reached the drive but answered non-GOOD (CHECK CONDITION)
       is MOS_ERR_IO, distinct from a transport failure's mapped IOReturn. */
    mos_fake_set_readtrackinfo_reply(0x02 /*CHECK CONDITION*/, NULL, 0);
    const mos_track_info *t = (const mos_track_info *)0x1;
    EXPECT_EQ(MOS_ERR_IO, mos_query_track_info(h, &t));
    EXPECT(t == NULL);

    mos_close(h);
    EXPECT_EQ(0, mos_fake_lock_balance());
    return 0;
}

TEST(adapter_drive_perf_non_good_status_is_io_error)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL && err == MOS_OK);

    /* The READ direction is the gate: a non-GOOD GET PERFORMANCE there fails
       the whole query with MOS_ERR_IO. */
    mos_fake_set_perf_reply(0x02 /*CHECK CONDITION*/, NULL, 0);
    const mos_drive_perf *pf = (const mos_drive_perf *)0x1;
    EXPECT_EQ(MOS_ERR_IO, mos_query_drive_perf(h, &pf));
    EXPECT(pf == NULL);

    mos_close(h);
    EXPECT_EQ(0, mos_fake_lock_balance());
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
    RUN(adapter_media_swap_to_absent_retries_coherent);
    RUN(adapter_media_swap_between_discs_retries_coherent);
    RUN(adapter_media_churn_refuses_mixed_observation);
    RUN(adapter_perf_swap_between_discs_retries_coherent);
    RUN(adapter_perf_churn_refuses_mixed_observation);
    RUN(adapter_physical_swap_between_discs_retries_coherent);
    RUN(adapter_physical_churn_refuses_mixed_observation);
    RUN(adapter_disc_info_replays_fixtures);
    RUN(adapter_toc_round_trip_and_fail_closed);
    RUN(adapter_da_volume_lookup_modalities);
    RUN(adapter_query_volume_gates_on_nub);
    RUN(adapter_drive_caps_roundtrip_and_absent);
    RUN(adapter_disc_id_decodes_and_fails_closed);
    RUN(adapter_feature_enumeration_order_and_stop);
    RUN(adapter_tray_cdbs_pinned_byte_for_byte);
    RUN(adapter_tray_eject_graceful_unmounts_and_ejects);
    RUN(adapter_tray_eject_busy_fs_surfaces_busy_never_forces);
    RUN(adapter_tray_eject_default_clears_os_mountlock);
    RUN(adapter_tray_eject_cold_lock_needs_force);
    RUN(adapter_da_unmount_is_name_based);
    RUN(adapter_da_unmount_bounded_when_callback_never_fires);
    RUN(adapter_tray_locked_eject_classifies_refused_locked);
    RUN(adapter_tray_refused_other_carries_its_sense);
    RUN(adapter_tray_exclusive_denied_is_negative_error);
    RUN(adapter_tray_lock_on_mounted_is_already_locked);
    RUN(adapter_tray_unlock_clears_both_states);
    RUN(adapter_raw_cdb_release_failure_poisons_handle);
    RUN(adapter_disc_info_transport_failure_maps_timeout);
    RUN(adapter_cdtext_transport_failure_maps_timeout);
    RUN(adapter_feature_enumeration_transport_failure_maps_timeout);
    RUN(adapter_physical_structure_transport_failure_maps_timeout);
    RUN(adapter_physical_structure_neither_format_is_io_error);
    RUN(adapter_track_info_non_good_status_is_io_error);
    RUN(adapter_drive_perf_non_good_status_is_io_error);
    printf("\n%d run, %d passed, %d failed\n",
           mos_tests_run, mos_tests_run - mos_tests_failed, mos_tests_failed);
    return mos_tests_failed ? 1 : 0;
}
