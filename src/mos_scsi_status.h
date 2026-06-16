/*
 * mos_scsi_status.h — SAM-5 §5.3 SCSI task status constants. No IOKit/CF
 * deps, so the contention classifier and its test stay SDK-free. Values
 * match Apple's kSCSITaskStatus_* enums.
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
