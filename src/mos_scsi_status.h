/*
 * mos_scsi_status.h — SAM-5 §5.3 SCSI task status constants.
 *
 * Pure-data header with no IOKit / CoreFoundation dependencies. Safe
 * to include from:
 *   - Runtime source that needs to classify task status (mos_state_core.c)
 *   - Pure-data test code that must compile without the SDK
 *     (tests/test_scsi_status.c)
 *
 * This is the single source of truth for these values. Do NOT redeclare
 * them elsewhere — a second copy drifts the first time only one is updated.
 *
 * We define our own constants rather than using Apple's kSCSITaskStatus_*
 * enums from SCSITask.h because the Apple set is incomplete: it omits
 * RESERVATION_CONFLICT, TASK_SET_FULL, and ACA_ACTIVE, which our state
 * machine treats uniformly as "drive contended." Having our own set lets
 * the contention classifier (and its test) work from one consistent
 * vocabulary.
 */

#ifndef MOS_SCSI_STATUS_H
#define MOS_SCSI_STATUS_H

#define MOS_SCSI_STATUS_GOOD                  0x00
#define MOS_SCSI_STATUS_CHECK_CONDITION       0x02
#define MOS_SCSI_STATUS_CONDITION_MET         0x04
#define MOS_SCSI_STATUS_BUSY                  0x08
#define MOS_SCSI_STATUS_RESERVATION_CONFLICT  0x18
#define MOS_SCSI_STATUS_TASK_SET_FULL         0x28
#define MOS_SCSI_STATUS_ACA_ACTIVE            0x30

#endif /* MOS_SCSI_STATUS_H */
