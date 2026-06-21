/*
 * mos_strings.c — pure string tables, escapers, and version. Separate TU so
 * mos_scsi.c stays exclusively IOKit-linked. No IOKit.
 */

/* mos_pure.h (which re-includes mos.h) declares the mos_internal_* helpers
   defined in this file, so the definitions are checked against their
   prototypes (-Wmissing-prototypes). Kept on its own line above the include:
   the amalgamator drops library-local #include lines wholesale, so a trailing
   block comment here would be orphaned into dist/mos.c (scripts/amalgamate.sh). */
#include "mos_pure.h"
#include <stdio.h>   /* snprintf for hex escapes */
#include <stddef.h>
#include <stdint.h>
#include <string.h>  /* strcmp for the media-type token map */

const char *mos_state_description(mos_state s)
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
        /* OTHER also the default; -Wswitch still fires on a new enumerator. */
        case MOS_DISC_OTHER: default: return "other";
    }
}

const char *mos_tray_outcome_description(mos_tray_outcome o)
{
    switch (o) {
        case MOS_TRAY_DONE:           return "done";
        case MOS_TRAY_REFUSED_LOCKED: return "refused_locked";
        case MOS_TRAY_ALREADY_LOCKED: return "already_locked";
        /* REFUSED_OTHER also the default; -Wswitch still fires on a new one. */
        case MOS_TRAY_REFUSED_OTHER: default: return "refused_other";
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
        case MOS_ERR_UNSUPPORTED:       return "operation unsupported by this drive, driver, or build";
        case MOS_ERR_OOM:               return "out of memory";
        default:                        return "unknown error";
    }
}

/* MMC-6 §5.4 Feature Header Profile Codes. Names follow cdrom_id/udev
   lower_snake_case (cdrom_id's ID_CDROM_MEDIA_BD_R becomes "bd_r"). Unknown
   codes return NULL so the consumer falls back to hex. Ordered by numeric
   value for grep-against-spec. */
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
    /* MMC-6 Annex profile ranges. Mirrors the name table above: a profile
       named there without a class here is named-but-classless, which the
       profile_class_total_over_name_table test forbids. */
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
            return NULL;   /* no-profile, MO, legacy removable, or unknown */
    }
}

/* Map a kernel optical-media Type string (the IORegistry `kIO{CD,DVD,BD}Media
   TypeKey` = "Type", verbatim "BD-R" / "DVD-ROM" / … per IO{CD,DVD,BD}Media.h,
   verified through macOS 26.4) to a mos token. This is the zero-MMC media-type
   axis: present even when the MMC profile is suppressed off the not-ready
   branch, and finer than mos_profile_class (ROM-vs-recordable). Design:
   doc/research/2026-06-18-media-class-not-ready-fallback.md. Unknown / hostile
   strings return NULL (fail-closed, input-space layer 4). Tokens follow the
   mos_profile_name lower_snake_case convention; the kernel Type is coarser than
   the MMC profile (no DL / restricted-vs-sequential split), so this token set
   is its own. The schema enum (mos.state.v1 media_type) mirrors this table —
   the C↔schema drift guard keeps them in lockstep. */
const char *mos_internal_media_type_token(const char *kernel_type)
{
    if (!kernel_type) return NULL;
    static const struct { const char *kernel, *token; } map[] = {
        { "CD-ROM",     "cd_rom"       },
        { "CD-R",       "cd_r"         },
        { "CD-RW",      "cd_rw"        },
        { "DVD-ROM",    "dvd_rom"      },
        { "DVD-R",      "dvd_minus_r"  },
        { "DVD-RW",     "dvd_minus_rw" },
        { "DVD+R",      "dvd_plus_r"   },
        { "DVD+RW",     "dvd_plus_rw"  },
        { "DVD-RAM",    "dvd_ram"      },
        { "HD DVD-ROM", "hd_dvd_rom"   },
        { "HD DVD-R",   "hd_dvd_r"     },
        { "HD DVD-RW",  "hd_dvd_rw"    },
        { "HD DVD-RAM", "hd_dvd_ram"   },
        { "BD-ROM",     "bd_rom"       },
        { "BD-R",       "bd_r"         },
        { "BD-RE",      "bd_re"        },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(kernel_type, map[i].kernel) == 0) return map[i].token;
    return NULL;
}

