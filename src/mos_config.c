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

/* Contract in mos_pure.h. The content-protection features all live in the same
   RT=0 walk. Version-carrying schemes (CSS/CPRM/AACS) put their version at
   payload byte 3 (Additional Length 4); a present-but-truncated payload reads
   as absent (fail closed, like the walker). SecurDisc/VCPS are presence-only
   (Additional Length 0 ⇒ no payload), so the find alone is the signal. AACS
   byte 0 carries BEC (bit 1, bus encryption) and WBE (bit 2, write bus
   encryption) — MMC-6 §5.3.44 Table 198. */
void mos_internal_protection_from_config(const uint8_t *buf, size_t len,
                                         mos_drive_caps *out)
{
    if (!out) return;
    *out = (mos_drive_caps){0};

    mos_drive_protection *p = &out->protection;
    mos_config_feature f;

    /* DVD CSS (0106h): CSS Version at payload byte 3. */
    if (mos_internal_config_find_feature(buf, len, 0x0106, &f) &&
        f.data && f.data_len >= 4) {
        p->css         = true;
        p->css_version = f.data[3];
    }
    /* DVD CPRM (010Bh): CPRM version at payload byte 3. */
    if (mos_internal_config_find_feature(buf, len, 0x010B, &f) &&
        f.data && f.data_len >= 4) {
        p->cprm         = true;
        p->cprm_version = f.data[3];
    }
    /* AACS (010Dh): BEC/WBE in byte 0, AACS Version in byte 3. */
    if (mos_internal_config_find_feature(buf, len, 0x010D, &f) &&
        f.data && f.data_len >= 4) {
        p->aacs                 = true;
        p->bus_encryption       = (f.data[0] & 0x02u) != 0;
        p->write_bus_encryption = (f.data[0] & 0x04u) != 0;
        p->aacs_version         = f.data[3];
    }
    /* SecurDisc (0113h): presence only (Additional Length 0). */
    if (mos_internal_config_find_feature(buf, len, 0x0113, &f))
        p->securdisc = true;
    /* VCPS (0110h): legacy (MMC-5), presence only. */
    if (mos_internal_config_find_feature(buf, len, 0x0110, &f))
        p->vcps = true;
}

/* Contract in mos_pure.h. Write Protect Feature (0004h), MMC-6 r02g §5.3.5
   Table 101: the descriptor payload byte 0 (f.data[0], after the 4-byte
   feature header) carries SSWPP (bit 0), SPWP (bit 1), WDCB (bit 2), DWP
   (bit 3). Presence alone (find) sets `present`; a truncated payload
   (data_len < 1) leaves the bits false (fail closed, like the protection
   decoders). Does NOT zero-init out (protection_from_config did). */
void mos_internal_write_protect_from_config(const uint8_t *buf, size_t len,
                                            mos_drive_caps *out)
{
    if (!out) return;
    mos_write_protect *w = &out->write_protect;

    mos_config_feature f;
    if (!mos_internal_config_find_feature(buf, len, 0x0004, &f)) return;
    w->present = true;
    if (!f.data || f.data_len < 1) return;        /* present, no capability byte */
    w->software_write_protect = (f.data[0] & 0x01u) != 0;
    w->persistent_write_protect  = (f.data[0] & 0x02u) != 0;
    w->write_inhibit_dcb  = (f.data[0] & 0x04u) != 0;
    w->disc_write_protect   = (f.data[0] & 0x08u) != 0;
}

/* Contract in mos_pure.h. Curated capability-presence flags from the RT=0
   walk: Real-Time Streaming (0107h), Power Management (0100h), Time-out
   (0105h). Presence only (find), matching the SecurDisc/VCPS presence
   pattern; no payload byte is decoded. Does NOT zero-init out. */
