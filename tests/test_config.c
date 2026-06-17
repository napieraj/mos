/*
 * test_config.c — GET CONFIGURATION feature walk: well-formed decode plus
 * hostile/malformed buffers. Load-bearing property is no-OOB (ASan aborts
 * on any over-read). The malformed fixtures — lying Data Length, Additional
 * Length past the buffer, truncated header — are the device-controlled-length
 * attacks the walk neutralizes.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

TEST(config_walks_two_well_formed_features)
{
    /* data length 12 (= 16 - 4), profile 0x0010; then Profile List
       (0x0000, cur+persistent) and Core (0x0001, cur). */
    uint8_t buf[] = {
        0,0,0,12,  0,0, 0x00,0x10,
        0x00,0x00, 0x03, 0x00,
        0x00,0x01, 0x01, 0x00,
    };
    size_t cur = 8; mos_config_feature f;

    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT(f.feature_code == 0x0000);
    EXPECT(f.current && f.persistent);
    EXPECT(f.version == 0);
    EXPECT(f.data_len == 0 && f.data == NULL);

    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT(f.feature_code == 0x0001);
    EXPECT(f.current && !f.persistent);
    EXPECT(f.data_len == 0);

    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_feature_with_payload_exposes_bounded_slice)
{
    uint8_t buf[] = {
        0,0,0,12,  0,0, 0,0,
        0x00,0x2B, 0x03, 0x04,  0xAA,0xBB,0xCC,0xDD,
    };
    size_t cur = 8; mos_config_feature f;

    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT(f.feature_code == 0x002B);
    EXPECT(f.data_len == 4 && f.data != NULL);
    EXPECT(f.data[0] == 0xAA && f.data[3] == 0xDD);   /* in-bounds read */
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_additional_length_past_buffer_is_rejected)
{
    /* add = 0xFC = 252: largest 4-aligned one-byte Additional Length, the
       worst-case aligned over-read. Being aligned it passes the %4 gate, so
       this is a pure bounds test — only 2 payload bytes exist, so the walk
       must refuse on bounds rather than read 252 absent bytes. */
    uint8_t buf[] = {
        0,0,0,0xFC,  0,0, 0,0,
        0x00,0x2B, 0x00, 0xFC,  0x01,0x02,
    };
    size_t cur = 8; mos_config_feature f;
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_truncated_descriptor_header_is_rejected)
{
    uint8_t buf[] = {
        0,0,0,8,  0,0, 0,0,
        0x00,0x01, 0x01,            /* only 3 of the 4 header bytes present */
    };
    size_t cur = 8; mos_config_feature f;
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_lying_large_data_length_clamped_to_buffer)
{
    /* Data Length claims ~4 GiB; walk clamps to sizeof(buf), yielding only
       the one real descriptor with no phantom features past it. */
    uint8_t buf[] = {
        0xFF,0xFF,0xFF,0xFF,  0,0, 0,0,
        0x00,0x01, 0x01, 0x00,
    };
    size_t cur = 8; mos_config_feature f;
    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT(f.feature_code == 0x0001);
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_header_only_yields_no_features)
{
    uint8_t buf[8] = { 0,0,0,4, 0,0, 0x00,0x10 };   /* RT=2 style: header only */
    size_t cur = 8; mos_config_feature f;
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_short_buffer_and_misuse_stay_in_bounds)
{
    uint8_t buf[5] = { 0,0,0,99, 0 };               /* len < 8, bogus dlen */
    size_t cur = 8; mos_config_feature f;
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    cur = 0;                                         /* caller misuse: still safe */
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_null_args_are_safe)
{
    uint8_t buf[8] = {0};
    size_t cur = 8; mos_config_feature f;
    EXPECT(!mos_internal_config_next_feature(NULL, 8, &cur, &f));
    EXPECT(!mos_internal_config_next_feature(buf, 8, NULL, &f));
    EXPECT(!mos_internal_config_next_feature(buf, 8, &cur, NULL));
    return 0;
}

TEST(config_dense_fill_terminates_and_consumes_exactly)
{
    uint8_t buf[8 + 4*5] = {0};
    buf[3] = (uint8_t)(sizeof buf - 4);             /* data length = total - 4 */
    for (int i = 0; i < 5; i++) {
        buf[8 + i*4 + 1] = (uint8_t)i;              /* feature code = i */
        buf[8 + i*4 + 2] = 0x01;                    /* current */
        buf[8 + i*4 + 3] = 0x00;                    /* add = 0 */
    }
    size_t cur = 8; mos_config_feature f; int n = 0;
    while (mos_internal_config_next_feature(buf, sizeof buf, &cur, &f)) {
        EXPECT(f.feature_code == (uint16_t)n);
        EXPECT(++n <= 5);                            /* must not over-yield */
    }
    EXPECT(n == 5);
    EXPECT(cur == sizeof buf);                       /* consumed exactly */
    return 0;
}

