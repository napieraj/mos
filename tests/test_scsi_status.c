/*
 * test_scsi_status.c — SAM-5 §5.3 contention status regression gate.
 * Constants from mos_scsi_status.h, classifier from mos_pure.h — both
 * shared with mos_state_core.c so tests exercise the real code.
 */

#include "test_harness.h"
#include "../src/mos_scsi_status.h"
#include "../src/mos_pure.h"
#include <stdbool.h>
#include <stdint.h>

TEST(sam5_status_values_match_spec)
{
    /* SAM-5 §5.3.2 Status Codes. These values are ABI-stable since
       SCSI-3 (1998) and are what every SCSI target uses. */
    EXPECT_EQ(MOS_SCSI_STATUS_GOOD,                 0x00);
    EXPECT_EQ(MOS_SCSI_STATUS_CHECK_CONDITION,      0x02);
    EXPECT_EQ(MOS_SCSI_STATUS_BUSY,                 0x08);
    EXPECT_EQ(MOS_SCSI_STATUS_RESERVATION_CONFLICT, 0x18);
    EXPECT_EQ(MOS_SCSI_STATUS_TASK_SET_FULL,        0x28);
    EXPECT_EQ(MOS_SCSI_STATUS_ACA_ACTIVE,           0x30);
    return 0;
}

TEST(contention_classification_includes_all_four)
{
    EXPECT_EQ(mos_internal_status_is_contended(MOS_SCSI_STATUS_BUSY),                 true);
    EXPECT_EQ(mos_internal_status_is_contended(MOS_SCSI_STATUS_RESERVATION_CONFLICT), true);
    EXPECT_EQ(mos_internal_status_is_contended(MOS_SCSI_STATUS_TASK_SET_FULL),        true);
    EXPECT_EQ(mos_internal_status_is_contended(MOS_SCSI_STATUS_ACA_ACTIVE),           true);
    return 0;
}

TEST(non_contention_statuses_do_not_classify_as_busy)
{
    EXPECT_EQ(mos_internal_status_is_contended(MOS_SCSI_STATUS_GOOD),            false);
    EXPECT_EQ(mos_internal_status_is_contended(MOS_SCSI_STATUS_CHECK_CONDITION), false);
    EXPECT_EQ(mos_internal_status_is_contended(MOS_SCSI_STATUS_CONDITION_MET),   false);
    EXPECT_EQ(mos_internal_status_is_contended(0xFF),                            false);
    EXPECT_EQ(mos_internal_status_is_contended(0x00),                            false);
    return 0;
}

void register_scsi_status_tests(void)
{
    RUN(sam5_status_values_match_spec);
    RUN(contention_classification_includes_all_four);
    RUN(non_contention_statuses_do_not_classify_as_busy);
}
