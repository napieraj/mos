/*
 * mos_modepage.c — pure, bounds-safe decode of MODE SENSE(10) replies for
 * the two optical-specific pages the scope doctrine admits (read-only;
 * AGENTS.md 2026-06-13 addendum): page 0x2A (CD/DVD Capabilities &
 * Mechanical Status — loading mechanism, eject/lock, buffer size) and
 * page 0x01 (Read/Write Error Recovery — AWRE/ARRE/PER/DCR + read-retry
 * count). NO MODE SELECT: mos reports configuration, never tunes it.
 *
 * No IOKit. The IOKit shell issues MODE SENSE(10) via the ModeSense10
 * convenience method into a fixed, zero-initialized buffer and hands that
 * buffer plus its size here. Every length is device-reported and hostile;
 * the shared page walker keeps the declared lengths from steering a read
 * outside [buf, buf+len) and cannot loop (each step strictly advances).
 *
 * MODE SENSE(10) mode parameter header:
 *   [0..1] Mode Data Length (BE) — bytes AFTER this field
 *   [2]    Medium Type   [3] Device-Specific Parameter
 *   [4..5] reserved      [6..7] Block Descriptor Length (BE)
 *   [8 + block_descriptor_length ..] the mode pages, each:
 *     page[0] PS(7) SPF(6) Page Code(5:0); page[1] Page Length (n)
 *     page[2..] page data (n bytes). (Sub-page format SPF=1 has a 4-byte
 *     header with a BE16 length; pages 0x2A/0x01 are page_0 format.)
 *
 * Page 0x2A offsets (relative to page start) are from the Linux kernel
 * sr.c get_capabilities() — loading mechanism page[6]>>5, eject
 * page[6]&0x08 — plus the standard MMC-3 page-2A buffer size (page[12..13]
 * BE, KB) and lock bits (page[6] bit1 supported, bit2 state). Page 0x01
 * is the canonical SPC Read/Write Error Recovery page. The buffer-size
 * and lock-bit positions have no in-repo capture yet (kernel confirms
 * only loadmech+eject) — a real MODE SENSE capture is a falsifier per the
 * hardware ADR, not a design input. No payload byte is ever used as an
 * offset. No-OOB property gated headless under ASan/UBSan by
 * tests/test_modepage.c and tests/fuzz_pure.c.
 */

#include "mos_pure.h"

#define MP_HDR  8u   /* MODE SENSE(10) parameter header */

/* Locate a page_0-format mode page by code in a MODE SENSE(10) reply.
   On success sets *poff (page start) and *plen (page length, i.e. data
   bytes after the 2-byte page header) and returns true. Bounded: every
   iteration advances off by at least the page header, so a hostile
   page-length field cannot loop or read out of bounds. */
static bool mos_internal_mode_page_find(const uint8_t *buf, size_t len,
                                        uint8_t want,
                                        size_t *poff, size_t *plen)
{
    if (!buf || len < MP_HDR) return false;

    size_t declared = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < declared) ? len : declared;
    if (end < MP_HDR) return false;

    size_t bdl = (size_t)(((uint16_t)buf[6] << 8) | buf[7]);
    size_t off = MP_HDR + bdl;

    while (off + 2u <= end) {
        uint8_t  code = buf[off] & 0x3f;
        int      spf  = (buf[off] & 0x40) != 0;
        size_t   hdr, page_len;

        if (spf) {
            if (off + 4u > end) break;
            hdr = 4u;
            page_len = (size_t)(((uint16_t)buf[off + 2] << 8) | buf[off + 3]);
        } else {
            hdr = 2u;
            page_len = buf[off + 1];
        }

        size_t page_total = hdr + page_len;
        if (off + page_total > end) break;   /* page claims past trusted end */

        if (!spf && code == want) {
            *poff = off;
            *plen = page_len;
            return true;
        }
        if (page_total == 0) break;          /* no-progress guard */
        off += page_total;
    }
    return false;
}

bool mos_internal_mode_caps_parse(const uint8_t *buf, size_t len,
                                  struct mos_mode_caps *out)
{
    if (!out) return false;
    *out = (struct mos_mode_caps){0};

    size_t poff, plen;
    if (!mos_internal_mode_page_find(buf, len, 0x2A, &poff, &plen))
        return false;
    /* Need page bytes through 13 (buffer size at page[12..13]); the
       walker has already bounded poff + 2 + plen to the trusted end. */
    if (plen < 12u) return false;

    const uint8_t *p = &buf[poff];
    out->loading_mechanism = (uint8_t)(p[6] >> 5);
    out->can_eject         = (p[6] & 0x08) != 0;
    out->lock_supported    = (p[6] & 0x02) != 0;
    out->locked            = (p[6] & 0x04) != 0;
    out->buffer_kb         = (uint16_t)((p[12] << 8) | p[13]);
    out->have              = true;
    return true;
}

bool mos_internal_error_recovery_parse(const uint8_t *buf, size_t len,
                                       struct mos_error_recovery *out)
{
    if (!out) return false;
    *out = (struct mos_error_recovery){0};

    size_t poff, plen;
    if (!mos_internal_mode_page_find(buf, len, 0x01, &poff, &plen))
        return false;
    if (plen < 2u) return false;             /* need page bytes 2 and 3 */

    const uint8_t *p = &buf[poff];
    out->awre             = (p[2] & 0x80) != 0;
    out->arre             = (p[2] & 0x40) != 0;
    out->per              = (p[2] & 0x04) != 0;
    out->dcr              = (p[2] & 0x01) != 0;
    out->read_retry_count = p[3];
    out->have             = true;
    return true;
}
