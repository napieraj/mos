/*
 * mos_vpd80.c — pure, bounds-safe decode of INQUIRY VPD page 0x80 (Unit
 * Serial Number). The one identity field DiscRecording's directory does not
 * cache and no convenience method can carry: MMCDeviceInterface's Inquiry
 * issues only a standard INQUIRY (no EVPD / PAGE CODE), so page 0x80 needs a
 * raw INQUIRY (mos_serial.c). Design + layer-1 raw-verb showing:
 * doc/research/2026-06-16-serial-vpd-0x80-feasibility.md.
 *
 * No IOKit: the shell hands us a fixed zero-init buffer (filled via
 * mos_internal_raw_cdb) bounded to the bytes the transport actually returned
 * (dual-length rule O-4 — the realized count, not the device-claimed
 * length, is the trusted span). The page's own PAGE LENGTH never extends
 * past that span; if it exceeds it (the transport under-delivered) the
 * reply is refused rather than emitted as a prefix — a durable identity key
 * is complete or nothing (the canonical-data corollary of O-4).
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
   page code 0x80 and carries a complete, non-empty serial (trailing spaces /
   NULs trimmed). out is always NUL-terminated. A drive that does not implement
   the page (it is optional), has none programmed (all-spaces), or under-
   delivers the reply (PAGE LENGTH > bytes received) returns false → the caller
   leaves serial null, never an empty or truncated string. A serial that is
   complete on the wire but longer than out_cap is truncated with a visible
   "..." marker (never a silent prefix — see the copy step). Non-ASCII bytes
   are copied verbatim and escaped at the output sink (mos_cli_json_str /
   mos_safe_ascii), as with vendor/product/revision. */
bool mos_internal_vpd80_serial_parse(const uint8_t *buf, size_t len,
                                     char *out, size_t out_cap)
{
    if (out && out_cap) out[0] = 0;
    if (!buf || !out || out_cap == 0) return false;
    if (len < VPD_HDR) return false;          /* no room for the VPD header */
    if (buf[1] != 0x80) return false;         /* wrong page echoed — refuse */

    /* PAGE LENGTH (byte 3) is the serial byte count the drive CLAIMS. The
       reply must actually carry all of it. If PAGE LENGTH exceeds the bytes
       delivered (avail — the realized-transfer span, O-4), the transport
       under-filled (a non-conformant USB-SATA bridge, a short transfer) and we
       hold only a PREFIX of the serial. REFUSE rather than emit it: this value
       is a DURABLE IDENTITY KEY the caller caches sticky (mos watch grabs it
       once per session), and a silent prefix can collide with another drive or
       misidentify this one — an incomplete key is worse than none. Complete or
       nothing. (A conforming drive, given mos_serial.c's ample 252-byte
       allocation, returns the whole serial, so page_len <= avail holds.) This
       still bounds the read: refusing happens before any serial byte is read,
       so a hostile over-long PAGE LENGTH cannot read past the trusted span. */
    size_t page_len = buf[3];
    size_t avail    = len - VPD_HDR;
    if (page_len > avail) return false;
    size_t serial_len = page_len;

    /* Trim trailing wire padding — spaces (SPC pad) and NULs. Leading and
       interior bytes are data and stay (mirrors mos_dr.c identity trim). */
    while (serial_len > 0) {
        uint8_t c = buf[VPD_HDR + serial_len - 1];
        if (c != ' ' && c != 0x00) break;
        serial_len--;
    }
    if (serial_len == 0) return false;        /* page present, no serial */

    /* Copy into out, reserving the NUL. A serial that fits is copied whole.
       One that exceeds the buffer is truncated WITH A VISIBLE TRAILING MARKER
       ("…" as ASCII "..."), never silently: this value can be cached as a
       durable identity key, and a silent prefix is indistinguishable from a
       complete serial — a different-but-equally-wrong key. The marker signals
       "incomplete" so the consumer never mistakes the prefix for the whole
       serial. ASCII so it survives both sinks unescaped (mos_safe_ascii /
       mos_cli_json_str). Real serials sit far below the buffer (mos_serial.c
       sinks 64 bytes), so this fires only on a pathological/hostile over-long
       serial; it never overflows. (Distinct from the transport-under-delivery
       refusal above: there we hold only a prefix of UNKNOWN length and refuse;
       here we hold the WHOLE serial and our sink is merely too small, so the
       marked prefix is honest about exactly what it is.) */
    static const char MARK[] = "...";
    const size_t mark_len = sizeof MARK - 1u;       /* 3 */
    if (serial_len <= out_cap - 1u) {
        for (size_t i = 0; i < serial_len; i++) out[i] = (char)buf[VPD_HDR + i];
        out[serial_len] = 0;
    } else if (out_cap > mark_len + 1u) {
        size_t copy = out_cap - 1u - mark_len;
        for (size_t i = 0; i < copy; i++) out[i] = (char)buf[VPD_HDR + i];
        for (size_t i = 0; i < mark_len; i++) out[copy + i] = MARK[i];
        out[copy + mark_len] = 0;
    } else {
        /* out_cap too small even to hold the marker — plain bounded copy. */
        size_t copy = out_cap - 1u;
        for (size_t i = 0; i < copy; i++) out[i] = (char)buf[VPD_HDR + i];
        out[copy] = 0;
    }
    return true;
}
