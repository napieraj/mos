/*
 * mos_discinfo.c — pure, bounds-safe decode of a READ DISC INFORMATION
 * (MMC 0x51, standard data type 000b) response.
 *
 * No IOKit. The IOKit shell issues READ DISC INFORMATION into a fixed,
 * zero-initialized buffer and hands that buffer plus its size here. The
 * Disc Information Length is device-reported and therefore hostile; this
 * file keeps it from steering a read outside [buf, buf+len).
 *
 * Layout (MMC-6, READ DISC INFORMATION standard response, first 12 bytes):
 *
 *   [0..1] Disc Information Length (BE) — bytes available AFTER this field;
 *          the response occupies 2 + value bytes.
 *   [2]    reserved | Erasable(bit4) | State of Last Session(bits 3:2)
 *                                     | Disc Status(bits 1:0)
 *   [3]    Number of First Track on Disc
 *   [4]    Number of Sessions (LSB)
 *   [5]    First Track Number in Last Session (LSB)
 *   [6]    Last Track Number in Last Session (LSB)
 *   [7]    DID_V DBC_V URU DAC_V .. BG Format Status
 *   [8]    Disc Type
 *   [9]    Number of Sessions (MSB)
 *   [10]   First Track Number in Last Session (MSB)
 *   [11]   Last Track Number in Last Session (MSB)
 *   (bytes 12+ : Disc Identification, lead-in / lead-out addresses, bar
 *    code, OPC table — not decoded; informational, not the status.)
 *
 * Safety contract (the device controls the length here):
 *   - `len` is the only trusted ceiling; the Disc Information Length can
 *     only shrink the trusted region (clamped under `len`), never extend it.
 *   - The fixed numeric region (through byte 11) must be present per both
 *     `len` and the declared length; a shorter response is refused.
 *
 * No-OOB property gated headless under ASan/UBSan by tests/test_discinfo.c.
 */

#include "mos_pure.h"

bool mos_internal_disc_info_parse(const uint8_t *buf, size_t len,
                                  mos_disc_info *out)
{
    if (!buf || !out) return false;

    /* The fixed numeric fields this decode promises run through byte 11, so
       the trusted region must reach at least byte 12. */
    if (len < 12) return false;

    /* Disc Information Length (bytes 0-1) counts bytes AFTER itself, so the
       response occupies declared + 2 bytes. Clamp the trusted region to the
       smaller of that and len — a device length only ever shrinks it. */
    size_t declared_end = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < declared_end) ? len : declared_end;
    if (end < 12) return false;        /* device declares fewer bytes than the fields */

    uint8_t b2 = buf[2];
    out->status             = (mos_disc_status)(b2 & 0x03u);
    out->last_session_state = (uint8_t)((b2 >> 2) & 0x03u);
    out->erasable           = (b2 & 0x10u) != 0;

    out->first_track_on_disc      = buf[3];
    out->number_of_sessions       = (uint16_t)(((uint16_t)buf[9]  << 8) | buf[4]);
    out->first_track_last_session = (uint16_t)(((uint16_t)buf[10] << 8) | buf[5]);
    out->last_track_last_session  = (uint16_t)(((uint16_t)buf[11] << 8) | buf[6]);
    return true;
}
