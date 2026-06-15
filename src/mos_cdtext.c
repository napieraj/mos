/*
 * mos_cdtext.c — pure, bounds-safe decode of the disc-level (album)
 * Title and Performer from a READ TOC/PMA/ATIP format 0101b (CD-TEXT)
 * reply.
 *
 * No IOKit. The IOKit shell issues READ TOC/PMA/ATIP with format 0101b
 * via the non-exclusive ReadTableOfContents convenience method into a
 * fixed, zero-initialized buffer and hands it plus its size here. Every
 * length and text byte is disc-reported and therefore hostile; this file
 * keeps the declared CD-TEXT Data Length from steering a read outside
 * [buf, buf+len) and copies text bytes verbatim into fixed buffers (the
 * CLI layer escapes them at emit, same as the volume name and INQUIRY
 * identity). No payload byte is ever used as an offset or length.
 *
 * SCOPE — the album Title/Performer (the "which album is in the drive"
 * disambiguator, parallel to the mounted volume name) plus the per-track
 * TITLES and PERFORMERS, all from the FIRST language block (block 0) in
 * single-byte charset. The field types and language blocks deliberately
 * NOT decoded are listed in SPEC.md; a double-byte (DBCC) field reads as
 * absent rather than mis-decoded as Latin-1. CD-TEXT is BEST-EFFORT
 * DISPLAY TEXT, not a
 * fail-closed fingerprint: audio-CD dedup keys ride on the TOC
 * (mos_internal_toc_parse), the fail-closed identity primitive.
 *
 * Stream model (MMC / Red Book): within one (pack-type, block) the
 * per-track strings are NUL-separated and chopped across the 12-byte pack
 * payloads; the first pack's Track Number field seeds the running index,
 * so the stream is [track S, track S+1, ...] (S = 0 is album-level for
 * the title/performer types). We walk the block-0 packs in buffer order,
 * reconstruct that stream, and dispatch each string by its track number.
 *
 * Pack layout (READ TOC format 0101b, MMC-3 §6.27 / Red Book CD-TEXT;
 * libcdio cross-check in SPEC.md):
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

/* Bounded NUL-terminated copy into a fixed buffer (truncates beyond
   cap-1). Pure — no stdio in this layer. */
static void cdtext_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* Dispatch one reconstructed string `s` to its destination by track
   number: track 0 is the album field (`album_dst`); tracks 1..MAX go to
   `tracks[track-1]` when `tracks` is non-NULL, bumping *max_track to the
   highest track number that carried a NON-EMPTY title. */
static void cdtext_store(const char *s, uint32_t track,
                         char *album_dst, size_t album_cap,
                         char tracks[][MOS_CDTEXT_TRACK_TITLE_CAP],
                         uint8_t *max_track)
{
    if (track == 0) {
        cdtext_copy(album_dst, album_cap, s);
        return;
    }
    if (tracks && track >= 1 && track <= MOS_CDTEXT_MAX_TRACKS) {
        cdtext_copy(tracks[track - 1], MOS_CDTEXT_TRACK_TITLE_CAP, s);
        if (s[0] && (uint8_t)track > *max_track) *max_track = (uint8_t)track;
    }
}

/* Walk the block-0 packs of `want_type` in buffer order, reconstruct the
   NUL-separated per-track string stream, and store each string by its
   track number (the first qualifying pack's Track Number field seeds the
   running index). The album string (track 0) lands in `album_dst`;
   per-track strings (track 1..) land in `tracks` when non-NULL. Single-
   byte only: a double-byte (DBCC) STARTING pack decodes nothing (album
   left ""), a mid-stream DBCC stops the walk keeping the prefix already
   stored. */
static void cdtext_decode_type(const uint8_t *buf, size_t span,
                               uint8_t want_type,
                               char *album_dst, size_t album_cap,
                               char tracks[][MOS_CDTEXT_TRACK_TITLE_CAP],
                               uint8_t *max_track)
{
    album_dst[0] = '\0';

    char     cur[MOS_CDTEXT_STR_CAP];   /* current-string accumulator */
    size_t   n       = 0;
    uint32_t track   = 0;               /* track number of the current string */
    bool     started = false;

    for (size_t p = CDTEXT_HDR; p + CDTEXT_PACK_LEN <= span;
         p += CDTEXT_PACK_LEN) {
        if (buf[p] != want_type) continue;            /* wrong pack type   */
        uint8_t b3 = buf[p + 3];
        if (((b3 >> 4) & 0x07) != 0) continue;        /* first block only  */
        bool dbcc = (b3 & 0x80) != 0;

        if (!started) {
            if (dbcc) return;                          /* double-byte: skip */
            track   = (uint32_t)(buf[p + 1] & 0x7Fu);  /* starting track #  */
            started = true;
        } else if (dbcc) {
            break;            /* charset flip mid-stream: keep what we have */
        }

        for (size_t j = 0; j < CDTEXT_TEXT_LEN; j++) {
            uint8_t c = buf[p + CDTEXT_TEXT_OFF + j];
            if (c == 0x00) {                           /* end of one string */
                cur[n] = '\0';
                cdtext_store(cur, track, album_dst, album_cap,
                             tracks, max_track);
                n = 0;
                track++;
                continue;
            }
            if (n + 1 < sizeof cur) cur[n++] = (char)c; /* else truncate    */
        }
    }
    /* Trailing unterminated string (clamped data ended mid-string): keep
       it, best-effort. */
    if (started && n > 0) {
        cur[n] = '\0';
        cdtext_store(cur, track, album_dst, album_cap, tracks, max_track);
    }
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

    cdtext_decode_type(buf, span, CDTEXT_PACK_TITLE,
                       out->title, sizeof out->title,
                       out->track_titles, &out->track_count);
    cdtext_decode_type(buf, span, CDTEXT_PACK_PERFORMER,
                       out->performer, sizeof out->performer,
                       out->track_performers, &out->track_count);

    /* "have" is the useful-identity gate: an empty result (no album
       field, no per-track title) is not identity. Return false → the
       adapter reports no CD-TEXT (null), same as the other media reads. */
    out->have = out->title[0] || out->performer[0] || out->track_count > 0;
    return out->have;
}
