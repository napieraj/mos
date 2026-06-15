/*
 * mos_trackinfo.c — pure, bounds-safe decode of a READ TRACK INFORMATION
 * (MMC 0x52) Track Information Block: the capacity / append-state surface
 * (track start, next writable address, free blocks, track size, last
 * recorded address) plus the track/data mode and blank/damage bits.
 *
 * No IOKit. The IOKit shell issues READ TRACK INFORMATION via the
 * ReadTrackInformation convenience method into a fixed, zero-initialized
 * buffer and hands it plus its size here. Every length and value byte is
 * device-reported and therefore hostile; this file keeps the declared
 * length from steering a read outside [buf, buf+len) and reads only fixed
 * offsets.
 *
 * Wire layout (the MMC Track Information Block; cdrom.h offset cross-check
 * in SPEC.md):
 *   [0..1]  Track Information Length (BE) — bytes AFTER this field
 *   [2]     Track Number (LSB)
 *   [3]     Session Number (LSB)
 *   [4]     reserved
 *   [5]     reserved(7:6) | damage(5) | copy(4) | track_mode(3:0)
 *   [6]     rt(7) | blank(6) | packet(5) | fp(4) | data_mode(3:0)
 *   [7]     reserved(7:2) | lra_v(1) | nwa_v(0)
 *   [8..11]  Track Start Address (BE)
 *   [12..15] Next Writable Address (BE)   — valid iff nwa_v
 *   [16..19] Free Blocks (BE)
 *   [20..23] Fixed Packet Size (BE)
 *   [24..27] Track Size (BE)
 *   [28..31] Last Recorded Address (BE)   — valid iff lra_v
 *   [32..33] Track / Session Number MSB   — MMC-6 longer reply, optional
 *
 * NWA and LRA are surfaced only when their *_v validity bit is set;
 * otherwise the *_valid accessor is false and the value is meaningless —
 * the consumer must check. No payload byte is ever used as an offset.
 */

#include "mos_pure.h"

#define TI_MIN_LEN  32u   /* through Last Recorded Address (byte 31) */
#define TI_MSB_LEN  34u   /* track/session MSB present (byte 33) */

static uint32_t mos_internal_ti_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | p[3];
}

bool mos_internal_track_info_parse(const uint8_t *buf, size_t len,
                                   struct mos_track_info *out)
{
    if (!out) return false;
    *out = (struct mos_track_info){0};
    if (!buf || len < TI_MIN_LEN) return false;

    size_t declared = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < declared) ? len : declared;
    if (end < TI_MIN_LEN) return false;

    out->track_number   = buf[2];
    out->session_number = buf[3];
    out->track_mode     = (uint8_t)(buf[5] & 0x0f);
    out->damage         = (buf[5] >> 5) & 0x01;
    out->data_mode      = (uint8_t)(buf[6] & 0x0f);
    out->blank          = (buf[6] >> 6) & 0x01;
    out->lra_valid      = (buf[7] >> 1) & 0x01;
    out->nwa_valid      = buf[7] & 0x01;
    out->track_start    = mos_internal_ti_be32(&buf[8]);
    out->next_writable  = mos_internal_ti_be32(&buf[12]);
    out->free_blocks    = mos_internal_ti_be32(&buf[16]);
    out->track_size     = mos_internal_ti_be32(&buf[24]);
    out->last_recorded  = mos_internal_ti_be32(&buf[28]);

    /* MMC-6 longer reply folds the high byte of track/session number in
       at 32/33; only if the trusted region reaches them. */
    if (end >= TI_MSB_LEN) {
        out->track_number   = (uint16_t)(out->track_number   | (buf[32] << 8));
        out->session_number = (uint16_t)(out->session_number | (buf[33] << 8));
    }
    return true;
}
