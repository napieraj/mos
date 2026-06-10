/*
 * test_bsd_name.c — BSD name normalization regression tests.
 *
 * Exercises the real mos_internal_normalize_bsd_name and
 * mos_internal_bsd_name_is_whole_shape via mos_pure.h. The previous version
 * of this file mirrored the logic; that was drift-prone.
 */

#include "test_harness.h"
#include "../src/mos_pure.h"
#include <stdbool.h>
#include <string.h>

TEST(plain_diskN_passes_through)
{
    EXPECT_STREQ(mos_internal_normalize_bsd_name("disk4"), "disk4");
    return 0;
}

TEST(rdiskN_strips_leading_r)
{
    EXPECT_STREQ(mos_internal_normalize_bsd_name("rdisk4"), "disk4");
    return 0;
}

TEST(dev_diskN_strips_dev_prefix)
{
    EXPECT_STREQ(mos_internal_normalize_bsd_name("/dev/disk4"), "disk4");
    return 0;
}

TEST(dev_rdiskN_strips_both_prefixes)
{
    EXPECT_STREQ(mos_internal_normalize_bsd_name("/dev/rdisk4"), "disk4");
    return 0;
}

TEST(longer_disk_numbers_work)
{
    EXPECT_STREQ(mos_internal_normalize_bsd_name("/dev/rdisk127"), "disk127");
    return 0;
}

TEST(unrelated_names_untouched)
{
    /* A name that doesn't start with "rdisk" must not be stripped. */
    EXPECT_STREQ(mos_internal_normalize_bsd_name("rdsmith"), "rdsmith");
    EXPECT_STREQ(mos_internal_normalize_bsd_name("rd"),      "rd");
    EXPECT_STREQ(mos_internal_normalize_bsd_name("disk"),    "disk");
    return 0;
}

TEST(whole_shape_accepts_whole_disks)
{
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("disk4"),    1);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("disk0"),    1);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("disk127"),  1);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("rdisk4"),   1);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("rdisk42"),  1);
    return 0;
}

TEST(whole_shape_rejects_partitions)
{
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("disk4s1"),   0);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("disk4s10"),  0);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("rdisk4s1"),  0);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("disk4s1s2"), 0);  /* APFS slice */
    return 0;
}

TEST(whole_shape_rejects_nonsense)
{
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape(""),        0);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape(NULL),      0);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("disk"),    0);   /* no digit */
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("diska"),   0);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("vdisk4"),  0);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("4"),       0);
    EXPECT_EQ(mos_internal_bsd_name_is_whole_shape("disk4x"),  0);   /* trailing garbage */
    return 0;
}

TEST(self_or_partition_exact_match)
{
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4",  4),  1);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk0",  0),  1);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk42", 42), 1);
    return 0;
}

TEST(self_or_partition_partition_child)
{
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s1",   4), 1);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s10",  4), 1);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s1s2", 4), 1);
    return 0;
}

TEST(self_or_partition_rejects_prefix_collisions)
{
    /* The classic bug: bare strncmp(reported, "disk4", 5) matched disk40
       against disk4. The numeric compare makes it 40 != 4 by construction. */
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk40",   4), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk42",   4), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4abc", 4), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4x",   4), 0);
    return 0;
}

TEST(self_or_partition_rejects_nulls_and_empty)
{
    EXPECT_EQ(mos_internal_bsd_unit_matches(NULL,    4),  0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4", -1), 0);  /* no unit to match */
    EXPECT_EQ(mos_internal_bsd_unit_matches(NULL,    -1), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("",      4),  0);  /* no "disk" prefix */
    EXPECT_EQ(mos_internal_bsd_unit_matches("",      -1), 0);
    return 0;
}

TEST(self_or_partition_unrelated_disks)
{
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk5",   4), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk5s1", 4), 0);
    /* A watcher of disk40 must not match disk4 (4 != 40). */
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4",   40), 0);
    return 0;
}

TEST(self_or_partition_rejects_malformed_partition_suffix)
{
    /* All start "disk4s" but are not names DA would emit. 's' must be
       followed by >=1 digit, with no trailing garbage. */
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s",    4), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4sx",   4), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4sabc", 4), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s1x",  4), 0);
    return 0;
}

TEST(self_or_partition_accepts_apfs_sub_slices)
{
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s1s2",   4), 1);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s10s3",  4), 1);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s1s2s3", 4), 1);
    /* Still strict about every segment having digits: */
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s1s",    4), 0);
    EXPECT_EQ(mos_internal_bsd_unit_matches("disk4s1sx",   4), 0);
    return 0;
}

