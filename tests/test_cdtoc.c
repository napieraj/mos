/*
 * test_cdtoc.c — decode of the macOS kernel-cached full-TOC blob (the Apple
 * CDTOC struct, kIOCDMediaTOCKey) into per-session boundaries. Fixtures are
 * built to the IOCDTypes.h wire layout (libcdio read_toc_osx cross-check); the
 * hostile cases pin the no-OOB property — a device-reported CDTOC length must
 * only ever SHRINK the descriptor walk.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>

/* CDTOC = 4-byte header + 11-byte descriptors. Each descriptor:
   [0]session [1](adr<<4)|control [2]tno [3]point [4..6]addr MSF [7]zero
   [8..10]p MSF (PMIN/PSEC/PFRAME). */
#define DESC 11u

/* Append one descriptor at b[*off]; advance *off. adr is fixed to 1 (the
   position descriptors the parser keys on) unless overridden. */
static void put_desc(uint8_t *b, size_t *off, uint8_t session, uint8_t adr,
                     uint8_t point, uint8_t pmin, uint8_t psec, uint8_t pframe)
{
    uint8_t *d = &b[*off];
    memset(d, 0, DESC);
    d[0] = session;
    d[1] = (uint8_t)((adr & 0x0f) << 4);   /* control low nibble = 0 */
    d[3] = point;
    d[8] = pmin; d[9] = psec; d[10] = pframe;
    *off += DESC;
}

/* Finalize the 2-byte BE length = total - 2, with sessionFirst/sessionLast. */
static void finalize(uint8_t *b, size_t total, uint8_t sfirst, uint8_t slast)
{
    uint16_t len = (uint16_t)(total - 2u);
    b[0] = (uint8_t)(len >> 8); b[1] = (uint8_t)len;
    b[2] = sfirst; b[3] = slast;
}

/* MSF -> LBA, mirroring the parser: (m*60+s)*75 + f - 150. */
static uint32_t msf_lba(uint8_t m, uint8_t s, uint8_t f)
{
    uint32_t fr = ((uint32_t)m * 60u + s) * 75u + f;
    return fr >= 150u ? fr - 150u : 0u;
}

TEST(cdtoc_single_session)
{
    uint8_t b[256] = {0};
    size_t off = 4;
    /* One session: first track 1, last track 10, lead-out at 54:00:00. The
       per-track POINTs (1..10) are present but carry no session boundary. */
    put_desc(b, &off, 1, 1, 0xA0, 1, 0, 0);
    put_desc(b, &off, 1, 1, 0xA1, 10, 0, 0);
    put_desc(b, &off, 1, 1, 0xA2, 54, 0, 0);
    put_desc(b, &off, 1, 1, 1, 0, 2, 0);     /* track 1 start, ignored */
    finalize(b, off, 1, 1);

    mos_session_layout s;
    EXPECT(mos_internal_cdtoc_parse(b, off, &s));
    EXPECT_EQ(mos_session_layout_count(&s), 1);
    EXPECT_EQ(mos_session_layout_session(&s, 0), 1);
    EXPECT_EQ(mos_session_layout_first_track(&s, 0), 1);
    EXPECT_EQ(mos_session_layout_last_track(&s, 0), 10);
    EXPECT(mos_session_layout_have_leadout(&s, 0));
    EXPECT_EQ(mos_session_layout_leadout_lba(&s, 0), msf_lba(54, 0, 0));
    return 0;
}

TEST(cdtoc_multisession_cd_extra)
{
    /* CD-Extra: session 1 = audio tracks 1-12, session 2 = data track 13. */
    uint8_t b[512] = {0};
    size_t off = 4;
    put_desc(b, &off, 1, 1, 0xA0, 1, 0, 0);
    put_desc(b, &off, 1, 1, 0xA1, 12, 0, 0);
    put_desc(b, &off, 1, 1, 0xA2, 50, 0, 0);
    put_desc(b, &off, 2, 1, 0xA0, 13, 0, 0);
    put_desc(b, &off, 2, 1, 0xA1, 13, 0, 0);
    put_desc(b, &off, 2, 1, 0xA2, 60, 30, 0);
    finalize(b, off, 1, 2);

    mos_session_layout s;
    EXPECT(mos_internal_cdtoc_parse(b, off, &s));
    EXPECT_EQ(mos_session_layout_count(&s), 2);
    EXPECT_EQ(mos_session_layout_session(&s, 0), 1);
    EXPECT_EQ(mos_session_layout_last_track(&s, 0), 12);
    EXPECT_EQ(mos_session_layout_session(&s, 1), 2);
    EXPECT_EQ(mos_session_layout_first_track(&s, 1), 13);
    EXPECT_EQ(mos_session_layout_last_track(&s, 1), 13);
    EXPECT_EQ(mos_session_layout_leadout_lba(&s, 1), msf_lba(60, 30, 0));
    return 0;
}

