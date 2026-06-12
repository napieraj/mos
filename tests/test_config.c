/*
 * test_config.c — GET CONFIGURATION feature walk: well-formed decode plus
 * hostile/malformed buffers. The load-bearing property is no-OOB: under
 * AddressSanitizer any out-of-bounds read here aborts. The malformed
 * fixtures (lying Data Length, Additional Length past the buffer,
 * truncated header) are the device-controlled-length attacks the walk
 * exists to neutralize.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

TEST(config_walks_two_well_formed_features)
{
    /* header: data length = 12 (= total 16 - 4), profile 0x0010; then
       Profile List (0x0000, cur+persistent) and Core (0x0001, cur). */
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
    /* add = 0xFC = 252: the largest 4-aligned value a one-byte Additional
       Length can hold, i.e. the worst-case aligned over-read a descriptor
       can claim. Being aligned it passes the %4 gate, so this stays a pure
       bounds test (distinct from the malformed-length case below): only 2
       payload bytes are present, so the walk must refuse it on bounds
       rather than read 252 absent bytes. */
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
    /* Data Length claims ~4 GiB; the walk must clamp to sizeof(buf) and
       yield only the one real descriptor, no phantom features past it. */
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
    /* feat A carries a 4-byte payload, feat B follows. Proves the cursor
       advances over A's payload (span = 4 + add) to land exactly on B, and
       that a nonzero version field decodes. */
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
    /* Buffer physically holds two descriptors, but Data Length covers only
       the first. The trailing in-buffer descriptor must NOT be yielded. */
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
    /* Real commercial pressing: Ginuwine "Pony" CD single (1996), 4
       tracks — MusicBrainz disc TX6lKZ481BHv1ZW6pd6007j6OY4-,
       AccurateRip-confirmed (whipper-team/whipper PR #382 rip log).
       LBAs derived from the log's attested toc string
       1+4+86497+150+24687+47627+68002 (MB offset = LBA + 150); ADR=1
       and copy-bit=0 are the standard-pressing assumption (fixtures
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
    /* MakeMKV drive dump, HL-DT-ST BD-RE WH16NS40 1.05: "Bus
       encryption flags: 17" (= 0x17 per the libaacs bit map:
       RDC|WBE|BEC|BNG), "Highest AACS version: 78". Scaffold bytes
       (profile, feature-header byte 2, nonce/AGID counts) are
       unattested — see the fixtures README entry. Mirrors
       fixtures/getconfig_aacs_wh16ns40.bin. */
    static const uint8_t cfg[] = {
        0,0,0,16,  0,0, 0x00,0x00,
        0x01,0x0D, 0x00, 0x04,  0x17, 0x00, 0x00, 78,
    };
    mos_drive_caps c;
    mos_internal_aacs_caps_from_config(cfg, sizeof cfg, &c);
    EXPECT(c.aacs);
    EXPECT(c.bus_encryption);          /* 0x17 & 0x02 */
    EXPECT_EQ(c.aacs_version, 78);
    return 0;
}

TEST(toc_parses_realistic_audio_cd)
{
    /* 3-track audio CD + lead-out, MMC format-0 shape. Header Data
       Length = 2 (first/last) + 4*8 (descriptors) = 34. */
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

    /* Non-ascending and duplicate INTERIOR tracks under headers whose
       range/endpoints are otherwise coherent — fixtures that reach the
       ordering gate without the header-consistency checks masking it. */
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

    /* Lying Data Length: claims more than the buffer (O-4 clamp), and
       the clamp lands mid-descriptor — partial descriptor = malformed. */
    t[4 + 2] = 1; t[12 + 2] = 2; t[20 + 2] = 0xAA;
    t[1] = 200;
    EXPECT_EQ(false, mos_internal_toc_parse(t, 4 + 20, &toc));

    /* Honest short claim that truncates cleanly used to parse as a
       partial table; under the header-consistency rule it is the
       half-parsed-identity case and fails: the header still declares
       tracks 1..2 but the claimed span (2 + 8 = 10) covers only one
       descriptor. */
    t[4 + 2] = 1; t[12 + 2] = 2; t[20 + 2] = 0xAA;
    t[1] = 10;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* The same short claim with a COHERENT header (1..1) parses: the
       clamp itself is fine when the table it leaves is whole. */
    t[3] = 1;
    EXPECT_EQ(true, mos_internal_toc_parse(t, sizeof t, &toc));
    EXPECT_EQ(1, toc.track_count);
    EXPECT_EQ(false, toc.have_leadout);
    return 0;
}

