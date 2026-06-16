/*
 * mos_discstruct.c — pure, bounds-safe decode of a READ DISC STRUCTURE
 * (MMC-5 0xAD) Blu-ray Disc Information (DI) reply: the disc's registered
 * Disc Manufacturer ID + Media Type ID. No IOKit: the shell hands us a
 * fixed zero-init buffer (filled via ReadDiscStructure) and its size.
 * Every length and string byte is disc-reported, hence hostile — the
 * declared length must never steer a read outside [buf, buf+len), and ID
 * bytes are copied verbatim (the CLI escapes them at emit, like INQUIRY).
 *
 * Layout (response buffer):
 *   [0..1] Disc Structure Data Length (BE) — bytes available AFTER this
 *          field; the response occupies 2 + value bytes.
 *   [2..3] reserved
 *   [4..]  the Disc Information (DI), a sequence of 112-byte DI units.
 *          The first unit carries the identity:
 *            [4+0..1]   "DI" signature
 *            [4+8..10]  Disc Type Identifier (3 bytes) "BDR"/"BDW"/"BDO"
 *            [4+100..105] Disc Manufacturer ID (6 bytes)  e.g. "MILLEN"
 *            [4+106..108] Media Type ID        (3 bytes)  e.g. "MR1"
 *            [4+111]      Product Revision Number (1 byte) e.g. '0'
 *
 * The DI offsets (8 / 100 / 106 / 111) are MMC-5 / BDA-registered; the
 * undecoded write-parameter region is in SPEC.md. Classification (e.g.
 * "MILLEN" => M-DISC) is the consumer's: this surfaces the registered ID
 * bytes faithfully and stops there (scope doctrine).
 */

#include "mos_pure.h"

#define DI_HDR       4u                  /* 2-byte length + 2 reserved   */
#define DI_SIG_HI    (DI_HDR + 0u)       /* 'D'                          */
#define DI_SIG_LO    (DI_HDR + 1u)       /* 'I'                          */
#define DI_DISCTYPE  (DI_HDR + 8u)       /* 3 bytes: BDR/BDW/BDO         */
#define DI_MANUF     (DI_HDR + 100u)     /* 6 bytes                      */
#define DI_MEDIA     (DI_HDR + 106u)     /* 3 bytes                      */
#define DI_REVISION  (DI_HDR + 111u)     /* 1 byte                       */
#define DI_MIN_LEN   (DI_REVISION + 1u)  /* must reach the revision byte */

/* Copy a fixed-width DI field verbatim, NUL-terminate, strip trailing
   spaces (space-padded, like the INQUIRY identity copies). dst holds n+1. */
static void mos_internal_di_copy(const uint8_t *src, size_t n, char *dst)
{
    size_t i;
    for (i = 0; i < n; i++) dst[i] = (char)src[i];
    dst[i] = '\0';
    while (i > 0 && dst[i - 1] == ' ') dst[--i] = '\0';
}

bool mos_internal_bd_disc_id_parse(const uint8_t *buf, size_t len,
                                   struct mos_disc_id *out)
{
    if (!out) return false;
    *out = (struct mos_disc_id){0};
    if (!buf) return false;

    /* Identity region must be present per BOTH the buffer and the reply's
       declared length; the declared length can only shrink the trusted
       region (computed wide so the +2 cannot wrap). */
    if (len < DI_MIN_LEN) return false;
    size_t declared = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < declared) ? len : declared;
    if (end < DI_MIN_LEN) return false;

    /* The DI signature gates the whole decode: a non-DI reply (wrong media
       type, drive returned something else) is refused, not read as identity. */
    if (buf[DI_SIG_HI] != 'D' || buf[DI_SIG_LO] != 'I') return false;

    mos_internal_di_copy(&buf[DI_DISCTYPE], 3, out->disc_type);
    mos_internal_di_copy(&buf[DI_MANUF],    6, out->manufacturer);
    mos_internal_di_copy(&buf[DI_MEDIA],    3, out->media_type);
    mos_internal_di_copy(&buf[DI_REVISION], 1, out->revision);
    return true;
}
