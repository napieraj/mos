/*
 * test_discinfo.c — READ DISC INFORMATION (0x51) decode. The matched
 * fixture pair (blank CD-R vs finalized CD-ROM) differs only in byte 2
 * and the lead-in/lead-out address fields, so it isolates the disc-status
 * decode — the disc-completion signal — from everything else. Plus the usual
 * hostile/short-buffer cases: under ASan any out-of-bounds read aborts.
 *
 * The inline buffers mirror tests/fixtures/readdiscinfo_*_*.bin.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>

/* Fixture A — blank CD-R. byte 2 = 0x00; bytes 16..23 carry ATIP-derived
   MSF lead-in 97:24:01 / lead-out 79:59:74 (a blank recordable disc has
   valid addresses, unlike a finalized one). */
static const uint8_t rdi_blank_cdr[34] = {
    0x00,0x20, 0x00, 0x01, 0x01, 0x01, 0x01, 0x20, 0x00, 0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x61,0x18,0x01,            /* Last Session Lead-in Start  = MSF 97:24:01 */
    0x00,0x4F,0x3B,0x4A,            /* Last Possible Lead-out Start = MSF 79:59:74 */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00, 0x00,
};

/* Fixture B — finalized CD-ROM. Identical to A except byte 2 = 0x0E
   (status Complete, last session Complete) and the address fields are
   FFh-filled (no further recording possible). */
static const uint8_t rdi_complete_cdrom[34] = {
    0x00,0x20, 0x0E, 0x01, 0x01, 0x01, 0x01, 0x20, 0x00, 0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00, 0x00,
};

TEST(discinfo_blank_cdr_decodes_blank)
{
    mos_disc_info di;
    EXPECT(mos_internal_disc_info_parse(rdi_blank_cdr, sizeof rdi_blank_cdr, &di));
    EXPECT_EQ(di.status, MOS_DISC_BLANK);
    EXPECT_EQ(di.last_session_state, 0);            /* empty */
    EXPECT(!di.erasable);                            /* CD-R is write-once */
    EXPECT_EQ(di.first_track_on_disc, 1);
    EXPECT_EQ(di.number_of_sessions, 1);
    EXPECT_EQ(di.first_track_last_session, 1);
    EXPECT_EQ(di.last_track_last_session, 1);
    return 0;
}

TEST(discinfo_blank_bdr_decodes_blank)
{
    /* Blank BD-R (HL-DT-ST BH16NS40 1.00, JVC-AM/S6L media), reversed
       from a published dvd+rw-mediainfo log through dvd+rw-mediainfo.cpp's
       byte map: "Disc status: blank", "Number of Sessions: 1",
       "Next Track: 1", "Number of Tracks: 1", last session empty.
       BD-R is write-once (erasable clear); the CD ATIP address fields
       are zero — not applicable to BD (fixtures README). Mirrors
       fixtures/readdiscinfo_blank_bdr.bin. */
    static const uint8_t rdi_blank_bdr[34] = {
        0x00,0x20, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00,
        /* bytes 8..33 zero */
    };
    mos_disc_info di;
    EXPECT(mos_internal_disc_info_parse(rdi_blank_bdr, sizeof rdi_blank_bdr, &di));
    EXPECT_EQ(di.status, MOS_DISC_BLANK);
    EXPECT_EQ(di.last_session_state, 0);            /* empty */
    EXPECT(!di.erasable);                            /* BD-R is write-once */
    EXPECT_EQ(di.number_of_sessions, 1);
    EXPECT_EQ(di.first_track_last_session, 1);
    EXPECT_EQ(di.last_track_last_session, 1);
    return 0;
}

TEST(discinfo_complete_cdrom_decodes_complete)
{
    mos_disc_info di;
    EXPECT(mos_internal_disc_info_parse(rdi_complete_cdrom, sizeof rdi_complete_cdrom, &di));
    EXPECT_EQ(di.status, MOS_DISC_COMPLETE);
    EXPECT_EQ(di.last_session_state, 3);            /* complete */
    EXPECT(!di.erasable);
    EXPECT_EQ(di.number_of_sessions, 1);
    return 0;
}

