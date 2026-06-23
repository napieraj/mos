/*
 * mos_atip.c — pure, bounds-safe decode of a READ TOC/PMA/ATIP Format=0100b
 * reply (the CD-R/RW Absolute Time In Pre-groove). No IOKit: the shell hands
 * us the reply buffer + the trusted byte count.
 *
 * Layout (MMC-6 r02g §6.25, Table 488 — "READ TOC/PMA/ATIP response data
 * (Format = 0100b)"):
 *
 *   [0..1] ATIP Data Length (BE) — bytes AFTER this field; total = 2 + value.
 *   [2..3] Reserved
 *   ATIP Descriptor (Special Information 1..3 + Additional Information):
 *   [4]    Indicative Target Writing Power[7:4] | Reserved[3] | RefSpeed[2:0]
 *   [5]    bit7=0 | URU[6] | Reserved[5:0]
 *   [6]    bit7=1 | DiscType[6] | DiscSubType[5:3] | A1V[2] | A2V[1] | A3V[0]
 *   [7]    Reserved
 *   [8..10]  ATIP Start Time of Lead-in   (Min, Sec, Frame) — the MID identity
 *   [11]   Reserved
 *   [12..14] Last Possible Start Time of Lead-out (Min, Sec, Frame) — capacity
 *   [15..]  A1/A2/A3/S4 values (not decoded)
 *
 * Safety contract (the device controls the length):
 *   - The reply must be at least 15 bytes to carry the descriptor through the
 *     lead-out frame (byte 14); shorter ⇒ fail closed (false, out zeroed).
 *   - The ATIP Data Length field may only SHRINK the trusted region, never
 *     extend it past `len` (dual-length rule).
 *   - No payload byte is used as an offset — fixed-offset reads only.
 *
 * mos surfaces the RAW spec fields; the MID→manufacturer NAME table is the
 * Orange Book's, curated and consumer-side (per the hardware-role ADR, mos
 * does not ship per-device identity tables).
 */

#include "mos_pure.h"

#include <string.h>

bool mos_internal_atip_parse(const uint8_t *buf, size_t len, mos_atip *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!buf || !out) return false;

    /* Trusted end: start at the buffer ceiling, then let the device's ATIP
       Data Length pull it IN iff it claims less. 64-bit so +2 cannot wrap. */
    size_t end = len;
    if (len >= 2) {
        uint64_t dlen = ((uint64_t)buf[0] << 8) | (uint64_t)buf[1];
        uint64_t declared = dlen + 2u;          /* total incl. the length field */
        if (declared < (uint64_t)end) end = (size_t)declared;
    }

    /* Need the descriptor through the lead-out frame at byte 14. */
    if (end < 15u) return false;

    out->reference_speed = (uint8_t)(buf[4] & 0x07u);
    out->unrestricted_use             = (buf[5] & 0x40u) != 0;
    out->disc_type       = (uint8_t)((buf[6] >> 6) & 0x01u);
    out->disc_sub_type   = (uint8_t)((buf[6] >> 3) & 0x07u);

    out->lead_in_min   = buf[8];
    out->lead_in_sec   = buf[9];
    out->lead_in_frame = buf[10];

    out->lead_out_min   = buf[12];
    out->lead_out_sec   = buf[13];
    out->lead_out_frame = buf[14];

    return true;
}
