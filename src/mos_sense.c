/*
 * mos_sense.c — SCSI sense-data parsing and state mapping. No IOKit;
 * unit-testable with fixture bytes.
 *
 * Fixed-format (response code 0x70 / 0x71): SPC-4 §4.5.3
 *   byte 0:  response code + valid bit
 *   byte 2:  bits 3:0 = sense key
 *   byte 12: ASC      byte 13: ASCQ
 *
 * Descriptor-format (response code 0x72 / 0x73): SPC-4 §4.5.2
 *   byte 0: response code   byte 1: bits 3:0 = sense key
 *   byte 2: ASC             byte 3: ASCQ
 *
 * Optical drives return fixed format in practice; the descriptor path is
 * here for correctness.
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
 * Enrichment, not tray detection: open/closed is already settled (GESN
 * door bit, or the sense fork in mos_state_core.c), so this never returns
 * OPEN/EMPTY_OR_OPEN — it names the *reason* a closed tray isn't ready. A
 * 3A/02 ("medium not present, tray open") reaching here has its tray hint
 * discarded (enrich, don't invalidate): 0x3A means EMPTY, any other
 * not-ready sense means a disc is engaged, unrecognized → UNKNOWN.
 *
 * T10 ASC/ASCQ list: https://www.t10.org/lists/asc-num.htm
 */
mos_state mos_internal_state_from_sense_closed(uint8_t sk, uint8_t asc, uint8_t ascq)
{
    /* HARDWARE ERROR (key 0x04): the drive faulted — outranks any
       medium/not-ready detail also set. */
    if (sk == 0x04) return MOS_STATE_DEVICE_FAULT;

    /* MEDIUM ERROR (key 0x03), or 57/00 UNABLE TO RECOVER TOC: disc loaded
       but unreadable. Not self-resolving, so not loading. */
    if (sk == 0x03 || (asc == 0x57 && ascq == 0x00))
        return MOS_STATE_MEDIA_UNREADABLE;

    /* 3A/xx MEDIUM NOT PRESENT: no medium (the ASCQ tray flavor is GESN's). */
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
