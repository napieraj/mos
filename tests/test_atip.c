/*
 * test_atip.c — READ TOC/PMA/ATIP Format=0100b decode (mos_atip.c).
 *
 * Spec-built reply (MMC-6 r02g §6.25, Table 488) — no in-repo hardware
 * capture yet, so a real `mos probe --capture` ATIP reply is the standing
 * falsifier per the hardware-role ADR. Mirrors fixtures/readtoc_atip_cdr.bin.
 * Coverage: a well-formed CD-R descriptor + the fail-closed bounds (short
 * reply, lying length, NULL args).
 */

#include "test_harness.h"
#include "../src/mos_pure.h"
#include <string.h>

/* A 28-byte ATIP Format=0100b reply: data length 26, URU set, CD-R
   (disc_type 0) sub-type 2, reference speed 1, lead-in 97:24:01 (a
   Taiyo-Yuden-shaped MID), last-possible lead-out 79:59:74 (a 74-min disc). */
static const uint8_t atip_cdr[] = {
    0x00, 0x1A,             /* ATIP Data Length = 26 (total 28)        */
    0x00, 0x00,             /* Reserved                                */
    0x51,                   /* [4] writing power 5, reference speed 1  */
    0x40,                   /* [5] URU = 1                             */
    0x90,                   /* [6] bit7=1, disc_type 0, sub_type 2     */
    0x00,                   /* [7] Reserved                            */
    97,   24,   1,          /* [8..10]  lead-in  M:S:F (the MID)       */
    0x00,                   /* [11] Reserved                           */
    79,   59,   74,         /* [12..14] last-possible lead-out M:S:F   */
    0x00,                   /* [15] Reserved                           */
    0,0,0, 0, 0,0,0, 0, 0,0,0, 0,   /* [16..27] A1/A2/A3/S4 (not decoded) */
};

TEST(atip_decodes_cdr_descriptor)
{
    mos_atip a;
    EXPECT(mos_internal_atip_parse(atip_cdr, sizeof atip_cdr, &a));
    EXPECT(a.unrestricted_use);
    EXPECT_EQ(a.disc_type, 0u);
    EXPECT_EQ(a.disc_sub_type, 2u);
    EXPECT_EQ(a.reference_speed, 1u);
    EXPECT_EQ(a.lead_in_min, 97u);
    EXPECT_EQ(a.lead_in_sec, 24u);
    EXPECT_EQ(a.lead_in_frame, 1u);
    EXPECT_EQ(a.lead_out_min, 79u);
    EXPECT_EQ(a.lead_out_sec, 59u);
    EXPECT_EQ(a.lead_out_frame, 74u);
    return 0;
}

TEST(atip_fail_closed_on_short_and_hostile)
{
    mos_atip a;

    /* Too short to carry the descriptor through the lead-out (byte 14). */
    EXPECT(!mos_internal_atip_parse(atip_cdr, 14, &a));
    /* The zeroing contract holds on failure. */
    EXPECT_EQ(a.lead_out_min, 0u);

    /* A lying SHORT data length shrinks the trusted region below 15 → fail
       closed even though the buffer itself is long enough. */
    uint8_t lying[28];
    memcpy(lying, atip_cdr, sizeof lying);
    lying[0] = 0x00; lying[1] = 0x04;     /* claims only 4 bytes follow → end 6 */
    EXPECT(!mos_internal_atip_parse(lying, sizeof lying, &a));

    /* A lying LARGE data length cannot extend past the real buffer. */
    uint8_t big[28];
    memcpy(big, atip_cdr, sizeof big);
    big[0] = 0xFF; big[1] = 0xFF;         /* claims 65535 → clamped to len     */
    EXPECT(mos_internal_atip_parse(big, sizeof big, &a));   /* still valid      */

    /* NULL args are safe. */
    EXPECT(!mos_internal_atip_parse(NULL, sizeof atip_cdr, &a));
    EXPECT(!mos_internal_atip_parse(atip_cdr, sizeof atip_cdr, NULL));
    return 0;
}

void register_atip_tests(void);
void register_atip_tests(void)
{
    RUN(atip_decodes_cdr_descriptor);
    RUN(atip_fail_closed_on_short_and_hostile);
}