TEST(cdtoc_partial_session_nulls)
{
    /* A session carrying only A2 (lead-out): first/last absent -> 0 sentinel,
       lead-out present. */
    uint8_t b[128] = {0};
    size_t off = 4;
    put_desc(b, &off, 1, 1, 0xA2, 20, 0, 0);
    finalize(b, off, 1, 1);

    mos_session_layout s;
    EXPECT(mos_internal_cdtoc_parse(b, off, &s));
    EXPECT_EQ(mos_session_layout_count(&s), 1);
    EXPECT_EQ(mos_session_layout_first_track(&s, 0), 0);   /* absent */
    EXPECT_EQ(mos_session_layout_last_track(&s, 0), 0);    /* absent */
    EXPECT(mos_session_layout_have_leadout(&s, 0));
    return 0;
}

TEST(cdtoc_non_adr1_ignored)
{
    /* ADR != 1 descriptors (catalogue/ISRC) carry no boundary; a blob with
       only those yields no sessions -> parse returns false. */
    uint8_t b[64] = {0};
    size_t off = 4;
    put_desc(b, &off, 1, 2, 0xA0, 1, 0, 0);   /* adr 2 */
    put_desc(b, &off, 1, 3, 0xA1, 9, 0, 0);   /* adr 3 */
    finalize(b, off, 1, 1);

    mos_session_layout s;
    EXPECT(!mos_internal_cdtoc_parse(b, off, &s));
    EXPECT_EQ(mos_session_layout_count(&s), 0);
    return 0;
}

TEST(cdtoc_fail_closed_on_hostile_buffers)
{
    uint8_t b[256] = {0};
    size_t off = 4;
    put_desc(b, &off, 1, 1, 0xA0, 1, 0, 0);
    put_desc(b, &off, 1, 1, 0xA1, 10, 0, 0);
    put_desc(b, &off, 1, 1, 0xA2, 54, 0, 0);
    finalize(b, off, 1, 1);

    mos_session_layout s;

    /* A declared length larger than the buffer must not read past it: clamp to
       the real length, which still yields the three boundary descriptors. */
    b[0] = 0xFF; b[1] = 0xFF;
    EXPECT(mos_internal_cdtoc_parse(b, off, &s));
    EXPECT_EQ(mos_session_layout_count(&s), 1);

    /* A declared length SHORTER than the descriptors shrinks the walk: header
       claims only itself (length 2 -> end 4), so no descriptors parse. */
    finalize(b, off, 1, 1);
    b[0] = 0x00; b[1] = 0x02;
    EXPECT(!mos_internal_cdtoc_parse(b, sizeof b, &s));

    /* Too-short, NULL, and NULL-out all refuse in bounds. */
    EXPECT(!mos_internal_cdtoc_parse(b, 3, &s));
    EXPECT(!mos_internal_cdtoc_parse(NULL, sizeof b, &s));
    finalize(b, off, 1, 1);
    b[0] = 0xFF; b[1] = 0xFF;
    EXPECT(!mos_internal_cdtoc_parse(b, off, NULL));
    return 0;
}

/* ---- mos_internal_cdtoc_to_toc: the primary-CD-TOC-source decode --------- */

/* Put a track POINT (1..99): adr 1, the given control nibble, p MSF = start. */
static void put_track(uint8_t *b, size_t *off, uint8_t session, uint8_t track,
                      uint8_t control, uint8_t m, uint8_t s, uint8_t f)
{
    uint8_t *d = &b[*off];
    memset(d, 0, DESC);
    d[0] = session;
    d[1] = (uint8_t)((1u << 4) | (control & 0x0f));   /* adr 1, control */
    d[3] = track;
    d[8] = m; d[9] = s; d[10] = f;
    *off += DESC;
}