TEST(config_skips_payload_to_next_descriptor)
{
    /* feat A carries a 4-byte payload, feat B follows: the cursor must
       advance over A's payload (span = 4 + add) to land exactly on B, and
       a nonzero version field must decode. */
    uint8_t buf[] = {
        0,0,0,16,  0,0, 0,0,                          /* dlen = 16 (= total 20 - 4) */
        0x00,0x2B, 0x25, 0x04,  0xDE,0xAD,0xBE,0xEF,  /* A: 0x002B, ver=9 cur, add=4 */
        0x00,0x01, 0x01, 0x00,                        /* B: 0x0001, cur, add=0       */
    };
    size_t cur = 8; mos_config_feature f;

    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT(f.feature_code == 0x002B);
    EXPECT(f.version == 9 && f.current && !f.persistent);
    EXPECT(f.data_len == 4 && f.data[0] == 0xDE && f.data[3] == 0xEF);

    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f)); /* skipped A's payload */
    EXPECT(f.feature_code == 0x0001);

    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_honors_data_length_shorter_than_buffer)
{
    /* Buffer holds two descriptors but Data Length covers only the first;
       the trailing in-buffer descriptor must NOT be yielded. */
    uint8_t buf[] = {
        0,0,0,8,  0,0, 0,0,                /* dlen = 8 → feature region ends at offset 12 */
        0x00,0x0A, 0x01, 0x00,             /* first  — within Data Length                */
        0x00,0x0B, 0x01, 0x00,             /* second — beyond it, must be ignored        */
    };
    size_t cur = 8; mos_config_feature f;
    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT(f.feature_code == 0x000A);
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f)); /* 0x000B not reached */
    return 0;
}

TEST(toc_parses_real_pony_cd_single)
{
    /* Real pressing: Ginuwine "Pony" CD single (1996), 4 tracks —
       MusicBrainz disc TX6lKZ481BHv1ZW6pd6007j6OY4-, AccurateRip-confirmed
       (whipper-team/whipper PR #382 rip log). LBAs from the log's attested
       toc string 1+4+86497+150+24687+47627+68002 (MB offset = LBA + 150);
       ADR=1 / copy-bit=0 are the standard-pressing assumption (fixtures
       README). Mirrors fixtures/readtoc_f0_audio_cd_single.bin. */
    static const uint8_t t[] = {
        0x00,0x2A, 0x01,0x04,
        0x00,0x10, 0x01, 0x00,  0x00,0x00,0x00,0x00,
        0x00,0x10, 0x02, 0x00,  0x00,0x00,0x5F,0xD9,
        0x00,0x10, 0x03, 0x00,  0x00,0x00,0xB9,0x75,
        0x00,0x10, 0x04, 0x00,  0x00,0x01,0x09,0x0C,
        0x00,0x10, 0xAA, 0x00,  0x00,0x01,0x51,0x4B,
    };
    mos_toc toc;
    EXPECT(mos_internal_toc_parse(t, sizeof t, &toc));
    EXPECT_EQ(toc.first_track, 1);
    EXPECT_EQ(toc.last_track, 4);
    EXPECT_EQ(toc.track_count, 4);
    EXPECT(toc.have_leadout);
    EXPECT_EQ(toc.leadout_lba, 86347u);
    EXPECT_EQ(toc.tracks[0].start_lba, 0u);
    EXPECT_EQ(toc.tracks[1].start_lba, 24537u);
    EXPECT_EQ(toc.tracks[2].start_lba, 47477u);
    EXPECT_EQ(toc.tracks[3].start_lba, 67852u);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(toc.tracks[i].adr, 1);
        EXPECT(!(toc.tracks[i].control & 0x4));      /* audio */
    }
    return 0;
}

TEST(aacs_caps_from_real_wh16ns40_capture)
{
    /* MakeMKV drive dump, HL-DT-ST BD-RE WH16NS40 1.05: "Bus encryption
       flags: 17" — the ONE attested descriptor byte (payload byte 0 = 0x17
       per the libaacs bit map: RDC|WBE|BEC|BNG). The AACS-version byte
       (payload byte 3) is NOT attested: MakeMKV's "Highest AACS version" is
       a MakeMKV-local statistic, not the 0x010D byte. So byte 3 (and
       profile, header byte 2, nonce/AGID counts) is illustrative scaffold
       (fixtures README). Load-bearing assertion is bus_encryption; the
       version assertion only proves byte-3 extraction. Mirrors
       fixtures/getconfig_aacs_wh16ns40.bin. */
    static const uint8_t cfg[] = {
        0,0,0,16,  0,0, 0x00,0x00,
        0x01,0x0D, 0x00, 0x04,  0x17, 0x00, 0x00, 78,
    };
    mos_drive_caps c;
    mos_internal_protection_from_config(cfg, sizeof cfg, &c);
    EXPECT(c.protection.aacs);                    /* feature 0x010D present  */
    EXPECT(c.protection.bus_encryption);          /* 0x17 & 0x02 BEC — attested   */
    EXPECT(c.protection.write_bus_encryption);    /* 0x17 & 0x04 WBE — attested   */
    EXPECT_EQ(c.protection.aacs_version, 78);     /* scaffold byte; extraction only */
    return 0;
}

