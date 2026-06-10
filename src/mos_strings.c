/*
 * mos_strings.c — pure string tables, escapers, and version. Separate TU
 * so mos_scsi.c stays exclusively IOKit-linked. No IOKit.
 */

#include "mos.h"
#include <stdio.h>   /* snprintf for hex escapes */
#include <stddef.h>
#include <stdint.h>

const char *mos_state_description(mos_state_enum s)
{
    switch (s) {
        case MOS_STATE_OPEN:    return "open";
        case MOS_STATE_EMPTY:   return "empty";
        case MOS_STATE_LOADING: return "loading";
        case MOS_STATE_READY:   return "ready";
        case MOS_STATE_BUSY:    return "busy";
        case MOS_STATE_FORMATTING:       return "formatting";
        case MOS_STATE_MEDIA_UNREADABLE: return "media_unreadable";
        case MOS_STATE_DEVICE_FAULT:     return "device_fault";
        case MOS_STATE_EMPTY_OR_OPEN:    return "empty_or_open";
        case MOS_STATE_UNKNOWN: default: return "unknown";
    }
}

const char *mos_disc_status_description(mos_disc_status s)
{
    switch (s) {
        case MOS_DISC_BLANK:          return "blank";
        case MOS_DISC_APPENDABLE:     return "appendable";
        case MOS_DISC_COMPLETE:       return "complete";
        /* OTHER doubles as the out-of-enum fallback, same pinned-
           coverage style as mos_state_description: -Wswitch still
           fires when a new enumerator appears. */
        case MOS_DISC_OTHER: default: return "other";
    }
}

const char *mos_error_description(mos_error e)
{
    switch (e) {
        case MOS_OK:                    return "ok";
        case MOS_ERR_INVALID_ARG:       return "invalid argument";
        case MOS_ERR_NO_DEVICE:         return "no matching optical drive";
        case MOS_ERR_DRIVER_REJECTED:   return "IOKit did not attach a SCSITaskUserClient to this drive";
        case MOS_ERR_EXCLUSIVE_ACCESS:  return "another process holds the drive";
        case MOS_ERR_BUSY:              return "drive reports busy";
        case MOS_ERR_TIMEOUT:           return "timed out";
        case MOS_ERR_IO:                return "IOKit error";
        case MOS_ERR_UNSUPPORTED:       return "not implemented in this build";
        case MOS_ERR_OOM:               return "out of memory";
        default:                        return "unknown error";
    }
}

/* MMC-6 §5.4 Feature Header Profile Codes. Names follow cdrom_id /
   udev conventions in lower_snake_case form (e.g. cdrom_id emits
   ID_CDROM_MEDIA_BD_R, which becomes "bd_r" here). Codes not in this
   table return NULL so the consumer can fall back to the hex form.
   Order in the switch is by numeric value for grep-against-spec. */
const char *mos_profile_name(uint16_t profile_code)
{
    switch (profile_code) {
        case 0x0000: return "no_current_profile";
        case 0x0001: return "non_removable_disk"; /* legacy / rare */
        case 0x0002: return "removable_disk";     /* legacy / rare */
        case 0x0003: return "mo_erasable";        /* magneto-optical */
        case 0x0008: return "cd_rom";
        case 0x0009: return "cd_r";
        case 0x000A: return "cd_rw";
        case 0x0010: return "dvd_rom";
        case 0x0011: return "dvd_minus_r";
        case 0x0012: return "dvd_ram";
        case 0x0013: return "dvd_minus_rw_restricted";
        case 0x0014: return "dvd_minus_rw_sequential";
        case 0x0015: return "dvd_minus_r_dl_sequential";
        case 0x0016: return "dvd_minus_r_dl_jump";
        case 0x0017: return "dvd_minus_rw_dl";
        case 0x001A: return "dvd_plus_rw";
        case 0x001B: return "dvd_plus_r";
        case 0x002A: return "dvd_plus_rw_dl";
        case 0x002B: return "dvd_plus_r_dl";
        case 0x0040: return "bd_rom";
        case 0x0041: return "bd_r";              /* SRM */
        case 0x0042: return "bd_r_rrm";          /* random recording */
        case 0x0043: return "bd_re";
        case 0x0050: return "hd_dvd_rom";
        case 0x0051: return "hd_dvd_r";
        case 0x0052: return "hd_dvd_ram";
        case 0x0053: return "hd_dvd_rw";
        case 0x0058: return "hd_dvd_r_dl";
        case 0x005A: return "hd_dvd_rw_dl";
        default:     return NULL;
    }
}

