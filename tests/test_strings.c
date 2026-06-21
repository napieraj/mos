/*
 * test_strings.c — totality of the enum→token / enum→int tables in
 * mos_strings.c.
 *
 * These tables feed JSON output, the human view, and the exit-code
 * contract. Per-fixture tests elsewhere hit whichever values a fixture
 * happens to carry, and the macOS validate.py drift guard checks that the
 * schema-tracked tables' string SETS match the schemas. Neither asserts the
 * property pinned here: that EVERY defined enumerator yields a non-empty
 * token, and that an out-of-range value takes the documented fallback (NULL,
 * or the "unknown" sentinel for the two functions that have one). That is
 * distinct coverage — and it exercises the case + default arms the scattered
 * per-fixture tests leave red on the combined coverage report. Pure-layer,
 * no IOKit; runs on every platform.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>

/* A token must be present and non-empty. */
#define EXPECT_TOKEN(s) do {                                            \
    const char *_t = (s);                                              \
    EXPECT(_t != NULL && _t[0] != '\0');                              \
} while (0)

/* ---- Closed public string enums: every enumerator → a token -------------
 * mos_state_description and mos_error_description return a non-NULL "unknown"
 * sentinel for an out-of-range value (the others return NULL); both arms are
 * pinned. The exact strings are the schema/CLI contract — but here we assert
 * only presence, leaving exact-string set-equality to validate.py so this
 * stays coverage, not a second copy of that guard. */

TEST(strings_state_description_total)
{
    static const mos_state all[] = {
        MOS_STATE_OPEN, MOS_STATE_EMPTY, MOS_STATE_LOADING, MOS_STATE_READY,
        MOS_STATE_BUSY, MOS_STATE_FORMATTING, MOS_STATE_MEDIA_UNREADABLE,
        MOS_STATE_DEVICE_FAULT, MOS_STATE_EMPTY_OR_OPEN, MOS_STATE_UNKNOWN,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        EXPECT_TOKEN(mos_state_description(all[i]));
    /* Out-of-range → the non-NULL "unknown" sentinel, not a crash. */
    EXPECT_STREQ(mos_state_description((mos_state)999), "unknown");
    return 0;
}

TEST(strings_disc_status_description_total)
{
    static const mos_disc_status all[] = {
        MOS_DISC_BLANK, MOS_DISC_APPENDABLE, MOS_DISC_COMPLETE, MOS_DISC_OTHER,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        EXPECT_TOKEN(mos_disc_status_description(all[i]));
    EXPECT_STREQ(mos_disc_status_description((mos_disc_status)99), "other");
    return 0;
}

TEST(strings_tray_outcome_description_total)
{
    static const mos_tray_outcome all[] = {
        MOS_TRAY_DONE, MOS_TRAY_REFUSED_LOCKED, MOS_TRAY_ALREADY_LOCKED,
        MOS_TRAY_REFUSED_OTHER,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        EXPECT_TOKEN(mos_tray_outcome_description(all[i]));
    EXPECT_STREQ(mos_tray_outcome_description((mos_tray_outcome)99), "refused_other");
    return 0;
}

/* The mos_error enumerators, reused by the three error-keyed tables below. */
static const mos_error all_errors[] = {
    MOS_OK, MOS_ERR_INVALID_ARG, MOS_ERR_NO_DEVICE, MOS_ERR_DRIVER_REJECTED,
    MOS_ERR_EXCLUSIVE_ACCESS, MOS_ERR_BUSY, MOS_ERR_TIMEOUT, MOS_ERR_IO,
    MOS_ERR_UNSUPPORTED, MOS_ERR_OOM,
};

TEST(strings_error_description_total)
{
    for (size_t i = 0; i < sizeof all_errors / sizeof all_errors[0]; i++)
        EXPECT_TOKEN(mos_error_description(all_errors[i]));
    EXPECT_STREQ(mos_error_description((mos_error)999), "unknown error");
    return 0;
}

TEST(strings_error_sysexit_total)
{
    /* Exact sysexits.h classes — not schema-tracked, so pinning the mapping
       here is pure gain, and the contract is the doc table in mos.h. */
    EXPECT_EQ(mos_error_sysexit(MOS_OK), 0);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_INVALID_ARG), 64);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_NO_DEVICE), 66);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_DRIVER_REJECTED), 69);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_EXCLUSIVE_ACCESS), 75);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_BUSY), 75);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_TIMEOUT), 75);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_IO), 74);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_UNSUPPORTED), 69);
    EXPECT_EQ(mos_error_sysexit(MOS_ERR_OOM), 71);
    EXPECT_EQ(mos_error_sysexit((mos_error)999), 70);   /* EX_SOFTWARE */
    return 0;
}

TEST(strings_error_is_recoverable_total)
{
    /* Only the transient three are recoverable; everything else (and an
       unknown value) is not. */
    for (size_t i = 0; i < sizeof all_errors / sizeof all_errors[0]; i++) {
        mos_error e = all_errors[i];
        bool want = (e == MOS_ERR_EXCLUSIVE_ACCESS || e == MOS_ERR_BUSY ||
                     e == MOS_ERR_TIMEOUT);
        EXPECT_EQ(mos_error_is_recoverable(e), want);
    }
    EXPECT_EQ(mos_error_is_recoverable((mos_error)999), false);
    return 0;
}

/* ---- Numeric-code tables: each named code → a token, unknown → NULL ------ */

