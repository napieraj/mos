/*
 * test_vpd80.c — INQUIRY VPD page 0x80 (Unit Serial Number) decode, plus
 * the parser's hostile-input behaviour: the page-code echo must gate, and the
 * serial is a durable identity key that is complete-or-unavailable — anything
 * that cannot be represented WHOLE as a C string is REFUSED, never emitted as a
 * prefix. That covers an under-delivered reply (PAGE LENGTH claims more than
 * the trusted span the shell passes), a serial longer than the output buffer,
 * and a serial with an interior NUL (which would sever the C string). A prefix
 * is an indistinguishable, wrong key two drives could share.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"

#include <string.h>

/* Build a VPD page 0x80 reply: 4-byte header + serial bytes. page_len is the
   value written to byte[3] (the device-claimed length) — equal to serial_len
   for a well-formed reply, set independently to forge a hostile one. Returns
   total bytes written. */
static size_t build_vpd80(uint8_t *b, uint8_t page_code, uint8_t page_len,
                          const uint8_t *serial, size_t serial_len)
{
    b[0] = 0x05;            /* peripheral: CD/DVD device type (ignored) */
    b[1] = page_code;
    b[2] = 0x00;            /* reserved */
    b[3] = page_len;
    if (serial_len) memcpy(&b[4], serial, serial_len);
    return 4 + serial_len;
}

TEST(vpd80_serial_basic)
{
    const uint8_t s[] = "M63IBOA5100";
    uint8_t b[64];
    size_t total = build_vpd80(b, 0x80, 11, s, 11);

    char out[64] = {0};
    EXPECT(mos_internal_vpd80_serial_parse(b, total, out, sizeof out));
    EXPECT_STREQ(out, "M63IBOA5100");
    return 0;
}

TEST(vpd80_trims_trailing_space_and_nul)
{
    /* Left-justified, space- then NUL-padded on the wire; interior space
       stays (it is data). */
    const uint8_t s[] = { 'A','B',' ','C',' ',' ',0x00,0x00 };
    uint8_t b[64];
    size_t total = build_vpd80(b, 0x80, 8, s, 8);

    char out[64] = {0};
    EXPECT(mos_internal_vpd80_serial_parse(b, total, out, sizeof out));
    EXPECT_STREQ(out, "AB C");
    return 0;
}

TEST(vpd80_under_delivery_refused)
{
    /* PAGE LENGTH claims more than the bytes actually delivered (a USB bridge
       under-filling the reply) — the parser holds only a PREFIX of unknown
       length. For a durable cached identity key a silent prefix can collide
       with or misidentify a drive, so the parse REFUSES (false). It still
       bounds the read: refusal precedes any serial byte, so a hostile over-
       long PAGE LENGTH cannot read past the trusted span. */
    const uint8_t s[] = "SER123";
    uint8_t b[64];
    (void)build_vpd80(b, 0x80, 200, s, 6);   /* byte[3]=200, but only 6 present */

    char out[64];
    out[0] = 'x';
    /* Hand the parser only the 10 bytes that actually arrived (4 hdr + 6). */
    EXPECT(!mos_internal_vpd80_serial_parse(b, 10, out, sizeof out));
    EXPECT(out[0] == 0);                      /* NUL-terminated on refusal */
    return 0;
}

TEST(vpd80_overlong_serial_refused)
{
    /* A serial COMPLETE on the wire but longer than the output buffer cannot
       be held whole, so it is REFUSED (false), serial left null — never a
       prefix (an indistinguishable, wrong cache key) and never an overflow.
       Two serials sharing a prefix but differing past out_cap must not both
       decode to the same truncated key — refusal makes that impossible. */
    uint8_t s[40];
    memset(s, 'X', sizeof s);
    uint8_t b[64];
    size_t total = build_vpd80(b, 0x80, (uint8_t)sizeof s, s, sizeof s);

    char out[8];                              /* far too small for 40 bytes */
    out[0] = 'z';
    EXPECT(!mos_internal_vpd80_serial_parse(b, total, out, sizeof out));
    EXPECT(out[0] == 0);                      /* NUL-terminated on refusal */

    /* A serial that fits exactly (out_cap-1 bytes) still decodes whole. */
    uint8_t s7[7];
    memset(s7, 'Y', sizeof s7);
    total = build_vpd80(b, 0x80, (uint8_t)sizeof s7, s7, sizeof s7);
    EXPECT(mos_internal_vpd80_serial_parse(b, total, out, sizeof out));
    EXPECT_STREQ(out, "YYYYYYY");             /* 7 chars exactly fills out[8] */
    return 0;
}

TEST(vpd80_interior_nul_refused)
{
    /* An interior NUL (not trailing padding) would sever the C string
       invisibly at the NUL — two serials "ABC\0X" and "ABC\0Y" would both
       decode to "ABC", a collision. The parser refuses such a reply. */
    const uint8_t s[] = { 'A','B','C',0x00,'X' };
    uint8_t b[64];
    size_t total = build_vpd80(b, 0x80, (uint8_t)sizeof s, s, sizeof s);

    char out[64];
    out[0] = 'z';
    EXPECT(!mos_internal_vpd80_serial_parse(b, total, out, sizeof out));
    EXPECT(out[0] == 0);                      /* NUL-terminated on refusal */
    return 0;
}

TEST(vpd80_fail_closed)
{
    const uint8_t s[] = "SER123";
    uint8_t b[64];
    char out[64];

    /* Wrong page echoed (drive answered standard INQUIRY or another page). */
    size_t total = build_vpd80(b, 0x00, 6, s, 6);
    EXPECT(!mos_internal_vpd80_serial_parse(b, total, out, sizeof out));
    EXPECT(out[0] == 0);                      /* NUL-terminated on false */

    /* All-spaces serial: page present, nothing programmed → null, not "". */
    const uint8_t blank[] = { ' ',' ',' ',' ' };
    total = build_vpd80(b, 0x80, 4, blank, 4);
    EXPECT(!mos_internal_vpd80_serial_parse(b, total, out, sizeof out));

    /* Zero-length serial (page_len 0). */
    total = build_vpd80(b, 0x80, 0, NULL, 0);
    EXPECT(!mos_internal_vpd80_serial_parse(b, total, out, sizeof out));

    /* Buffer shorter than the 4-byte VPD header. */
    total = build_vpd80(b, 0x80, 6, s, 6);
    EXPECT(!mos_internal_vpd80_serial_parse(b, 3, out, sizeof out));

    /* NULL / zero-cap out, NULL buf — no crash, clean false. */
    EXPECT(!mos_internal_vpd80_serial_parse(b, total, NULL, sizeof out));
    EXPECT(!mos_internal_vpd80_serial_parse(b, total, out, 0));
    EXPECT(!mos_internal_vpd80_serial_parse(NULL, total, out, sizeof out));
    return 0;
}

void register_vpd80_tests(void)
{
    RUN(vpd80_serial_basic);
    RUN(vpd80_trims_trailing_space_and_nul);
    RUN(vpd80_under_delivery_refused);
    RUN(vpd80_overlong_serial_refused);
    RUN(vpd80_interior_nul_refused);
    RUN(vpd80_fail_closed);
}
