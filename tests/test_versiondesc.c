/*
 * test_versiondesc.c — STANDARD INQUIRY decode: the version byte and the
 * version-descriptor list (bytes 58-73), plus the dual-length bound (the
 * reply's own Additional Length and the passed len both cap the descriptor
 * region) and empty-slot / short-buffer handling.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

TEST(versiondesc_version_and_descriptors)
{
    uint8_t buf[96] = {0};
    buf[0] = 0x05;                 /* MMC peripheral device type */
    buf[2] = 0x06;                 /* VERSION = SPC-4            */
    buf[4] = 91;                   /* Additional Length → total 96 */
    put_be16(&buf[58], 0x00A0);
    put_be16(&buf[60], 0x0460);
    put_be16(&buf[62], 0x02A0);    /* slots 4-8 left zero (empty) */

    mos_drive_standards s;
    EXPECT(mos_internal_versiondesc_parse(buf, sizeof buf, &s));
    EXPECT_EQ(s.spc_version, 0x06);
    EXPECT_EQ(s.descriptor_count, 3);
    EXPECT_EQ(s.descriptors[0], 0x00A0);
    EXPECT_EQ(s.descriptors[1], 0x0460);
    EXPECT_EQ(s.descriptors[2], 0x02A0);
    return 0;
}

TEST(versiondesc_additional_length_and_len_both_bound)
{
    uint8_t buf[96] = {0};
    buf[2] = 0x05;                 /* SPC-3 */
    put_be16(&buf[58], 0x00A0);
    put_be16(&buf[60], 0x0460);

    /* Additional Length covers only the 36-byte region (add=31 → total 36):
       descriptors must NOT be read even though the bytes exist in buf. */
    buf[4] = 31;
    mos_drive_standards s;
    EXPECT(mos_internal_versiondesc_parse(buf, sizeof buf, &s));
    EXPECT_EQ(s.spc_version, 0x05);
    EXPECT_EQ(s.descriptor_count, 0);

    /* Honest full Additional Length → both descriptors visible. */
    buf[4] = 91;
    EXPECT(mos_internal_versiondesc_parse(buf, sizeof buf, &s));
    EXPECT_EQ(s.descriptor_count, 2);

    /* A lying-long Additional Length cannot read past the passed len: len 60
       admits the descriptor at 58 (ends at 60) but not the one at 60. */
    EXPECT(mos_internal_versiondesc_parse(buf, 60, &s));
    EXPECT_EQ(s.descriptor_count, 1);
    EXPECT_EQ(s.descriptors[0], 0x00A0);
    return 0;
}

TEST(versiondesc_skips_empty_slots_short_and_null)
{
    uint8_t buf[96] = {0};
    buf[2] = 0x04; buf[4] = 91;    /* SPC-2 */
    put_be16(&buf[58], 0x0000);    /* empty slot — skipped */
    put_be16(&buf[60], 0x0140);
    put_be16(&buf[62], 0x0000);    /* empty slot — skipped */
    put_be16(&buf[64], 0x0600);

    mos_drive_standards s;
    EXPECT(mos_internal_versiondesc_parse(buf, sizeof buf, &s));
    EXPECT_EQ(s.descriptor_count, 2);
    EXPECT_EQ(s.descriptors[0], 0x0140);
    EXPECT_EQ(s.descriptors[1], 0x0600);

    /* Shorter than the 5-byte fixed header → false; NULL args safe. */
    EXPECT(!mos_internal_versiondesc_parse(buf, 4, &s));
    EXPECT(!mos_internal_versiondesc_parse(NULL, 96, &s));
    EXPECT(!mos_internal_versiondesc_parse(buf, 96, NULL));
    return 0;
}

void register_versiondesc_tests(void)
{
    RUN(versiondesc_version_and_descriptors);
    RUN(versiondesc_additional_length_and_len_both_bound);
    RUN(versiondesc_skips_empty_slots_short_and_null);
}