TEST(strings_spc_version_name_total)
{
    static const uint8_t named[] = { 0x03, 0x04, 0x05, 0x06, 0x07 };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        EXPECT_TOKEN(mos_spc_version_name(named[i]));
    EXPECT(mos_spc_version_name(0x00) == NULL);   /* none */
    EXPECT(mos_spc_version_name(0x02) == NULL);   /* legacy SCSI-2 */
    EXPECT(mos_spc_version_name(0xFF) == NULL);
    return 0;
}

TEST(strings_version_descriptor_name_total)
{
    static const uint16_t named[] = {
        0x0020, 0x0040, 0x0060, 0x0080, 0x00A0, 0x00C0, 0x0120, 0x0140,
        0x0180, 0x0240, 0x0260, 0x02A0, 0x0300, 0x0320, 0x03A0, 0x0420,
        0x0460, 0x04C0, 0x04E0, 0x05C0, 0x0600, 0x1EA0, 0x1EC0, 0x1EE0,
        0x1F00,
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        EXPECT_TOKEN(mos_version_descriptor_name(named[i]));
    EXPECT(mos_version_descriptor_name(0x0000) == NULL);
    EXPECT(mos_version_descriptor_name(0x0121) == NULL);   /* per-revision */
    EXPECT(mos_version_descriptor_name(0xFFFF) == NULL);
    return 0;
}

TEST(strings_book_type_name_total)
{
    static const uint8_t named[] = {
        0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x9, 0xA, 0xD, 0xE,
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        EXPECT_TOKEN(mos_book_type_name(named[i]));
    EXPECT(mos_book_type_name(0x7) == NULL);   /* reserved */
    EXPECT(mos_book_type_name(0xF) == NULL);
    return 0;
}

TEST(strings_protection_name_total)
{
    EXPECT_TOKEN(mos_protection_name(0x00));   /* none */
    EXPECT_TOKEN(mos_protection_name(0x01));   /* css_cppm */
    EXPECT_TOKEN(mos_protection_name(0x02));   /* cprm */
    EXPECT_TOKEN(mos_protection_name(0x03));   /* aacs */
    EXPECT(mos_protection_name(0x04) == NULL);
    EXPECT(mos_protection_name(0xFF) == NULL);
    return 0;
}

TEST(strings_format_capacity_type_name_total)
{
    EXPECT_TOKEN(mos_format_capacity_type_name(1));   /* unformatted */
    EXPECT_TOKEN(mos_format_capacity_type_name(2));   /* formatted */
    EXPECT_TOKEN(mos_format_capacity_type_name(3));   /* no_media */
    EXPECT(mos_format_capacity_type_name(0) == NULL);   /* reserved */
    EXPECT(mos_format_capacity_type_name(99) == NULL);
    return 0;
}

TEST(strings_loading_mechanism_name_total)
{
    static const uint8_t named[] = { 0, 1, 2, 4, 5 };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        EXPECT_TOKEN(mos_loading_mechanism_name(named[i]));
    EXPECT(mos_loading_mechanism_name(3) == NULL);   /* reserved */
    EXPECT(mos_loading_mechanism_name(6) == NULL);
    EXPECT(mos_loading_mechanism_name(7) == NULL);
    return 0;
}

TEST(strings_bg_format_status_name_total)
{
    EXPECT_TOKEN(mos_bg_format_status_name(0));
    EXPECT_TOKEN(mos_bg_format_status_name(1));
    EXPECT_TOKEN(mos_bg_format_status_name(2));
    EXPECT_TOKEN(mos_bg_format_status_name(3));
    /* The public accessor masks to 0-3, so the default is reachable only
       through a direct out-of-range call. */
    EXPECT(mos_bg_format_status_name(4) == NULL);
    return 0;
}

TEST(strings_track_path_name_total)
{
    /* Masks to one bit, so both arms are total. */
    EXPECT_STREQ(mos_track_path_name(0), "ptp");
    EXPECT_STREQ(mos_track_path_name(1), "otp");
    EXPECT_STREQ(mos_track_path_name(0xFE), "ptp");   /* even → ptp */
    EXPECT_STREQ(mos_track_path_name(0xFF), "otp");   /* odd  → otp */
    return 0;
}

TEST(strings_profile_name_unknown_falls_back)
{
    /* The named profile codes are walked by test_config's
       profile_class_total_over_name_table; here we only pin the unknown-code
       default arm (the one line that test leaves red), to avoid re-asserting
       the table it already covers. */
    EXPECT(mos_profile_name(0xFFFF) == NULL);
    EXPECT(mos_profile_name(0x0007) == NULL);   /* gap between cd_rom and mo */
    return 0;
}

void register_strings_tests(void)
{
    RUN(strings_state_description_total);
    RUN(strings_disc_status_description_total);
    RUN(strings_tray_outcome_description_total);
    RUN(strings_error_description_total);
    RUN(strings_error_sysexit_total);
    RUN(strings_error_is_recoverable_total);
    RUN(strings_spc_version_name_total);
    RUN(strings_version_descriptor_name_total);
    RUN(strings_book_type_name_total);
    RUN(strings_protection_name_total);
    RUN(strings_format_capacity_type_name_total);
    RUN(strings_loading_mechanism_name_total);
    RUN(strings_bg_format_status_name_total);
    RUN(strings_track_path_name_total);
    RUN(strings_profile_name_unknown_falls_back);
}
