/*
 * test_cdtext.c — READ TOC/PMA/ATIP format 0101b (CD-TEXT) decode.
 * Spec-derived packs (no in-repo capture yet — a real CD-TEXT reply is a
 * falsifier per the hardware ADR) plus the hostile-buffer cases the
 * decode exists to neutralize: a device-controlled CD-TEXT Data Length
 * must only ever SHRINK the trusted span, never extend a read, and the
 * fixed-stride pack walk must stay in bounds whatever the drive claims.
 * No payload byte is ever used as an offset.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <string.h>

/* Append one 18-byte CD-TEXT pack at `off`, copying up to 12 text bytes
   (a NUL inside `text` and the zero-fill beyond it both terminate the
   per-track string). Returns the new offset. */
static size_t put_pack(uint8_t *b, size_t off, uint8_t type, uint8_t track,
                       uint8_t seq, uint8_t b3, const char *text)
{
    memset(&b[off], 0, 18);
    b[off + 0] = type;
    b[off + 1] = track;
    b[off + 2] = seq;
    b[off + 3] = b3;
    for (size_t i = 0; i < 12 && text[i]; i++)
        b[off + 4 + i] = (uint8_t)text[i];
    return off + 18;
}

/* Write the 4-byte CD-TEXT header for `packs_bytes` of pack data: the
   declared Data Length counts everything after its own two bytes (the 2
   reserved + the packs). */
static void finalize(uint8_t *b, size_t packs_bytes)
{
    uint16_t dl = (uint16_t)(2u + packs_bytes);
    b[0] = (uint8_t)(dl >> 8);
    b[1] = (uint8_t)(dl & 0xFF);
    b[2] = b[3] = 0;
}

TEST(cdtext_decodes_album_title_and_performer)
{
    /* "Blue Train" is < 12 bytes, so its terminating NUL lands inside
       pack 0; the walk stops there and never folds in the track-1 pack. */
    uint8_t b[4 + 4 * 18] = {0};
    size_t off = 4;
    off = put_pack(b, off, 0x80, 0, 0, 0x00, "Blue Train");
    off = put_pack(b, off, 0x80, 1, 1, 0x00, "Moments");      /* track 1 — ignored */
    off = put_pack(b, off, 0x81, 0, 2, 0x00, "John");
    finalize(b, off - 4);

    struct mos_cdtext c;
    EXPECT(mos_internal_cdtext_parse(b, off, &c));
    EXPECT(c.have);
    EXPECT(strcmp(c.title, "Blue Train") == 0);
    EXPECT(strcmp(c.performer, "John") == 0);
    return 0;
}

TEST(cdtext_title_spans_two_packs)
{
    /* A title longer than 12 bytes runs into the next same-type block-0
       pack; the album string ends at the first NUL across the run. */
    uint8_t b[4 + 3 * 18] = {0};
    size_t off = 4;
    off = put_pack(b, off, 0x80, 0, 0, 0x00, "A Love Supr");  /* 11 bytes, no NUL... */
    /* make the first pack a full 12 with no terminator, continuation holds the rest */
    memcpy(&b[4 + 4], "A Love Supre", 12);                    /* 12 bytes, no NUL */
    off = put_pack(b, off, 0x80, 0, 1, 0x00, "me");           /* "me\0" */
    finalize(b, off - 4);

    struct mos_cdtext c;
    EXPECT(mos_internal_cdtext_parse(b, off, &c));
    EXPECT(strcmp(c.title, "A Love Supreme") == 0);
    EXPECT(mos_cdtext_title(&c) != NULL);
    EXPECT(mos_cdtext_performer(&c) == NULL);   /* "" reads as NULL */
    return 0;
}

TEST(cdtext_no_album_level_field_reads_absent)
{
    /* Title pack present but its first block-0 pack is track 1 (no
       album title) — the album field is absent, not the track-1 text. */
    uint8_t b[4 + 1 * 18] = {0};
    size_t off = put_pack(b, 4, 0x80, 1, 0, 0x00, "Track One");
    finalize(b, off - 4);

    struct mos_cdtext c;
    EXPECT(!mos_internal_cdtext_parse(b, off, &c));
    EXPECT(!c.have);
    EXPECT(c.title[0] == 0);
    return 0;
}

TEST(cdtext_double_byte_field_not_decoded)
{
    /* A DBCC (double-byte) album title reads as absent — never
       mis-decoded as single-byte; performer (single-byte) still decodes. */
    uint8_t b[4 + 2 * 18] = {0};
    size_t off = 4;
    off = put_pack(b, off, 0x80, 0, 0, 0x80, "??");          /* DBCC set */
    off = put_pack(b, off, 0x81, 0, 1, 0x00, "Single Byte");
    finalize(b, off - 4);

    struct mos_cdtext c;
    EXPECT(mos_internal_cdtext_parse(b, off, &c));
    EXPECT(c.title[0] == 0);                                  /* DBCC -> absent */
    EXPECT(strcmp(c.performer, "Single Byte") == 0);
    return 0;
}

TEST(cdtext_non_zero_block_ignored)
{
    /* Only the first language block (block 0) is surfaced; a block-1
       title must not be read as the album title. */
    uint8_t b[4 + 1 * 18] = {0};
    size_t off = put_pack(b, 4, 0x80, 0, 0, 0x10, "Block One"); /* block 1 */
    finalize(b, off - 4);

    struct mos_cdtext c;
    EXPECT(!mos_internal_cdtext_parse(b, off, &c));
    EXPECT(c.title[0] == 0);
    return 0;
}

TEST(cdtext_fail_closed_on_hostile_buffers)
{
    uint8_t b[4 + 2 * 18] = {0};
    size_t off = 4;
    off = put_pack(b, off, 0x80, 0, 0, 0x00, "Title");
    off = put_pack(b, off, 0x81, 0, 1, 0x00, "Artist");
    finalize(b, off - 4);
    struct mos_cdtext c;

    /* A lying CD-TEXT Data Length claiming far more than the buffer holds
       must NOT extend the walk past the real `len`. */
    b[0] = 0xFF; b[1] = 0xFF;                  /* declared ~64KB */
    EXPECT(mos_internal_cdtext_parse(b, off, &c)); /* clamps to off, still decodes */
    EXPECT(strcmp(c.title, "Title") == 0);

    /* A declared length that SHRINKS below the first pack refuses. */
    finalize(b, off - 4);
    b[0] = 0x00; b[1] = 0x02;                  /* claimed end = 4: no pack */
    EXPECT(!mos_internal_cdtext_parse(b, off, &c));

    /* Truncated / NULL / degenerate inputs stay in bounds and refuse. */
    EXPECT(!mos_internal_cdtext_parse(b, 3, &c));
    EXPECT(!mos_internal_cdtext_parse(b, 4, &c));      /* header only, no pack */
    EXPECT(!mos_internal_cdtext_parse(NULL, off, &c));
    EXPECT(!mos_internal_cdtext_parse(b, off, NULL));
    return 0;
}

void register_cdtext_tests(void)
{
    RUN(cdtext_decodes_album_title_and_performer);
    RUN(cdtext_title_spans_two_packs);
    RUN(cdtext_no_album_level_field_reads_absent);
    RUN(cdtext_double_byte_field_not_decoded);
    RUN(cdtext_non_zero_block_ignored);
    RUN(cdtext_fail_closed_on_hostile_buffers);
}
