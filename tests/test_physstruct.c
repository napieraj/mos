/*
 * test_physstruct.c — READ DISC STRUCTURE Physical Format Information
 * (format 0x00) and Copyright Management Information (format 0x01) decode
 * for the DVD / HD-DVD media-type family. Wire fixtures follow the
 * kernel's documented byte parse; the hostile cases pin the no-OOB
 * invariant the decode exists to hold — a device-controlled Disc
 * Structure Data Length must only SHRINK the trusted region, never
 * extend a read.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>

/* Build a Physical Format Information reply (22 bytes: 4-byte header +
   through base[16]) with the given fields at their wire offsets. */
static void build_phys(uint8_t b[22], uint8_t book_type, uint8_t part_version,
                       uint8_t disc_size, uint8_t max_rate, uint8_t layer_type,
                       uint8_t track_path, uint8_t num_layers,
                       uint8_t linear_density, uint8_t track_density,
                       uint32_t start, uint32_t end, uint32_t end_l0,
                       bool bca)
{
    memset(b, 0, 22);
    b[0] = 0x00; b[1] = 20;                       /* data length = 22 - 2 */
    uint8_t *base = &b[4];
    base[0] = (uint8_t)((book_type << 4) | (part_version & 0x0f));
    base[1] = (uint8_t)((disc_size << 4) | (max_rate & 0x0f));
    base[2] = (uint8_t)((((num_layers - 1) & 0x03) << 5) |
                        ((track_path & 0x01) << 4) | (layer_type & 0x0f));
    base[3] = (uint8_t)((linear_density << 4) | (track_density & 0x0f));
    base[5] = (uint8_t)(start >> 16); base[6] = (uint8_t)(start >> 8);
    base[7] = (uint8_t)start;
    base[9] = (uint8_t)(end >> 16); base[10] = (uint8_t)(end >> 8);
    base[11] = (uint8_t)end;
    base[13] = (uint8_t)(end_l0 >> 16); base[14] = (uint8_t)(end_l0 >> 8);
    base[15] = (uint8_t)end_l0;
    base[16] = (uint8_t)(bca ? 0x80 : 0x00);
}

static void build_copyright(uint8_t b[6], uint8_t cpst, uint8_t rmi)
{
    memset(b, 0, 6);
    b[0] = 0x00; b[1] = 4;                        /* data length = 6 - 2 */
    b[4] = cpst;
    b[5] = rmi;
}

TEST(physstruct_single_layer_dvdrom)
{
    /* DVD-ROM, part version 1, 120mm, PTP single layer, data area
       0x030000.. (the canonical DVD-ROM start PSN), no BCA. */
    uint8_t b[22];
    build_phys(b, 0x0 /*dvd_rom*/, 1, 0 /*120mm*/, 0x0f, 1 /*layer*/,
               0 /*ptp*/, 1 /*layers*/, 0, 0, 0x030000, 0x04E000, 0, false);

    struct mos_physical_structure d;
    EXPECT(mos_internal_physical_format_parse(b, sizeof b, &d));
    EXPECT(d.have_physical);
    EXPECT(d.book_type == 0x0);
    EXPECT(d.part_version == 1);
    EXPECT(d.disc_size == 0);
    EXPECT(d.track_path == 0);
    EXPECT(d.num_layers == 1);
    EXPECT(d.start_sector == 0x030000);
    EXPECT(d.end_sector == 0x04E000);
    EXPECT(d.end_sector_l0 == 0);
    EXPECT(!d.bca);
    /* token mapping */
    EXPECT(strcmp(mos_book_type_name(d.book_type), "dvd_rom") == 0);
    EXPECT(strcmp(mos_track_path_name(d.track_path), "ptp") == 0);
    return 0;
}

TEST(physstruct_dual_layer_otp)
{
    /* DVD+R DL, dual layer OTP: the layer break is end_sector_l0. */
    uint8_t b[22];
    build_phys(b, 0xE /*dvd_plus_r_dl*/, 1, 0, 0x0f, 1,
               1 /*otp*/, 2 /*layers*/, 0, 0, 0x030000, 0x3AA000,
               0x1F23FF, true);

    struct mos_physical_structure d;
    EXPECT(mos_internal_physical_format_parse(b, sizeof b, &d));
    EXPECT(d.num_layers == 2);
    EXPECT(d.track_path == 1);
    EXPECT(d.end_sector_l0 == 0x1F23FF);   /* the layer break */
    EXPECT(d.bca);
    EXPECT(strcmp(mos_book_type_name(d.book_type), "dvd_plus_r_dl") == 0);
    EXPECT(strcmp(mos_track_path_name(d.track_path), "otp") == 0);
    return 0;
}