TEST(discinfo_status_is_byte2_low_two_bits)
{
    /* The four disc-status code points decode independently of the rest of
       the byte (here last-session and erasable bits are also set, proving
       the mask isolates bits 1:0). */
    uint8_t buf[12] = { 0,10, 0x00, 0,0,0,0,0,0,0,0,0 };
    mos_disc_info di;

    buf[2] = 0x00; EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.status, MOS_DISC_BLANK);
    buf[2] = 0x1D; EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di)); /* erasable+state+01 */
    EXPECT_EQ(di.status, MOS_DISC_APPENDABLE);
    EXPECT(di.erasable);
    buf[2] = 0x0E; EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.status, MOS_DISC_COMPLETE);
    buf[2] = 0x03; EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.status, MOS_DISC_OTHER);
    return 0;
}

TEST(discinfo_bg_format_status_byte7_low_two_bits)
{
    /* BG Format Status is byte 7 bits 1:0, isolated from the high
       validity bits (DID_V/DBC_V/URU/DAC_V) that share the byte. */
    uint8_t buf[12] = { 0,10, 0x00, 0,0,0,0, 0x00, 0,0,0,0 };
    mos_disc_info di;

    buf[7] = 0xFC;   /* every high bit set, bg bits clear */
    EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.bg_format_status, 0);
    buf[7] = 0x01;
    EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.bg_format_status, 1);
    buf[7] = 0xFE;   /* high bits set, bg = 10b */
    EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.bg_format_status, 2);
    buf[7] = 0x03;
    EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.bg_format_status, 3);
    return 0;
}

TEST(discinfo_bg_format_status_name_tokens)
{
    EXPECT(strcmp(mos_bg_format_status_name(0), "none") == 0);
    EXPECT(strcmp(mos_bg_format_status_name(1), "inactive") == 0);
    EXPECT(strcmp(mos_bg_format_status_name(2), "active") == 0);
    EXPECT(strcmp(mos_bg_format_status_name(3), "complete") == 0);
    EXPECT(mos_bg_format_status_name(4) == NULL);    /* out of range */
    return 0;
}

TEST(discinfo_combines_session_count_msb_lsb)
{
    /* Number of Sessions is split: LSB at byte 4, MSB at byte 9. */
    uint8_t buf[12] = { 0,10, 0x02, 0, 0x05, 0,0,0,0, 0x01, 0,0 };
    mos_disc_info di;
    EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.number_of_sessions, 0x0105);        /* 261 */
    return 0;
}

TEST(discinfo_short_buffer_is_rejected)
{
    uint8_t buf[11] = {0};                            /* one byte short of the field region */
    mos_disc_info di;
    EXPECT(!mos_internal_disc_info_parse(buf, sizeof buf, &di));
    return 0;
}

TEST(discinfo_declared_length_shorter_than_fields_is_rejected)
{
    /* Buffer physically holds 12 bytes, but Disc Information Length claims
       only 6 (→ trusted end = 8), short of byte 11. Refuse rather than read
       device-unclaimed bytes. */
    uint8_t buf[12] = { 0,6, 0x00, 0,0,0,0,0,0,0,0,0 };
    mos_disc_info di;
    EXPECT(!mos_internal_disc_info_parse(buf, sizeof buf, &di));
    return 0;
}

TEST(discinfo_lying_large_length_clamps_to_buffer)
{
    /* Disc Information Length claims ~64 KiB; only 12 real bytes exist. The
       clamp to `len` must hold and the decode must still succeed in-bounds. */
    uint8_t buf[12] = { 0xFF,0xFF, 0x0E, 0x01, 0x01,0,0,0,0, 0,0,0 };
    mos_disc_info di;
    EXPECT(mos_internal_disc_info_parse(buf, sizeof buf, &di));
    EXPECT_EQ(di.status, MOS_DISC_COMPLETE);
    return 0;
}

TEST(discinfo_null_args_are_safe)
{
    uint8_t buf[12] = {0};
    mos_disc_info di;
    EXPECT(!mos_internal_disc_info_parse(NULL, 12, &di));
    EXPECT(!mos_internal_disc_info_parse(buf, 12, NULL));
    return 0;
}

/* v0.4 typed-accessor surface: the public read path over the decoded
   struct, exercised on the same fixture pair the decoder is built to,
   plus NULL tolerance (every result accessor's contract) and the
   description tokens. */