TEST(toc_parses_realistic_audio_cd)
{
    /* 3-track audio CD + lead-out, MMC format-0. Data Length = 2 + 4*8 = 34. */
    uint8_t t[4 + 32] = {0};
    t[0] = 0x00; t[1] = 34; t[2] = 1; t[3] = 3;
    struct { uint8_t trk; uint32_t lba; } d[4] = {
        {1, 0}, {2, 18250}, {3, 41090}, {0xAA, 210895}
    };
    for (int i = 0; i < 4; i++) {
        size_t off = 4 + (size_t)i * 8;
        t[off + 1] = 0x10;                 /* ADR=1, CONTROL=0 (audio) */
        t[off + 2] = d[i].trk;
        t[off + 4] = (uint8_t)(d[i].lba >> 24);
        t[off + 5] = (uint8_t)(d[i].lba >> 16);
        t[off + 6] = (uint8_t)(d[i].lba >> 8);
        t[off + 7] = (uint8_t)(d[i].lba);
    }
    mos_toc toc;
    EXPECT_EQ(true, mos_internal_toc_parse(t, sizeof t, &toc));
    EXPECT_EQ(1, toc.first_track);
    EXPECT_EQ(3, toc.last_track);
    EXPECT_EQ(3, toc.track_count);
    EXPECT_EQ(true, toc.have_leadout);
    EXPECT_EQ(210895, (int)toc.leadout_lba);
    EXPECT_EQ(18250, (int)toc.tracks[1].start_lba);
    EXPECT_EQ(0, toc.tracks[0].control);   /* audio */
    EXPECT_EQ(1, toc.tracks[0].adr);
    return 0;
}

TEST(toc_fail_closed_on_hostile_shapes)
{
    uint8_t t[4 + 24] = {0};
    t[1] = 26; t[2] = 1; t[3] = 2;
    mos_toc toc;

    /* Duplicate track number: 1, 1, lead-out. */
    t[4 + 2] = 1; t[12 + 2] = 1; t[20 + 2] = 0xAA;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Non-ascending: 2 then 1. */
    t[4 + 2] = 2; t[12 + 2] = 1;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Non-ascending and duplicate INTERIOR tracks under otherwise-coherent
       headers — reaches the ordering gate without header checks masking it. */
    {
        uint8_t u[4 + 32] = {0};
        mos_toc toc2;
        u[1] = 34; u[2] = 1; u[3] = 4;
        u[4 + 2] = 1; u[12 + 2] = 3; u[20 + 2] = 2; u[28 + 2] = 4;
        EXPECT_EQ(false, mos_internal_toc_parse(u, sizeof u, &toc2));
        u[12 + 2] = 2; u[20 + 2] = 2;          /* {1, 2, 2, 4} */
        EXPECT_EQ(false, mos_internal_toc_parse(u, sizeof u, &toc2));
    }

    /* Reserved track number 100. */
    t[4 + 2] = 1; t[12 + 2] = 100;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Track AFTER the lead-out. */
    t[4 + 2] = 0xAA; t[12 + 2] = 1; t[20 + 2] = 2;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Lying Data Length over the buffer (O-4 clamp) landing mid-descriptor;
       a partial descriptor is malformed. */
    t[4 + 2] = 1; t[12 + 2] = 2; t[20 + 2] = 0xAA;
    t[1] = 200;
    EXPECT_EQ(false, mos_internal_toc_parse(t, 4 + 20, &toc));

    /* Honest short claim truncating cleanly — the half-parsed-identity
       case: header declares 1..2 but the span (2 + 8 = 10) covers only one
       descriptor, so the header-consistency rule must reject it. */
    t[4 + 2] = 1; t[12 + 2] = 2; t[20 + 2] = 0xAA;
    t[1] = 10;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Same short claim with a COHERENT header (1..1) parses — the clamp is
       fine when the table it leaves is whole. */
    t[3] = 1;
    EXPECT_EQ(true, mos_internal_toc_parse(t, sizeof t, &toc));
    EXPECT_EQ(1, toc.track_count);
    EXPECT_EQ(false, toc.have_leadout);
    return 0;
}

TEST(toc_fail_closed_on_header_descriptor_mismatch)
{
    /* Well-formed descriptors under a header declaring a different table.
       All hostile; all rejected whole. */
    uint8_t t[4 + 16] = {0};
    mos_toc toc;

    /* Header range 5..6, descriptor list {1, lead-out}. */
    t[1] = 18; t[2] = 5; t[3] = 6;
    t[4 + 1] = 0x10; t[4 + 2] = 1;
    t[12 + 1] = 0x10; t[12 + 2] = 0xAA;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* first_track = 0 (reserved) and > 99; inverted range. */
    t[2] = 0; t[3] = 1; t[4 + 2] = 1; t[12 + 2] = 0xAA;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));
    t[2] = 200; t[3] = 201;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));
    t[2] = 3; t[3] = 1;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Count matches the range but endpoints don't: header 1..2,
       descriptors {1, 3} — the pigeonhole edge. */
    t[2] = 1; t[3] = 2;
    t[4 + 2] = 1; t[12 + 2] = 3;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Endpoints match but a declared track is missing: header 1..3,
       descriptors {1, 3} — only the count check catches it. */
    t[3] = 3;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Header-only TOC: a declared range with zero descriptors is
       incomplete, not empty. */
    t[1] = 2; t[2] = 1; t[3] = 1;
    EXPECT_EQ(false, mos_internal_toc_parse(t, 4, &toc));

    /* Coherent table not starting at 1 parses: header 2..4, descriptors
       {2, 3, 4}, no lead-out. */
    uint8_t ok[4 + 24] = {0};
    ok[1] = 26; ok[2] = 2; ok[3] = 4;
    ok[4 + 1] = 0x10; ok[4 + 2] = 2;
    ok[12 + 1] = 0x10; ok[12 + 2] = 3;
    ok[20 + 1] = 0x10; ok[20 + 2] = 4;
    EXPECT_EQ(true, mos_internal_toc_parse(ok, sizeof ok, &toc));
    EXPECT_EQ(3, toc.track_count);
    EXPECT_EQ(2, toc.first_track);
    EXPECT_EQ(4, toc.last_track);
    return 0;
}