/* True for current profiles whose media supports FORMAT UNIT — i.e. where READ
   FORMAT CAPACITIES (0x23) returns a meaningful formattable view: the
   rewritable optical profiles (CD-RW; DVD-RAM, DVD-RW RO/sequential, DVD+RW
   and +RW DL; HD DVD-RAM, HD DVD-RW and -RW DL; BD-RE) plus BD-R (formattable
   to pseudo-overwrite). Pressed (ROM), write-once sequential CD-R / DVD±R /
   HD DVD-R, and the no-media case report nothing to format, so
   mos_query_capacity gates the 0x23 read on this — for those profiles it issues
   no READ FORMAT CAPACITIES at all. The gate is purely SEMANTIC (only
   formattable media has a formattable view), not lock avoidance: 0x23 goes
   through the non-exclusive ReadFormatCapacities convenience method, which takes
   no exclusive access and so works on mounted media too. MMC-6 profile codes
   (§5.4); the formattable subset of mos_profile_class above. */
bool mos_internal_profile_is_formattable(uint16_t profile)
{
    switch (profile) {
        case 0x000A:  /* CD-RW                       */
        case 0x0012:  /* DVD-RAM                     */
        case 0x0013:  /* DVD-RW Restricted Overwrite */
        case 0x0014:  /* DVD-RW Sequential Recording */
        case 0x001A:  /* DVD+RW                      */
        case 0x002A:  /* DVD+RW Dual Layer           */
        case 0x0052:  /* HD DVD-RAM                  */
        case 0x0053:  /* HD DVD-RW                   */
        case 0x005A:  /* HD DVD-RW Dual Layer        */
        case 0x0041:  /* BD-R SRM                    */
        case 0x0042:  /* BD-R RRM (random recording) */
        case 0x0043:  /* BD-RE                       */
            return true;
        default:
            return false;
    }
}

/* Standard INQUIRY VERSION byte (byte 2) → SPC compliance token. Values from
   the Linux kernel scsi.h table (SCSI_SPC_* are resp[2]+1; the wire byte is
   one less). Unknown / legacy SCSI-1/2 values return NULL (numeric fallback). */
const char *mos_spc_version_name(uint8_t version)
{
    switch (version) {
        case 0x03: return "spc";
        case 0x04: return "spc_2";
        case 0x05: return "spc_3";
        case 0x06: return "spc_4";
        case 0x07: return "spc_5";
        default:   return NULL;   /* 0x00 none, legacy SCSI-1/2, or unknown */
    }
}

/* Version-descriptor code (INQUIRY bytes 58-73) → standard token. The
   "no version claimed" family codes from sg3_utils sg_version_descriptor_arr
   (the ones drives actually emit). A specific-revision or non-listed code
   returns NULL and is surfaced as hex (the unknown-code rule). Lower_snake. */
const char *mos_version_descriptor_name(uint16_t code)
{
    switch (code) {
        case 0x0020: return "sam";
        case 0x0040: return "sam_2";
        case 0x0060: return "sam_3";
        case 0x0080: return "sam_4";
        case 0x00A0: return "sam_5";
        case 0x00C0: return "sam_6";
        case 0x0120: return "spc";
        case 0x0140: return "mmc";
        case 0x0180: return "sbc";
        case 0x0240: return "mmc_2";
        case 0x0260: return "spc_2";
        case 0x02A0: return "mmc_3";
        case 0x0300: return "spc_3";
        case 0x0320: return "sbc_2";
        case 0x03A0: return "mmc_4";
        case 0x0420: return "mmc_5";
        case 0x0460: return "spc_4";
        case 0x04C0: return "sbc_3";
        case 0x04E0: return "mmc_6";
        case 0x05C0: return "spc_5";
        case 0x0600: return "sbc_4";
        case 0x1EA0: return "sat";
        case 0x1EC0: return "sat_2";
        case 0x1EE0: return "sat_3";
        case 0x1F00: return "sat_4";
        default:     return NULL;   /* per-revision / non-listed → hex fallback */
    }
}

/* Physical Format Information book-type codes (MMC-5 / Linux uapi dvd_layer
   values), shared by DVD and HD-DVD. Lower_snake_case; unknown codes return
   NULL for numeric fallback. The schema's book-type enum tracks this table
   (validate.py drift guard). */
const char *mos_book_type_name(uint8_t book_type)
{
    switch (book_type) {
        case 0x0: return "dvd_rom";
        case 0x1: return "dvd_ram";
        case 0x2: return "dvd_r";
        case 0x3: return "dvd_rw";
        case 0x4: return "hd_dvd_rom";
        case 0x5: return "hd_dvd_ram";
        case 0x6: return "hd_dvd_r";
        case 0x9: return "dvd_plus_rw";
        case 0xA: return "dvd_plus_r";
        case 0xD: return "dvd_plus_rw_dl";
        case 0xE: return "dvd_plus_r_dl";
        default:  return NULL;
    }
}