TEST(toc_fail_closed_on_header_descriptor_mismatch)
{
    /* Descriptors can be individually well-formed while the header
       declares a different table. All hostile; all rejected whole. */
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

    /* Count matches the range but the endpoints don't: header 1..2,
       descriptors {1, 3} — the pigeonhole edge. */
    t[2] = 1; t[3] = 2;
    t[4 + 2] = 1; t[12 + 2] = 3;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Endpoints match but a declared track is missing: header 1..3,
       descriptors {1, 3} — only the count check catches this. */
    t[3] = 3;
    EXPECT_EQ(false, mos_internal_toc_parse(t, sizeof t, &toc));

    /* Header-only TOC (claimed length covers no descriptors): a
       declared range with zero descriptors is incomplete, not empty. */
    t[1] = 2; t[2] = 1; t[3] = 1;
    EXPECT_EQ(false, mos_internal_toc_parse(t, 4, &toc));

    /* Coherent table not starting at 1 parses: header 2..4,
       descriptors {2, 3, 4} exactly, no lead-out. */
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
    /* Every profile the name table knows must map to a class or be a
       DELIBERATE classless entry. The classless set is closed:
       no-profile, the legacy/MO trio. A profile added to the name
       table without a class lands in default NULL and fails here —
       the staleness pair for the two switch statements. */
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
    /* Spot semantics + the unknown code. */
    EXPECT_STREQ("cd",     mos_profile_class(0x0009));
    EXPECT_STREQ("dvd",    mos_profile_class(0x0017));
    EXPECT_STREQ("bd",     mos_profile_class(0x0043));
    EXPECT_STREQ("hd_dvd", mos_profile_class(0x0050));
    EXPECT(mos_profile_class(0xBEEF) == NULL);
    return 0;
}




TEST(trusted_len_each_authority_binds)
{
    /* Seam contract O-4 (the dual-length rule): trusted region is
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
    /* The attack shape from the audit conversation: header claims
       0xFFFF (or worse) over a tiny transfer — the classic SCSI
       allocation-length overread. And a claim computed as
       `data_length + header` at maximum field value, which must have
       been computed in 64-bit by the caller and must clamp here, not
       wrap. Plus the zero degeneracies: any zero authority zeroes the
       region (a zero-byte transfer trusts nothing, regardless of what
       the header inside those zero bytes would have claimed). */
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
    /* Fourth review, finding 7 (donated regression test, adapted): the
       companion test above uses a buffer short enough that the BOUNDS
       check rejects the misaligned descriptor too — the mutation campaign
       deleted the alignment guard (`add & 3`) and the suite stayed green.
       Here the malformed descriptor's span FITS entirely inside the
       trusted region, so only the alignment guard can stop the walk; a
       desync would fabricate a feature 0xDEAD from misaligned bytes.
       Verified to kill the guard-deletion mutant. */
    uint8_t buf[32] = {0};
    buf[3] = 28;                          /* Data Length -> trusted end 32 */
    /* Descriptor at cursor 8: code 0x1234, Additional Length 5 (NOT a
       multiple of 4). span = 4 + 5 = 9; 8 + 9 = 17 <= 32 — in bounds. */
    buf[8] = 0x12; buf[9] = 0x34; buf[10] = 0x00; buf[11] = 5;
    /* Bytes a desynced walk would decode at cursor 17 as "feature 0xDEAD". */
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
    /* add = 2 is not a multiple of 4 → malformed; the walk stops rather
       than desyncing onto misaligned bytes. */
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
    /* A complete, spec-valid GET CONFIGURATION (RT=0) response for a DVD-ROM
       drive: 8-byte header (Data Length 0x24 = 36 → 40 bytes total, current
       profile 0x0010) then Profile List (0x0000), Core (0x0001), Removable
       Medium (0x0003). Hand-built to the T10/MMC layout and verified here by
       walking it with the production iterator — the drive-independent
       conformance check that matters, not "matches my drive." Raw bytes
       mirrored at fixtures/getconfig_dvdrom_current.bin. */
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
    /* A full header reporting profile 0x0000 is a legitimate "no current
       profile", distinct from a truncated reply — accepted, profile 0. */
    uint8_t buf[16] = {0};
    buf[3] = 0x04;                 /* full header present */
    uint16_t p = 0xFFFF;
    EXPECT_EQ(mos_internal_config_current_profile(buf, sizeof buf, &p), true);
    EXPECT_EQ(p, 0x0000);
    return 0;
}

