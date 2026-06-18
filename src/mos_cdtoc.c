/*
 * mos_cdtoc.c — pure, bounds-safe decode of the macOS kernel-cached full-TOC
 * blob (kIOCDMediaTOCKey, the Apple CDTOC struct) into per-session boundaries.
 * No IOKit: the shell (mos_scsi.c) copies the registry CFData into a fixed
 * zero-init buffer and hands us its size. The CDTOC length field is
 * device-reported, hence hostile — it may only SHRINK the descriptor walk,
 * never extend it; no payload byte is ever used as an offset.
 *
 * This is the richer session structure the issued READ TOC format-0000b
 * (mos_internal_toc_parse) omits: per session, the first/last track and the
 * lead-out. CD-only (the property lives only on IOCDMedia). Wire layout and
 * provenance (IOCDTypes.h; libcdio lib/driver/osx.c read_toc_osx cross-check)
 * are in mos_pure.h and SPEC.md.
 */

#include "mos_pure.h"

#include <string.h>

#define CDTOC_HEADER   4u   /* length(2) + sessionFirst(1) + sessionLast(1)   */
#define CDTOC_DESC_LEN 11u  /* sizeof(CDTOCDescriptor): 1+1+1+1+3+1+3          */

/* CDConvertMSFToLBA (IOCDTypes.h): (min*60 + sec)*75 + frame - 150. The 150 is
   the 2-second pregap offset; clamp a degenerate sub-150 MSF to 0 rather than
   wrap (hostile input, not a conforming lead-out). */
static uint32_t mos_internal_cdmsf_to_lba(uint8_t m, uint8_t s, uint8_t f)
{
    uint32_t frames = ((uint32_t)m * 60u + s) * 75u + f;
    return frames >= 150u ? frames - 150u : 0u;
}

bool mos_internal_cdtoc_parse(const uint8_t *buf, size_t len,
                              mos_session_layout *out)
{
    if (!out) return false;
    *out = (mos_session_layout){0};
    if (!buf || len < CDTOC_HEADER) return false;

    /* tocSize = length + sizeof(length): the descriptor region is what remains
       after the 4-byte header. The device length only shrinks the trusted end
       (dual-length rule O-4); the header range bytes 2/3 are advisory — the
       descriptors are the truth, so the compaction below trusts them, not the
       header's sessionFirst/sessionLast. */
    size_t toc_size = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < toc_size) ? len : toc_size;
    if (end < CDTOC_HEADER) return false;

    /* Working boundaries indexed by session number 1..99 (index 0 unused). */
    struct { bool first, last, lead; uint8_t ft, lt; uint32_t lo; }
        acc[MOS_SESSION_MAX + 1];
    memset(acc, 0, sizeof acc);

    for (size_t off = CDTOC_HEADER; off + CDTOC_DESC_LEN <= end;
         off += CDTOC_DESC_LEN) {
        const uint8_t *d = &buf[off];
        uint8_t session = d[0];
        uint8_t adr     = (uint8_t)((d[1] >> 4) & 0x0f);  /* high nibble, both
                                                             endiannesses */
        uint8_t point   = d[3];

        /* Only ADR=1 (Q-channel position) descriptors bound sessions — the
           libcdio filter; ADR 2/3 (catalogue/ISRC) carry no boundary. */
        if (adr != 0x01) continue;
        if (session < 1u || session > MOS_SESSION_MAX) continue;

        uint8_t pmin = d[8], psec = d[9], pframe = d[10];   /* p MSF */
        switch (point) {
        case 0xA0:                                          /* first track # */
            acc[session].first = true; acc[session].ft = pmin; break;
        case 0xA1:                                          /* last track #  */
            acc[session].last = true; acc[session].lt = pmin; break;
        case 0xA2:                                          /* lead-out MSF  */
            acc[session].lead = true;
            acc[session].lo = mos_internal_cdmsf_to_lba(pmin, psec, pframe);
            break;
        default: break;   /* track points (1..99) carry no session boundary */
        }
    }

    /* Compact populated sessions in ascending order. */
    for (uint8_t s = 1; s <= MOS_SESSION_MAX && out->count < MOS_SESSION_MAX;
         s++) {
        if (!acc[s].first && !acc[s].last && !acc[s].lead) continue;
        mos_session_entry *e = &out->sessions[out->count++];
        e->session      = s;
        e->have_first   = acc[s].first;  e->first_track  = acc[s].ft;
        e->have_last    = acc[s].last;   e->last_track   = acc[s].lt;
        e->have_leadout = acc[s].lead;   e->leadout_lba  = acc[s].lo;
    }
    return out->count > 0;
}

bool mos_internal_cdtoc_to_toc(const uint8_t *buf, size_t len, mos_toc *out)
{
    if (!out) return false;
    *out = (mos_toc){0};
    if (!buf || len < CDTOC_HEADER) return false;

    size_t toc_size = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < toc_size) ? len : toc_size;
    if (end < CDTOC_HEADER) return false;

    /* Track descriptors indexed by track number 1..99; A2 carries the disc
       lead-out (the highest session's). FAIL-CLOSED to the format-0000b
       standard: a duplicate track or a gap in first..last refuses the whole,
       and mos_query_toc falls back to the issued READ TOC. */
    struct { bool seen; uint8_t adr, control; uint32_t lba; }
        tk[MOS_TOC_MAX_TRACKS + 1];
    memset(tk, 0, sizeof tk);
    bool     have_leadout = false;
    uint32_t leadout = 0;
    uint8_t  leadout_session = 0;

    for (size_t off = CDTOC_HEADER; off + CDTOC_DESC_LEN <= end;
         off += CDTOC_DESC_LEN) {
        const uint8_t *d = &buf[off];
        if (((d[1] >> 4) & 0x0f) != 0x01) continue;     /* adr 1 only */
        uint8_t session = d[0];
        uint8_t point   = d[3];
        uint32_t lba = mos_internal_cdmsf_to_lba(d[8], d[9], d[10]);

        if (point >= 1u && point <= MOS_TOC_MAX_TRACKS) {
            if (tk[point].seen) return false;           /* duplicate = incoherent */
            tk[point].seen    = true;
            tk[point].adr     = 0x01;
            tk[point].control = (uint8_t)(d[1] & 0x0f);
            tk[point].lba     = lba;
        } else if (point == 0xA2) {                     /* lead-out (per session) */
            if (!have_leadout || session >= leadout_session) {
                have_leadout = true; leadout = lba; leadout_session = session;
            }
        }
        /* A0/A1 first/last-track POINTs are advisory; first/last are taken from
           the identity-bearing track set below. */
    }

    uint8_t first = 0, last = 0;
    for (uint8_t t = 1; t <= MOS_TOC_MAX_TRACKS; t++)
        if (tk[t].seen) { if (!first) first = t; last = t; }
    if (!first) return false;                           /* no tracks */

    uint8_t n = 0;
    for (uint8_t t = first; t <= last; t++) {
        if (!tk[t].seen) return false;                  /* gap in first..last */
        out->tracks[n].track     = t;
        out->tracks[n].adr       = tk[t].adr;
        out->tracks[n].control   = tk[t].control;
        out->tracks[n].start_lba = tk[t].lba;
        n++;
    }
    out->first_track  = first;
    out->last_track   = last;
    out->track_count  = n;
    out->have_leadout = have_leadout;
    out->leadout_lba  = leadout;
    return true;
}