void mos_internal_capabilities_from_config(const uint8_t *buf, size_t len,
                                           mos_drive_caps *out)
{
    if (!out) return;
    mos_drive_capabilities *c = &out->capabilities;
    mos_config_feature f;
    c->real_time_streaming = mos_internal_config_find_feature(buf, len, 0x0107, &f);
    c->power_management     = mos_internal_config_find_feature(buf, len, 0x0100, &f);
    c->timeout              = mos_internal_config_find_feature(buf, len, 0x0105, &f);
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

/* Contract in mos_pure.h. Firmware Information feature (010Ch), MMC-6 r02g
   §5.3.43 Table 197: the feature payload (f.data, after the 4-byte header)
   is Century[2] Year[2] Month[2] Day[2] Hour[2] Minute[2] Second[2]
   Reserved[2], all decimal ASCII (GMT). We emit RFC 3339 UTC
   "YYYY-MM-DDTHH:MM:SSZ" — the SAME form mos.event.v1's `ts` uses
   (mos_watch_core.c::format_rfc3339), integer seconds + trailing Z.
   The 14 date/time bytes must all be decimal ASCII AND form a real calendar
   date/time (month 1-12, day valid for the month incl. leap years, hour 0-23,
   minute/second 0-59) or the reply is refused (empty out) — fail closed on a
   malformed or out-of-range descriptor rather than emit a fake RFC 3339
   string. Profile is stricter than RFC 3339: no leap second (second 0-59). */
void mos_internal_firmware_date_from_config(const uint8_t *buf, size_t len,
                                            char *out, size_t out_cap)
{
    if (out && out_cap) out[0] = 0;
    if (!out || out_cap < 21u) return;           /* "....-..-..T..:..:..Z"+NUL */

    mos_config_feature f;
    if (!mos_internal_config_find_feature(buf, len, 0x010C, &f)) return;
    if (!f.data || f.data_len < 14u) return;     /* need Century..Second */

    for (size_t i = 0; i < 14u; i++)
        if (f.data[i] < '0' || f.data[i] > '9') return;   /* must be ASCII digits */

    const uint8_t *d = f.data;
    /* d[0..1] Century, [2..3] Year, [4..5] Month, [6..7] Day,
       [8..9] Hour, [10..11] Minute, [12..13] Second. */

    /* Reject impossible calendar values. We emit an RFC 3339 timestamp, so a
       shape-valid-but-nonsense descriptor (month 99, day 99, hour 99 — a
       hostile bridge or a firmware bug) must fail closed (empty out), not be
       dressed up as a standards-conforming date. Range profile, STRICTER than
       RFC 3339 in one respect: the second is 0-59 — leap second 60 is not
       accepted (a firmware creation stamp is never at a leap second, and this
       keeps the C decoder in step with the schema's date-time format check). */
    #define MOS_V2(i) ((unsigned)((d[(i)] - '0') * 10 + (d[(i) + 1] - '0')))
    unsigned year   = MOS_V2(0) * 100u + MOS_V2(2);
    unsigned month  = MOS_V2(4);
    unsigned day    = MOS_V2(6);
    unsigned hour   = MOS_V2(8);
    unsigned minute = MOS_V2(10);
    unsigned second = MOS_V2(12);
    #undef MOS_V2
    static const unsigned mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
    if (month < 1u || month > 12u) return;
    unsigned dmax = mdays[month - 1u] + ((month == 2u && leap) ? 1u : 0u);
    if (day < 1u || day > dmax) return;
    if (hour > 23u || minute > 59u || second > 59u) return;
    out[0]=(char)d[0];  out[1]=(char)d[1];  out[2]=(char)d[2];  out[3]=(char)d[3];
    out[4]='-';  out[5]=(char)d[4];  out[6]=(char)d[5];
    out[7]='-';  out[8]=(char)d[6];  out[9]=(char)d[7];
    out[10]='T'; out[11]=(char)d[8]; out[12]=(char)d[9];
    out[13]=':'; out[14]=(char)d[10]; out[15]=(char)d[11];
    out[16]=':'; out[17]=(char)d[12]; out[18]=(char)d[13];
    out[19]='Z'; out[20]='\0';
}

/* Logical Unit Serial Number — GET CONFIGURATION feature 0108h, from the same
   RT=0 reply. The descriptor payload is the drive's serial as ASCII (MMC: the
   Logical Unit Serial Number feature carries an ASCII serial in its feature
   data). PRIMARY serial source: this rides the non-exclusive GetConfiguration
   convenience walk (no exclusive access, reads even on mounted media), and is
   the hardware-validated path. Decode discipline: trailing space/NUL padding
   trimmed, interior NUL or over-length refused, complete-or-unavailable — a
   durable identity key is whole or nothing (a prefix is a wrong key two drives
   can share). out NUL-terminated;
   empty out (out[0]==0) when the feature is absent or carries no serial. */
void mos_internal_serial_from_config(const uint8_t *buf, size_t len,
                                     char *out, size_t out_cap)
{
    if (out && out_cap) out[0] = 0;
    if (!out || out_cap == 0) return;

    mos_config_feature f;
    if (!mos_internal_config_find_feature(buf, len, 0x0108, &f)) return;
    if (!f.data || f.data_len == 0) return;          /* feature present, no data */

    /* Trim trailing wire padding (spaces + NULs); leading/interior bytes stay. */
    size_t n = f.data_len;
    while (n > 0) {
        uint8_t c = f.data[n - 1];
        if (c != ' ' && c != 0x00) break;
        n--;
    }
    if (n == 0) return;                               /* present, no serial */

    /* Complete-or-unavailable: refuse an interior NUL (would sever the C string)
       or a serial that does not fit WHOLE — same as the VPD-0x80 rule. */
    for (size_t i = 0; i < n; i++)
        if (f.data[i] == 0x00) return;                /* interior NUL */
    if (n > out_cap - 1u) return;                     /* would not fit whole */

    for (size_t i = 0; i < n; i++) out[i] = (char)f.data[i];
    out[n] = 0;
}
