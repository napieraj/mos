/*
 * test_inqdata.c — STANDARD INQUIRY decode: the version byte and the
 * version-descriptor list (bytes 58-73), plus the dual-length bound (the
 * reply's own Additional Length and the passed len both cap the descriptor
 * region) and empty-slot / short-buffer handling.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"

#include <string.h>

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

TEST(inqdata_identity_version_and_descriptors)
{
    uint8_t buf[96] = {0};
    buf[0] = 0x05;                 /* MMC peripheral device type */
    buf[2] = 0x06;                 /* VERSION = SPC-4            */
    buf[4] = 91;                   /* Additional Length → total 96 */
    memcpy(&buf[8],  "HL-DT-ST", 8);              /* vendor   (8)  */
    memcpy(&buf[16], "BD-RE  WH16NS40 ", 16);     /* product  (16, trailing pad) */
    memcpy(&buf[32], "1.05", 4);                  /* revision (4)  */
    put_be16(&buf[58], 0x00A0);
    put_be16(&buf[60], 0x0460);
    put_be16(&buf[62], 0x02A0);    /* slots 4-8 left zero (empty) */

    mos_drive_inquiry s;
    EXPECT(mos_internal_inqdata_parse(buf, sizeof buf, &s));
    EXPECT_STREQ(s.vendor, "HL-DT-ST");
    EXPECT_STREQ(s.product, "BD-RE  WH16NS40");   /* trailing space trimmed */
    EXPECT_STREQ(s.revision, "1.05");
    EXPECT_EQ(s.spc_version, 0x06);
    EXPECT_EQ(s.descriptor_count, 3);
    EXPECT_EQ(s.descriptors[0], 0x00A0);
    EXPECT_EQ(s.descriptors[1], 0x0460);
    EXPECT_EQ(s.descriptors[2], 0x02A0);
    return 0;
}

TEST(inqdata_identity_trim_and_length_bound)
{
    uint8_t buf[96] = {0};
    buf[2] = 0x06;
    memcpy(&buf[8],  "VENDOR  ", 8);              /* trailing spaces → trimmed */
    memcpy(&buf[16], "                ", 16);     /* all spaces → ""           */
    memcpy(&buf[32], "ABCD", 4);

    /* Additional Length covers only through byte 15 (add=11 → total 16):
       vendor (8-15) is present, product (16-31) and revision (32-35) are
       beyond the trusted region and must come back empty. */
    buf[4] = 11;
    mos_drive_inquiry s;
    EXPECT(mos_internal_inqdata_parse(buf, sizeof buf, &s));
    EXPECT_STREQ(s.vendor, "VENDOR");
    EXPECT(s.product[0] == 0);
    EXPECT(s.revision[0] == 0);

    /* Full length: all-spaces product trims to "", revision present. */
    buf[4] = 91;
    EXPECT(mos_internal_inqdata_parse(buf, sizeof buf, &s));
    EXPECT_STREQ(s.vendor, "VENDOR");
    EXPECT(s.product[0] == 0);
    EXPECT_STREQ(s.revision, "ABCD");
    return 0;
}

TEST(inqdata_additional_length_and_len_both_bound)
{
    uint8_t buf[96] = {0};
    buf[2] = 0x05;                 /* SPC-3 */
    put_be16(&buf[58], 0x00A0);
    put_be16(&buf[60], 0x0460);

    /* Additional Length covers only the 36-byte region (add=31 → total 36):
       descriptors must NOT be read even though the bytes exist in buf. */
    buf[4] = 31;
    mos_drive_inquiry s;
    EXPECT(mos_internal_inqdata_parse(buf, sizeof buf, &s));
    EXPECT_EQ(s.spc_version, 0x05);
    EXPECT_EQ(s.descriptor_count, 0);

    /* Honest full Additional Length → both descriptors visible. */
    buf[4] = 91;
    EXPECT(mos_internal_inqdata_parse(buf, sizeof buf, &s));
    EXPECT_EQ(s.descriptor_count, 2);

    /* A lying-long Additional Length cannot read past the passed len: len 60
       admits the descriptor at 58 (ends at 60) but not the one at 60. */
    EXPECT(mos_internal_inqdata_parse(buf, 60, &s));
    EXPECT_EQ(s.descriptor_count, 1);
    EXPECT_EQ(s.descriptors[0], 0x00A0);
    return 0;
}

TEST(inqdata_skips_empty_slots_short_and_null)
{
    uint8_t buf[96] = {0};
    buf[2] = 0x04; buf[4] = 91;    /* SPC-2 */
    put_be16(&buf[58], 0x0000);    /* empty slot — skipped */
    put_be16(&buf[60], 0x0140);
    put_be16(&buf[62], 0x0000);    /* empty slot — skipped */
    put_be16(&buf[64], 0x0600);

    mos_drive_inquiry s;
    EXPECT(mos_internal_inqdata_parse(buf, sizeof buf, &s));
    EXPECT_EQ(s.descriptor_count, 2);
    EXPECT_EQ(s.descriptors[0], 0x0140);
    EXPECT_EQ(s.descriptors[1], 0x0600);

    /* Shorter than the 5-byte fixed header → false; NULL args safe. */
    EXPECT(!mos_internal_inqdata_parse(buf, 4, &s));
    EXPECT(!mos_internal_inqdata_parse(NULL, 96, &s));
    EXPECT(!mos_internal_inqdata_parse(buf, 96, NULL));
    return 0;
}

void register_inqdata_tests(void)
{
    RUN(inqdata_identity_version_and_descriptors);
    RUN(inqdata_identity_trim_and_length_bound);
    RUN(inqdata_additional_length_and_len_both_bound);
    RUN(inqdata_skips_empty_slots_short_and_null);
}