TEST(physstruct_hd_dvd_book_type)
{
    /* HD DVD-ROM (book type 0x4): the same media-type-0 structure
       carries HD-DVD book types — why this decode is "physical
       structure", not "dvd". */
    uint8_t b[22];
    build_phys(b, 0x4 /*hd_dvd_rom*/, 1, 0, 0x0f, 1, 0, 1, 0, 0,
               0x030000, 0x04E000, 0, false);

    struct mos_physical_structure d;
    EXPECT(mos_internal_physical_format_parse(b, sizeof b, &d));
    EXPECT(d.book_type == 0x4);
    EXPECT(strcmp(mos_book_type_name(d.book_type), "hd_dvd_rom") == 0);
    return 0;
}

TEST(physstruct_copyright_css_region1)
{
    /* CSS-protected, region-1-only (RMI bit clear = playable, DVD
       convention). We surface the raw mask; classification is the
       consumer's. */
    uint8_t b[6];
    build_copyright(b, 0x01 /*CSS/CPPM*/, 0xFE /*region 1 playable*/);

    struct mos_physical_structure d = {0};
    EXPECT(mos_internal_copyright_mgmt_parse(b, sizeof b, &d));
    EXPECT(d.have_copyright);
    EXPECT(d.protection == 0x01);
    EXPECT(d.region == 0xFE);
    EXPECT(strcmp(mos_protection_name(d.protection), "css_cppm") == 0);
    EXPECT(strcmp(mos_protection_name(0x00), "none") == 0);
    EXPECT(strcmp(mos_protection_name(0x02), "cprm") == 0);
    EXPECT(mos_protection_name(0x7f) == NULL);    /* reserved -> NULL */
    EXPECT(mos_book_type_name(0x7) == NULL);      /* reserved -> NULL */
    return 0;
}

TEST(physstruct_fail_closed_on_hostile_buffers)
{
    uint8_t b[22];
    struct mos_physical_structure d;

    /* Lying Disc Structure Data Length must NOT extend the read: a
       buffer shorter than the physical region with a huge declared
       length is refused on the true `len`. */
    build_phys(b, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0x030000, 0x04E000, 0, false);
    b[0] = 0xFF; b[1] = 0xFF;
    EXPECT(!mos_internal_physical_format_parse(b, 12, &d));   /* len < region */
    EXPECT(!d.have_physical);

    /* A declared length that SHRINKS below the region refuses even when
       the buffer physically holds it. */
    build_phys(b, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0x030000, 0x04E000, 0, false);
    b[0] = 0x00; b[1] = 6;                  /* declared end = 8 < 21 */
    EXPECT(!mos_internal_physical_format_parse(b, sizeof b, &d));

    /* Truncated / NULL / degenerate inputs stay in bounds and refuse. */
    EXPECT(!mos_internal_physical_format_parse(b, 10, &d));
    EXPECT(!mos_internal_physical_format_parse(NULL, sizeof b, &d));
    EXPECT(!mos_internal_physical_format_parse(b, sizeof b, NULL));

    uint8_t c[6];
    build_copyright(c, 0x01, 0xFE);
    EXPECT(!mos_internal_copyright_mgmt_parse(c, 5, &d));   /* short */
    EXPECT(!mos_internal_copyright_mgmt_parse(NULL, sizeof c, &d));
    EXPECT(!mos_internal_copyright_mgmt_parse(c, sizeof c, NULL));
    /* declared length shrinking below the RMI byte refuses */
    build_copyright(c, 0x01, 0xFE);
    c[0] = 0x00; c[1] = 1;                  /* declared end = 3 < 6 */
    EXPECT(!mos_internal_copyright_mgmt_parse(c, sizeof c, &d));
    return 0;
}

void register_physstruct_tests(void)
{
    RUN(physstruct_single_layer_dvdrom);
    RUN(physstruct_dual_layer_otp);
    RUN(physstruct_hd_dvd_book_type);
    RUN(physstruct_copyright_css_region1);
    RUN(physstruct_fail_closed_on_hostile_buffers);
}