TEST(config_profile_rejected_on_short_data_length)
{
    /* Data Length < 4 → the profile field was not returned; reject rather
       than read a zeroed buf[6]/buf[7] as a confident 0x0000. This is the
       truncated-GOOD-response case. */
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
    /* One descriptor whose Additional Length lands the cursor exactly
       on the declared span — the inclusive boundary the walker's
       bounds arithmetic must accept (one byte more is the existing
       rejection tests' territory). Header Data Length counts bytes
       AFTER itself: 16 total - 4 = 12; descriptor add-len = 4, so the
       descriptor's span ends exactly at the trusted end. */
    uint8_t buf[] = {
        0,0,0,12,  0,0, 0,0,
        0x00,0x1E, 0x01, 0x04,  0xDE,0xAD,0xBE,0xEF,
    };
    size_t cur = 8; mos_config_feature f;
    EXPECT(mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    EXPECT(f.feature_code == 0x001E);
    EXPECT(f.data_len == 4);
    EXPECT(f.data != NULL && f.data[0] == 0xDE && f.data[3] == 0xEF);
    /* Exactly consumed: the walk ends cleanly, no further feature. */
    EXPECT(!mos_internal_config_next_feature(buf, sizeof buf, &cur, &f));
    return 0;
}

TEST(aacs_caps_decode_and_fail_closed)
{
    /* AACS feature, version 2, BEC set, AACS version 68. */
    uint8_t buf[] = {
        0,0,0,16,  0,0, 0x00,0x40,
        0x00,0x00, 0x03, 0x00,
        0x01,0x0D, 0x09, 0x04,  0x03, 0x00, 0x01, 68,
    };
    mos_drive_caps c;
    mos_internal_aacs_caps_from_config(buf, sizeof buf, &c);
    EXPECT(c.aacs && c.bus_encryption && c.aacs_version == 68);

    /* BEC clear. */
    buf[16] = 0x01;
    mos_internal_aacs_caps_from_config(buf, sizeof buf, &c);
    EXPECT(c.aacs && !c.bus_encryption && c.aacs_version == 68);

    /* Feature absent. */
    uint8_t plain[] = { 0,0,0,8,  0,0, 0x00,0x10,  0x00,0x00, 0x03, 0x00 };
    mos_internal_aacs_caps_from_config(plain, sizeof plain, &c);
    EXPECT(!c.aacs && !c.bus_encryption && c.aacs_version == 0);

    /* Feature present but payload truncated to 0 bytes: malformed,
       reads as absent (fail closed). */
    uint8_t trunc[] = { 0,0,0,8,  0,0, 0x00,0x40,  0x01,0x0D, 0x09, 0x00 };
    mos_internal_aacs_caps_from_config(trunc, sizeof trunc, &c);
    EXPECT(!c.aacs);

    mos_internal_aacs_caps_from_config(NULL, 0, &c);
    EXPECT(!c.aacs);
    mos_internal_aacs_caps_from_config(buf, sizeof buf, NULL); /* no crash */
    return 0;
}

TEST(config_find_feature_returns_match_with_payload)
{
    /* Profile List, then an AACS descriptor (0x010D, version 2 in the
       header bits, 4 payload bytes: BNG, nonce blocks, AGIDs, AACS
       version 68 — the MMC-6 AACS feature shape). */
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
    /* Misaligned Additional Length ends the walk before any match:
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

void register_config_tests(void)
{
    RUN(toc_parses_real_pony_cd_single);
    RUN(aacs_caps_from_real_wh16ns40_capture);
    RUN(aacs_caps_decode_and_fail_closed);
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
