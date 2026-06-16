/*
 * mos_perf.c — pure, bounds-safe decode of a GET PERFORMANCE (MMC 0xAC)
 * Performance Data response, TYPE 00h — the type Apple's GetPerformance
 * retrieves (the TYPE 03h write-speed carve-out is in SPEC.md). Direction
 * is the CDB WRITE bit, so the adapter issues this twice (WRITE=0/1); this
 * decode returns the max performance found in one reply.
 *
 * No IOKit: the shell hands us a fixed zero-init buffer (filled via
 * GetPerformance) and its size. Every length is device-reported, hence
 * hostile — the declared length must never steer a read outside
 * [buf, buf+len); only fixed offsets within each descriptor are read.
 *
 * Wire layout (MMC GET PERFORMANCE, Performance Data, TYPE 00h):
 *   [0..3]  Performance Data Length (BE) — bytes AFTER byte 3
 *   [4]     bit1 Write, bit0 Except (echo)   [5..7] reserved
 *   [8..]   Nominal Performance Descriptors, 16 bytes each:
 *     desc[0..3]   Start LBA
 *     desc[4..7]   Start Performance (kB/s, BE)
 *     desc[8..11]  End LBA
 *     desc[12..15] End Performance (kB/s, BE)
 *
 * Layout is the MMC-6 Nominal Performance Descriptor, built to spec (per
 * the hardware ADR a capture falsifies, it does not steer offsets). An
 * empty list (drive declines the direction) is data (count 0), not an
 * error. No payload byte is ever used as an offset.
 */

#include "mos_pure.h"

#define GP_HDR        8u     /* 4-byte data length + 4 reserved/echo */
#define GP_DESC       16u    /* one Nominal Performance Descriptor */
#define GP_DESC_CAP   256u   /* clamp: no real drive lists this many */

static uint32_t mos_internal_gp_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | p[3];
}

/* Decode one Performance Data reply: max performance (kB/s) across its
   descriptors and the count. True when the 8-byte header is present and
   coherent (the descriptor list may be empty). */
bool mos_internal_perf_data_parse(const uint8_t *buf, size_t len,
                                  uint32_t *max_kbps, uint16_t *count)
{
    if (max_kbps) *max_kbps = 0;
    if (count)    *count = 0;
    if (!buf || len < GP_HDR) return false;

    /* Performance Data Length counts bytes AFTER byte 3 (response = 4 +
       value). Declared can only shrink the trusted region; computed wide
       so the +4 cannot wrap. */
    size_t declared = (size_t)mos_internal_gp_be32(&buf[0]) + 4u;
    size_t end = (len < declared) ? len : declared;
    if (end < GP_HDR) return false;

    size_t n = (end - GP_HDR) / GP_DESC;
    if (n > GP_DESC_CAP) n = GP_DESC_CAP;

    uint32_t mx = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *d = &buf[GP_HDR + i * GP_DESC];
        uint32_t sp = mos_internal_gp_be32(&d[4]);    /* Start Performance */
        uint32_t ep = mos_internal_gp_be32(&d[12]);   /* End Performance   */
        if (sp > mx) mx = sp;
        if (ep > mx) mx = ep;
    }

    if (max_kbps) *max_kbps = mx;
    if (count)    *count = (uint16_t)n;
    return true;
}
