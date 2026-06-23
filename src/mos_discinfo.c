/*
 * mos_discinfo.c — pure, bounds-safe decode of a READ DISC INFORMATION
 * (MMC 0x51, standard data type 000b) response. No IOKit: the shell hands
 * us a fixed zero-init buffer and its size. The Disc Information Length is
 * device-reported, hence hostile — it must never steer a read outside
 * [buf, buf+len).
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
 *   [12..15] Disc Identification (BE) — valid iff byte7 DID_V (bit7)
 *   [16..19] Last Session Lead-in Start (MSF)   — undecoded
 *   [20..23] Last Possible Lead-out Start (MSF) — undecoded
 *   [24..31] Disc Bar Code — valid iff byte7 DBC_V (bit6)
 *   (bytes 32+ : reserved / OPC table — undecoded; see SPEC.md.)
 *
 * Safety contract (the device controls the length): `len` is the only
 * trusted ceiling; the Disc Information Length can only shrink the trusted
 * region, never extend it. The fixed numeric region (through byte 11) must
 * be present per both `len` and the declared length; a shorter response is
 * refused.
 */

#include "mos_pure.h"

bool mos_internal_disc_info_parse(const uint8_t *buf, size_t len,
                                  mos_disc_info *out)
{
    if (!buf || !out) return false;

    /* Fixed numeric fields run through byte 11; trusted region must reach 12. */
    if (len < 12) return false;

    /* Disc Information Length (bytes 0-1) counts bytes AFTER itself; clamp the
       trusted region to the smaller of declared+2 and len. */
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

    /* BG Format Status (byte 7 bits 1:0): background-format state of
       DVD+RW / BD-RE / Mount Rainier media. Values match Linux CDM_MRW_*. */
    out->bg_format_status = (uint8_t)(buf[7] & 0x03u);

    /* Disc Type (byte 8) — always within the >=12 trusted floor. */
    out->disc_type = buf[8];

    /* Validity-gated identifiers. Each is decoded only when its validity bit
       (byte 7) is set AND the trusted region (end) actually reaches the
       field — the device-reported length can only shrink that region, so a
       bit that claims validity over bytes the reply does not carry yields a
       false *_valid, never an OOB read. */
    out->disc_id_valid = false;
    out->disc_id       = 0;
    if ((buf[7] & 0x80u) && end >= 16) {            /* DID_V, bytes 12..15 */
        out->disc_id = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16)
                     | ((uint32_t)buf[14] <<  8) |  (uint32_t)buf[15];
        out->disc_id_valid = true;
    }

    out->bar_code_valid = false;
    for (size_t i = 0; i < sizeof out->bar_code; i++) out->bar_code[i] = 0;
    if ((buf[7] & 0x40u) && end >= 32) {            /* DBC_V, bytes 24..31 */
        for (size_t i = 0; i < 8; i++) out->bar_code[i] = buf[24 + i];
        out->bar_code_valid = true;
    }

    return true;
}