const char *mos_profile_class(uint16_t profile_code)
{
    /* MMC-6 Annex profile ranges. Deliberately mirrors the name table
       above (the staleness pair: a profile added there without a class
       here yields a named-but-classless profile, which the
       profile_class_total_over_name_table test forbids). */
    switch (profile_code) {
        case 0x0008: case 0x0009: case 0x000A:
            return "cd";
        case 0x0010: case 0x0011: case 0x0012: case 0x0013:
        case 0x0014: case 0x0015: case 0x0016: case 0x0017:
        case 0x001A: case 0x001B: case 0x002A: case 0x002B:
            return "dvd";
        case 0x0040: case 0x0041: case 0x0042: case 0x0043:
            return "bd";
        case 0x0050: case 0x0051: case 0x0052: case 0x0053:
        case 0x0058: case 0x005A:
            return "hd_dvd";
        default:
            return NULL;   /* no-profile, MO, legacy removable, unknown */
    }
}

/* sysexits.h class for a mos_error. Kept in sync with the table in
   include/mos.h's mos_error_sysexit() doc-comment. */
int mos_error_sysexit(mos_error e)
{
    switch (e) {
        case MOS_OK:                    return 0;  /* EX_OK */
        case MOS_ERR_INVALID_ARG:       return 64; /* EX_USAGE */
        case MOS_ERR_NO_DEVICE:         return 66; /* EX_NOINPUT */
        case MOS_ERR_DRIVER_REJECTED:   return 69; /* EX_UNAVAILABLE */
        case MOS_ERR_EXCLUSIVE_ACCESS:  return 75; /* EX_TEMPFAIL */
        case MOS_ERR_BUSY:              return 75; /* EX_TEMPFAIL */
        case MOS_ERR_TIMEOUT:           return 75; /* EX_TEMPFAIL */
        case MOS_ERR_IO:                return 74; /* EX_IOERR */
        case MOS_ERR_UNSUPPORTED:       return 69; /* EX_UNAVAILABLE */
        case MOS_ERR_OOM:               return 71; /* EX_OSERR */
        default:                        return 70; /* EX_SOFTWARE: unknown enum value */
    }
}

bool mos_error_is_recoverable(mos_error e)
{
    switch (e) {
        case MOS_ERR_EXCLUSIVE_ACCESS:
        case MOS_ERR_BUSY:
        case MOS_ERR_TIMEOUT:
            return true;
        case MOS_OK:
        case MOS_ERR_INVALID_ARG:
        case MOS_ERR_NO_DEVICE:
        case MOS_ERR_DRIVER_REJECTED:
        case MOS_ERR_IO:
        case MOS_ERR_UNSUPPORTED:
        case MOS_ERR_OOM:
        default:
            return false;
    }
}

const char *mos_version_string(void) { return MOS_VERSION_STRING; }

/* ---- Pure string escapers ---------------------------------------------
 *
 * Implementation: track `total` (the count we'd write with infinite
 * cap) and `pos` (the count we actually wrote, bounded by cap-1 to
 * leave room for NUL). Inner helpers write one byte or one string
 * conditionally based on `pos < cap - 1`. Return value is `total`,
 * letting callers detect truncation via `return >= out_cap`. */

static inline void write_byte(char *out, size_t out_cap, size_t *pos,
                              size_t *total, char c)
{
    if (*pos + 1 < out_cap) {
        out[(*pos)++] = c;
    }
    (*total)++;
}

static inline void write_str(char *out, size_t out_cap, size_t *pos,
                             size_t *total, const char *s)
{
    for (; *s; ++s) write_byte(out, out_cap, pos, total, *s);
}

