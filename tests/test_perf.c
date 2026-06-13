/*
 * test_perf.c — GET PERFORMANCE (0xAC, Type 03h Write Speed) decode.
 * Fixtures are built to the MMC-6 Write Speed Performance Descriptor
 * layout (spec-derived; a real capture is a falsifier per the hardware
 * ADR). The hostile cases pin the no-OOB property — a device-controlled
 * data length and a wild descriptor count must never read past the
 * buffer.
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

/* Build a GET PERFORMANCE reply: 8-byte header + n 16-byte descriptors,
   each carrying (read_kbps, write_kbps) at offsets 8 and 12. */
static size_t build_perf(uint8_t *b, size_t n,
                         const uint32_t *read_kbps, const uint32_t *write_kbps)
{
    size_t total = 8 + n * 16;
    memset(b, 0, total);
    be32(&b[0], (uint32_t)(total - 4));     /* data length = bytes after byte 3 */
    for (size_t i = 0; i < n; i++) {
        uint8_t *d = &b[8 + i * 16];
        be32(&d[4], 0x10000);               /* end LBA / capacity */
        be32(&d[8], read_kbps[i]);
        be32(&d[12], write_kbps[i]);
    }
    return total;
}

TEST(perf_max_across_descriptors)
{
    uint8_t b[8 + 3 * 16];
    uint32_t rd[3] = { 7212, 10560, 5400 };
    uint32_t wr[3] = { 5540,  8310, 2770 };
    size_t total = build_perf(b, 3, rd, wr);

    struct mos_drive_perf p;
    EXPECT(mos_internal_drive_perf_parse(b, total, &p));
    EXPECT(p.have);
    EXPECT(p.descriptor_count == 3);
    EXPECT(p.max_read_kbps == 10560);
    EXPECT(p.max_write_kbps == 8310);
    return 0;
}

TEST(perf_empty_descriptor_list)
{
    /* Header only, no descriptors (read-only drive / no media): valid
       parse, have=false. */
    uint8_t b[8];
    (void)build_perf(b, 0, NULL, NULL);

    struct mos_drive_perf p;
    EXPECT(mos_internal_drive_perf_parse(b, sizeof b, &p));
    EXPECT(!p.have);
    EXPECT(p.descriptor_count == 0);
    EXPECT(p.max_write_kbps == 0);
    return 0;
}

TEST(perf_fail_closed_on_hostile_buffers)
{
    uint8_t b[8 + 2 * 16];
    uint32_t rd[2] = { 7212, 10560 };
    uint32_t wr[2] = { 5540,  8310 };
    (void)build_perf(b, 2, rd, wr);
    struct mos_drive_perf p;

    /* Lying data length must NOT extend the read past the real buffer:
       a huge declared length with a short buffer parses only the
       descriptors the buffer actually holds. */
    be32(&b[0], 0xFFFFFFu);                  /* declared ~16MB */
    EXPECT(mos_internal_drive_perf_parse(b, 8 + 16, &p));   /* only 1 desc fits */
    EXPECT(p.descriptor_count == 1);

    /* Header shorter than 8 bytes refuses. */
    EXPECT(!mos_internal_drive_perf_parse(b, 7, &p));
    EXPECT(!mos_internal_drive_perf_parse(b, 0, &p));
    EXPECT(!mos_internal_drive_perf_parse(NULL, sizeof b, &p));
    EXPECT(!mos_internal_drive_perf_parse(b, sizeof b, NULL));

    /* A trailing partial descriptor (not a full 16 bytes) is ignored,
       not read out of bounds. */
    EXPECT(mos_internal_drive_perf_parse(b, 8 + 16 + 8, &p));
    EXPECT(p.descriptor_count == 1);         /* the half descriptor dropped */
    return 0;
}

void register_perf_tests(void)
{
    RUN(perf_max_across_descriptors);
    RUN(perf_empty_descriptor_list);
    RUN(perf_fail_closed_on_hostile_buffers);
}
