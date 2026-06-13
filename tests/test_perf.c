/*
 * test_perf.c — GET PERFORMANCE (0xAC, Type 00h Performance Data) decode.
 * Fixtures are built to the MMC-6 Nominal Performance Descriptor layout
 * (spec-derived; a real capture is a falsifier per the hardware ADR).
 * The hostile cases pin the no-OOB property — a device-controlled data
 * length and a wild descriptor count must never read past the buffer.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Build a Performance Data reply: 8-byte header + n 16-byte descriptors,
   each carrying (start_perf, end_perf) kB/s at offsets 4 and 12. */
static size_t build_perf(uint8_t *b, size_t n,
                         const uint32_t *start_perf, const uint32_t *end_perf)
{
    size_t total = 8 + n * 16;
    memset(b, 0, total);
    be32(&b[0], (uint32_t)(total - 4));     /* data length = bytes after byte 3 */
    for (size_t i = 0; i < n; i++) {
        uint8_t *d = &b[8 + i * 16];
        be32(&d[0], 0);                     /* start LBA */
        be32(&d[4], start_perf[i]);         /* start performance */
        be32(&d[8], 0x10000);               /* end LBA */
        be32(&d[12], end_perf[i]);          /* end performance */
    }
    return total;
}

TEST(perf_max_across_descriptors)
{
    uint8_t b[8 + 3 * 16];
    uint32_t sp[3] = { 7212, 10560, 5400 };
    uint32_t ep[3] = { 7212,  9000, 5400 };
    size_t total = build_perf(b, 3, sp, ep);

    uint32_t max_kbps = 0;
    uint16_t count = 0;
    EXPECT(mos_internal_perf_data_parse(b, total, &max_kbps, &count));
    EXPECT(count == 3);
    EXPECT(max_kbps == 10560);              /* max of all start/end perf */
    return 0;
}

TEST(perf_empty_descriptor_list)
{
    /* Header only, no descriptors: valid parse, count 0, max 0. */
    uint8_t b[8];
    (void)build_perf(b, 0, NULL, NULL);

    uint32_t max_kbps = 99;
    uint16_t count = 99;
    EXPECT(mos_internal_perf_data_parse(b, sizeof b, &max_kbps, &count));
    EXPECT(count == 0);
    EXPECT(max_kbps == 0);
    return 0;
}

TEST(perf_null_out_params_tolerated)
{
    uint8_t b[8 + 16];
    uint32_t sp[1] = { 5540 }, ep[1] = { 5540 };
    size_t total = build_perf(b, 1, sp, ep);
    /* Either out-param may be NULL. */
    EXPECT(mos_internal_perf_data_parse(b, total, NULL, NULL));
    uint32_t m = 0;
    EXPECT(mos_internal_perf_data_parse(b, total, &m, NULL));
    EXPECT(m == 5540);
    return 0;
}

TEST(perf_fail_closed_on_hostile_buffers)
{
    uint8_t b[8 + 2 * 16];
    uint32_t sp[2] = { 7212, 10560 };
    uint32_t ep[2] = { 7212,  9000 };
    (void)build_perf(b, 2, sp, ep);
    uint32_t max_kbps = 0;
    uint16_t count = 0;

    /* Lying data length must NOT extend the read past the real buffer:
       a huge declared length with a short buffer parses only the
       descriptors the buffer actually holds. */
    be32(&b[0], 0xFFFFFFu);                  /* declared ~16MB */
    EXPECT(mos_internal_perf_data_parse(b, 8 + 16, &max_kbps, &count));
    EXPECT(count == 1);                      /* only 1 descriptor fits */

    /* Header shorter than 8 bytes refuses. */
    EXPECT(!mos_internal_perf_data_parse(b, 7, &max_kbps, &count));
    EXPECT(!mos_internal_perf_data_parse(b, 0, &max_kbps, &count));
    EXPECT(!mos_internal_perf_data_parse(NULL, sizeof b, &max_kbps, &count));

    /* A trailing partial descriptor (not a full 16 bytes) is ignored,
       not read out of bounds. */
    (void)build_perf(b, 2, sp, ep);
    EXPECT(mos_internal_perf_data_parse(b, 8 + 16 + 8, &max_kbps, &count));
    EXPECT(count == 1);                      /* the half descriptor dropped */
    return 0;
}

void register_perf_tests(void)
{
    RUN(perf_max_across_descriptors);
    RUN(perf_empty_descriptor_list);
    RUN(perf_null_out_params_tolerated);
    RUN(perf_fail_closed_on_hostile_buffers);
}