TEST(profile_class_total_over_name_table)
{
    /* Every named profile must map to a class or be a DELIBERATE classless
       entry (the closed set: no-profile + the legacy/MO trio). A profile
       added to the name table without a class lands in default NULL and
       fails here — the staleness pair for the two switch statements. */
    static const uint16_t named[] = {
        0x0000, 0x0001, 0x0002, 0x0003,
        0x0008, 0x0009, 0x000A,
        0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
        0x001A, 0x001B, 0x002A, 0x002B,
        0x0040, 0x0041, 0x0042, 0x0043,
        0x0050, 0x0051, 0x0052, 0x0053, 0x0058, 0x005A,
    };
    static const uint16_t classless[] = { 0x0000, 0x0001, 0x0002, 0x0003 };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        EXPECT(mos_profile_name(named[i]) != NULL);
        bool is_classless = false;
        for (size_t j = 0; j < 4; j++)
            if (named[i] == classless[j]) is_classless = true;
        if (is_classless) EXPECT(mos_profile_class(named[i]) == NULL);
        else              EXPECT(mos_profile_class(named[i]) != NULL);
    }
    /* Spot-check the class strings + the unknown code. */
    EXPECT_STREQ("cd",     mos_profile_class(0x0009));
    EXPECT_STREQ("dvd",    mos_profile_class(0x0017));
    EXPECT_STREQ("bd",     mos_profile_class(0x0043));
    EXPECT_STREQ("hd_dvd", mos_profile_class(0x0050));
    EXPECT(mos_profile_class(0xBEEF) == NULL);
    return 0;
}




TEST(trusted_len_each_authority_binds)
{
    /* Seam contract O-4 (dual-length rule): trusted region is
       min(allocated, transferred), with the device claim able only to
       SHRINK it. One case per binding authority. */
    EXPECT_EQ((size_t)8,  mos_internal_trusted_len(8, 64, 1000));   /* allocator   */
    EXPECT_EQ((size_t)8,  mos_internal_trusted_len(64, 8, 1000));   /* transport   */
    EXPECT_EQ((size_t)8,  mos_internal_trusted_len(64, 64, 8));     /* honest claim shrinks */
    EXPECT_EQ((size_t)64, mos_internal_trusted_len(64, 64, 1000));  /* lying claim ignored  */
    return 0;
}

TEST(trusted_len_hostile_and_degenerate_inputs)
{
    /* Attack shapes: header claims 0xFFFF (or worse) over a tiny transfer
       (the classic SCSI allocation-length over-read); a `data_length +
       header` claim at the max field value that must clamp, not wrap; and
       the zero degeneracies — any zero authority zeroes the region (a
       zero-byte transfer trusts nothing). */
    EXPECT_EQ((size_t)8, mos_internal_trusted_len(8, 8, 0xFFFFULL));
    EXPECT_EQ((size_t)8, mos_internal_trusted_len(8, 8, UINT64_MAX));
    EXPECT_EQ((size_t)8, mos_internal_trusted_len(8, 8,
                          (uint64_t)0xFFFFFFFFULL + 4));  /* max GET CONFIG DL + header */
    EXPECT_EQ((size_t)0, mos_internal_trusted_len(0, 64, 1000));
    EXPECT_EQ((size_t)0, mos_internal_trusted_len(64, 0, 1000));
    EXPECT_EQ((size_t)0, mos_internal_trusted_len(64, 64, 0));
    return 0;
}

TEST(config_misaligned_additional_length_span_fits)
{
    /* The companion test below uses a buffer short enough that the BOUNDS
       check also rejects the misaligned descriptor, so a deleted alignment
       guard (`add & 3`) would still pass there. Here the malformed
       descriptor's span fits inside the trusted region, so ONLY the
       alignment guard can stop the walk — a desync would fabricate feature
       0xDEAD from misaligned bytes. */
    uint8_t buf[32] = {0};
    buf[3] = 28;                          /* Data Length -> trusted end 32 */
    /* Descriptor at cursor 8: code 0x1234, Additional Length 5 (not %4).
       span = 4 + 5 = 9; 8 + 9 = 17 <= 32 — in bounds. */
    buf[8] = 0x12; buf[9] = 0x34; buf[10] = 0x00; buf[11] = 5;
    /* Bytes a desynced walk would decode at cursor 17 as feature 0xDEAD. */
    buf[17] = 0xDE; buf[18] = 0xAD; buf[19] = 0x00; buf[20] = 0x00;

    size_t cursor = 8;
    mos_config_feature f;
    int yielded = 0;
    uint16_t last_code = 0;
    while (mos_internal_config_next_feature(buf, sizeof buf, &cursor, &f)) {
        yielded++;
        last_code = f.feature_code;
        if (yielded > 4) break;          /* runaway guard for the test */
    }
    EXPECT_EQ(0, yielded);
    EXPECT(last_code != 0xDEAD);
    return 0;
}

