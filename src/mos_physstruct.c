/*
 * mos_physstruct.c — pure, bounds-safe decode of READ DISC STRUCTURE
 * (MMC-5 0xAD) replies for the DVD / HD-DVD media-type family
 * (MEDIA_TYPE = 0): Physical Format Information (format 0x00) and
 * Copyright Management Information (format 0x01).
 *
 * "Physical structure", not "DVD": the same media-type-0 reply carries
 * HD-DVD book types (0x4..0x6) alongside DVD, so this is not DVD-specific.
 * The BD half (DI) is a different media type — mos_discstruct.c.
 *
 * No IOKit: the shell hands us a fixed zero-init buffer (filled via
 * ReadDiscStructure) and its size. Every length is device-reported, hence
 * hostile — the declared length must never steer a read outside
 * [buf, buf+len); only fixed offsets are read.
 *
 * Wire layout (both formats share the READ DISC STRUCTURE 4-byte header):
 *   [0..1] Disc Structure Data Length (BE) — bytes AFTER this field;
 *          the response occupies 2 + value bytes. Only ever SHRINKS the
 *          trusted region.
 *   [2..3] reserved
 *   [4..]  the format payload. With `base = &buf[4]`:
 *
 *   Format 0x00 (Physical Format Information):
 *     base[0]  book_type (7:4) | part_version (3:0)
 *     base[1]  disc_size (7:4) | maximum_rate (3:0)
 *     base[2]  (rsv 7) | num_layers (6:5) | track_path (4) | layer_type (3:0)
 *     base[3]  linear_density (7:4) | track_density (3:0)
 *     base[5..7]   Starting PSN of Data Area (24-bit BE)
 *     base[9..11]  End PSN of Data Area (24-bit BE)
 *     base[13..15] End PSN in Layer 0 (24-bit BE) — the layer break
 *     base[16] bca (bit 7)
 *
 *   Format 0x01 (Copyright Management Information):
 *     buf[4] Copyright Protection System Type (CPST)
 *     buf[5] Region Management Information (RMI)
 *
 * The Linux cdrom.c cross-check is in SPEC.md. Classification (book_type
 * => media name, cpst => "CSS-protected") is the consumer's; this surfaces
 * the registered values faithfully and stops there (scope doctrine).
 */

#include "mos_pure.h"

#define PS_HDR        4u                 /* 2-byte length + 2 reserved   */
/* Physical (0x00): must reach base[16] = buf[PS_HDR+16] = buf[20]. */
#define PHYS_MIN_LEN  (PS_HDR + 16u + 1u)
/* Copyright (0x01): must reach the RMI byte, buf[5]. */
#define COPY_MIN_LEN  6u

/* Trusted end: the smaller of the buffer and the reply's declared length
   (+2 for the length field). Computed wide so the +2 cannot wrap. */
static size_t mos_internal_ps_trusted_end(const uint8_t *buf, size_t len)
{
    size_t declared = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    return (len < declared) ? len : declared;
}

bool mos_internal_physical_format_parse(const uint8_t *buf, size_t len,
                                        struct mos_physical_structure *out)
{
    if (!out) return false;
    out->have_physical = false;
    if (!buf || len < PHYS_MIN_LEN) return false;
    if (mos_internal_ps_trusted_end(buf, len) < PHYS_MIN_LEN) return false;

    const uint8_t *b = &buf[PS_HDR];
    out->book_type      = (uint8_t)(b[0] >> 4);
    out->part_version   = (uint8_t)(b[0] & 0x0f);
    out->disc_size      = (uint8_t)(b[1] >> 4);
    out->max_rate       = (uint8_t)(b[1] & 0x0f);
    out->layer_type     = (uint8_t)(b[2] & 0x0f);
    out->track_path     = (uint8_t)((b[2] >> 4) & 0x01);
    /* MMC "Number of Layers" code 0/1 => human count 1/2. */
    out->num_layers     = (uint8_t)(((b[2] >> 5) & 0x03) + 1u);
    out->linear_density = (uint8_t)(b[3] >> 4);
    out->track_density  = (uint8_t)(b[3] & 0x0f);
    out->start_sector   = (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
    out->end_sector     = (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11];
    out->end_sector_l0  = (uint32_t)b[13] << 16 | (uint32_t)b[14] << 8 | b[15];
    out->bca            = (b[16] >> 7) & 0x01;
    out->have_physical  = true;
    return true;
}

bool mos_internal_copyright_mgmt_parse(const uint8_t *buf, size_t len,
                                       struct mos_physical_structure *out)
{
    if (!out) return false;
    out->have_copyright = false;
    if (!buf || len < COPY_MIN_LEN) return false;
    if (mos_internal_ps_trusted_end(buf, len) < COPY_MIN_LEN) return false;

    out->protection     = buf[4];   /* CPST */
    out->region         = buf[5];   /* RMI region mask */
    out->have_copyright = true;
    return true;
}
