/*
 * mos_stdinq.c — pure decode of STANDARD INQUIRY data (EVPD=0): the version
 * byte (SPC compliance level) and the version-descriptor list (the T10/ISO
 * standards the drive claims). Read raw because macOS's convenience Inquiry
 * returns only the 36-byte header, so the descriptors at bytes 58-73 are
 * structurally unreachable through it (mos_standards.c does the raw read;
 * AGENTS.md scope-doctrine ADR for the EVPD=0 INQUIRY mode). Naming is
 * applied at the output sink (mos_spc_version_name / mos_version_descriptor_name).
 *
 * No IOKit: the shell hands us a fixed zero-init buffer bounded to the bytes
 * the transport actually returned. Every length here is device-reported,
 * hence hostile; the descriptor region is bounded by BOTH the passed len and
 * the reply's own Additional Length (byte 4) — the dual-length rule (O-4).
 *
 * Standard INQUIRY layout (SPC-4 §6.4.2):
 *   [0]    PERIPHERAL QUALIFIER (7:5) | PERIPHERAL DEVICE TYPE (4:0)
 *   [1]    RMB (bit7) | …
 *   [2]    VERSION              — SPC compliance level (the byte we keep)
 *   [3]    response data format …
 *   [4]    ADDITIONAL LENGTH (n-4) — bytes that follow; total = 5 + this
 *   …
 *   [58..73] VERSION DESCRIPTORS — up to eight 2-byte BE codes (0 = none)
 */

#include "mos_pure.h"

#define STDINQ_HDR        5u    /* through ADDITIONAL LENGTH (byte 4) */
#define STDINQ_VD_OFFSET 58u    /* first version descriptor           */
#define STDINQ_VD_MAX     8u    /* eight descriptor slots, bytes 58-73 */

bool mos_internal_stdinq_parse(const uint8_t *buf, size_t len,
                               mos_drive_standards *out)
{
    if (!out) return false;
    *out = (mos_drive_standards){0};
    if (!buf || len < STDINQ_HDR) return false;   /* need the fixed header */

    out->spc_version = buf[2];

    /* Trusted end: the smaller of the buffer span and the reply's own
       declared total (5 + Additional Length). A lying-long Additional Length
       cannot extend past `len`; an honest-short one shrinks the region. */
    size_t declared = STDINQ_HDR + (size_t)buf[4];
    size_t end = (declared < len) ? declared : len;

    uint8_t n = 0;
    for (uint8_t i = 0; i < STDINQ_VD_MAX; i++) {
        size_t off = STDINQ_VD_OFFSET + (size_t)i * 2u;
        if (off + 2u > end) break;                /* descriptor not present */
        uint16_t code = (uint16_t)(((uint16_t)buf[off] << 8) | buf[off + 1]);
        if (code != 0) out->descriptors[n++] = code;   /* 0x0000 = empty slot */
    }
    out->descriptor_count = n;
    return true;
}
