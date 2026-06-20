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

    /* IDENTITY-EXACT BY CONSTRUCTION (#1): a stale / wrong media id resolves to
       NO IOMedia (its registry entry is gone, never reused), so DA is never
       consulted and another disc can't be misattributed — even with a mount
       present. No "expected vs actual" verification; the resolution IS the
       identity. */
    mos_fake_set_da_volume("ARRIVAL", "/Volumes/ARRIVAL");
    EXPECT(!mos_internal_da_volume(0xDEADBEEFull, name, sizeof name,
                                   path, sizeof path));
    EXPECT(name[0] == 0 && path[0] == 0);

    /* Zero media id (identity unknown / bridge fallback): fail closed. */
    EXPECT(!mos_internal_da_volume(0, name, sizeof name, path, sizeof path));
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
{ return mos_tray_lock(h, false, o, NULL); }
static mos_error call_lock_p(mos_handle_t *h, mos_tray_outcome *o)
{ return mos_tray_lock(h, true, o, NULL); }
static mos_error call_unlock(mos_handle_t *h, mos_tray_outcome *o)
{ return mos_tray_unlock(h, false, o, NULL); }
static mos_error call_unlock_p(mos_handle_t *h, mos_tray_outcome *o)
{ return mos_tray_unlock(h, true, o, NULL); }
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
    /* PREVENT ALLOW 0x1E byte4 {PERSISTENT,PREVENT} (04-349r1 Table 8). */
    static const uint8_t lock_[6]    = { 0x1E, 0, 0, 0, 0x01, 0 };
    static const uint8_t lock_p[6]   = { 0x1E, 0, 0, 0, 0x03, 0 };
    static const uint8_t unlock_[6]  = { 0x1E, 0, 0, 0, 0x00, 0 };
    static const uint8_t unlock_p[6] = { 0x1E, 0, 0, 0, 0x02, 0 };

    int rc = 0;
    rc |= pin_tray_cdb(h, call_eject,    eject,    MOS_TRAY_DONE);
    rc |= pin_tray_cdb(h, call_close,    close_,   MOS_TRAY_DONE);
    rc |= pin_tray_cdb(h, call_lock,     lock_,    MOS_TRAY_DONE);
    rc |= pin_tray_cdb(h, call_lock_p,   lock_p,   MOS_TRAY_DONE);
    rc |= pin_tray_cdb(h, call_unlock,   unlock_,  MOS_TRAY_DONE);
    rc |= pin_tray_cdb(h, call_unlock_p, unlock_p, MOS_TRAY_DONE);
    mos_close(h);
    return rc;
}

TEST(adapter_tray_eject_force_unmounts_and_ejects)
{
    /* `tray eject --force` is name-semantics (diskutil-class), enabled at the
       library: a mounted disc makes the first eject's ObtainExclusiveAccess
       return BUSY; --force force-unmounts the volume (DADiskUnmount clears the
       mount) and reconverges on the eject CDB, which now succeeds → DONE. The
       SELECTOR gate (index/regid opt-in) lives in cli/tray.c, not here. */
    mos_fake_reset();
    mos_fake_set_mounted_busy(true);     /* first eject sees the mount as BUSY */
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    mos_tray_outcome out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_OK, mos_tray_eject(h, /*force=*/true, &out, NULL));
    EXPECT_EQ(MOS_TRAY_DONE, out);
    /* The re-eject took the exclusive lock once and released it (the BUSY first
       attempt never acquired); balance is clean. */
    EXPECT_EQ(0, mos_fake_lock_balance());

    /* Without --force, the same mount surfaces as MOS_ERR_BUSY (unchanged). */
    mos_fake_set_mounted_busy(true);
    out = (mos_tray_outcome)-1;
    EXPECT_EQ(MOS_ERR_BUSY, mos_tray_eject(h, /*force=*/false, &out, NULL));
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

    /* A drive without Persistent Prevent rejects 0x03 with 5/24/00 (INVALID
       FIELD IN CDB) — refused_other, and the sense triple must reach the
       caller (the gap Plan A closed). */
    uint8_t sense[18] = {0};
    sense[0] = 0x70; sense[2] = 0x05; sense[12] = 0x24; sense[13] = 0x00;
    mos_fake_set_raw_reply(0x02 /*CHECK CONDITION*/, NULL, 0, 0, sense);

    mos_tray_outcome out = (mos_tray_outcome)-1;
    uint8_t triple[3] = {0};
    EXPECT_EQ(MOS_OK, mos_tray_lock(h, /*persistent=*/true, &out, triple));
    EXPECT_EQ(MOS_TRAY_REFUSED_OTHER, out);
    EXPECT_EQ(0x05, triple[0]);
    EXPECT_EQ(0x24, triple[1]);
    EXPECT_EQ(0x00, triple[2]);
    mos_close(h);
    return 0;
}

TEST(adapter_da_unmount_is_name_based)
{
    /* Force-unmount is NAME semantics (diskutil-class): mos_internal_da_unmount
       unmounts the disc currently named "diskN", with no identity bind — the
       public DA API can't provide one (the daemon re-resolves by name; verified
       in DADisk.c). The consent against a reused name is the CLI selector gate,
       not a library bind. Here we pin the library contract: a non-empty name
       unmounts (fake DADiskUnmount succeeds), degenerate names fail closed. */
    mos_fake_reset();
    EXPECT(mos_internal_da_unmount("disk4"));   /* succeeds by name */
    EXPECT(!mos_internal_da_unmount(NULL));      /* no name: false */
    EXPECT(!mos_internal_da_unmount(""));        /* empty name: false */
    return 0;
}

TEST(adapter_tray_exclusive_denied_is_negative_error)
{
    mos_fake_reset();
    mos_error err = MOS_ERR_IO;
    mos_handle_t *h = mos_open_by_index(1, &err);
    EXPECT(h != NULL);

    /* Another client holds the drive: ObtainExclusiveAccess fails, so the
       verb is a transport failure (negative), not an answered refusal. */
    mos_fake_set_exclusive_denied(true);
    mos_tray_outcome out = MOS_TRAY_DONE;
    uint8_t triple[3] = { 0xFF, 0xFF, 0xFF };
    mos_error e = mos_tray_lock(h, false, &out, triple);
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
    RUN(adapter_disc_info_replays_fixtures);
    RUN(adapter_toc_round_trip_and_fail_closed);
    RUN(adapter_da_volume_lookup_modalities);
    RUN(adapter_query_volume_gates_on_nub);
    RUN(adapter_drive_caps_roundtrip_and_absent);
    RUN(adapter_disc_id_decodes_and_fails_closed);
    RUN(adapter_feature_enumeration_order_and_stop);
    RUN(adapter_tray_cdbs_pinned_byte_for_byte);
    RUN(adapter_tray_eject_force_unmounts_and_ejects);
    RUN(adapter_da_unmount_is_name_based);
    RUN(adapter_tray_locked_eject_classifies_refused_locked);
    RUN(adapter_tray_refused_other_carries_its_sense);
    RUN(adapter_tray_exclusive_denied_is_negative_error);
    RUN(adapter_raw_cdb_release_failure_poisons_handle);
    printf("\n%d run, %d passed, %d failed\n",
           mos_tests_run, mos_tests_run - mos_tests_failed, mos_tests_failed);
    return mos_tests_failed ? 1 : 0;
}
