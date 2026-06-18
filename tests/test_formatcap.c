/*
 * test_formatcap.c — READ FORMAT CAPACITIES (0x23) Capacity List decode.
 * Fixtures are built to MMC-6 §6.24; the hostile cases pin the no-OOB
 * property — a device-controlled CAPACITY LIST LENGTH must only ever SHRINK
 * the trusted region, and a non-multiple-of-8 list is refused.
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
static void be24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

/* Header (4) + Current/Max (8) + n Formattable descriptors (8 each). */
static size_t build_fc(uint8_t *b, uint32_t cur_blocks, uint8_t cur_type,
                       uint32_t cur_block_len, size_t n,
                       const uint32_t *blocks, const uint8_t *ftype,
                       const uint32_t *param)
{
    size_t list_len = 8u + 8u * n;          /* bytes after the 4-byte header */
    memset(b, 0, 4 + list_len);
    b[3] = (uint8_t)list_len;
    be32(&b[4], cur_blocks);
    b[8] = (uint8_t)(cur_type & 0x03);
    be24(&b[9], cur_block_len);
    for (size_t i = 0; i < n; i++) {
        uint8_t *d = b + 12 + i * 8;
        be32(&d[0], blocks[i]);
        d[4] = (uint8_t)((ftype[i] & 0x3f) << 2);
        be24(&d[5], param[i]);
    }
    return 4 + list_len;
}

TEST(formatcap_blank_bdre_unformatted)
{
    /* A blank BD-RE: Current/Max says unformatted, one formattable size. */
    uint8_t b[64];
    uint32_t blk[1] = { 11826176 }; uint8_t ft[1] = { 0x00 };
    uint32_t pr[1]  = { 2048 };
    size_t len = build_fc(b, 11826176, 1 /*unformatted*/, 2048,
                          1, blk, ft, pr);

    struct mos_format_caps fc;
    EXPECT(mos_internal_format_caps_parse(b, len, &fc));
    EXPECT(fc.cur_type == 1);
    EXPECT(fc.cur_blocks == 11826176);
    EXPECT(fc.cur_block_bytes == 2048);
    EXPECT(fc.count == 1);
    EXPECT(fc.d[0].blocks == 11826176);
    EXPECT(fc.d[0].format_type == 0x00);
    EXPECT(fc.d[0].param == 2048);
    return 0;
}

TEST(formatcap_dvdram_multiple_sizes)
{
    /* DVD-RAM advertising several formattable capacities + a formatted
       Current/Max descriptor. */
    uint8_t b[64];
    uint32_t blk[3] = { 2295104, 2295072, 1 };
    uint8_t  ft[3]  = { 0x00, 0x01, 0x26 };
    uint32_t pr[3]  = { 2048, 2048, 0 };
    size_t len = build_fc(b, 2295104, 2 /*formatted*/, 2048, 3, blk, ft, pr);

    struct mos_format_caps fc;
    EXPECT(mos_internal_format_caps_parse(b, len, &fc));
    EXPECT(fc.cur_type == 2);
    EXPECT(fc.count == 3);
    EXPECT(fc.d[1].blocks == 2295072);
    EXPECT(fc.d[1].format_type == 0x01);
    EXPECT(fc.d[2].format_type == 0x26);
    return 0;
}

TEST(formatcap_only_current_descriptor)
{
    /* List length == 8: just the Current/Max descriptor, no formattable
       list (e.g. a no-media or fixed-capacity reply). Still parses. */
    uint8_t b[32];
    size_t len = build_fc(b, 0, 3 /*no media*/, 0, 0, NULL, NULL, NULL);

    struct mos_format_caps fc;
    EXPECT(mos_internal_format_caps_parse(b, len, &fc));
    EXPECT(fc.cur_type == 3);
    EXPECT(fc.count == 0);
    return 0;
}

TEST(formatcap_list_length_clamped_to_transfer)
{
    /* CAPACITY LIST LENGTH claims more descriptors than were delivered: the
       parse must clamp to the realized span (O-4), never read past it. */
    uint8_t b[64];
    uint32_t blk[1] = { 100 }; uint8_t ft[1] = { 0 }; uint32_t pr[1] = { 2048 };
    (void)build_fc(b, 100, 1, 2048, 1, blk, ft, pr);  /* real list = 16 */
    b[3] = 80;   /* lie: claim 80 bytes (10 descriptors) */

    struct mos_format_caps fc;
    /* Only header + current/max + 1 formattable actually present (len=20). */
    EXPECT(mos_internal_format_caps_parse(b, 20, &fc));
    EXPECT(fc.cur_blocks == 100);
    EXPECT(fc.count == 1);    /* (20-4)/8 - 1 = 1, not the claimed 9 */
    return 0;
}

