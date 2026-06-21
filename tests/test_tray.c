/*
 * test_tray.c — tray-command outcome classifier regression gate.
 * Constants from mos_scsi_status.h, classifier + public enum from
 * mos_pure.h/mos.h — the same code mos_tray.c routes every verb through,
 * so the test exercises the real classification, not a mirror.
 */

#include "test_harness.h"
#include "../src/mos_scsi_status.h"
#include "../src/mos_pure.h"
#include <stdint.h>

TEST(tray_good_status_is_done)
{
    /* A GOOD command is DONE regardless of any stale sense bytes (the
       sense buffer is only meaningful on CHECK CONDITION). */
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_GOOD, 0, 0, 0),
              MOS_TRAY_DONE);
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_GOOD, 0x05, 0x53, 0x02),
              MOS_TRAY_DONE);
    return 0;
}

TEST(tray_locked_eject_is_refused_locked)
{
    /* CHECK CONDITION + 53/02 MEDIA REMOVAL PREVENTED: a tray Prevent lock
       refusing an eject/close (T10 04-349r1 Table 9). The sense KEY is
       contextual and does NOT gate the verdict — 05 (ILLEGAL REQUEST) with
       media present, 02 (NOT READY) on an EMPTY drive (LG WH16NS60 hardware,
       2026-06-21). Both classify refused_locked on the ASC/ASCQ alone. */
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_CHECK_CONDITION,
                                         0x05, 0x53, 0x02),
              MOS_TRAY_REFUSED_LOCKED);
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_CHECK_CONDITION,
                                         0x02, 0x53, 0x02),   /* empty drive */
              MOS_TRAY_REFUSED_LOCKED);
    return 0;
}

TEST(tray_other_check_condition_is_refused_other)
{
    /* Any other answered CHECK CONDITION. Load-bearing case: a drive
       lacking the Persistent Prevent (PDTE) state rejects 0x02/0x03 with
       5/24/00 (04-349r1 Table 2 note a); and a near-miss on the locked
       triple must NOT read as locked. */
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_CHECK_CONDITION,
                                         0x05, 0x24, 0x00),
              MOS_TRAY_REFUSED_OTHER);
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_CHECK_CONDITION,
                                         0x02, 0x3A, 0x00),  /* NOT READY-ish */
              MOS_TRAY_REFUSED_OTHER);
    /* 5/53/01 and 5/53/00 are NOT the locked code — only 5/53/02 is. */
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_CHECK_CONDITION,
                                         0x05, 0x53, 0x01),
              MOS_TRAY_REFUSED_OTHER);
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_CHECK_CONDITION,
                                         0x05, 0x53, 0x00),
              MOS_TRAY_REFUSED_OTHER);
    return 0;
}

TEST(tray_nongood_nonsense_status_is_refused_other)
{
    /* A contention status reaching the classifier (the adapter normally maps
       these to MOS_ERR_BUSY out-of-band before classifying, but the pure
       function must still be total): non-GOOD with no locked sense triple. */
    EXPECT_EQ(mos_internal_tray_classify(MOS_SCSI_STATUS_BUSY, 0, 0, 0),
              MOS_TRAY_REFUSED_OTHER);
    EXPECT_EQ(mos_internal_tray_classify(0xFF, 0xFF, 0xFF, 0xFF),
              MOS_TRAY_REFUSED_OTHER);
    return 0;
}

TEST(tray_outcome_tokens_are_stable)
{
    EXPECT_STREQ(mos_tray_outcome_description(MOS_TRAY_DONE),           "done");
    EXPECT_STREQ(mos_tray_outcome_description(MOS_TRAY_REFUSED_LOCKED), "refused_locked");
    EXPECT_STREQ(mos_tray_outcome_description(MOS_TRAY_REFUSED_OTHER),  "refused_other");
    return 0;
}

void register_tray_tests(void)
{
    RUN(tray_good_status_is_done);
    RUN(tray_locked_eject_is_refused_locked);
    RUN(tray_other_check_condition_is_refused_other);
    RUN(tray_nongood_nonsense_status_is_refused_other);
    RUN(tray_outcome_tokens_are_stable);
}