TEST(parse_bsd_unit_accepts_forms_rejects_nonwhole_and_overflow)
{
    EXPECT_EQ(mos_internal_parse_bsd_unit("disk4"),       4);
    EXPECT_EQ(mos_internal_parse_bsd_unit("rdisk4"),      4);
    EXPECT_EQ(mos_internal_parse_bsd_unit("/dev/disk4"),  4);
    EXPECT_EQ(mos_internal_parse_bsd_unit("/dev/rdisk4"), 4);
    EXPECT_EQ(mos_internal_parse_bsd_unit("disk0"),       0);
    EXPECT_EQ(mos_internal_parse_bsd_unit("disk127"),     127);
    /* Non-whole / junk / empty / NULL → -1 (no unit). */
    EXPECT_EQ(mos_internal_parse_bsd_unit("disk4s1"),     -1);
    EXPECT_EQ(mos_internal_parse_bsd_unit("diska"),       -1);
    EXPECT_EQ(mos_internal_parse_bsd_unit("disk"),        -1);
    EXPECT_EQ(mos_internal_parse_bsd_unit(""),            -1);
    EXPECT_EQ(mos_internal_parse_bsd_unit(NULL),          -1);
    /* 32-bit boundary: UINT32_MAX parses, one past overflows → -1. */
    EXPECT_EQ(mos_internal_parse_bsd_unit("disk4294967295"), 4294967295LL);
    EXPECT_EQ(mos_internal_parse_bsd_unit("disk4294967296"), -1);
    return 0;
}

TEST(bsd_name_format_valid_units_and_uint32_boundary)
{
    char b[16];
    EXPECT(mos_bsd_name_format(0, b, sizeof b));      EXPECT_STREQ(b, "disk0");
    EXPECT(mos_bsd_name_format(4, b, sizeof b));      EXPECT_STREQ(b, "disk4");
    EXPECT(mos_bsd_name_format(127, b, sizeof b));    EXPECT_STREQ(b, "disk127");
    /* UINT32_MAX is the largest valid unit: "disk4294967295" = 14 chars. */
    EXPECT(mos_bsd_name_format(4294967295LL, b, sizeof b));
    EXPECT_STREQ(b, "disk4294967295");
    return 0;
}

TEST(bsd_name_format_rejects_out_of_domain)
{
    char b[16];
    /* no media */
    EXPECT(!mos_bsd_name_format(-1, b, sizeof b));            EXPECT_STREQ(b, "");
    /* one past UINT32_MAX — fits a 16-byte buffer ("disk4294967296"),
       so this is the case truncation alone would NOT catch: must be
       rejected by the domain check, not emitted as a valid-looking name. */
    EXPECT(!mos_bsd_name_format(4294967296LL, b, sizeof b));  EXPECT_STREQ(b, "");
    /* well past the domain (also truncates) */
    EXPECT(!mos_bsd_name_format(5000000000LL, b, sizeof b));  EXPECT_STREQ(b, "");
    EXPECT(!mos_bsd_name_format(9223372036854775807LL, b, sizeof b)); /* INT64_MAX */
    EXPECT_STREQ(b, "");
    return 0;
}

TEST(bsd_name_format_too_small_buffer_returns_false)
{
    /* In-domain unit but the caller buffer can't hold "disk12345" (10
       bytes needed); snprintf truncates and the function must report it
       AND empty the buffer (documented contract: writes "" on failure
       when cap > 0, never a truncated partial name). */
    char small[8];
    small[0] = 'X';
    EXPECT(!mos_bsd_name_format(12345, small, sizeof small));
    EXPECT_EQ(small[0], '\0');
    /* cap == 0: nothing written, false. */
    EXPECT(!mos_bsd_name_format(4, small, 0));
    return 0;
}

TEST(bsd_name_format_rejects_null_buffer)
{
    /* Public footgun: documented as NUL-terminating when cap > 0, but a
       NULL buf with a non-zero cap must not be dereferenced. Returns
       false rather than crashing in snprintf. */
    EXPECT(!mos_bsd_name_format(4, NULL, 16));
    EXPECT(!mos_bsd_name_format(0, NULL, 16));
    EXPECT(!mos_bsd_name_format(4, NULL, 0));
    return 0;
}

void register_bsd_name_tests(void)
{
    RUN(plain_diskN_passes_through);
    RUN(rdiskN_strips_leading_r);
    RUN(dev_diskN_strips_dev_prefix);
    RUN(dev_rdiskN_strips_both_prefixes);
    RUN(longer_disk_numbers_work);
    RUN(unrelated_names_untouched);
    RUN(whole_shape_accepts_whole_disks);
    RUN(whole_shape_rejects_partitions);
    RUN(whole_shape_rejects_nonsense);
    RUN(self_or_partition_exact_match);
    RUN(self_or_partition_partition_child);
    RUN(self_or_partition_rejects_prefix_collisions);
    RUN(self_or_partition_rejects_nulls_and_empty);
    RUN(self_or_partition_unrelated_disks);
    RUN(self_or_partition_rejects_malformed_partition_suffix);
    RUN(self_or_partition_accepts_apfs_sub_slices);
    RUN(parse_bsd_unit_accepts_forms_rejects_nonwhole_and_overflow);
    RUN(bsd_name_format_valid_units_and_uint32_boundary);
    RUN(bsd_name_format_rejects_out_of_domain);
    RUN(bsd_name_format_too_small_buffer_returns_false);
    RUN(bsd_name_format_rejects_null_buffer);
}
