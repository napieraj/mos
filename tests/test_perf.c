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

/* Build a Type 03h Write Speed reply: 8-byte header + n 16-byte Write Speed
   Descriptors carrying (read_speed, write_speed) at offsets 8 and 12. */
static size_t build_write_speeds(uint8_t *b, size_t n,
                                 const uint32_t *rd, const uint32_t *wr)
{
    size_t total = 8 + n * 16;
    memset(b, 0, total);
    be32(&b[0], (uint32_t)(total - 4));
    for (size_t i = 0; i < n; i++) {
        uint8_t *d = &b[8 + i * 16];
        be32(&d[8],  rd[i]);                /* Read Speed  */
        be32(&d[12], wr[i]);                /* Write Speed */
    }
    return total;
}

TEST(perf_write_speeds_decode_list)
{
    uint8_t b[8 + 3 * 16];
    uint32_t rd[3] = { 7212, 10560, 14256 };
    uint32_t wr[3] = { 5540,  8310, 11080 };
    size_t total = build_write_speeds(b, 3, rd, wr);

    uint32_t ro[8] = {0}, wo[8] = {0};
    uint16_t n = mos_internal_perf_write_speeds_parse(b, total, ro, wo, 8);
    EXPECT(n == 3);
    EXPECT(ro[0] == 7212 && wo[0] == 5540);
    EXPECT(ro[2] == 14256 && wo[2] == 11080);
    return 0;
}

TEST(perf_write_speeds_cap_and_hostile)
{
    uint8_t b[8 + 4 * 16];
    uint32_t rd[4] = { 1, 2, 3, 4 };
    uint32_t wr[4] = { 5, 6, 7, 8 };
    (void)build_write_speeds(b, 4, rd, wr);

    /* cap clamps the count written. */
    uint32_t ro[2] = {0}, wo[2] = {0};
    EXPECT(mos_internal_perf_write_speeds_parse(b, sizeof b, ro, wo, 2) == 2);
    EXPECT(ro[1] == 2 && wo[1] == 6);

    /* Lying data length cannot extend past the real buffer. */
    uint32_t r8[8] = {0}, w8[8] = {0};
    be32(&b[0], 0xFFFFFFu);
    EXPECT(mos_internal_perf_write_speeds_parse(b, 8 + 16, r8, w8, 8) == 1);

    /* Short header / NULLs / zero cap return 0 (no OOB). */
    EXPECT(mos_internal_perf_write_speeds_parse(b, 7, r8, w8, 8) == 0);
    EXPECT(mos_internal_perf_write_speeds_parse(NULL, sizeof b, r8, w8, 8) == 0);
    EXPECT(mos_internal_perf_write_speeds_parse(b, sizeof b, r8, w8, 0) == 0);
    EXPECT(mos_internal_perf_write_speeds_parse(b, sizeof b, NULL, w8, 8) == 0);
    return 0;
}

TEST(perf_descriptor_accessors_and_null)
{
    /* The public accessors over a hand-filled struct, plus NULL/range. */
    struct mos_drive_perf p = {0};
    p.descriptor_count = 2;
    p.desc_read_kbps[0] = 7212;  p.desc_write_kbps[0] = 5540;
    p.desc_read_kbps[1] = 10560; p.desc_write_kbps[1] = 8310;

    EXPECT(mos_drive_perf_descriptor_count(&p) == 2);
    EXPECT(mos_drive_perf_descriptor_read_kbps(&p, 0) == 7212);
    EXPECT(mos_drive_perf_descriptor_write_kbps(&p, 1) == 8310);
    EXPECT(mos_drive_perf_descriptor_read_kbps(&p, 2) == 0);   /* out of range */
    EXPECT(mos_drive_perf_descriptor_count(NULL) == 0);
    EXPECT(mos_drive_perf_descriptor_read_kbps(NULL, 0) == 0);
    EXPECT(mos_drive_perf_descriptor_write_kbps(NULL, 0) == 0);
    return 0;
}

void register_perf_tests(void)
{
    RUN(perf_max_across_descriptors);
    RUN(perf_empty_descriptor_list);
    RUN(perf_null_out_params_tolerated);
    RUN(perf_fail_closed_on_hostile_buffers);
    RUN(perf_write_speeds_decode_list);
    RUN(perf_write_speeds_cap_and_hostile);
    RUN(perf_descriptor_accessors_and_null);
}
