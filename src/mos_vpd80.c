/*
 * mos_vpd80.c — pure, bounds-safe decode of INQUIRY VPD page 0x80 (Unit
 * Serial Number). The one identity field DiscRecording's directory does not
 * cache and no convenience method can carry: MMCDeviceInterface's Inquiry
 * issues only a standard INQUIRY (no EVPD / PAGE CODE), so page 0x80 needs a
 * raw INQUIRY (mos_serial.c). Design + layer-1 raw-verb showing:
 * doc/research/2026-06-16-serial-vpd-0x80-feasibility.md.
 *
 * No IOKit: the shell hands us a fixed zero-init buffer (filled via
 * mos_raw_cdb) bounded to the bytes the transport actually returned
 * (dual-length rule O-4 — the realized count, not the device-claimed
 * length, is the trusted span). The page's own PAGE LENGTH can only shrink
 * the serial region within that span, never extend it.
 *
 * Page 0x80 layout (SPC-4 §7.7.13, Unit Serial Number VPD page):
 *   [0]      PERIPHERAL QUALIFIER (7:5) | PERIPHERAL DEVICE TYPE (4:0)
 *   [1]      PAGE CODE = 80h        — the drive echoes the page we asked for
 *   [2]      reserved
 *   [3]      PAGE LENGTH (n-3)      — serial byte count
 *   [4..n]   PRODUCT SERIAL NUMBER  — ASCII, left-justified, space-padded
 * Byte 2 stays reserved for this page (it is NOT the high byte of a 2-byte
 * length — that generalization is page 0x83's, not 0x80's). A real page-0x80
 * capture is a falsifier per the hardware ADR, not a design input.
 */

#include "mos_pure.h"

#define VPD_HDR 4u   /* bytes 0..3 before the serial */

/* Decode page 0x80 into out[0..out_cap). True only when the reply echoes
   page code 0x80 and carries a non-empty serial (trailing spaces / NULs
   trimmed). out is always NUL-terminated. A drive that does not implement
   the page (it is optional) or has none programmed (all-spaces) returns
   false → the caller leaves serial null, never an empty string. Non-ASCII
   bytes are copied verbatim and escaped at the output sink
   (mos_cli_json_str / mos_safe_ascii), as with vendor/product/revision. */
bool mos_internal_vpd80_serial_parse(const uint8_t *buf, size_t len,
                                     char *out, size_t out_cap)
{
    if (out && out_cap) out[0] = 0;
    if (!buf || !out || out_cap == 0) return false;
    if (len < VPD_HDR) return false;          /* no room for the VPD header */
    if (buf[1] != 0x80) return false;         /* wrong page echoed — refuse */

    /* PAGE LENGTH (byte 3) bounded by the bytes actually present (O-4): a
       hostile/over-long length cannot read past the trusted span. */
    size_t page_len = buf[3];
    size_t avail    = len - VPD_HDR;
    size_t serial_len = (page_len < avail) ? page_len : avail;

    /* Trim trailing wire padding — spaces (SPC pad) and NULs. Leading and
       interior bytes are data and stay (mirrors mos_dr.c identity trim). */
    while (serial_len > 0) {
        uint8_t c = buf[VPD_HDR + serial_len - 1];
        if (c != ' ' && c != 0x00) break;
        serial_len--;
    }
    if (serial_len == 0) return false;        /* page present, no serial */

    /* Copy bounded by out_cap (reserve the NUL). A serial longer than the
       buffer is truncated, never overflowed — real serials are << the cap. */
    size_t copy = (serial_len < out_cap - 1) ? serial_len : out_cap - 1;
    for (size_t i = 0; i < copy; i++) out[i] = (char)buf[VPD_HDR + i];
    out[copy] = 0;
    return true;
}
