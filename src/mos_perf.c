/*
 * mos_perf.c — pure, bounds-safe decode of a GET PERFORMANCE (MMC 0xAC,
 * Type 03h = Write Speed) response: the drive's supported read/write
 * speed descriptor list, summarized as the max read and max write speed
 * (kB/s) and the descriptor count.
 *
 * No IOKit. The IOKit shell issues GET PERFORMANCE via the GetPerformance
 * convenience method into a fixed, zero-initialized buffer and hands that
 * buffer plus its size here. Every length and value byte is device-
 * reported and hostile; this file keeps the declared length from steering
 * a read outside [buf, buf+len) and reads only fixed offsets within each
 * descriptor.
 *
 * Wire layout (MMC-6 GET PERFORMANCE, Type 03h Write Speed):
 *   [0..3]  Write Speed Descriptor Data Length (BE) — bytes AFTER byte 3
 *   [4..7]  reserved
 *   [8..]   Write Speed Performance Descriptors, 16 bytes each:
 *     desc[0]     flag byte (MRW/Exact/RDD/WRC) — not surfaced
 *     desc[4..7]  End LBA / capacity (BE)
 *     desc[8..11] Read Speed  (kB/s, BE)
 *     desc[12..15] Write Speed (kB/s, BE)
 *
 * SPEC-DERIVED, no in-repo capture yet: the descriptor offsets are the
 * MMC-6 Write Speed Performance Descriptor layout. Per the AGENTS
 * hardware ADR this is built to spec; a real GET PERFORMANCE capture is
 * a falsification-matrix item that can refute or feed these offsets, it
 * does not steer them. GET PERFORMANCE is also media-dependent (an empty
 * or read-only drive may report zero write-speed descriptors) — that is
 * data (have=false), not an error. No payload byte is ever used as an
 * offset. No-OOB property gated headless under ASan/UBSan by
 * tests/test_perf.c and tests/fuzz_pure.c.
 */

#include "mos_pure.h"

#define GP_HDR        8u     /* 4-byte data length + 4 reserved */
#define GP_DESC       16u    /* one Write Speed Performance Descriptor */
#define GP_DESC_CAP   256u   /* clamp: no real drive lists this many speeds */

static uint32_t mos_internal_gp_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | p[3];
}

bool mos_internal_drive_perf_parse(const uint8_t *buf, size_t len,
                                   struct mos_drive_perf *out)
{
    if (!out) return false;
    *out = (struct mos_drive_perf){0};
    if (!buf || len < GP_HDR) return false;

    /* Performance Data Length counts the bytes AFTER byte 3, so the
       response occupies 4 + value bytes. Declared only ever shrinks the
       trusted region; computed wide so the +4 cannot wrap. */
    size_t declared = (size_t)mos_internal_gp_be32(&buf[0]) + 4u;
    size_t end = (len < declared) ? len : declared;
    if (end < GP_HDR) return false;

    size_t avail = end - GP_HDR;
    size_t n = avail / GP_DESC;
    if (n > GP_DESC_CAP) n = GP_DESC_CAP;

    uint32_t max_read = 0, max_write = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *d = &buf[GP_HDR + i * GP_DESC];
        uint32_t rd = mos_internal_gp_be32(&d[8]);
        uint32_t wr = mos_internal_gp_be32(&d[12]);
        if (rd > max_read)  max_read  = rd;
        if (wr > max_write) max_write = wr;
    }

    out->descriptor_count = (uint16_t)n;
    out->max_read_kbps    = max_read;
    out->max_write_kbps   = max_write;
    out->have             = (n > 0);
    return true;
}