TEST(discinfo_accessors_read_fixture_fields)
{
    mos_disc_info di;
    EXPECT(mos_internal_disc_info_parse(rdi_complete_cdrom,
                                        sizeof rdi_complete_cdrom, &di));
    EXPECT_EQ(MOS_DISC_COMPLETE, mos_disc_info_status(&di));
    EXPECT_EQ(3,                 mos_disc_info_last_session_state(&di));
    EXPECT_EQ(false,             mos_disc_info_erasable(&di));
    EXPECT_EQ(1,                 mos_disc_info_first_track(&di));
    EXPECT_EQ(1,                 mos_disc_info_session_count(&di));
    EXPECT_EQ(1,                 mos_disc_info_first_track_last_session(&di));
    EXPECT_EQ(1,                 mos_disc_info_last_track_last_session(&di));
    EXPECT_EQ(0,                 mos_disc_info_bg_format_status(&di)); /* byte7=0x20 */
    return 0;
}

TEST(discinfo_accessors_null_tolerant)
{
    EXPECT_EQ(MOS_DISC_OTHER, mos_disc_info_status(NULL));
    EXPECT_EQ(false,          mos_disc_info_erasable(NULL));
    EXPECT_EQ(0,              mos_disc_info_first_track(NULL));
    EXPECT_EQ(0,              mos_disc_info_session_count(NULL));
    EXPECT_EQ(0,              mos_disc_info_first_track_last_session(NULL));
    EXPECT_EQ(0,              mos_disc_info_last_track_last_session(NULL));
    EXPECT_EQ(0,              mos_disc_info_last_session_state(NULL));
    EXPECT_EQ(0,              mos_disc_info_bg_format_status(NULL));
    return 0;
}

TEST(discinfo_status_description_tokens)
{
    EXPECT(strcmp(mos_disc_status_description(MOS_DISC_BLANK),
                  "blank") == 0);
    EXPECT(strcmp(mos_disc_status_description(MOS_DISC_APPENDABLE),
                  "appendable") == 0);
    EXPECT(strcmp(mos_disc_status_description(MOS_DISC_COMPLETE),
                  "complete") == 0);
    EXPECT(strcmp(mos_disc_status_description(MOS_DISC_OTHER),
                  "other") == 0);
    return 0;
}

TEST(discinfo_complete_bdre_decodes_erasable)
{
    /* BD-RE, Pioneer BDR-209D 1.51, media CMCMAG/CN2 — reversed from a
       verbatim (<pre>-preserved) dvd+rw-mediainfo dump in the
       cdwrite@debian list, msg14498: "Disc status: complete / Number
       of Sessions: 1 / State of Last Session: complete / Number of
       Tracks: 1". The erasable bit is set per the 43h BD-RE rewritable
       profile, NOT from the log — dvd+rw-mediainfo prints no Erasable
       line (it consumes byte-2 bit 4 only as a READ FORMAT CAPACITIES
       gate, source L901). first_track_last_session is likewise
       inferred (the tool suppresses "Next Track" once complete). The
       attested fields are status / sessions / last-session / tracks.
       Mirrors fixtures/readdiscinfo_complete_bdre.bin; the first
       erasable=true fixture. */
    static const uint8_t rdi[34] = {
        0x00,0x20, 0x1E, 0x01, 0x01, 0x01, 0x01, 0x00,
        /* bytes 8..33 zero */
    };
    mos_disc_info di;
    EXPECT(mos_internal_disc_info_parse(rdi, sizeof rdi, &di));
    EXPECT_EQ(di.status, MOS_DISC_COMPLETE);
    EXPECT_EQ(di.last_session_state, 3);            /* complete */
    EXPECT(di.erasable);                             /* BD-RE is rewritable */
    EXPECT_EQ(di.number_of_sessions, 1);
    EXPECT_EQ(di.last_track_last_session, 1);
    return 0;
}

void register_discinfo_tests(void)
{
    RUN(discinfo_accessors_read_fixture_fields);
    RUN(discinfo_accessors_null_tolerant);
    RUN(discinfo_status_description_tokens);
    RUN(discinfo_blank_cdr_decodes_blank);
    RUN(discinfo_blank_bdr_decodes_blank);
    RUN(discinfo_complete_bdre_decodes_erasable);
    RUN(discinfo_complete_cdrom_decodes_complete);
    RUN(discinfo_status_is_byte2_low_two_bits);
    RUN(discinfo_bg_format_status_byte7_low_two_bits);
    RUN(discinfo_bg_format_status_name_tokens);
    RUN(discinfo_combines_session_count_msb_lsb);
    RUN(discinfo_short_buffer_is_rejected);
    RUN(discinfo_declared_length_shorter_than_fields_is_rejected);
    RUN(discinfo_lying_large_length_clamps_to_buffer);
    RUN(discinfo_null_args_are_safe);
}
