/*
 * mos_cdtext.c — pure, bounds-safe decode of the disc-level (album)
 * Title and Performer from a READ TOC/PMA/ATIP format 0101b (CD-TEXT)
 * reply.
 *
 * No IOKit. The IOKit shell issues READ TOC/PMA/ATIP with format 0101b
 * via the non-exclusive ReadTableOfContents convenience method (the same
 * wrapper the format-0000b TOC uses) into a fixed, zero-initialized
 * buffer and hands that buffer plus its size here. Every length and text
 * byte is disc-reported and therefore hostile; this file keeps the
 * declared CD-TEXT Data Length from steering a read outside [buf,
 * buf+len), and copies text bytes verbatim into fixed buffers (the CLI
 * layer escapes them at emit, same as the volume name and INQUIRY
 * identity). No payload byte is ever used as an offset or length.
 *
 * SCOPE — disc-level identity only (the "which album is in the drive"
 * disambiguator, parallel to the mounted volume name for data discs).
 * This increment decodes the track-0 (album) Title and Performer of the
 * FIRST language block (block 0) in single-byte charset. Deliberately
 * NOT decoded here, deferred with named falsifiers (design doc
 * 2026-06-14 addendum): per-track titles; the other field types
 * (songwriter/composer/arranger/message/genre/ISRC/UPC/disc-id);
 * additional language blocks (1..7); and double-byte (DBCC) text
 * (MS-JIS / 16-bit) — a DBCC album field reads as absent rather than
 * being mis-decoded as Latin-1. CD-TEXT is BEST-EFFORT DISPLAY TEXT,
 * not a fail-closed fingerprint: audio-CD dedup keys ride on the TOC
 * (mos_internal_toc_parse), which is the fail-closed identity primitive.
 *
 * Pack layout (READ TOC format 0101b, MMC-3 §6.27 / Red Book CD-TEXT;
 * cross-verified against libcdio lib/driver/cdtext.c):
 *   [0..1] CD-TEXT Data Length (BE) — bytes available AFTER this field;
 *          the reply occupies 2 + value bytes.
 *   [2..3] reserved
 *   [4..]  a sequence of 18-byte CD-TEXT packs:
 *            [0]      Pack Type (0x80 Title, 0x81 Performer, ...)
 *            [1]      Track Number (bits 6:0; bit 7 reserved/extension)
 *            [2]      Sequence Number
 *            [3]      bit7 = double-byte (DBCC); bits 6:4 = Block Number
 *                     (language, 0..7); bits 3:0 = Character Position
 *            [4..15]  12 text bytes (NUL separates per-track strings)
 *            [16..17] CRC (present; not verified here, as in libcdio)
 *
 * No-OOB / termination gated headless under ASan/UBSan by
 * tests/test_cdtext.c and the fuzz_pure CD-TEXT phase.
 */

#include "mos_pure.h"

#include <string.h>

#define CDTEXT_HDR        4u    /* 2-byte data length + 2 reserved        */
#define CDTEXT_PACK_LEN  18u
#define CDTEXT_TEXT_OFF   4u    /* text bytes within a pack: [4..15]      */
#define CDTEXT_TEXT_LEN  12u

#define CDTEXT_PACK_TITLE     0x80u
#define CDTEXT_PACK_PERFORMER 0x81u

/* Reconstruct the album-level (track 0), block-0 string for one pack
   type into `dst` (cap bytes, ALWAYS NUL-terminated). Returns true when
   a track-0 string for the type was found (possibly empty after a lone
   NUL). Begins at the first block-0 pack of the type whose track number
   is 0, then consumes its text bytes — and those of following block-0
   packs of the same type — until the first NUL (end of the album
   string) or the clamped data ends. Single-byte only: a double-byte
   (DBCC) starting pack returns false (album field absent, not
   mis-decoded). */
static bool cdtext_collect(const uint8_t *buf, size_t span,
                           uint8_t want_type, char *dst, size_t cap)
{
    dst[0] = '\0';
    bool   started = false;
    size_t n       = 0;
    for (size_t p = CDTEXT_HDR; p + CDTEXT_PACK_LEN <= span;
         p += CDTEXT_PACK_LEN) {
        if (buf[p] != want_type) continue;            /* wrong pack type   */
        uint8_t b3    = buf[p + 3];
        uint8_t block = (uint8_t)((b3 >> 4) & 0x07);
        bool    dbcc  = (b3 & 0x80) != 0;
        if (block != 0) continue;                     /* first block only  */

        if (!started) {
            if ((buf[p + 1] & 0x7Fu) != 0) return false; /* no album string */
            if (dbcc)                       return false; /* double-byte     */
            started = true;
        } else if (dbcc) {
            break;            /* charset flip mid-string: keep the prefix   */
        }

        for (size_t j = 0; j < CDTEXT_TEXT_LEN; j++) {
            uint8_t c = buf[p + CDTEXT_TEXT_OFF + j];
            if (c == 0x00) { dst[n] = '\0'; return true; }   /* string end  */
            if (n + 1 < cap) dst[n++] = (char)c;             /* else truncate */
        }
    }
    dst[n] = '\0';            /* ran to end of clamped data without a NUL    */
    return started;
}

bool mos_internal_cdtext_parse(const uint8_t *buf, size_t len,
                               struct mos_cdtext *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!buf || len < CDTEXT_HDR) return false;

    /* Device claim: CD-TEXT Data Length counts bytes AFTER its own two.
       64-bit total, clamped by the trusted length (O-4) — a lying length
       can only shrink the span, never extend a read. */
    uint64_t claimed = 2u + (uint64_t)(((uint32_t)buf[0] << 8) | buf[1]);
    size_t   span    = mos_internal_trusted_len(len, len, claimed);
    if (span < CDTEXT_HDR + CDTEXT_PACK_LEN) return false;  /* no whole pack */

    bool t = cdtext_collect(buf, span, CDTEXT_PACK_TITLE,
                            out->title, sizeof out->title);
    bool p = cdtext_collect(buf, span, CDTEXT_PACK_PERFORMER,
                            out->performer, sizeof out->performer);

    /* "have" is the useful-identity gate: an empty album field (lone NUL,
       or absent) is not identity. Return false → the adapter reports no
       CD-TEXT (null), same convention as the other media reads. */
    out->have = (t && out->title[0]) || (p && out->performer[0]);
    return out->have;
}
