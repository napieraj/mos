/*
 * mos_perf.c — pure, bounds-safe decode of a GET PERFORMANCE (MMC 0xAC)
 * Performance Data response, TYPE 00h — the data type Apple's
 * GetPerformance convenience method retrieves (the TYPE 03h write-speed
 * carve-out is in SPEC.md). The read-vs-write direction is the WRITE bit
 * in the CDB, so the adapter issues this twice (WRITE=0, WRITE=1) and
 * this decode returns the max performance found in one reply.
 *
 * No IOKit. The IOKit shell issues GET PERFORMANCE via GetPerformance
 * into a fixed, zero-initialized buffer and hands it plus its size here.
 * Every length and value byte is device-reported and hostile; this file
 * keeps the declared length from steering a read outside [buf, buf+len)
 * and reads only fixed offsets within each descriptor.
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
 * The descriptor layout is the MMC-6 Nominal Performance Descriptor,
 * built to spec (AGENTS hardware ADR: a real capture is a falsifier, it
 * does not steer the offsets). The list may be empty (a drive that
 * declines the direction) — that is data (count 0), not an error. No
 * payload byte is ever used as an offset.
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

/* Decode one GET PERFORMANCE Performance Data reply: the maximum
   performance (kB/s) across its descriptors and the descriptor count.
   True when the 8-byte header is present and coherent (the descriptor
   list may be empty). */
bool mos_internal_perf_data_parse(const uint8_t *buf, size_t len,
                                  uint32_t *max_kbps, uint16_t *count)
{
    if (max_kbps) *max_kbps = 0;
    if (count)    *count = 0;
    if (!buf || len < GP_HDR) return false;

    /* Performance Data Length counts bytes AFTER byte 3, so the response
       occupies 4 + value bytes. Declared only ever shrinks the trusted
       region; computed wide so the +4 cannot wrap. */
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
