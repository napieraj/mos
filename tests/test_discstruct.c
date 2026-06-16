/*
 * test_discstruct.c — READ DISC STRUCTURE / BD Disc Information decode.
 * Matched real-capture fixtures plus the hostile-buffer cases the decode
 * exists to neutralize: a device-controlled Disc Structure Data Length
 * must only ever SHRINK the trusted region, never extend a read, and the
 * fixed-offset ID copies must stay in bounds under ASan regardless of
 * what the drive claims. No payload byte is ever used as an offset.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>

/* Build a one-DI-unit BD reply (116 bytes) with the given ASCII fields
   at their registered offsets. Caller may then corrupt it. */
static void build_di(uint8_t b[116], const char *disc_type,
                     const char *manuf, const char *media, char rev)
{
    memset(b, 0, 116);
    b[0] = 0x00; b[1] = 114;                 /* data length = 116 - 2 */
    b[4] = 'D';  b[5] = 'I';
    memcpy(&b[12],  disc_type, 3);           /* DI+8   */
    memcpy(&b[104], manuf,     6);           /* DI+100 */
    memcpy(&b[110], media,     3);           /* DI+106 */
    b[115] = (uint8_t)rev;                   /* DI+111 */
}

TEST(discstruct_decodes_mdisc_bd_r)
{
    /* M-DISC BD-R: registered MILLEN/MR1, disc type BDR. Values attested
       by the xorriso capture (Media product: MILLEN/MR1/0); mirrors
       fixtures/readdiscstruct_bd_di_mdisc.bin. */
    uint8_t b[116];
    build_di(b, "BDR", "MILLEN", "MR1", '0');

    struct mos_disc_id id;
    EXPECT(mos_internal_bd_disc_id_parse(b, sizeof b, &id));
    EXPECT(strcmp(id.disc_type, "BDR") == 0);
    EXPECT(strcmp(id.manufacturer, "MILLEN") == 0);
    EXPECT(strcmp(id.media_type, "MR1") == 0);
    EXPECT(strcmp(id.revision, "0") == 0);
    return 0;
}

TEST(discstruct_decodes_ordinary_bd_re)
{
    /* Not M-DISC: the field is general disc identity. CMCMAG/CN2 is the
       real manufacturer/media ID from the BDR-209D BD-RE capture; disc
       type BDW (BD-RE). Trailing-space padding is stripped. */
    uint8_t b[116];
    build_di(b, "BDW", "CMCMAG", "CN2", '1');

    struct mos_disc_id id;
    EXPECT(mos_internal_bd_disc_id_parse(b, sizeof b, &id));
    EXPECT(strcmp(id.disc_type, "BDW") == 0);
    EXPECT(strcmp(id.manufacturer, "CMCMAG") == 0);
    EXPECT(strcmp(id.media_type, "CN2") == 0);

    /* A short manufacturer ID is space-padded in the field; the strip
       leaves the bare token, not 6 chars. */
    build_di(b, "BDR", "RITEK ", "BR3", '0');
    EXPECT(mos_internal_bd_disc_id_parse(b, sizeof b, &id));
    EXPECT(strcmp(id.manufacturer, "RITEK") == 0);   /* trailing space gone */
    return 0;
}

TEST(discstruct_fail_closed_on_hostile_buffers)
{
    uint8_t b[116];
    struct mos_disc_id id;

    /* Missing 'DI' signature: the whole decode is refused, not read as
       identity from arbitrary bytes. */
    build_di(b, "BDR", "MILLEN", "MR1", '0');
    b[4] = 'X';
    EXPECT(!mos_internal_bd_disc_id_parse(b, sizeof b, &id));
    EXPECT(id.manufacturer[0] == 0);

    /* Lying Disc Structure Data Length: a buffer SHORTER than the
       identity region but declaring the full structure must refuse on
       the true `len`, never extend the read to the claim. */
    build_di(b, "BDR", "MILLEN", "MR1", '0');
    b[0] = 0xFF; b[1] = 0xFF;                 /* declared ~64KB */
    EXPECT(!mos_internal_bd_disc_id_parse(b, 64, &id));   /* len < DI region */
    EXPECT(id.disc_type[0] == 0);

    /* A declared length that SHRINKS below the identity region refuses
       even when the buffer physically holds it. */
    build_di(b, "BDR", "MILLEN", "MR1", '0');
    b[0] = 0x00; b[1] = 10;                   /* declared end = 12 */
    EXPECT(!mos_internal_bd_disc_id_parse(b, sizeof b, &id));

    /* Truncated / NULL / degenerate inputs stay in bounds and refuse. */
    EXPECT(!mos_internal_bd_disc_id_parse(b, 5, &id));
    EXPECT(!mos_internal_bd_disc_id_parse(NULL, sizeof b, &id));
    EXPECT(!mos_internal_bd_disc_id_parse(b, sizeof b, NULL));
    return 0;
}

void register_discstruct_tests(void)
{
    RUN(discstruct_decodes_mdisc_bd_r);
    RUN(discstruct_decodes_ordinary_bd_re);
    RUN(discstruct_fail_closed_on_hostile_buffers);
}
