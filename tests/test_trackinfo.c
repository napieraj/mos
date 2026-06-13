/*
 * test_trackinfo.c — READ TRACK INFORMATION (0x52) Track Information
 * Block decode. Fixtures are built to the kernel's struct
 * track_information layout (include/uapi/linux/cdrom.h); the hostile
 * cases pin the no-OOB property — a device-controlled Track Information
 * Length must only ever SHRINK the trusted region.
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

/* Build a 40-byte Track Information Block. */
static void build_ti(uint8_t b[40], uint8_t track, uint8_t session,
                     uint8_t track_mode, bool damage, uint8_t data_mode,
                     bool blank, bool lra_v, bool nwa_v,
                     uint32_t start, uint32_t nwa, uint32_t freeb,
                     uint32_t size, uint32_t lra)
{
    memset(b, 0, 40);
    b[0] = 0x00; b[1] = 38;                       /* data length = 40 - 2 */
    b[2] = track;
    b[3] = session;
    b[5] = (uint8_t)((damage ? 0x20 : 0) | (track_mode & 0x0f));
    b[6] = (uint8_t)((blank ? 0x40 : 0) | (data_mode & 0x0f));
    b[7] = (uint8_t)((lra_v ? 0x02 : 0) | (nwa_v ? 0x01 : 0));
    be32(&b[8],  start);
    be32(&b[12], nwa);
    be32(&b[16], freeb);
    be32(&b[24], size);
    be32(&b[28], lra);
}

TEST(trackinfo_appendable_track)
{
    /* Blank/appendable: NWA valid, the append point; LRA not valid. */
    uint8_t b[40];
    build_ti(b, 1, 1, 0x4 /*data*/, false, 1, true /*blank*/,
             false /*lra_v*/, true /*nwa_v*/,
             0 /*start*/, 12345 /*nwa*/, 2000000 /*free*/,
             2298496 /*size*/, 0 /*lra*/);

    struct mos_track_info t;
    EXPECT(mos_internal_track_info_parse(b, sizeof b, &t));
    EXPECT(t.track_number == 1);
    EXPECT(t.session_number == 1);
    EXPECT(t.track_mode == 0x4);
    EXPECT(t.blank);
    EXPECT(t.nwa_valid);
    EXPECT(!t.lra_valid);
    EXPECT(t.next_writable == 12345);
    EXPECT(t.free_blocks == 2000000);
    EXPECT(t.track_size == 2298496);
    return 0;
}

TEST(trackinfo_complete_track)
{
    /* Finalized single-track DVD-ROM: not blank, LRA valid (last
       recorded), NWA not valid; track_size == disc capacity. */
    uint8_t b[40];
    build_ti(b, 1, 1, 0x4, false, 1, false /*blank*/,
             true /*lra_v*/, false /*nwa_v*/,
             0, 0, 0 /*free*/, 2298496 /*size*/, 2298495 /*lra*/);

    struct mos_track_info t;
    EXPECT(mos_internal_track_info_parse(b, sizeof b, &t));
    EXPECT(!t.blank);
    EXPECT(t.lra_valid);
    EXPECT(!t.nwa_valid);
    EXPECT(t.last_recorded == 2298495);
    EXPECT(t.track_size == 2298496);
    EXPECT(t.track_start == 0);
    return 0;
}

TEST(trackinfo_msb_fold)
{
    /* A long reply (>=34 bytes trusted) folds the track/session MSB. */
    uint8_t b[40];
    build_ti(b, 0x05, 0x02, 0x4, false, 1, false, true, false,
             0, 0, 0, 100, 99);
    b[32] = 0x01;   /* track MSB   -> 0x0105 = 261 */
    b[33] = 0x00;   /* session MSB -> 0x0002 = 2   */

    struct mos_track_info t;
    EXPECT(mos_internal_track_info_parse(b, sizeof b, &t));
    EXPECT(t.track_number == 0x0105);
    EXPECT(t.session_number == 0x0002);
    return 0;
}

TEST(trackinfo_fail_closed_on_hostile_buffers)
{
    uint8_t b[40];
    struct mos_track_info t;

    /* Lying Track Information Length must NOT extend the read. */
    build_ti(b, 1, 1, 4, false, 1, true, false, true, 0, 1, 1, 1, 0);
    b[0] = 0xFF; b[1] = 0xFF;
    EXPECT(!mos_internal_track_info_parse(b, 20, &t));   /* len < 32 */
    EXPECT(t.track_number == 0);

    /* Declared length shrinking below the block refuses even with a full
       buffer. */
    build_ti(b, 1, 1, 4, false, 1, true, false, true, 0, 1, 1, 1, 0);
    b[0] = 0x00; b[1] = 10;                  /* declared end = 12 < 32 */
    EXPECT(!mos_internal_track_info_parse(b, sizeof b, &t));

    /* Truncated / NULL / degenerate inputs stay in bounds and refuse. */
    EXPECT(!mos_internal_track_info_parse(b, 31, &t));
    EXPECT(!mos_internal_track_info_parse(NULL, sizeof b, &t));
    EXPECT(!mos_internal_track_info_parse(b, sizeof b, NULL));
    return 0;
}

void register_trackinfo_tests(void)
{
    RUN(trackinfo_appendable_track);
    RUN(trackinfo_complete_track);
    RUN(trackinfo_msb_fold);
    RUN(trackinfo_fail_closed_on_hostile_buffers);
}