/* Track path: parallel (single-layer/sequential) vs opposite. Explicit
   returns, not a ternary, so the validate.py drift guard can harvest the
   token set. */
const char *mos_track_path_name(uint8_t track_path)
{
    switch (track_path & 0x01) {
        case 0:  return "ptp";
        default: return "otp";
    }
}

/* Copyright Protection System Type (READ DISC STRUCTURE format 0x01, CPST
   byte). Unknown/reserved codes return NULL. */
const char *mos_protection_name(uint8_t protection)
{
    switch (protection) {
        case 0x00: return "none";
        case 0x01: return "css_cppm";
        case 0x02: return "cprm";
        case 0x03: return "aacs";
        default:   return NULL;
    }
}

/* BG Format Status (READ DISC INFORMATION byte 7 bits 1:0). The 2-bit field
   is total, so the default is unreachable from mos_disc_info_bg_format_status
   (masked 0-3) but kept NULL for an out-of-range public-accessor call. Names
   track the Linux CDM_MRW_* macros (cdrom.h); explicit returns feed the
   validate.py drift guard. */
const char *mos_bg_format_status_name(uint8_t status)
{
    switch (status) {
        case 0:  return "none";       /* CDM_MRW_NOTMRW            */
        case 1:  return "inactive";   /* CDM_MRW_BGFORMAT_INACTIVE */
        case 2:  return "active";     /* CDM_MRW_BGFORMAT_ACTIVE   */
        case 3:  return "complete";   /* CDM_MRW_BGFORMAT_COMPLETE */
        default: return NULL;
    }
}

/* Current/Maximum Capacity Descriptor type (READ FORMAT CAPACITIES, byte 8
   bits 1:0). 0 is reserved → NULL (consumer falls back to the numeric code). */
const char *mos_format_capacity_type_name(uint8_t type)
{
    switch (type) {
        case 1:  return "unformatted";
        case 2:  return "formatted";
        case 3:  return "no_media";
        default: return NULL;
    }
}

/* Loading-mechanism type (MODE SENSE page 0x2A byte 6 bits 7:5). Explicit
   returns feed the validate.py drift guard; unknown/reserved codes NULL. */
const char *mos_loading_mechanism_name(uint8_t code)
{
    switch (code) {
        case 0:  return "caddy";
        case 1:  return "tray";
        case 2:  return "popup";
        case 4:  return "changer_disc";
        case 5:  return "changer_cartridge";
        default: return NULL;
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
 * `total` is the count we'd write with infinite cap; `pos` the count
 * actually written (bounded by cap-1 to leave room for NUL). Returning
 * `total` lets callers detect truncation via `return >= out_cap`. */

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
                    /* RFC 8259 requires escaping < 0x20. We also escape
                       0x7F (DEL) and bytes >= 0x80: INQUIRY fields aren't
                       guaranteed UTF-8, and escaping high bytes keeps the
                       output valid JSON in any encoding the consumer uses. */
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
            /* Printable ASCII only; everything else (control bytes, 0x7F,
               0x80+) renders as \xNN. Blocks terminal-control-sequence
               injection (ANSI escape, OSC 52 clipboard, cursor reports,
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

/* Render a whole-disk unit to its canonical "diskN" name (see mos.h).
   16 bytes always suffices ("disk" + 32-bit unit + NUL = 15 max). Returns
   false (and "" when cap > 0) for a no-media unit (< 0) or a buffer too
   small. */
bool mos_bsd_name_format(int64_t unit, char *buf, size_t cap)
{
    if (!buf || cap == 0) return false;
    /* The upper bound is correctness, not just truncation: a value in
       (UINT32_MAX, ~1e11) still fits 16 bytes and would emit a different,
       valid-looking "diskN". Refuse both < 0 and over-domain with "" + false. */
    if (unit < 0 || unit > (int64_t)UINT32_MAX) { buf[0] = 0; return false; }
    int n = snprintf(buf, cap, "disk%llu", (unsigned long long)unit);
    if (n <= 0 || (size_t)n >= cap) { buf[0] = 0; return false; }
    return true;
}

bool mos_bsd_dev_node(int64_t unit, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return false;
    out[0] = 0;
    /* Same domain/rationale as mos_bsd_name_format: a unit in
       (UINT32_MAX, ~1e14) still fits a generous buffer and would render a
       different, valid-looking node — refuse rather than emit one no real
       disk can have. */
    if (unit < 0 || unit > (int64_t)UINT32_MAX) return false;
    int n = snprintf(out, out_cap, "/dev/disk%lld", (long long)unit);
    if (n < 0 || (size_t)n >= out_cap) { out[0] = 0; return false; }
    return true;
}