TEST(cdtoc_to_toc_audio_cd)
{
    /* 3-track audio CD, single session: tracks at 0/10/20 s, lead-out 30 s. */
    uint8_t b[256] = {0};
    size_t off = 4;
    put_desc(b, &off, 1, 1, 0xA0, 1, 0, 0);
    put_desc(b, &off, 1, 1, 0xA1, 3, 0, 0);
    put_desc(b, &off, 1, 1, 0xA2, 0, 30, 0);          /* lead-out */
    put_track(b, &off, 1, 1, 0, 0, 2, 0);             /* track starts (MSF) */
    put_track(b, &off, 1, 2, 0, 0, 12, 0);
    put_track(b, &off, 1, 3, 0, 0, 22, 0);
    finalize(b, off, 1, 1);

    mos_toc t;
    EXPECT(mos_internal_cdtoc_to_toc(b, off, &t));
    EXPECT_EQ(mos_toc_first_track(&t), 1);
    EXPECT_EQ(mos_toc_last_track(&t), 3);
    EXPECT_EQ(mos_toc_track_count(&t), 3);
    EXPECT(mos_toc_have_leadout(&t));
    EXPECT_EQ(mos_toc_leadout_lba(&t), msf_lba(0, 30, 0));
    EXPECT_EQ(mos_toc_track_number(&t, 0), 1);
    EXPECT_EQ(mos_toc_track_start_lba(&t, 0), msf_lba(0, 2, 0));
    EXPECT_EQ(mos_toc_track_control(&t, 0), 0);        /* audio */
    return 0;
}

TEST(cdtoc_to_toc_data_control_bit)
{
    /* A data track carries control bit 2 — the data-vs-audio determinant. */
    uint8_t b[128] = {0};
    size_t off = 4;
    put_desc(b, &off, 1, 1, 0xA2, 1, 0, 0);
    put_track(b, &off, 1, 1, 0x4, 0, 2, 0);           /* control 0x4 = data */
    finalize(b, off, 1, 1);

    mos_toc t;
    EXPECT(mos_internal_cdtoc_to_toc(b, off, &t));
    EXPECT_EQ(mos_toc_track_count(&t), 1);
    EXPECT_EQ(mos_toc_track_control(&t, 0), 0x4);      /* bit 2 = data track */
    EXPECT((mos_toc_track_control(&t, 0) & 0x4) != 0);
    return 0;
}

TEST(cdtoc_to_toc_fail_closed)
{
    mos_toc t;

    /* A gap in first..last (track 2 missing) refuses the whole -> fall back. */
    uint8_t g[256] = {0};
    size_t off = 4;
    put_track(g, &off, 1, 1, 0, 0, 2, 0);
    put_track(g, &off, 1, 3, 0, 0, 22, 0);            /* 2 missing */
    finalize(g, off, 1, 1);
    EXPECT(!mos_internal_cdtoc_to_toc(g, off, &t));

    /* A duplicate track number is incoherent -> refuse. */
    uint8_t d[256] = {0};
    off = 4;
    put_track(d, &off, 1, 1, 0, 0, 2, 0);
    put_track(d, &off, 1, 1, 0, 0, 12, 0);            /* dup */
    finalize(d, off, 1, 1);
    EXPECT(!mos_internal_cdtoc_to_toc(d, off, &t));

    /* No track POINTs at all (only lead-out) -> no identity -> refuse. */
    uint8_t n[64] = {0};
    off = 4;
    put_desc(n, &off, 1, 1, 0xA2, 1, 0, 0);
    finalize(n, off, 1, 1);
    EXPECT(!mos_internal_cdtoc_to_toc(n, off, &t));

    /* Too-short / NULL stay in bounds and refuse. */
    EXPECT(!mos_internal_cdtoc_to_toc(n, 3, &t));
    EXPECT(!mos_internal_cdtoc_to_toc(NULL, sizeof n, &t));
    EXPECT(!mos_internal_cdtoc_to_toc(n, sizeof n, NULL));
    return 0;
}

void register_cdtoc_tests(void)
{
    RUN(cdtoc_single_session);
    RUN(cdtoc_multisession_cd_extra);
    RUN(cdtoc_partial_session_nulls);
    RUN(cdtoc_non_adr1_ignored);
    RUN(cdtoc_fail_closed_on_hostile_buffers);
    RUN(cdtoc_to_toc_audio_cd);
    RUN(cdtoc_to_toc_data_control_bit);
    RUN(cdtoc_to_toc_fail_closed);
}
