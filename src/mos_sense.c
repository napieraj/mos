/*
 * mos_sense.c — SCSI sense-data parsing and state mapping.
 *
 * No IOKit dependency; trivially unit-testable with fixture bytes.
 *
 * Fixed-format (response code 0x70 / 0x71): SPC-4 §4.5.3
 *   byte 0: response code + valid bit
 *   byte 2: bits 3:0 = sense key
 *   byte 12: Additional Sense Code (ASC)
 *   byte 13: Additional Sense Code Qualifier (ASCQ)
 *
 * Descriptor-format (response code 0x72 / 0x73): SPC-4 §4.5.2
 *   byte 0: response code
 *   byte 1: bits 3:0 = sense key
 *   byte 2: ASC
 *   byte 3: ASCQ
 *
 * Optical drives in practice always return fixed format. The descriptor
 * path is here for correctness, not because we've seen it in the wild.
 */

#include "mos_pure.h"
#include <string.h>

void mos_internal_parse_sense(const uint8_t sense[18],
                              uint8_t *sk, uint8_t *asc, uint8_t *ascq)
{
    if (sk)   *sk   = 0;
    if (asc)  *asc  = 0;
    if (ascq) *ascq = 0;
    if (!sense) return;

    uint8_t rc = sense[0] & 0x7F;

    if (rc == 0x70 || rc == 0x71) {
        /* Fixed format */
        if (sk)   *sk   = sense[2] & 0x0F;
        if (asc)  *asc  = sense[12];
        if (ascq) *ascq = sense[13];
    } else if (rc == 0x72 || rc == 0x73) {
        /* Descriptor format */
        if (sk)   *sk   = sense[1] & 0x0F;
        if (asc)  *asc  = sense[2];
        if (ascq) *ascq = sense[3];
    }
}

/*
 * Map (sense_key, asc, ascq) → state, GIVEN THE TRAY IS CLOSED.
 *
 * This is the closed-branch enrichment, not a tray detector. By the time
 * it runs, the open/closed verdict already belongs to GESN's door bit (or,
 * when GESN was silent, to the sense fork in mos_state_core.c). So this
 * function never returns OPEN or EMPTY_OR_OPEN: it only refines a closed
 * tray into the *reason* the unit isn't ready. A 3A/02 ("medium not
 * present, tray open") reaching here means GESN already said closed and the
 * ASCQ's tray hint is deliberately discarded — enrich, don't invalidate.
 *
 * Presence is the hoist: asc == 0x3A means no medium → EMPTY; any other
 * not-ready sense means a disc is engaged and we classify why.
 *
 * Returns MOS_STATE_UNKNOWN for sense we won't assert a meaning for; the
 * tray is still known-closed, the raw sense still rides along on the result.
 *
 * References:
 *   T10 ASC/ASCQ public list: https://www.t10.org/lists/asc-num.htm
 *   MMC-6 / SBC-4 sense usage is consistent with the generic SCSI table.
 */
mos_state_enum mos_internal_state_from_sense_closed(uint8_t sk, uint8_t asc, uint8_t ascq)
{
    /* HARDWARE ERROR (key 0x04): the drive itself faulted — outranks any
       medium/not-ready detail that might also be set. */
    if (sk == 0x04) return MOS_STATE_DEVICE_FAULT;

    /* MEDIUM ERROR (key 0x03), or 57/00 UNABLE TO RECOVER TABLE-OF-CONTENTS:
       a disc is loaded but the drive can't read it. Not self-resolving, so
       it is NOT loading. */
    if (sk == 0x03 || (asc == 0x57 && ascq == 0x00))
        return MOS_STATE_MEDIA_UNREADABLE;

    /* 3A/xx MEDIUM NOT PRESENT, tray closed (per the hoist: no medium). The
       ASCQ open/closed flavor is moot here — GESN owns that. */
    if (asc == 0x3A) return MOS_STATE_EMPTY;

    /* 04/xx LOGICAL UNIT NOT READY: a disc is engaged; the qualifier says
       why it isn't ready yet. */
    if (asc == 0x04) {
        switch (ascq) {
            case 0x01:  /* becoming ready                 */
            case 0x02:  /* initialize command required    */
            case 0x07:  /* operation in progress          */
                return MOS_STATE_LOADING;   /* self-resolving by waiting */
            case 0x04:  /* format in progress             */
                return MOS_STATE_FORMATTING;
            case 0x08:  /* long write in progress         */
                return MOS_STATE_BUSY;      /* actively writing; back off */
            default:
                return MOS_STATE_UNKNOWN;
        }
    }

    return MOS_STATE_UNKNOWN;
}
