/*
 * mos_config.c — pure, bounds-safe iteration over a GET CONFIGURATION
 * (MMC) response buffer. No IOKit: the shell hands us a fixed zero-init
 * buffer plus the byte count it trusts (sizeof the buffer — the MMC
 * GetConfiguration reports no realized-transfer count). Every payload
 * length is device-reported, hence hostile; this file is the choke point
 * keeping those lengths from steering a read outside [buf, buf+len).
 *
 * Layout (MMC-6 §5.2, GET CONFIGURATION response):
 *
 *   Feature header (8 bytes):
 *     [0..3] Data Length      (BE) — bytes of data available AFTER this
 *                                    field; total response = 4 + value.
 *     [4..5] Reserved
 *     [6..7] Current Profile  (BE)
 *   Feature descriptors, each:
 *     [0..1] Feature Code     (BE)
 *     [2]    (Version<<2) | (Persistent<<1) | Current
 *     [3]    Additional Length — feature bytes that follow; the
 *            descriptor spans 4 + Additional Length bytes total.
 *
 * Safety contract (the device controls every length here):
 *   - `len` is the only trusted ceiling. The header Data Length can only
 *     SHRINK the walked region (clamped under `len`), never extend it.
 *   - A descriptor is decoded only if its 4-byte header AND its
 *     Additional-Length payload fit within the trusted region.
 *   - A malformed Additional Length (not a multiple of 4, per MMC) ends
 *     the walk rather than desyncing it onto misaligned bytes.
 *   - The cursor advances by span >= 4 on every yield, so the walk makes
 *     forward progress and terminates in <= len/4 steps; a single-byte
 *     Additional Length cannot wrap the cursor.
 *   - The bool return is intentionally undifferentiated: false means
 *     "stop" — end of list OR a malformed/over-long descriptor, alike.
 *     Callers walk this as a plain `while (next(...))`; the malformed
 *     branch is unreachable on conformant hardware.
 */

#include "mos_pure.h"

bool mos_internal_config_next_feature(const uint8_t *buf, size_t len,
                                      size_t *cursor, mos_config_feature *out)
{
    if (!buf || !cursor || !out) return false;

    /* Trusted end. Start at the buffer ceiling, then let the device's Data
       Length pull it IN iff it claims less. 64-bit so the +4 cannot wrap
       before the compare; a wrapped or oversized claim fails to shrink, so
       `len` stands — device length only ever shortens the walk. */
    size_t end = len;
    if (len >= 4) {
        uint64_t dlen = ((uint64_t)buf[0] << 24) | ((uint64_t)buf[1] << 16)
                      | ((uint64_t)buf[2] << 8)  |  (uint64_t)buf[3];
        uint64_t declared = dlen + 4u;            /* total incl. length field */
        if (declared < (uint64_t)end) end = (size_t)declared;
    }

    size_t c = *cursor;

    /* Descriptor header must fit. `c > end` also catches a cursor past the
       trusted region; `end - c` is computed only when c <= end, no wrap. */
    if (c > end || end - c < 4) return false;

    uint8_t add  = buf[c + 3];                    /* Additional Length, 0..255 */

    /* MMC requires Additional Length to be a multiple of 4. Tolerating a
       non-multiple would let a hostile device desync the walk so later
       descriptors decode from misaligned (in-bounds but attacker-chosen)
       bytes. Fail closed at the first malformed descriptor. */
    if (add & 3u) return false;

    size_t  span = (size_t)4 + add;               /* >= 4, cannot wrap        */

    /* Whole descriptor (header + feature payload) must fit. */
    if (end - c < span) return false;

    uint8_t b2        = buf[c + 2];
    out->feature_code = (uint16_t)(((uint16_t)buf[c] << 8) | buf[c + 1]);
    out->current      = (b2 & 0x01u) != 0;
    out->persistent   = (b2 & 0x02u) != 0;
    out->version      = (uint8_t)((b2 >> 2) & 0x0Fu);
    out->data         = add ? &buf[c + 4] : NULL;
    out->data_len     = add;

    *cursor = c + span;                           /* strict forward progress */
    return true;
}

/* Find one feature by code: walk until a match. Same trust bounds; first
   match wins (MMC lists each feature once — a hostile duplicate yields the
   earlier copy, never a re-scan). */
bool mos_internal_config_find_feature(const uint8_t *buf, size_t len,
                                      uint16_t feature_code,
                                      mos_config_feature *out)
{
    if (!out) return false;
    size_t cursor = 8;                            /* skip the feature header */
    mos_config_feature f;
    while (mos_internal_config_next_feature(buf, len, &cursor, &f)) {
        if (f.feature_code == feature_code) { *out = f; return true; }
    }
    return false;
}

/* Current Profile = feature-header bytes 6-7, gated on the header's Data
   Length (bytes 0-3, counting bytes that FOLLOW it): the field exists only
   when the drive claims >= 4 following bytes. The gate keeps a truncated
   reply from reading as profile 0x0000 (= "no media"). Contract in mos_pure.h. */
bool mos_internal_config_current_profile(const uint8_t *buf, size_t len,
                                         uint16_t *profile)
{
    if (!buf || !profile) return false;
    if (len < 8) return false;                       /* need through byte 7 */

    uint32_t data_len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                        ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    if (data_len < 4) return false;                  /* truncated: profile not returned */

    *profile = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);
    return true;
}

/* Contract in mos_pure.h. */
void mos_internal_aacs_caps_from_config(const uint8_t *buf, size_t len,
                                        mos_drive_caps *out)
{
    if (!out) return;
    *out = (mos_drive_caps){0};

    mos_config_feature f;
    if (!mos_internal_config_find_feature(buf, len, 0x010D, &f)) return;
    if (f.data_len < 4 || !f.data) return;    /* malformed: reads as absent */

    out->aacs           = true;
    out->bus_encryption = (f.data[0] & 0x02u) != 0;
    out->aacs_version   = f.data[3];
}

/* Contract in mos_pure.h. The Profile List feature (0x0000) payload is a
   sequence of 4-byte Profile Descriptors; we keep the drive-static set of
   Profile Numbers and ignore the per-descriptor CurrentP bit (which reflects
   the loaded medium, not the drive). Bounded by the feature's data_len and
   cap; the feature walk already proved f.data spans data_len bytes in-bounds. */
void mos_internal_profile_list_from_config(const uint8_t *buf, size_t len,
                                           uint16_t *out_codes, uint8_t cap,
                                           uint8_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!out_codes || cap == 0 || !out_count) return;

    mos_config_feature f;
    if (!mos_internal_config_find_feature(buf, len, 0x0000, &f)) return;
    if (!f.data || f.data_len < 4) return;       /* no descriptors */

    uint8_t n = 0;
    for (size_t i = 0; i + 4u <= f.data_len && n < cap; i += 4u) {
        out_codes[n++] = (uint16_t)(((uint16_t)f.data[i] << 8) | f.data[i + 1]);
    }
    *out_count = n;
}
