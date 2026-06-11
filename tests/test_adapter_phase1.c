/*
 * test_adapter_phase1.c — runs the REAL one-shot adapter TUs
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

    /* Identity flowed through the DR directory seam. */
    EXPECT_EQ(4, mos_handle_bsd_unit(h));

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

int main(void)
{
    printf("adapter phase-1 (headless, link-seam fake):\n");
    RUN(adapter_open_index_query_ready);
    RUN(adapter_open_index_no_drive_is_no_device);
    printf("\n%d run, %d passed, %d failed\n",
           mos_tests_run, mos_tests_run - mos_tests_failed, mos_tests_failed);
    return mos_tests_failed ? 1 : 0;
}