TEST(formatcap_many_descriptors)
{
    /* A long-but-conformant list (20 descriptors; CAPACITY LIST LENGTH = 168,
       within the single byte) parses fully without truncation — the array
       bound (32) sits above the single-byte spec max (~30). */
    uint8_t b[4 + 8 + 8 * 20];
    uint32_t blk[20]; uint8_t ft[20]; uint32_t pr[20];
    for (size_t i = 0; i < 20; i++) { blk[i] = (uint32_t)(i + 1); ft[i] = 0; pr[i] = 2048; }
    size_t len = build_fc(b, 1, 2, 2048, 20, blk, ft, pr);

    struct mos_format_caps fc;
    EXPECT(mos_internal_format_caps_parse(b, len, &fc));
    EXPECT(fc.count == 20);
    EXPECT(fc.d[19].blocks == 20);
    return 0;
}

TEST(formatcap_fail_closed_on_hostile_buffers)
{
    uint8_t b[64];
    uint32_t blk[1] = { 1 }; uint8_t ft[1] = { 0 }; uint32_t pr[1] = { 2048 };
    (void)build_fc(b, 1, 1, 2048, 1, blk, ft, pr);
    struct mos_format_caps fc;

    /* Too short for header + Current/Max descriptor (need >= 12). */
    EXPECT(!mos_internal_format_caps_parse(b, 11, &fc));
    EXPECT(fc.cur_blocks == 0);

    /* A list length that floors below one descriptor (e.g. delivered 4 bytes
       of list, < 8) refuses rather than inventing a Current/Max. */
    b[3] = 4;
    EXPECT(!mos_internal_format_caps_parse(b, 8, &fc));

    /* NULL / degenerate inputs stay in bounds and refuse. */
    EXPECT(!mos_internal_format_caps_parse(NULL, sizeof b, &fc));
    EXPECT(!mos_internal_format_caps_parse(b, sizeof b, NULL));
    return 0;
}

TEST(formatcap_profile_gate)
{
    /* Formattable: rewritable optical + BD-R (capacity issues the raw read). */
    EXPECT(mos_internal_profile_is_formattable(0x000A));  /* CD-RW    */
    EXPECT(mos_internal_profile_is_formattable(0x0012));  /* DVD-RAM  */
    EXPECT(mos_internal_profile_is_formattable(0x0013));  /* DVD-RW RO */
    EXPECT(mos_internal_profile_is_formattable(0x001A));  /* DVD+RW   */
    EXPECT(mos_internal_profile_is_formattable(0x002A));  /* DVD+RW DL */
    EXPECT(mos_internal_profile_is_formattable(0x0052));  /* HD DVD-RAM */
    EXPECT(mos_internal_profile_is_formattable(0x0053));  /* HD DVD-RW */
    EXPECT(mos_internal_profile_is_formattable(0x005A));  /* HD DVD-RW DL */
    EXPECT(mos_internal_profile_is_formattable(0x0041));  /* BD-R SRM */
    EXPECT(mos_internal_profile_is_formattable(0x0043));  /* BD-RE    */
    /* Not formattable: pressed, write-once sequential, no media → no raw read. */
    EXPECT(!mos_internal_profile_is_formattable(0x0008)); /* CD-ROM   */
    EXPECT(!mos_internal_profile_is_formattable(0x0009)); /* CD-R     */
    EXPECT(!mos_internal_profile_is_formattable(0x0010)); /* DVD-ROM  */
    EXPECT(!mos_internal_profile_is_formattable(0x0011)); /* DVD-R    */
    EXPECT(!mos_internal_profile_is_formattable(0x001B)); /* DVD+R    */
    EXPECT(!mos_internal_profile_is_formattable(0x0040)); /* BD-ROM   */
    EXPECT(!mos_internal_profile_is_formattable(0x0050)); /* HD DVD-ROM */
    EXPECT(!mos_internal_profile_is_formattable(0x0051)); /* HD DVD-R */
    EXPECT(!mos_internal_profile_is_formattable(0x0000)); /* no profile */
    return 0;
}

void register_formatcap_tests(void)
{
    RUN(formatcap_blank_bdre_unformatted);
    RUN(formatcap_dvdram_multiple_sizes);
    RUN(formatcap_only_current_descriptor);
    RUN(formatcap_list_length_clamped_to_transfer);
    RUN(formatcap_many_descriptors);
    RUN(formatcap_profile_gate);
    RUN(formatcap_fail_closed_on_hostile_buffers);
}