size_t mos_json_escape(const char *in, char *out, size_t out_cap)
{
    size_t pos = 0;
    size_t total = 0;

    if (in) {
        for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
            switch (*p) {
                case '"':  write_str(out, out_cap, &pos, &total, "\\\""); break;
                case '\\': write_str(out, out_cap, &pos, &total, "\\\\"); break;
                case '\b': write_str(out, out_cap, &pos, &total, "\\b");  break;
                case '\f': write_str(out, out_cap, &pos, &total, "\\f");  break;
                case '\n': write_str(out, out_cap, &pos, &total, "\\n");  break;
                case '\r': write_str(out, out_cap, &pos, &total, "\\r");  break;
                case '\t': write_str(out, out_cap, &pos, &total, "\\t");  break;
                default:
                    /* RFC 8259 requires escaping < 0x20. We additionally
                       escape 0x7F (DEL — historically a control byte
                       though RFC 8259 doesn't mandate it) and bytes
                       >= 0x80 (INQUIRY fields aren't guaranteed UTF-8;
                       escaping high bytes keeps the output valid JSON
                       in every encoding the consumer might use). */
                    if (*p < 0x20 || *p >= 0x7f) {
                        char buf[8];
                        int n = snprintf(buf, sizeof(buf), "\\u%04x", *p);
                        if (n > 0) write_str(out, out_cap, &pos, &total, buf);
                    } else {
                        write_byte(out, out_cap, &pos, &total, (char)*p);
                    }
            }
        }
    }

    if (out_cap > 0) out[pos] = 0;
    return total;
}

size_t mos_safe_ascii(const char *in, char *out, size_t out_cap)
{
    size_t pos = 0;
    size_t total = 0;

    if (in) {
        for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
            /* Printable ASCII range only. Everything else — including
               0x7F (DEL), high bytes (0x80+), control bytes — renders
               as \xNN. Prevents terminal-control-sequence injection
               (ANSI escape, OSC 52 clipboard, cursor-position-report,
               title-bar manipulation) from drive-controlled bytes. */
            if (*p >= 0x20 && *p < 0x7f) {
                write_byte(out, out_cap, &pos, &total, (char)*p);
            } else {
                char buf[8];
                int n = snprintf(buf, sizeof(buf), "\\x%02x", *p);
                if (n > 0) write_str(out, out_cap, &pos, &total, buf);
            }
        }
    }

    if (out_cap > 0) out[pos] = 0;
    return total;
}

/* See mos.h. Render a whole-disk unit to its canonical "diskN" name.
   The single point where the integer identity becomes the "diskN"
   string the JSON/plaintext renderers and humans expect; the JSON wire
   contract is unchanged. 16 bytes always suffices ("disk" + a 32-bit
   unit + NUL = 15 max). Returns false (and "" when cap > 0) for a
   no-media unit (< 0) or a buffer too small to hold the result. */
bool mos_bsd_name_format(int64_t unit, char *buf, size_t cap)
{
    if (!buf || cap == 0) return false;
    /* Reject < 0 (no media) and anything above the 32-bit unit domain. The
       upper bound matters for correctness, not just truncation: a value in
       (UINT32_MAX, ~1e11) would still fit a 16-byte buffer and emit a
       different, valid-looking "diskN". Refuse both with "" + false. */
    if (unit < 0 || unit > (int64_t)UINT32_MAX) { buf[0] = 0; return false; }
    int n = snprintf(buf, cap, "disk%llu", (unsigned long long)unit);
    if (n <= 0 || (size_t)n >= cap) { buf[0] = 0; return false; }
    return true;
}

bool mos_bsd_dev_node(int64_t unit, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return false;
    out[0] = 0;
    /* Same domain as mos_bsd_name_format, same rationale: a unit in
       (UINT32_MAX, ~1e14) would still fit a generous buffer and render
       a different, valid-LOOKING node — refuse rather than emit a node
       no real disk can have. (Fifth review, F5: the guard had been
       copy-adapted away.) */
    if (unit < 0 || unit > (int64_t)UINT32_MAX) return false;
    int n = snprintf(out, out_cap, "/dev/disk%lld", (long long)unit);
    if (n < 0 || (size_t)n >= out_cap) { out[0] = 0; return false; }
    return true;
}
