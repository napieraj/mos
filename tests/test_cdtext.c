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

/* Pack a DENSE NUL-separated text stream (`stream`, `len` bytes — the
   real CD-TEXT wire form: strings concatenated and chopped at 12-byte
   boundaries, NOT one padded pack per string) into packs of `type`,
   block 0. The first pack carries `start_track` in its Track Number
   field; continuation packs' field is left 0 (the decoder seeds from the
   first pack and ignores the rest). Returns the new offset. */
static size_t put_stream(uint8_t *b, size_t off, uint8_t type,
                         uint8_t start_track, const uint8_t *stream, size_t len)
{
    size_t pos = 0;
    int    first = 1;
    while (pos < len) {
        memset(&b[off], 0, 18);
        b[off + 0] = type;
        b[off + 1] = (uint8_t)(first ? start_track : 0);
        b[off + 3] = 0;                          /* block 0, single-byte */
        size_t chunk = (len - pos < 12) ? (len - pos) : 12;
        memcpy(&b[off + 4], &stream[pos], chunk);
        pos += chunk;
        off += 18;
        first = 0;
    }
    return off;
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

TEST(cdtext_decodes_per_track_titles)
{
    /* Dense Title stream "Album\0Song A\0Song B\0", track 0 album then
       tracks 1,2. Spans packs at the 12-byte boundary, with trailing
       zero padding in the last pack (which must NOT inflate the count). */
    static const uint8_t stream[] = {
        'A','l','b','u','m', 0,
        'S','o','n','g',' ','A', 0,
        'S','o','n','g',' ','B', 0,
    };
    uint8_t b[4 + 3 * 18] = {0};
    size_t off = put_stream(b, 4, 0x80, 0, stream, sizeof stream);
    finalize(b, off - 4);

    struct mos_cdtext c;
    EXPECT(mos_internal_cdtext_parse(b, off, &c));
    EXPECT(strcmp(c.title, "Album") == 0);
    EXPECT_EQ(mos_cdtext_track_count(&c), 2);
    EXPECT(strcmp(mos_cdtext_track_title(&c, 1), "Song A") == 0);
    EXPECT(strcmp(mos_cdtext_track_title(&c, 2), "Song B") == 0);
    EXPECT(mos_cdtext_track_title(&c, 3) == NULL);   /* trailing pad, not a track */
    EXPECT(mos_cdtext_track_title(&c, 0) == NULL);   /* 1-based */
    return 0;
}

TEST(cdtext_per_track_titles_without_album)
{
    /* No album title: the stream starts at track 1 (first pack's Track
       Number field = 1). Album reads absent; per-track titles decode. */
    static const uint8_t stream[] = {
        'F','i','r','s','t', 0,
        'S','e','c','o','n','d', 0,
    };
    uint8_t b[4 + 2 * 18] = {0};
    size_t off = put_stream(b, 4, 0x80, 1, stream, sizeof stream);
    finalize(b, off - 4);

    struct mos_cdtext c;
    EXPECT(mos_internal_cdtext_parse(b, off, &c));   /* have via track titles */
    EXPECT(c.title[0] == 0);                          /* no album title */
    EXPECT_EQ(mos_cdtext_track_count(&c), 2);
    EXPECT(strcmp(mos_cdtext_track_title(&c, 1), "First") == 0);
    EXPECT(strcmp(mos_cdtext_track_title(&c, 2), "Second") == 0);
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
    RUN(cdtext_decodes_per_track_titles);
    RUN(cdtext_per_track_titles_without_album);
    RUN(cdtext_double_byte_field_not_decoded);
    RUN(cdtext_non_zero_block_ignored);
    RUN(cdtext_fail_closed_on_hostile_buffers);
}