TEST(config_misaligned_additional_length_is_rejected)
{
    /* add = 2 is not %4 → malformed; the walk stops rather than desyncing
       onto misaligned bytes. */
    uint8_t buf[] = {
        0,0,0,8,  0,0, 0,0,
        0x00,0x2B, 0x01, 0x02,  0xAA,0xBB,
    };
    size_t cur = 8; mos_config_feature f;
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(config_walks_real_dvdrom_profile_list)
{
    /* A complete spec-valid GET CONFIGURATION (RT=0) DVD-ROM response:
       8-byte header (Data Length 0x24 = 36 → 40 total, current profile
       0x0010) then Profile List (0x0000), Core (0x0001), Removable Medium
       (0x0003). Hand-built to T10/MMC and walked with the production
       iterator — a drive-independent conformance check, not "matches my
       drive." Mirrors fixtures/getconfig_dvdrom_current.bin. */
    uint8_t buf[] = {
        0x00,0x00,0x00,0x24,  0x00,0x00, 0x00,0x10,
        0x00,0x00, 0x03, 0x08,  0x00,0x10,0x01,0x00,  0x00,0x08,0x00,0x00,
        0x00,0x01, 0x0B, 0x08,  0x00,0x00,0x00,0x02,  0x03,0x00,0x00,0x00,
        0x00,0x03, 0x0B, 0x04,  0x29,0x00,0x00,0x00,
    };
    EXPECT_EQ(sizeof buf, 40u);                    /* Data Length 0x24 + 4 */
    size_t cur = 8; mos_config_feature f;

    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT_EQ(f.feature_code, 0x0000);             /* Profile List */
    EXPECT(f.current && f.persistent);
    EXPECT_EQ(f.version, 0);
    EXPECT_EQ(f.data_len, 8);                       /* two 4-byte profile descriptors */
    EXPECT(f.data[0] == 0x00 && f.data[1] == 0x10 && f.data[2] == 0x01); /* DVD-ROM, current */
    EXPECT(f.data[4] == 0x00 && f.data[5] == 0x08 && f.data[6] == 0x00); /* CD-ROM, not current */

    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT_EQ(f.feature_code, 0x0001);             /* Core */
    EXPECT(f.current && f.persistent);
    EXPECT_EQ(f.version, 2);
    EXPECT_EQ(f.data_len, 8);
    EXPECT_EQ(f.data[3], 0x02);                     /* Physical Interface Standard = ATAPI */

    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT_EQ(f.feature_code, 0x0003);             /* Removable Medium */
    EXPECT(f.current && f.persistent);
    EXPECT_EQ(f.version, 2);
    EXPECT_EQ(f.data_len, 4);
    EXPECT_EQ(f.data[0], 0x29);                     /* tray-load, eject, lock capable */

    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT_EQ(cur, 40u);                            /* consumed exactly, no phantom feature */
    return 0;
}

/* ---- Current-profile extraction (length-gated) ------------------------- */

TEST(config_profile_extracted_when_data_length_covers_it)
{
    /* Data Length = 4 (bytes 4-7 follow), current profile 0x0010 at bytes 6-7. */
    uint8_t buf[16] = {0};
    buf[3] = 0x04;
    buf[6] = 0x00; buf[7] = 0x10;
    uint16_t p = 0xFFFF;
    EXPECT_EQ(mos_internal_config_current_profile(buf, sizeof buf, &p), true);
    EXPECT_EQ(p, 0x0010);
    return 0;
}

TEST(config_profile_zero_is_valid_when_full_header_present)
{
    /* A full header reporting 0x0000 is a legitimate "no current profile",
       distinct from a truncated reply — accepted, profile 0. */
    uint8_t buf[16] = {0};
    buf[3] = 0x04;                 /* full header present */
    uint16_t p = 0xFFFF;
    EXPECT_EQ(mos_internal_config_current_profile(buf, sizeof buf, &p), true);
    EXPECT_EQ(p, 0x0000);
    return 0;
}

TEST(config_profile_rejected_on_short_data_length)
{
    /* Data Length < 4 → profile field not returned; reject rather than read
       a zeroed buf[6]/buf[7] as a confident 0x0000 (the truncated-GOOD
       case). */
    uint8_t buf[16] = {0};
    buf[3] = 0x02;                 /* claims only 2 following bytes */
    buf[6] = 0x00; buf[7] = 0x10;  /* stale-looking bytes present but not covered */
    uint16_t p = 0xFFFF;
    EXPECT_EQ(mos_internal_config_current_profile(buf, sizeof buf, &p), false);
    return 0;
}

TEST(config_profile_rejected_on_short_buffer)
{
    uint8_t buf[6] = {0};          /* fewer than 8 bytes — can't reach profile */
    buf[3] = 0x04;
    uint16_t p = 0xFFFF;
    EXPECT_EQ(mos_internal_config_current_profile(buf, sizeof buf, &p), false);
    return 0;
}

TEST(config_profile_null_args_are_safe)
{
    uint8_t buf[16] = {0};
    uint16_t p = 0;
    EXPECT_EQ(mos_internal_config_current_profile(NULL, 16, &p), false);
    EXPECT_EQ(mos_internal_config_current_profile(buf, sizeof buf, NULL), false);
    return 0;
}


TEST(config_payload_ending_exactly_at_span_is_accepted)
{
    /* One descriptor whose Additional Length lands the cursor exactly on
       the declared span — the inclusive boundary the bounds arithmetic must
       accept (one byte more is the rejection tests' territory). Data Length
       counts bytes after itself (16 - 4 = 12); add-len 4 ends the span
       exactly at the trusted end. */
    uint8_t buf[] = {
        0,0,0,12,  0,0, 0,0,
        0x00,0x1E, 0x01, 0x04,  0xDE,0xAD,0xBE,0xEF,
    };
    size_t cur = 8; mos_config_feature f;
    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT(f.feature_code == 0x001E);
    EXPECT(f.data_len == 4);
    EXPECT(f.data != NULL && f.data[0] == 0xDE && f.data[3] == 0xEF);
    /* Exactly consumed: walk ends cleanly, no further feature. */
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(aacs_caps_decode_and_fail_closed)
{
    /* AACS feature, version 2, BEC set (not WBE), AACS version 68. */
    uint8_t buf[] = {
        0,0,0,16,  0,0, 0x00,0x40,
        0x00,0x00, 0x03, 0x00,
        0x01,0x0D, 0x09, 0x04,  0x03, 0x00, 0x01, 68,
    };
    mos_drive_caps c;
    mos_internal_protection_from_config(buf, sizeof buf, &c);
    EXPECT(c.protection.aacs && c.protection.bus_encryption &&
           !c.protection.write_bus_encryption && c.protection.aacs_version == 68);

    /* WBE set too (byte 0 bit 2). */
    buf[16] = 0x07;
    mos_internal_protection_from_config(buf, sizeof buf, &c);
    EXPECT(c.protection.aacs && c.protection.bus_encryption &&
           c.protection.write_bus_encryption);

    /* BEC clear. */
    buf[16] = 0x01;
    mos_internal_protection_from_config(buf, sizeof buf, &c);
    EXPECT(c.protection.aacs && !c.protection.bus_encryption &&
           c.protection.aacs_version == 68);

    /* Feature absent. */
    uint8_t plain[] = { 0,0,0,8,  0,0, 0x00,0x10,  0x00,0x00, 0x03, 0x00 };
    mos_internal_protection_from_config(plain, sizeof plain, &c);
    EXPECT(!c.protection.aacs && !c.protection.bus_encryption &&
           c.protection.aacs_version == 0);

    /* Feature present but payload truncated to 0 bytes: malformed, reads as
       absent (fail closed). */
    uint8_t trunc[] = { 0,0,0,8,  0,0, 0x00,0x40,  0x01,0x0D, 0x09, 0x00 };
    mos_internal_protection_from_config(trunc, sizeof trunc, &c);
    EXPECT(!c.protection.aacs);

    mos_internal_protection_from_config(NULL, 0, &c);
    EXPECT(!c.protection.aacs);
    mos_internal_protection_from_config(buf, sizeof buf, NULL); /* no crash */
    return 0;
}

TEST(protection_all_schemes_decode)
{
    /* RT=0 reply carrying CSS (0106h v1), CPRM (010Bh v3), AACS (010Dh
       BEC+WBE, v68), SecurDisc (0113h, AddLen 0), VCPS (0110h, AddLen 0).
       Each version-scheme has Additional Length 4; the presence-only schemes
       have Additional Length 0. */
    uint8_t buf[] = {
        0,0,0,40,  0,0, 0x00,0x40,
        0x01,0x06, 0x01, 0x04,  0x00, 0x00, 0x00, 0x01,   /* CSS  v1   */
        0x01,0x0B, 0x01, 0x04,  0x00, 0x00, 0x00, 0x03,   /* CPRM v3   */
        0x01,0x0D, 0x09, 0x04,  0x06, 0x00, 0x01, 68,     /* AACS BEC+WBE v68 */
        0x01,0x13, 0x01, 0x00,                            /* SecurDisc */
        0x01,0x10, 0x01, 0x00,                            /* VCPS      */
    };
    mos_drive_caps c;
    mos_internal_protection_from_config(buf, sizeof buf, &c);
    EXPECT(c.protection.css  && c.protection.css_version  == 1);
    EXPECT(c.protection.cprm && c.protection.cprm_version == 3);
    EXPECT(c.protection.aacs && c.protection.aacs_version == 68);
    EXPECT(c.protection.bus_encryption && c.protection.write_bus_encryption);
    EXPECT(c.protection.securdisc);
    EXPECT(c.protection.vcps);

    /* None present: every scheme reads false/0. */
    uint8_t plain[] = { 0,0,0,8,  0,0, 0x00,0x10,  0x00,0x00, 0x03, 0x00 };
    mos_internal_protection_from_config(plain, sizeof plain, &c);
    EXPECT(!c.protection.css && !c.protection.cprm && !c.protection.aacs &&
           !c.protection.securdisc && !c.protection.vcps);
    EXPECT(c.protection.css_version == 0 && c.protection.cprm_version == 0);
    return 0;
}

TEST(config_find_feature_returns_match_with_payload)
{
    /* Profile List, then an AACS descriptor (0x010D, header-bit version 2,
       4 payload bytes: BNG, nonce blocks, AGIDs, AACS version 68 — the
       MMC-6 AACS feature shape). */
    uint8_t buf[] = {
        0,0,0,16,  0,0, 0x00,0x40,
        0x00,0x00, 0x03, 0x00,
        0x01,0x0D, 0x09, 0x04,  0x01, 0x00, 0x01, 68,
    };
    mos_config_feature f;
    EXPECT(mos_internal_config_find_feature(buf, sizeof buf, 0x010D, &f));
    EXPECT(f.feature_code == 0x010D);
    EXPECT(f.current);
    EXPECT(f.version == 2);
    EXPECT(f.data_len == 4 && f.data != NULL);
    EXPECT(f.data[3] == 68);
    /* Earlier feature is reachable too — find is not a tail scan. */
    EXPECT(mos_internal_config_find_feature(buf, sizeof buf, 0x0000, &f));
    EXPECT(f.feature_code == 0x0000);
    return 0;
}

TEST(config_find_feature_absent_or_hostile_is_false)
{
    uint8_t buf[] = {
        0,0,0,8,  0,0, 0x00,0x10,
        0x00,0x00, 0x03, 0x00,
    };
    mos_config_feature f;
    EXPECT(!mos_internal_config_find_feature(buf, sizeof buf, 0x010D, &f));
    /* Misaligned Additional Length ends the walk before any match —
       fail-closed propagates to not-found. */
    uint8_t bad[] = {
        0,0,0,8,  0,0, 0x00,0x10,
        0x01,0x0D, 0x03, 0x03,
    };
    EXPECT(!mos_internal_config_find_feature(bad, sizeof bad, 0x010D, &f));
    EXPECT(!mos_internal_config_find_feature(NULL, 0, 0x010D, &f));
    EXPECT(!mos_internal_config_find_feature(bad, sizeof bad, 0x010D, NULL));
    return 0;
}

TEST(profile_list_extracts_drive_static_set)
{
    /* GET CONFIG RT=0: 8-byte header (current 0x0010) + Profile List (0x0000)
       with three 4-byte descriptors — DVD-ROM (CurrentP set), CD-ROM, BD-RE.
       The per-descriptor CurrentP bit is media-dependent and must be ignored;
       the extracted set is the drive-static profile numbers in list order. */
    uint8_t buf[] = {
        0x00,0x00,0x00,0x14,  0x00,0x00, 0x00,0x10,   /* dlen 20 → total 24 */
        0x00,0x00, 0x03, 0x0C,                         /* Profile List, add=12 */
        0x00,0x10, 0x01, 0x00,                         /* DVD-ROM, CurrentP=1  */
        0x00,0x08, 0x00, 0x00,                         /* CD-ROM               */
        0x00,0x43, 0x00, 0x00,                         /* BD-RE                */
    };
    uint16_t codes[8]; uint8_t n = 99;
    mos_internal_profile_list_from_config(buf, sizeof buf, codes, 8, &n);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(codes[0], 0x0010);
    EXPECT_EQ(codes[1], 0x0008);
    EXPECT_EQ(codes[2], 0x0043);
    EXPECT_STREQ("bd_re", mos_profile_name(codes[2]));
    return 0;
}

TEST(profile_list_absent_empty_bounds_and_null)
{
    uint16_t codes[2]; uint8_t n = 99;

    /* Feature absent (only Core present) → count 0. */
    uint8_t plain[] = { 0,0,0,8, 0,0, 0x00,0x10, 0x00,0x01, 0x03, 0x00 };
    mos_internal_profile_list_from_config(plain, sizeof plain, codes, 2, &n);
    EXPECT_EQ(n, 0);

    /* Present but empty (add=0) → count 0, not malformed. */
    uint8_t empty[] = { 0,0,0,8, 0,0, 0,0, 0x00,0x00, 0x03, 0x00 };
    n = 99;
    mos_internal_profile_list_from_config(empty, sizeof empty, codes, 2, &n);
    EXPECT_EQ(n, 0);

    /* Three descriptors but cap = 2 → truncates to cap, no overflow. */
    uint8_t buf[] = {
        0,0,0,0x14,  0,0, 0,0,
        0x00,0x00, 0x03, 0x0C,
        0x00,0x10, 0x00, 0x00,
        0x00,0x08, 0x00, 0x00,
        0x00,0x43, 0x00, 0x00,
    };
    n = 99;
    mos_internal_profile_list_from_config(buf, sizeof buf, codes, 2, &n);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(codes[0], 0x0010);
    EXPECT_EQ(codes[1], 0x0008);

    /* NULL / zero-cap / NULL-buf are safe and yield count 0. */
    n = 99;
    mos_internal_profile_list_from_config(NULL, 0, codes, 2, &n);
    EXPECT_EQ(n, 0);
    mos_internal_profile_list_from_config(buf, sizeof buf, NULL, 2, &n);
    EXPECT_EQ(n, 0);
    mos_internal_profile_list_from_config(buf, sizeof buf, codes, 0, &n);
    EXPECT_EQ(n, 0);
    return 0;
}

TEST(firmware_date_decodes_iso8601)
{
    /* GET CONFIG RT=0: header + Firmware Information feature (010Ch), MMC-6
       Table 197. Payload = Century "20" Year "13" Month "08" Day "12"
       Hour "13" Minute "20" Second "43" Reserved — decimal ASCII. */
    uint8_t buf[] = {
        0x00,0x00,0x00,0x18,  0x00,0x00, 0x00,0x10,   /* dlen 24 → total 28 */
        0x01,0x0C, 0x03, 0x10,                         /* 010Ch, cur+pers, add=16 */
        0x32,0x30, 0x31,0x33, 0x30,0x38, 0x31,0x32,    /* 20 13 08 12 */
        0x31,0x33, 0x32,0x30, 0x34,0x33, 0x00,0x00,    /* 13 20 43 rsv */
    };
    char out[24];
    mos_internal_firmware_date_from_config(buf, sizeof buf, out, sizeof out);
    EXPECT_STREQ(out, "2013-08-12T13:20:43Z");
    return 0;
}

TEST(firmware_date_absent_malformed_and_bounds)
{
    char out[24];

    /* Feature absent → empty. */
    uint8_t plain[] = { 0,0,0,8, 0,0, 0x00,0x10, 0x00,0x01, 0x03, 0x00 };
    mos_internal_firmware_date_from_config(plain, sizeof plain, out, sizeof out);
    EXPECT(out[0] == 0);

    /* Present but a non-digit byte (Month high 'X') → fail closed, empty. */
    uint8_t bad[] = {
        0x00,0x00,0x00,0x18,  0x00,0x00, 0x00,0x10,
        0x01,0x0C, 0x03, 0x10,
        0x32,0x30, 0x31,0x33, 'X',0x38, 0x31,0x32,
        0x31,0x33, 0x32,0x30, 0x34,0x33, 0x00,0x00,
    };
    mos_internal_firmware_date_from_config(bad, sizeof bad, out, sizeof out);
    EXPECT(out[0] == 0);

    /* Additional Length too short (12 < 14 needed) → empty. */
    uint8_t shortp[] = {
        0x00,0x00,0x00,0x14,  0x00,0x00, 0x00,0x10,
        0x01,0x0C, 0x03, 0x0C,
        0x32,0x30, 0x31,0x33, 0x30,0x38, 0x31,0x32, 0x31,0x33, 0x32,0x30,
    };
    mos_internal_firmware_date_from_config(shortp, sizeof shortp, out, sizeof out);
    EXPECT(out[0] == 0);

    /* Too-small out buffer and NULL are safe. */
    uint8_t ok[] = {
        0x00,0x00,0x00,0x18,  0x00,0x00, 0x00,0x10,
        0x01,0x0C, 0x03, 0x10,
        0x32,0x30, 0x31,0x33, 0x30,0x38, 0x31,0x32,
        0x31,0x33, 0x32,0x30, 0x34,0x33, 0x00,0x00,
    };
    char tiny[8] = { 'x' };
    mos_internal_firmware_date_from_config(ok, sizeof ok, tiny, sizeof tiny);
    EXPECT(tiny[0] == 0);                       /* refused: cap < 21 */
    mos_internal_firmware_date_from_config(ok, sizeof ok, NULL, 24);   /* no crash */
    return 0;
}

void register_config_tests(void)
{
    RUN(profile_list_extracts_drive_static_set);
    RUN(profile_list_absent_empty_bounds_and_null);
    RUN(firmware_date_decodes_iso8601);
    RUN(firmware_date_absent_malformed_and_bounds);
    RUN(toc_parses_real_pony_cd_single);
    RUN(aacs_caps_from_real_wh16ns40_capture);
    RUN(aacs_caps_decode_and_fail_closed);
    RUN(protection_all_schemes_decode);
    RUN(config_find_feature_returns_match_with_payload);
    RUN(config_find_feature_absent_or_hostile_is_false);
    RUN(config_payload_ending_exactly_at_span_is_accepted);
    RUN(config_walks_real_dvdrom_profile_list);
    RUN(config_walks_two_well_formed_features);
    RUN(config_feature_with_payload_exposes_bounded_slice);
    RUN(config_skips_payload_to_next_descriptor);
    RUN(config_honors_data_length_shorter_than_buffer);
    RUN(config_additional_length_past_buffer_is_rejected);
    RUN(toc_parses_realistic_audio_cd);
    RUN(toc_fail_closed_on_hostile_shapes);
    RUN(toc_fail_closed_on_header_descriptor_mismatch);
    RUN(profile_class_total_over_name_table);
    RUN(trusted_len_each_authority_binds);
    RUN(trusted_len_hostile_and_degenerate_inputs);
    RUN(config_misaligned_additional_length_span_fits);
    RUN(config_misaligned_additional_length_is_rejected);
    RUN(config_truncated_descriptor_header_is_rejected);
    RUN(config_lying_large_data_length_clamped_to_buffer);
    RUN(config_header_only_yields_no_features);
    RUN(config_short_buffer_and_misuse_stay_in_bounds);
    RUN(config_null_args_are_safe);
    RUN(config_dense_fill_terminates_and_consumes_exactly);
    RUN(config_profile_extracted_when_data_length_covers_it);
    RUN(config_profile_zero_is_valid_when_full_header_present);
    RUN(config_profile_rejected_on_short_data_length);
    RUN(config_profile_rejected_on_short_buffer);
    RUN(config_profile_null_args_are_safe);
}
