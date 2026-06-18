/*
 * mos_inqdata.c — pure decode of STANDARD INQUIRY data (EVPD=0): the drive's
 * self-reported identity (vendor/product/revision) AND the SPC version byte +
 * version-descriptor list (the T10/ISO standards the drive claims). `mos drive`
 * issues this raw read for the canonical truth and prefers it over the
 * DiscRecording cache; the descriptors at bytes 58-73 are unreachable through
 * macOS's convenience Inquiry (a 36-byte header read), so the read is raw
 * (mos_drive_inquiry.c; AGENTS.md scope-doctrine ADR for the EVPD=0 mode).
 * Identity strings are trailing-trimmed here; non-ASCII is copied verbatim and
 * escaped at the output sink (mos_safe_ascii / mos_cli_json_str), like every
 * other identity string. Standard token naming is applied at the sink too
 * (mos_spc_version_name / mos_version_descriptor_name).
 *
 * No IOKit: the shell hands us a fixed zero-init buffer bounded to the bytes
 * the transport actually returned. Every length here is device-reported,
 * hence hostile; each field is bounded by BOTH the passed len and the reply's
 * own Additional Length (byte 4) — the dual-length rule (O-4). A reply whose
 * trusted region does not reach the 36-byte standard header (an under-
 * delivered transfer that cut the identity short) is REFUSED, not surfaced as
 * a partial identity: this is canonical drive truth the CLI prefers over the
 * DR cache, so an incomplete read must defer to it (see the parser body).
 *
 * Standard INQUIRY layout (SPC-4 §6.4.2):
 *   [2]      VERSION              — SPC compliance level
 *   [4]      ADDITIONAL LENGTH (n-4) — bytes that follow; total = 5 + this
 *   [8..15]  VENDOR IDENTIFICATION  (8 ASCII, space-padded)
 *   [16..31] PRODUCT IDENTIFICATION (16 ASCII)
 *   [32..35] PRODUCT REVISION LEVEL (4 ASCII)
 *   [58..73] VERSION DESCRIPTORS — up to eight 2-byte BE codes (0 = none)
 */

#include "mos_pure.h"

#define VD_HDR      5u   /* through ADDITIONAL LENGTH (byte 4)            */
#define INQ_STD_HDR 36u  /* the standard 36-byte header (vendor..revision) */
#define VD_OFFSET  58u   /* first version descriptor                      */
#define VD_MAX      8u   /* eight descriptor slots, bytes 58-73           */

/* Copy the ASCII field at [start, start+width) into out[0..out_cap), bounded
   by the trusted `end`, trailing spaces/NULs trimmed. out is "" when the
   field lies (even partly) outside the trusted region's start. */
static void copy_field(const uint8_t *buf, size_t end, size_t start,
                       size_t width, char *out, size_t out_cap)
{
    out[0] = 0;
    if (out_cap < 2u || start >= end) return;
    size_t avail = end - start;
    size_t n = (width < avail) ? width : avail;
    while (n > 0 && (buf[start + n - 1] == ' ' || buf[start + n - 1] == 0))
        n--;
    size_t copy = (n < out_cap - 1u) ? n : out_cap - 1u;
    for (size_t i = 0; i < copy; i++) out[i] = (char)buf[start + i];
    out[copy] = 0;
}

bool mos_internal_inqdata_parse(const uint8_t *buf, size_t len,
                               mos_drive_inquiry *out)
{
    if (!out) return false;
    *out = (mos_drive_inquiry){0};
    if (!buf || len < VD_HDR) return false;   /* need the fixed header */

    /* Trusted end: the smaller of the buffer span and the reply's own
       declared total (5 + Additional Length). A lying-long Additional Length
       cannot extend past `len`; an honest-short one shrinks the region. */
    size_t declared = VD_HDR + (size_t)buf[4];
    size_t end = (declared < len) ? declared : len;

    /* A conformant STANDARD INQUIRY always returns at least the 36-byte
       standard header (Additional Length >= 31), so vendor (8-15), product
       (16-31), and revision (32-35) are wholly present. If the trusted region
       stops short of byte 36 — the device declared a sub-header total OR the
       transport under-delivered (declared > delivered, so the dual-length
       floor caps `end` at the bytes that actually arrived) — at least one
       identity field is cut or absent. REFUSE the whole reply rather than
       surface a partial identity: `mos drive` treats this read as the
       drive's CANONICAL truth and prefers it over the DiscRecording cache
       (cli/drive.c), so a half-arrived "BD" (a USB-SATA bridge that cut the
       transfer mid-PRODUCT) would mask the full cached model. Returning false
       makes mos_query_drive_inquiry fail → the caller falls back to DR and the
       COMPLETE identity wins. Bounds stay safe: the gate precedes every field
       read, so a truncated reply is rejected before a byte is copied. This is
       the dual-length rule's canonical-data corollary — under-delivery of a
       value the caller trusts as authoritative is refused, not trusted-as-
       short (AGENTS.md; contrast a genuinely short reply, declared <= len,
       which reaches byte 36 and parses). */
    if (end < INQ_STD_HDR) return false;

    out->spc_version = buf[2];

    copy_field(buf, end,  8u,  8u, out->vendor,   sizeof out->vendor);
    copy_field(buf, end, 16u, 16u, out->product,  sizeof out->product);
    copy_field(buf, end, 32u,  4u, out->revision, sizeof out->revision);

    uint8_t n = 0;
    for (uint8_t i = 0; i < VD_MAX; i++) {
        size_t off = VD_OFFSET + (size_t)i * 2u;
        if (off + 2u > end) break;                /* descriptor not present */
        uint16_t code = (uint16_t)(((uint16_t)buf[off] << 8) | buf[off + 1]);
        if (code != 0) out->descriptors[n++] = code;   /* 0x0000 = empty slot */
    }
    out->descriptor_count = n;
    return true;
}
