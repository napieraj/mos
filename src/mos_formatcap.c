/*
 * mos_formatcap.c — pure, bounds-safe decode of READ FORMAT CAPACITIES (MMC
 * 0x23). The formattable view of the loaded medium: how big it is now, whether
 * it is unformatted, and the capacities the drive could format it to. This is
 * what mos_query_capacity's other two sources cannot report on a blank
 * REWRITABLE disc (DVD±RW, DVD-RAM, BD-RE): the kernel's cached READ CAPACITY
 * is 0 (nothing formatted) and there is no track for READ TRACK INFORMATION.
 *
 * No IOKit: the shell (src/mos_query.c) hands us a fixed zero-init buffer
 * filled via the ReadFormatCapacities convenience method (MMCDeviceInterface) —
 * NOT a raw CDB (correcting the earlier "fifth raw verb" call; the wrapper
 * exists in SCSITaskLib.h — see the AGENTS.md ADR +
 * doc/research/2026-06-18-readformatcapacities-convenience-exists.md). Read-
 * only: mos reports formattable capacities and never issues FORMAT UNIT (0x04).
 *
 * Reply layout (MMC-6 §6.24, Format Capacities):
 *   Capacity List Header (4 bytes)
 *     [0..2]  reserved
 *     [3]     CAPACITY LIST LENGTH  — bytes that follow (= 8 + 8*N)
 *   Current/Maximum Capacity Descriptor (8 bytes) at [4..11]
 *     [4..7]  NUMBER OF BLOCKS (BE u32)
 *     [8]     bits 1:0 DESCRIPTOR TYPE (1 unformatted, 2 formatted, 3 no media)
 *     [9..11] BLOCK LENGTH (BE u24)
 *   Formattable Capacity Descriptors (8 bytes each) from [12]
 *     [0..3]  NUMBER OF BLOCKS (BE u32)
 *     [4]     bits 7:2 FORMAT TYPE
 *     [5..7]  TYPE DEPENDENT PARAMETER (BE u24) — block length for most types
 *
 * Dual-length rule (O-4): the CAPACITY LIST LENGTH is trusted only up to the
 * bytes the transport actually delivered, then floored to whole 8-byte
 * descriptors — a non-conformant USB-SATA bridge that over-claims the length
 * cannot make us read past the realized span. A capture is a falsifier per the
 * hardware ADR, not a design input.
 */

#include "mos_pure.h"

#define FC_HDR  4u   /* Capacity List Header bytes 0..3 */
#define FC_DESC 8u   /* every capacity descriptor is 8 bytes */

static uint32_t fc_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t fc_be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

bool mos_internal_format_caps_parse(const uint8_t *buf, size_t len,
                                    struct mos_format_caps *out)
{
    if (out) *out = (struct mos_format_caps){0};
    if (!buf || !out) return false;
    /* Need the 4-byte header plus the Current/Maximum Capacity Descriptor —
       a reply too short to carry even that is not a usable answer. */
    if (len < FC_HDR + FC_DESC) return false;

    /* CAPACITY LIST LENGTH (byte 3) counts the bytes after the header. Clamp to
       what was actually delivered (O-4), then floor to whole descriptors: the
       list is one Current/Max descriptor plus zero or more Formattable ones,
       so a coherent length is a positive multiple of 8. */
    size_t list_len = buf[3];
    size_t avail    = len - FC_HDR;
    if (list_len > avail) list_len = avail;
    list_len -= list_len % FC_DESC;
    if (list_len < FC_DESC) return false;   /* no Current/Max descriptor */

    /* Current/Maximum Capacity Descriptor at [4..11]. */
    const uint8_t *cur = buf + FC_HDR;
    out->cur_blocks      = fc_be32(cur);
    out->cur_type        = (uint8_t)(cur[4] & 0x03);
    out->cur_block_bytes = fc_be24(cur + 5);

    /* Formattable Capacity Descriptors follow, capped at MOS_FORMATTABLE_MAX.
       n is bounded by list_len/8 - 1, and list_len <= avail = len - FC_HDR, so
       the deepest read below — buf[FC_HDR + (1+n)*FC_DESC - 1] — stays < len. */
    size_t n = (list_len / FC_DESC) - 1u;
    if (n > MOS_FORMATTABLE_MAX) n = MOS_FORMATTABLE_MAX;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *d = buf + FC_HDR + FC_DESC + i * FC_DESC;
        out->d[i].blocks      = fc_be32(d);
        out->d[i].format_type = (uint8_t)(d[4] >> 2);
        out->d[i].param       = fc_be24(d + 5);
    }
    out->count = (uint8_t)n;
    return true;
}
