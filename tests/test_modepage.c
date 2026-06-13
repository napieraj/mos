/*
 * test_modepage.c — MODE SENSE(10) page 0x2A (mechanical) and page 0x01
 * (error recovery) decode, plus the bounded page-walker's hostile-input
 * behaviour: device-controlled mode-data / block-descriptor / page
 * lengths must only ever SHRINK the trusted region, and the walk must
 * never loop or read out of bounds.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>

/* Build a MODE SENSE(10) reply: 8-byte header (no block descriptor) +
   the supplied raw page bytes. Returns total length. */
static size_t build_ms10(uint8_t *b, const uint8_t *page, size_t page_len)
{
    size_t total = 8 + page_len;
    memset(b, 0, total);
    b[0] = (uint8_t)((total - 2) >> 8);   /* mode data length = bytes after b[1] */
    b[1] = (uint8_t)(total - 2);
    /* b[6..7] block descriptor length = 0 */
    memcpy(&b[8], page, page_len);
    return total;
}

TEST(modepage_caps_2a)
{
    /* page 0x2A: tray loader (1<<5=0x20), eject-capable (0x08), lock
       supported (0x02) and currently locked (0x04); buffer 4096 KB. */
    uint8_t page[16] = {0};
    page[0] = 0x2A; page[1] = 14;           /* page length */
    page[6] = 0x20 | 0x08 | 0x02 | 0x04;
    page[12] = 0x10; page[13] = 0x00;       /* 0x1000 = 4096 KB */

    uint8_t b[64];
    size_t total = build_ms10(b, page, 16);

    struct mos_mode_caps m;
    EXPECT(mos_internal_mode_caps_parse(b, total, &m));
    EXPECT(m.have);
    EXPECT(m.loading_mechanism == 1);       /* tray */
    EXPECT(m.can_eject);
    EXPECT(m.lock_supported);
    EXPECT(m.locked);
    EXPECT(m.buffer_kb == 4096);
    EXPECT(strcmp(mos_loading_mechanism_name(1), "tray") == 0);
    EXPECT(strcmp(mos_loading_mechanism_name(0), "caddy") == 0);
    EXPECT(mos_loading_mechanism_name(3) == NULL);   /* reserved */
    return 0;
}

TEST(modepage_error_recovery_01)
{
    /* page 0x01: AWRE+ARRE+PER set, DCR clear; read retry count 20. */
    uint8_t page[12] = {0};
    page[0] = 0x01; page[1] = 10;
    page[2] = 0x80 | 0x40 | 0x04;           /* AWRE | ARRE | PER */
    page[3] = 20;

    uint8_t b[64];
    size_t total = build_ms10(b, page, 12);

    struct mos_error_recovery e;
    EXPECT(mos_internal_error_recovery_parse(b, total, &e));
    EXPECT(e.have);
    EXPECT(e.awre);
    EXPECT(e.arre);
    EXPECT(e.per);
    EXPECT(!e.dcr);
    EXPECT(e.read_retry_count == 20);
    return 0;
}

TEST(modepage_skips_block_descriptor_and_other_pages)
{
    /* A block descriptor (8 bytes) precedes the pages, and page 0x01
       precedes 0x2A — the walker must skip both to find 0x2A. */
    uint8_t b[96];
    memset(b, 0, sizeof b);
    /* header: data length filled below; block descriptor length = 8 */
    b[6] = 0x00; b[7] = 0x08;
    /* block descriptor at [8..15] (zeros). page 0x01 at [16..]. */
    b[16] = 0x01; b[17] = 10;               /* 12 bytes total */
    b[18] = 0x40;                           /* ARRE only */
    b[19] = 7;                              /* retry 7 */
    /* page 0x2A at [28..] */
    b[28] = 0x2A; b[29] = 14;
    b[28 + 6] = 0x40;                       /* popup loader (2<<5) */
    b[28 + 12] = 0x00; b[28 + 13] = 0x80;   /* 128 KB */
    size_t total = 28 + 16;
    b[0] = (uint8_t)((total - 2) >> 8);
    b[1] = (uint8_t)(total - 2);

    struct mos_mode_caps m;
    EXPECT(mos_internal_mode_caps_parse(b, total, &m));
    EXPECT(m.loading_mechanism == 2);       /* popup */
    EXPECT(m.buffer_kb == 128);

    struct mos_error_recovery e;
    EXPECT(mos_internal_error_recovery_parse(b, total, &e));
    EXPECT(e.arre && !e.awre);
    EXPECT(e.read_retry_count == 7);
    return 0;
}

TEST(modepage_fail_closed_on_hostile_buffers)
{
    uint8_t page[16] = {0};
    page[0] = 0x2A; page[1] = 14; page[6] = 0x20; page[12] = 0x10;
    uint8_t b[64];
    size_t total = build_ms10(b, page, 16);
    struct mos_mode_caps m;
    struct mos_error_recovery e;

    /* Lying mode data length must not extend the read. */
    b[0] = 0xFF; b[1] = 0xFF;
    EXPECT(mos_internal_mode_caps_parse(b, total, &m));   /* clamps to real len */

    /* Block descriptor length that runs off the end => page region
       empty => page not found => refuse. */
    total = build_ms10(b, page, 16);
    b[6] = 0xFF; b[7] = 0xFF;
    EXPECT(!mos_internal_mode_caps_parse(b, total, &m));

    /* A page length that claims past the trusted end is not trusted. */
    total = build_ms10(b, page, 16);
    b[9] = 0xFF;                            /* page 0x2A length huge */
    EXPECT(!mos_internal_mode_caps_parse(b, total, &m));

    /* Missing pages / short / NULL refuse cleanly (no loop, no OOB). */
    memset(b, 0, sizeof b); b[1] = 6;
    EXPECT(!mos_internal_mode_caps_parse(b, 8, &m));
    EXPECT(!mos_internal_error_recovery_parse(b, 8, &e));
    EXPECT(!mos_internal_mode_caps_parse(NULL, 64, &m));
    EXPECT(!mos_internal_mode_caps_parse(b, 64, NULL));
    EXPECT(!mos_internal_error_recovery_parse(b, 7, &e));
    return 0;
}

void register_modepage_tests(void)
{
    RUN(modepage_caps_2a);
    RUN(modepage_error_recovery_01);
    RUN(modepage_skips_block_descriptor_and_other_pages);
    RUN(modepage_fail_closed_on_hostile_buffers);
}
