/*
 * mos_pure.h — prototypes and layouts for the pure-data functions. No
 * IOKit / CoreFoundation, so the parsers and state mapper are testable
 * without linking IOKit (mos_internal.h re-includes this).
 */

#ifndef MOS_PURE_H
#define MOS_PURE_H

#include <stdint.h>
#include <stdbool.h>

#include "mos.h"  /* for mos_state */

/* ---- Returned-object layouts (opaque in the public header) --------- *
 *
 * mos.h exposes mos_state_result / mos_watch_event as opaque typedefs +
 * accessors; the full layout lives here for the pure core, the Apple fill
 * paths, and tests. Callers see only accessors, so appending a field is
 * ABI-safe — additions go at the end. */
struct mos_state_result {
    mos_state state;
    int64_t        bsd_unit;      /* whole-disk unit; -1 = no whole-disk IOMedia node (media absent) */
    uint64_t       registry_id;   /* DRIVE service entry ID (attachment
                                     identity, same value the watch emits);
                                     0 == unavailable, set at open. */
    uint64_t       media_id;      /* whole-disk IOMedia entry ID; 0 == none.
                                     Internal: compared for equality to detect
                                     a same-state media swap, never emitted. */
    const char    *vendor;        /* INQUIRY, NULL if absent */
    const char    *product;       /* INQUIRY, NULL if absent */
    const char    *revision;      /* INQUIRY firmware revision, NULL if absent */
    uint16_t       current_profile;
    uint8_t        sense_key;
    uint8_t        asc;
    uint8_t        ascq;
    /* Drive Unit Serial Number (INQUIRY VPD 0x80), NULL if absent. The state
       path (mos_query_state) NEVER sets this — it stays NULL there so serial
       never leaks into mos.state.v1, which keeps a no-lock-on-READY shape (a
       raw INQUIRY would need a lock). Only the watch adapter fills it, grabbed
       once per session on the probe handle (mos_watch.c). Appended at the end:
       ABI-safe. */
    const char    *serial;
    /* Kernel optical-media "Type" token (kIO{CD,DVD,BD}MediaTypeKey mapped by
       mos_internal_media_type_token), NULL if absent. Read zero-MMC off the
       media node in refresh_media_identity, so it is present even when the MMC
       profile (and thus media_class) is suppressed off the not-ready branch —
       a loading/busy/unreadable result can still name the disc. Appended:
       ABI-safe (accessor-only). */
    const char    *media_type;
    /* Kernel IOMedia Writable flag (kIOMediaWritableKey), read zero-MMC off the
       same optical media node as media_type. Tri-state: -1 = no media node / key
       absent, 0 = read-only (ROM or write-protected), 1 = writable. A MECHANISM
       fact (the kernel's own bit), NOT a blank/appendable assertion — the precise
       blank/appendable/complete tri-state needs READ DISC INFORMATION, off the
       poll path by design. Appended: ABI-safe (accessor-only). */
    signed char    writable;
    /* GET PERFORMANCE (0xAC, Type 00h) read/write speeds (kB/s) and the read-
       direction descriptor count — MEDIA-DEPENDENT (the loaded disc's nominal
       performance). The state core (mos_query_state) NEVER fills these: like
       serial they stay zero there, so the no-lock-on-READY core query issues no
       GetPerformance. They are filled by the watch adapter (mos_watch.c, grabbed
       once per media identity and cached) as the conduit into the event; the
       one-shot `mos state` CLI reads mos_query_drive_perf directly. GetPerformance
       is a NON-exclusive convenience method, so no lock, no raw CDB. speed_count
       == 0 ⇒ absent. Plain scalars (no re-home). Appended: ABI-safe. */
    uint32_t       max_read_kbps;
    uint32_t       max_write_kbps;
    uint16_t       speed_count;
};

/* DRIFT GUARD (R3 brief 3). The watch re-homes every BORROWED pointer field of
   this struct (vendor/product/revision/serial) into watch-static storage before
   mos_close (mos_watch.c watch_probe + watch_slot_probe); a borrowed pointer that
   escapes un-re-homed is a post-close use-after-free. A size change here usually
   means a new field: if it is a borrowed `const char *`, add its re-home to BOTH
   probes and a deref to the ASan lifetime test, THEN bump this number. Static-token
   (media_type) and scalar fields need no re-home — bump only. */
_Static_assert(sizeof(struct mos_state_result) == 96,
    "mos_state_result changed: if the new field is a borrowed pointer, re-home it in "
    "mos_watch.c (watch_probe + watch_slot_probe) before bumping this size");

struct mos_watch_event {
    mos_event_kind kind;
    uint64_t       seq;
    char           ts[24];        /* RFC 3339 UTC, NUL-terminated */
    /* Session identity, constant for the stream's life: registry_id
       (attachment identity; IORegistry entry ID on the Apple adapter) and
       stream_open_wall_ms (wall epoch at open). The pair is unique per
       session; JSON carries them separately. */
    uint64_t       registry_id;
    uint64_t       stream_open_wall_ms;
    int64_t        bsd_unit;
    const char    *vendor;
    const char    *product;
    const char    *revision;
    /* Drive Unit Serial Number (INQUIRY VPD 0x80), NULL if absent. Adapter-
       owned, same pointer-lifetime invariant as vendor/product/revision
       (mos_watch.c re-homes it to watch-static storage before mos_close).
       NULL in early event lines until a free (empty/not-ready) poll grabs it,
       then stable for the session. Grouped with the other identity strings;
       the object is opaque (accessor-only), so field order is not ABI. */
    const char    *serial;
    mos_state state;
    mos_state prev_state;
    uint16_t       current_profile;
    uint8_t        sense_key;
    uint8_t        asc;
    uint8_t        ascq;
    mos_error      error;
    uint32_t       latency_ms;
    /* Kernel optical-media "Type" token (see mos_state_result.media_type),
       NULL if absent. Appended: ABI-safe (accessor-only). */
    const char    *media_type;
    /* Kernel IOMedia Writable flag (see mos_state_result.writable). Tri-state:
       -1 absent, 0 read-only, 1 writable. Appended: ABI-safe (accessor-only). */
    signed char    writable;
    /* GET PERFORMANCE read/write speeds (see mos_state_result). MEDIA-DEPENDENT;
       the watch grabs them once per media identity and caches (mos_watch.c), so
       they are NULL in early lines until the first ready poll for a disc lands
       the read, then stable until the next media change. Forbidden on
       error/device_removed events (like the other media fields). speed_count
       == 0 ⇒ absent. Plain scalars (no re-home). Appended: ABI-safe. */
    uint32_t       max_read_kbps;
    uint32_t       max_write_kbps;
    uint16_t       speed_count;
};

/* DRIFT GUARD (R3 brief 3): same rule as mos_state_result above — a new borrowed
   `const char *` on the event must be re-homed before mos_close and dereffed in
   the ASan lifetime test; bump this size only after that. */
_Static_assert(sizeof(struct mos_watch_event) == 144,
    "mos_watch_event changed: re-home any new borrowed pointer in mos_watch.c "
    "before bumping this size");

/* ---- Fixed-buffer capacities -------------------------------------- *
 *
 * MOS_BSD_NAME_CAP bounds a transient "diskN" (longest "disk4294967295",
 * 14 chars + NUL; 32 is ample). Drive identity is the int64 unit — these
 * names are parsed to a unit and discarded. */
#define MOS_BSD_NAME_CAP  32

/* ---- Sense parser (mos_sense.c) ------------------------------------ *
 *
 * Parse 18-byte fixed-format SCSI sense data into (sense_key, ASC, ASCQ).
 * Each output pointer is independently optional. */
void mos_internal_parse_sense(const uint8_t sense[18],
                              uint8_t *sk, uint8_t *asc, uint8_t *ascq);

/* Refine a tray-CLOSED, not-ready drive into its reason from the sense
   triple. Open/closed is already resolved (GESN door bit or the sense
   fork); never returns OPEN/EMPTY_OR_OPEN, UNKNOWN when the sense carries
   no meaning we assert. See mos_sense.c, ARCHITECTURE.md §5. */
mos_state mos_internal_state_from_sense_closed(uint8_t sk, uint8_t asc, uint8_t ascq);

/* ---- BSD name normalization (mos_pure.c) --------------------------- *
 *
 * Normalize any accepted form ("disk4", "rdisk4", "/dev/disk4",
 * "/dev/rdisk4") to the IOKit canonical "disk4". Returns a pointer into
 * the input. Extracted so tests verify the real rule, not a hand-copy. */
const char *mos_internal_normalize_bsd_name(const char *in);

/* True if the name is a whole-disk entry (diskN/rdiskN), not a partition
   (diskNsM). */
bool mos_internal_bsd_name_is_whole_shape(const char *bsd_name);

/* Parse any accepted whole-disk form to its unit (the N) as int64_t in
   [0, UINT32_MAX]; -1 for NULL/empty, a non-whole shape, or 32-bit
   overflow. The one place a BSD name becomes the stored integer identity.
   Pinned by tests/test_bsd_name.c. */
int64_t mos_internal_parse_bsd_unit(const char *name);

/* True if `reported` (e.g. "disk4" / "disk4s1") names whole-disk unit
   `whole_unit` or one of its partition children. Unit compared numerically
   (disk40 != unit 4), suffix validated as `(s<digits>)*`; false for NULL,
   whole_unit < 0, non-"disk" prefix, or malformed suffix. No in-tree
   consumer — kept as the pinned partition-child rule for future event
   filtering. */
bool mos_internal_bsd_unit_matches(const char *reported, int64_t whole_unit);

/* xnu mints IORegistry entry IDs from a monotone counter starting at
   2^32+256; CLI indexes are 1..MOS_CLI_LIST_CAP. The two all-digit selector
   spaces are disjoint by kernel construction, so a parsed value classifies
   deterministically. Pinned by tests/test_bsd_name.c. */
#define MOS_REGISTRY_ID_FLOOR ((1ULL << 32) + 256)
bool mos_internal_value_is_registry_id(uint64_t v);

/* ---- GET CONFIGURATION feature walk (mos_config.c) --------------- *
 *
 * One decoded MMC feature descriptor. `data` borrows into the caller's
 * response buffer (valid only while it is); `data_len` is the Additional
 * Length, already proven to fit. */
typedef struct {
    uint16_t       feature_code;
    bool           current;     /* byte 2, bit 0 */
    bool           persistent;  /* byte 2, bit 1 */
    uint8_t        version;     /* byte 2, bits 2..5 */
    const uint8_t *data;        /* feature payload, borrowed; NULL if none */
    uint8_t        data_len;    /* Additional Length, clamped to the buffer */
} mos_config_feature;

/* READ TOC/PMA/ATIP format 0000b response — the normalized table of
 * contents, THE disc-identity primitive (MusicBrainz/CDDB ids are pure
 * functions of these fields), read unprivileged via ReadTableOfContents.
 * FAIL-CLOSED: an out-of-range, duplicate, or non-ascending track rejects
 * the whole TOC — a half-parsed hostile TOC would be a falsely-stable
 * fingerprint. The header range is held to the same standard: first/last
 * coherent (1..99, not inverted), descriptors covering exactly first..last
 * (a truncating span is half-parsed, not padding). A TOC without a lead-out
 * parses (have_leadout=false); identity consumers must require it. Byte
 * layout at the decoder (mos_pure.c). */
#define MOS_TOC_MAX_TRACKS 99
typedef struct {
    uint8_t  track;        /* 1..99 */
    uint8_t  adr;          /* descriptor byte 1, high nibble */
    uint8_t  control;      /* descriptor byte 1, low nibble; bit2 = data */
    uint32_t start_lba;
} mos_toc_entry;

typedef struct mos_toc {   /* tagged: mos.h forward-declares it opaquely */
    uint8_t       first_track;
    uint8_t       last_track;
    uint8_t       track_count;
    bool          have_leadout;
    uint32_t      leadout_lba;
    mos_toc_entry tracks[MOS_TOC_MAX_TRACKS];
} mos_toc;

bool mos_internal_toc_parse(const uint8_t *buf, size_t len, mos_toc *out);


/* ---- CDTOC (kernel-cached full-TOC) session layout (mos_cdtoc.c) ------- *
 * Decode of the macOS kernel-cached full-TOC blob (kIOCDMediaTOCKey, the
 * Apple CDTOC struct in IOKit/storage/IOCDTypes.h) into per-session
 * boundaries — the multi-session structure the issued READ TOC format-0000b
 * (mos_internal_toc_parse) omits, and that disc_info gives only for the LAST
 * session. CD-only; the blob is read off the IOCDMedia node with zero SCSI
 * commands and no exclusive access (mos_scsi.c).
 *
 * CDTOC wire layout (IOCDTypes.h; libcdio lib/driver/osx.c read_toc_osx
 * cross-check in SPEC.md):
 *   [0..1]  length (BE) — bytes AFTER this field; tocSize = length + 2
 *   [2]     sessionFirst   (advisory; the descriptors are authoritative)
 *   [3]     sessionLast
 *   [4..]   CDTOCDescriptor, 11 bytes each:
 *             [0]      session number
 *             [1]      (adr<<4)|control  — adr in the high nibble, control in
 *                                          the low, on both endiannesses (the
 *                                          IOCDTypes bitfield is laid out to
 *                                          this byte order either way)
 *             [2]      tno
 *             [3]      point
 *             [4..6]   address MSF (ATIME; not decoded)
 *             [7]      zero
 *             [8..10]  p MSF = PMIN / PSEC / PFRAME
 * Per-session POINTs carry the boundaries: 0xA0 PMIN = first track, 0xA1 PMIN
 * = last track, 0xA2 p MSF = lead-out (→ LBA, minus the 150-frame pregap).
 * Only adr==1 descriptors bound a session (the libcdio filter). FAIL-CLOSED:
 * a device-reported length may only SHRINK the descriptor walk. */
#define MOS_SESSION_MAX 99
typedef struct {
    uint8_t  session;       /* session number, 1..99                          */
    bool     have_first;
    bool     have_last;
    bool     have_leadout;
    uint8_t  first_track;   /* POINT 0xA0 PMIN  (meaningful iff have_first)    */
    uint8_t  last_track;    /* POINT 0xA1 PMIN  (meaningful iff have_last)     */
    uint32_t leadout_lba;   /* POINT 0xA2 MSF→LBA (meaningful iff have_leadout)*/
} mos_session_entry;

typedef struct mos_session_layout {  /* tagged: mos.h forward-declares opaquely */
    uint8_t           count;         /* populated entries, ascending session    */
    mos_session_entry sessions[MOS_SESSION_MAX];
} mos_session_layout;

bool mos_internal_cdtoc_parse(const uint8_t *buf, size_t len,
                              mos_session_layout *out);

/* ---- ATIP decode (mos_atip.c) ------------------------------------- *
 *
 * Absolute Time In Pre-groove — the CD-R/RW manufacturer/media identity in
 * the disc's pre-groove, read via READ TOC/PMA/ATIP Format=0100b (the
 * convenience ReadTableOfContents mos already issues for the TOC). CD
 * recordable only; ROM/DVD/BD carry no ATIP. mos surfaces the RAW spec fields
 * only — the MID-to-manufacturer NAME table is the Orange Book's, curated and
 * consumer-side (the per-device table the hardware-role ADR forbids).
 * MMC-6 r02g §6.25, Table 488. */
typedef struct mos_atip {        /* tagged: mos.h forward-declares opaquely */
    bool    uru;            /* Unrestricted Use Disc bit (byte5 bit6)          */
    uint8_t disc_type;      /* byte6 bit6 (0 = CD-R, 1 = CD-RW)                */
    uint8_t disc_sub_type;  /* byte6 bits5-3 (speed/class subdivision)         */
    uint8_t reference_speed;/* byte4 bits2-0                                   */
    /* ATIP Start Time of Lead-in (M:S:F) — the manufacturer/MID identity. */
    uint8_t lead_in_min, lead_in_sec, lead_in_frame;
    /* Last Possible Start Time of Lead-out (M:S:F) — nominal capacity. */
    uint8_t lead_out_min, lead_out_sec, lead_out_frame;
} mos_atip;

/* Parse a READ TOC/PMA/ATIP Format=0100b reply (MMC-6 §6.25 Table 488) into
   *out. Returns true iff the reply is long enough to carry the ATIP descriptor
   through the lead-out time (response byte 14). The device-reported ATIP Data
   Length (bytes 0-1, BE) may only SHRINK the trusted region (dual-length rule);
   no payload byte is used as an offset. Fail-closed: a short/hostile reply
   returns false and leaves *out zeroed. Pure, no-OOB — fuzz/ASan-gated. */
bool mos_internal_atip_parse(const uint8_t *buf, size_t len, mos_atip *out);

/* Decode the SAME CDTOC blob into the per-track mos_toc (format-0000b's shape):
   first/last track, lead-out (the highest session's A2), and {track, adr,
   control, start_lba} per track. This is what lets the cached full-TOC be the
   PRIMARY CD TOC source (mos_query_toc). Fail-closed to the same standard as
   mos_internal_toc_parse — a duplicate track or a gap in first..last refuses
   the whole, so the caller falls back to the issued READ TOC. False when no
   coherent track list is present. */
bool mos_internal_cdtoc_to_toc(const uint8_t *buf, size_t len, mos_toc *out);


/* THE DUAL-LENGTH RULE (seam contract O-4; AGENTS scope doctrine layer 3).
 *
 * Every variable-size transfer has three lengths from three authorities:
 *   allocated   — bytes the CALLER allocated / requested (ours);
 *   transferred — bytes the TRANSPORT reports delivered (the kernel's
 *                 realizedByteCount; ours-adjacent);
 *   claimed     — bytes the DEVICE's own header says exist (hostile).
 *
 * The trusted parse region is min(allocated, transferred), computed ONCE
 * at the seam; the device claim is DATA that may only SHRINK that bound,
 * never set or grow it. This generalizes the sense buffer's fixed-18 rule
 * to drive-sized replies and forecloses the classic SCSI allocation-length
 * overread (header claims 0xFFFF over an 8-byte transfer) by construction.
 * `claimed` is uint64_t so header-derived totals (e.g. GET CONFIGURATION's
 * `Data Length + 4`) cannot wrap before the clamp. Pure and total:
 * pathological inputs yield a smaller (possibly zero) trusted length. Every
 * variable-size parse bound MUST derive from this — doc/seam-contract.md O-4. */
size_t mos_internal_trusted_len(size_t allocated, size_t transferred,
                                uint64_t claimed);

/* Bounds-safe pull-iterator over a GET CONFIGURATION response. `len` is the
   byte count you trust (sizeof your zero-init buffer; GetConfiguration
   reports no realized count). Init *cursor = 8 to skip the header. True +
   *out per in-bounds descriptor (advancing *cursor by >= 4); false at
   end-of-data or the first descriptor past the trusted region. Device
   lengths only shorten the walk. ASan/fuzz-checked. */
bool mos_internal_config_next_feature(const uint8_t *buf, size_t len,
                                      size_t *cursor, mos_config_feature *out);

/* First feature matching `feature_code`, via the walker (same trust
   bounds). False when absent or the walk fails closed. The v0.4 drive
   verb reads AACS (0x010D) presence/version through this. */
bool mos_internal_config_find_feature(const uint8_t *buf, size_t len,
                                      uint16_t feature_code,
                                      mos_config_feature *out);

/* Copy/content-protection capabilities the drive advertises in its (RT=0)
   GET CONFIGURATION feature list. PRESENCE of a protection feature means the
   drive CAN authenticate that scheme — a drive-static capability; it does NOT
   mean protected media is loaded (the per-feature Current bit, media-dependent,
   is deliberately ignored) nor that protection is enforced (region/key state
   lives behind REPORT KEY, which mos does not issue — scope doctrine). A modern
   BD drive advertises AACS+CSS at minimum, so the set reads as "schemes this
   drive speaks", not per-disc state. Version bytes are the drive's self-reported
   scheme version (0 when the scheme is absent or carries no version field).
   A version-carrying feature present but payload-truncated (< 4 bytes) reads as
   absent — fail closed, like the walker. SecurDisc/VCPS are presence-only (no
   version field, Additional Length 0). */
typedef struct mos_drive_protection {
    bool    css;                  /* feature 0106h present (DVD CSS/CPPM)      */
    uint8_t css_version;          /* CSS Version, payload byte 3              */
    bool    cprm;                 /* feature 010Bh present (DVD CPRM)         */
    uint8_t cprm_version;         /* CPRM version, payload byte 3             */
    bool    aacs;                 /* feature 010Dh present                    */
    uint8_t aacs_version;         /* AACS Version, payload byte 3            */
    bool    bus_encryption;       /* AACS BEC bit, payload byte 0 bit 1      */
    bool    write_bus_encryption; /* AACS WBE bit, payload byte 0 bit 2      */
    bool    securdisc;            /* feature 0113h present (presence only)   */
    bool    vcps;                 /* feature 0110h present (legacy, MMC-5)   */
} mos_drive_protection;

/* Write Protect Feature (0004h) CAPABILITY bits — what the drive can
   report/change, NOT per-disc write-protect state (that is mode page 1Dh /
   MECHANISM STATUS, which mos does not read). MMC-6 r02g §5.3.5 Table 101,
   descriptor payload byte 0: bit0 SSWPP, bit1 SPWP, bit2 WDCB, bit3 DWP. */
typedef struct mos_write_protect {
    bool present;   /* feature 0004h present in the RT=0 walk                 */
    bool sswpp;     /* supports the SWPP bit of the Timeout & Protect page    */
    bool spwp;      /* supports set/release of Persistent Write Protect       */
    bool wdcb;      /* supports the Write Inhibit DCB on DVD+RW               */
    bool dwp;       /* supports the Disc Write Protect PAC on BD-R/-RE        */
} mos_write_protect;

/* Drive-static facts from a full (RT=0) GET CONFIGURATION response. */
typedef struct mos_drive_caps {
    mos_drive_protection protection;
    mos_write_protect    write_protect;
    /* Supported-profile set from the Profile List feature (0x0000), drive-
       static (the per-descriptor CurrentP bit is media-dependent, ignored).
       64 covers a conformant max (one-byte Additional Length ⇒ ≤63 codes). */
    uint8_t  profile_count;
    uint16_t profiles[64];
    /* Firmware creation timestamp from the Firmware Information feature
       (010Ch), "YYYY-MM-DDTHH:MM:SSZ" (GMT) or "" when absent. 24 holds the
       20-char ISO form + NUL. */
    char     firmware_date[24];
    /* Logical Unit Serial Number from the Logical Unit Serial Number feature
       (0108h), same RT=0 reply. ASCII, trimmed; "" when the feature is absent
       or carries no serial. PRIMARY drive-serial source — non-exclusive, reads
       even on mounted media. 64 matches the serial width used elsewhere. */
    char     serial[64];
    /* Current Profile from the RT=0 reply header (the loaded medium's profile;
       0x0000 = no current profile / tray empty). MEDIA-DEPENDENT, unlike the
       rest of this struct — surfaced only so a caller can name the loaded
       disc's class (e.g. to scale speeds to a 1x multiple). */
    uint16_t current_profile;
} mos_drive_caps;

#define MOS_DRIVE_PROFILE_CAP 64u

/* Decode the content-protection features (CSS 0106h, CPRM 010Bh, AACS 010Dh,
   SecurDisc 0113h, VCPS 0110h) from a full (RT=0) GET CONFIGURATION reply into
   out->protection. This zero-inits the WHOLE mos_drive_caps first (it is the
   struct's initializer; the profile/firmware-date decoders fill the rest), so
   an absent feature leaves its fields false/0. Pure, no-OOB — fuzz/ASan-gated. */
void mos_internal_protection_from_config(const uint8_t *buf, size_t len,
                                         mos_drive_caps *out);

/* Decode the Write Protect Feature (0004h) CAPABILITY bits into
   out->write_protect from a full (RT=0) GET CONFIGURATION reply. Does NOT
   zero-init (called after mos_internal_protection_from_config, which does).
   MMC-6 r02g §5.3.5 Table 101: descriptor payload byte 0 carries SSWPP/SPWP/
   WDCB/DWP; a present-but-truncated payload (data_len < 1) reads as absent
   (fail closed, like the protection decoders). These are capability bits — a
   drive's ability to report/change write protect — NOT per-disc write-protect
   state. Pure, no-OOB — fuzz/ASan-gated. */
void mos_internal_write_protect_from_config(const uint8_t *buf, size_t len,
                                            mos_drive_caps *out);

/* Decode the Profile List feature (0x0000) into out_codes[0..cap), setting
   *out_count. Each descriptor is 4 bytes: [0..1] Profile Number (BE),
   [2] bit0 CurrentP (media-dependent — NOT recorded), [3] reserved. Bounded
   by the feature's Additional Length and cap; a trailing partial descriptor
   is ignored. Pure, no-OOB — fuzz/ASan-gated. */
void mos_internal_profile_list_from_config(const uint8_t *buf, size_t len,
                                           uint16_t *out_codes, uint8_t cap,
                                           uint8_t *out_count);

/* Decode the Firmware Information feature (010Ch) into out as an ISO-8601 GMT
   timestamp "YYYY-MM-DDTHH:MM:SSZ" (out_cap >= 21), or out[0]=0 when the
   feature is absent / malformed. Payload (after the 4-byte feature header,
   MMC-6 r02g §5.3.43 Table 197): Century[2] Year[2] Month[2] Day[2] Hour[2]
   Minute[2] Second[2] Reserved[2], all decimal ASCII; non-digit bytes are
   rejected (out empty). Pure, no-OOB — fuzz/ASan-gated. */
void mos_internal_firmware_date_from_config(const uint8_t *buf, size_t len,
                                            char *out, size_t out_cap);

/* Decode the Logical Unit Serial Number feature (0108h) into out as an ASCII
   serial (out_cap >= 1), or out[0]=0 when the feature is absent / empty. The
   descriptor payload is the serial; trailing space/NUL padding trimmed, an
   interior NUL or over-length refused (complete-or-unavailable, like the
   VPD-0x80 parser). PRIMARY serial source. Pure, no-OOB — fuzz/ASan-gated. */
void mos_internal_serial_from_config(const uint8_t *buf, size_t len,
                                     char *out, size_t out_cap);

/* ---- Standard INQUIRY decode (mos_inqdata.c) ----------------------- *
 *
 * The drive's self-reported identity from STANDARD INQUIRY data (EVPD=0):
 * vendor/product/revision AND the SPC version + version-descriptor list (the
 * standards it claims). `mos drive` issues this raw read for the canonical
 * truth and prefers it over the DiscRecording cache (DR is fallback). The
 * convenience Inquiry returns only the 36-byte header, so the descriptors
 * (bytes 58-73) need allocation length >= 74 (mos_drive_inquiry.c).
 * descriptors are up to eight 2-byte BE codes, a 0x0000 slot = "none"
 * (skipped). New fields append at the END. */
typedef struct mos_drive_inquiry {
    /* Drive-reported identity, fresh from the wire (SPC-4 field widths,
       trailing-space trimmed); "" when the reply was too short to carry it. */
    char     vendor[9];          /* bytes 8-15  */
    char     product[17];        /* bytes 16-31 */
    char     revision[5];        /* bytes 32-35 */
    uint8_t  spc_version;        /* INQUIRY byte 2 (SPC compliance level) */
    uint8_t  descriptor_count;   /* non-zero version-descriptor codes     */
    uint16_t descriptors[8];     /* bytes 58-73, BE; 0x0000 slots skipped */
} mos_drive_inquiry;

/* Decode standard INQUIRY data into *out. True when the reply has at least
   the 5-byte header (through Additional Length); identity (vendor/product/
   revision) and the descriptor region are each bounded by both `len` and the
   reply's own Additional Length (byte 4, dual-length rule O-4) — a field not
   covered by the reply stays "". Pure, no-OOB — fuzz/ASan-gated. */
bool mos_internal_inqdata_parse(const uint8_t *buf, size_t len,
                               mos_drive_inquiry *out);

/* One feature for the public enumeration (mos_enumerate_features) —
   descriptor header facts only; payload bytes stay internal (a typed decode
   like the AACS caps is how payload facts go public). No internal typedef
   alias: mos.h owns the sole typedef, like struct mos_device_info. */
struct mos_feature_info {
    uint16_t code;
    bool     current;
    bool     persistent;
    uint8_t  version;
};

/* Current Profile from a GET CONFIGURATION response; false ("no
   profile") unless the reply's own Data Length covers the field, so a
   truncated reply is never read as profile 0x0000 (= "no media").
   Layout and gating at the decoder (mos_config.c). */
bool mos_internal_config_current_profile(const uint8_t *buf, size_t len,
                                         uint16_t *profile);

/* ---- READ DISC INFORMATION decode (mos_discinfo.c) --------------- *
 *
 * Disc status from MMC READ DISC INFORMATION (0x51) standard response —
 * the completion signal: Complete = finalized readable content, Blank =
 * nothing to rip. byte 2 carries status/last-session/erasable; the
 * session/track counts split LSB/MSB across bytes 4..11. The
 * mos_disc_status enum is public (mos.h); the struct stays internal, read
 * through the mos_result.c accessors. */
struct mos_disc_info {
    mos_disc_status status;              /* byte 2, bits 1:0 */
    uint8_t  last_session_state;         /* byte 2, bits 3:2: 0 empty,
                                            1 incomplete, 2 damaged, 3 complete */
    bool     erasable;                   /* byte 2, bit 4 */
    uint8_t  first_track_on_disc;        /* byte 3 */
    uint16_t number_of_sessions;         /* byte 9 (MSB) : byte 4 (LSB) */
    uint16_t first_track_last_session;   /* byte 10 : byte 5 */
    uint16_t last_track_last_session;    /* byte 11 : byte 6 */
    uint8_t  bg_format_status;           /* byte 7 bits 1:0: background-format
                                            state — 0 none, 1 inactive,
                                            2 active, 3 complete (Linux
                                            CDM_MRW_* macros) */
    uint8_t  disc_type;                  /* byte 8: 0x00 CD-DA/CD-ROM,
                                            0x10 CD-I, 0x20 CD-ROM XA,
                                            0xFF undefined */
    bool     disc_id_valid;              /* byte 7 bit 7 (DID_V) AND bytes
                                            12..15 present in trusted region */
    uint32_t disc_id;                    /* bytes 12..15 (BE), valid iff
                                            disc_id_valid — the writer-assigned
                                            32-bit Disc Identification */
    bool     bar_code_valid;             /* byte 7 bit 6 (DBC_V) AND bytes
                                            24..31 present in trusted region */
    uint8_t  bar_code[8];                /* bytes 24..31, valid iff bar_code_valid */
};

/* Decode a READ DISC INFORMATION (0x51, data type 000b) response into *out.
 * True only when the fixed numeric region (through byte 11) is present per
 * BOTH `len` and the reply's declared length. disc_type (byte 8) is always
 * decoded; the validity-gated 32-bit Disc Identification (bytes 12..15,
 * DID_V) and Disc Bar Code (bytes 24..31, DBC_V) are decoded only when both
 * their validity bit is set AND the trusted region reaches them — otherwise
 * the *_valid flag is false. Lead-in/lead-out addresses remain undecoded.
 * Layout and safety contract at the decoder (mos_discinfo.c). */
bool mos_internal_disc_info_parse(const uint8_t *buf, size_t len,
                                  mos_disc_info *out);

/* ---- READ DISC STRUCTURE / BD Disc Information decode (mos_discstruct.c) -- *
 *
 * The disc's REGISTERED identity from a Blu-ray Disc Information (DI)
 * reply (READ DISC STRUCTURE 0xAD, BD media type, format 0x00): Disc
 * Manufacturer ID, Media Type ID, Product Revision. Fixed-width ASCII read
 * at CONSTANT offsets in the first DI unit; no device value is ever used
 * as offset or length (the structure-data-length header can only shrink
 * the trusted region). Bytes copied verbatim into fixed buffers (CLI
 * escapes at emit, like INQUIRY); classification ("MILLEN" => M-DISC) is
 * the consumer's. NUL-terminated, space-padding stripped; "" when DI
 * absent. New fields append at the END. */
struct mos_disc_id {
    char disc_type[4];      /* DI+8,   3 bytes + NUL: "BDR"/"BDW"/"BDO" */
    char manufacturer[7];   /* DI+100, 6 bytes + NUL */
    char media_type[4];     /* DI+106, 3 bytes + NUL */
    char revision[2];       /* DI+111, 1 byte  + NUL */
};

/* Parse a BD DI reply into *out. True only when the 'DI' signature is
 * present AND the trusted region (min of `len` and the declared length)
 * reaches the product-revision byte; else false (*out emptied). Pure,
 * fixed-offset, no-OOB — fuzz/ASan-gated. */
bool mos_internal_bd_disc_id_parse(const uint8_t *buf, size_t len,
                                   struct mos_disc_id *out);

/* ---- READ TOC/PMA/ATIP format 0101b (CD-TEXT) decode (mos_cdtext.c) --- *
 *
 * The disc-level (album) Title and Performer from a CD-TEXT reply — the
 * "which album is in the drive" disambiguator, parallel to the mounted
 * volume name. Decoded from the FIRST language block (block 0), single-byte
 * charset; a double-byte (DBCC) album field reads as "", never mis-decoded.
 * Bytes copied verbatim into fixed buffers (CLI escapes at emit); the
 * CD-TEXT Data Length can only shrink the trusted span. BEST-EFFORT DISPLAY
 * TEXT, not a fingerprint — audio-CD dedup keys ride on the fail-closed
 * TOC. Other field types and language blocks not decoded. New fields
 * append at the END. */
#define MOS_CDTEXT_STR_CAP        160u
#define MOS_CDTEXT_MAX_TRACKS      99u
#define MOS_CDTEXT_TRACK_TITLE_CAP 64u
struct mos_cdtext {
    bool    have;                       /* a non-empty album field present */
    char    title[MOS_CDTEXT_STR_CAP];     /* album Title (track 0, block 0); "" if absent */
    char    performer[MOS_CDTEXT_STR_CAP]; /* album Performer; "" if absent   */
    /* Per-track titles (pack 0x80) / performers (pack 0x81), block 0,
       indexed by track: track_titles[n-1] / track_performers[n-1] are
       track n's strings ("" if absent — the arrays are independently
       sparse, e.g. a various-artists disc carries per-track performers).
       track_count is the highest track with EITHER a non-empty title or
       performer; entries above it are unset. */
    uint8_t track_count;
    char    track_titles[MOS_CDTEXT_MAX_TRACKS][MOS_CDTEXT_TRACK_TITLE_CAP];
    char    track_performers[MOS_CDTEXT_MAX_TRACKS][MOS_CDTEXT_TRACK_TITLE_CAP];
};

/* Parse a CD-TEXT (format 0101b) reply into *out. True only when at least
 * one non-empty album-level field (Title or Performer) was decoded within
 * the trusted region (min of `len` and the declared length); else false
 * (*out emptied). Pure, no-OOB, every string NUL-terminated — fuzz/ASan-
 * gated by tests/test_cdtext.c. */
bool mos_internal_cdtext_parse(const uint8_t *buf, size_t len,
                               struct mos_cdtext *out);

/* ---- READ DISC STRUCTURE / physical structure decode (mos_physstruct.c) --- *
 *
 * Physical Format Information (READ DISC STRUCTURE 0xAD, DVD/HD-DVD media
 * type, format 0x00) and Copyright Management Information (format 0x01).
 * "Physical structure" not "DVD": media-type-0 carries HD-DVD book types
 * (0x4..0x6) too. Physical fields are reported geometry (book type, layer
 * layout, data-area boundaries — end_sector_l0 is the layer break);
 * copyright fields are protection-system type and region mask. All at
 * CONSTANT offsets; the structure-data-length header can only shrink the
 * trusted region. Classification is the consumer's. have_physical /
 * have_copyright say which half merged in. New fields append at the END. */
struct mos_physical_structure {
    bool     have_physical;     /* format 0x00 was parsed */
    uint8_t  book_type;         /* base[0] 7:4  (0 DVD-ROM, 2 DVD-R, ...) */
    uint8_t  part_version;      /* base[0] 3:0  */
    uint8_t  disc_size;         /* base[1] 7:4  (0 120mm, 1 80mm) */
    uint8_t  max_rate;          /* base[1] 3:0  */
    uint8_t  layer_type;        /* base[2] 3:0  */
    uint8_t  track_path;        /* base[2] bit4 (0 ptp, 1 otp) */
    uint8_t  num_layers;        /* base[2] 6:5 + 1 (1 or 2 layers) */
    uint8_t  linear_density;    /* base[3] 7:4  */
    uint8_t  track_density;     /* base[3] 3:0  */
    bool     bca;               /* base[16] bit7 */
    uint32_t start_sector;      /* base[5..7]   24-bit PSN of data area */
    uint32_t end_sector;        /* base[9..11]  24-bit end PSN */
    uint32_t end_sector_l0;     /* base[13..15] 24-bit layer-0 end (break) */

    bool     have_copyright;    /* format 0x01 was parsed */
    uint8_t  protection;        /* CPST: 0 none, 1 CSS/CPPM, 2 CPRM, 3 AACS */
    uint8_t  region;            /* RMI region-management mask */
};

/* Parse the Physical Format Information (0x00) / Copyright Management
 * Information (0x01) halves into *out. Each sets its own have_* flag and
 * fills its own fields; the adapter zero-inits once and calls both. True
 * only when the trusted region (min of `len` and the declared length)
 * reaches the last needed byte. Pure, fixed-offset, no-OOB — fuzz/ASan-
 * gated. */
bool mos_internal_physical_format_parse(const uint8_t *buf, size_t len,
                                        struct mos_physical_structure *out);
bool mos_internal_copyright_mgmt_parse(const uint8_t *buf, size_t len,
                                       struct mos_physical_structure *out);

/* ---- READ TRACK INFORMATION decode (mos_trackinfo.c) --------------- *
 *
 * The capacity / append-state surface of one track from READ TRACK
 * INFORMATION (0x52): track start, next writable address, free blocks,
 * track size, last recorded address, plus track/data mode and blank/damage
 * bits. next_writable / last_recorded are meaningful only when nwa_valid /
 * lra_valid are set. All at CONSTANT offsets; the Track Information Length
 * header can only shrink the trusted region. New fields append at the END. */
struct mos_track_info {
    uint16_t track_number;     /* byte 2 (+ byte 32 MSB on long replies) */
    uint16_t session_number;   /* byte 3 (+ byte 33 MSB) */
    uint8_t  track_mode;       /* byte 5 bits 3:0 (Q-channel control) */
    uint8_t  data_mode;        /* byte 6 bits 3:0 */
    bool     blank;            /* byte 6 bit 6 */
    bool     damage;           /* byte 5 bit 5 */
    bool     nwa_valid;        /* byte 7 bit 0 */
    bool     lra_valid;        /* byte 7 bit 1 */
    uint32_t track_start;      /* bytes 8..11  */
    uint32_t next_writable;    /* bytes 12..15 (valid iff nwa_valid) */
    uint32_t free_blocks;      /* bytes 16..19 */
    uint32_t track_size;       /* bytes 24..27 */
    uint32_t last_recorded;    /* bytes 28..31 (valid iff lra_valid) */
};

/* Parse a Track Information Block into *out. True only when the trusted
 * region (min of `len` and the declared length) reaches the Last Recorded
 * Address (byte 31). Pure, fixed-offset, no-OOB — fuzz/ASan-gated. */
bool mos_internal_track_info_parse(const uint8_t *buf, size_t len,
                                   struct mos_track_info *out);

/* ---- Disc capacity (assembled, no command) ------------------------- *
 *
 * mos_capacity has NO pure decoder — no capacity reply to parse. The shell
 * assembles it from two sources it already holds:
 *   - the whole-disk IOMedia node's kernel-cached size/block size
 *     (kIOMediaSizeKey / kIOMediaPreferredBlockSizeKey) — the kernel's
 *     attach-time READ CAPACITY result, a registry read with no SCSI
 *     command and no exclusive access (so it works on MOUNTED media, where
 *     a raw READ CAPACITY would BUSY); and
 *   - the recordable / append-state view from READ TRACK INFORMATION.
 * media_bytes / block_bytes are 0 when the node carries no size (blank or
 * absent media); have_recordable is false when TRACK INFORMATION didn't
 * answer. New fields append at the END. */
/* ---- READ FORMAT CAPACITIES decode (mos_formatcap.c) --------------- *
 *
 * The formattable view of the loaded medium from READ FORMAT CAPACITIES
 * (MMC 0x23): the Current/Maximum Capacity Descriptor (how big it is now and
 * whether it is unformatted/formatted) plus the Formattable Capacity
 * Descriptor list — the capacities the drive could FORMAT it to (DVD-RAM's
 * size choices, BD-RE format types). The gap mos_query_capacity's other two
 * sources can't fill on blank rewritable media (no whole-disk node yet, no
 * track to read). Read-only: mos reports formattable capacities, it never
 * issues FORMAT UNIT. Issued via the ReadFormatCapacities convenience method
 * (MMCDeviceInterface) — NOT a raw CDB (correcting the earlier "fifth raw
 * verb" call; the wrapper exists in SCSITaskLib.h — see the AGENTS.md ADR +
 * doc/research/2026-06-18-readformatcapacities-convenience-exists.md). A
 * capture falsifies per the hardware ADR, never steers. */
/* Stored Formattable Capacity Descriptors. The reply's CAPACITY LIST LENGTH
   is a single byte (<= 255), so a conforming drive can list at most
   floor(255/8) - 1 = 30 formattable descriptors; 32 gives slack so a valid
   reply is never truncated, and the parser's cap is then a pure array-bounds
   guard (unreachable on conformant input, defensive against a hostile len). */
#define MOS_FORMATTABLE_MAX 32

struct mos_format_descriptor {
    uint32_t blocks;       /* Number of Blocks (bytes 0-3)                  */
    uint32_t param;        /* Type Dependent Parameter (bytes 5-7); the
                              block length for the common format types      */
    uint8_t  format_type;  /* Format Type (byte 4 bits 7:2)                 */
};

struct mos_format_caps {
    uint8_t  cur_type;        /* Current/Max Descriptor Type (byte 8 bits 1:0):
                                 1 unformatted, 2 formatted, 3 no media     */
    uint32_t cur_blocks;      /* Current/Max Number of Blocks               */
    uint32_t cur_block_bytes; /* Current/Max Block Length (bytes 9-11)      */
    uint8_t  count;           /* Formattable descriptors stored (<= MAX)    */
    struct mos_format_descriptor d[MOS_FORMATTABLE_MAX];
};

struct mos_capacity {
    uint64_t media_bytes;     /* kIOMediaSizeKey; 0 == no whole-disk size  */
    uint32_t block_bytes;     /* kIOMediaPreferredBlockSizeKey; 0 == none  */
    bool     have_recordable; /* READ TRACK INFORMATION answered           */
    bool     nwa_valid;       /* next_writable meaningful (TI reply bit)    */
    uint32_t free_blocks;     /* recordable free space (blocks)            */
    uint32_t next_writable;   /* append point (valid iff nwa_valid)        */
    uint32_t track_size;      /* first-track size (blocks); pressed-disc
                                 capacity for single-track media           */
    /* READ FORMAT CAPACITIES (0x23) — ReadFormatCapacities convenience read,
       media-dependent; stays unset (have_formattable=false) on non-formattable
       media or a unit that rejects 0x23. Appended after the original fields
       (mos_capacity is accessor-only across ABI). */
    bool                   have_formattable;
    struct mos_format_caps formattable;
};

/* Decode a READ FORMAT CAPACITIES (0x23) reply: the Capacity List header, the
   Current/Maximum Capacity Descriptor, and up to MOS_FORMATTABLE_MAX
   Formattable Capacity Descriptors. Pure, fixed-offset, no-OOB (fuzz/ASan-
   gated). Bounds the Capacity List Length to the provided buffer length
   (dual-length rule O-4) and to whole 8-byte descriptors; returns false on a
   reply too short to carry the header + Current/Max descriptor, or an
   incoherent (non-multiple-of-8) list. MMC-6 §6.24. */
bool mos_internal_format_caps_parse(const uint8_t *buf, size_t len,
                                    struct mos_format_caps *out);

/* True for current profiles whose media supports FORMAT UNIT — the gate for
   issuing READ FORMAT CAPACITIES (0x23) at all (mos_strings.c). Rewritable
   optical + BD-R; pressed and write-once CD-R/DVD±R and no-media are false, so
   capacity issues no read for them. MMC-6 §5.4. */
bool mos_internal_profile_is_formattable(uint16_t profile);

/* Map a kernel optical-media "Type" string (kIO{CD,DVD,BD}MediaTypeKey) to a
   mos token (cd_rom … bd_re), or NULL for unknown/hostile/NULL input. The
   zero-MMC media-type axis (present even when the MMC profile is suppressed off
   the not-ready branch; finer than mos_profile_class). Design:
   doc/research/2026-06-18-media-class-not-ready-fallback.md. */
const char *mos_internal_media_type_token(const char *kernel_type);

/* DiscRecording physical-interconnect tokens (pure). The DR adapter
   (mos_dr.c) maps the kDRDevicePhysicalInterconnect{,Location}Key CFString
   values to these small int codes; this function names them. Kept pure +
   here so the schema's interconnect enums are drift-guarded against the C
   token set (schemas/validate.py). NULL for code 0 (absent / unrecognized) so
   an unknown bus omits the field rather than inventing a token.
     interconnect: 1 atapi, 2 fibre_channel, 3 firewire, 4 usb, 5 scsi
     location:     1 internal, 2 external, 3 unknown                       */
const char *mos_internal_interconnect_token(int code);
const char *mos_internal_interconnect_location_token(int code);

/* ---- GET PERFORMANCE performance-data decode (mos_perf.c) ---------- *
 *
 * The drive's read/write performance from GET PERFORMANCE (0xAC, Type 00h
 * Performance Data — the type Apple exposes), summarized: max read and max
 * write (kB/s) and the descriptor count. The split is the CDB WRITE bit, so
 * the adapter issues the command twice and fills this from the two replies.
 * have is false when neither direction returned a descriptor (media-
 * dependent — data, not error). Spec-derived layout; a capture falsifies
 * per the hardware ADR. New fields append at the END. */
struct mos_drive_perf {
    bool     have;              /* >= 1 descriptor in either direction */
    uint16_t speed_count;       /* read-direction descriptor count      */
    uint32_t max_read_kbps;     /* max performance, WRITE=0 reply       */
    uint32_t max_write_kbps;    /* max performance, WRITE=1 reply       */
};

/* Decode one Performance Data reply: max performance (kB/s) across its
 * Nominal Performance Descriptors and the count. True when the 8-byte
 * header is present and coherent (list may be empty). Pure, fixed-offset,
 * no-OOB — fuzz/ASan-gated. The adapter assembles the struct above from
 * two calls (WRITE=0 / WRITE=1). */
bool mos_internal_perf_data_parse(const uint8_t *buf, size_t len,
                                  uint32_t *max_kbps, uint16_t *count);

/* ---- MODE SENSE(10) page decode (mos_modepage.c) ------------------- *
 *
 * Two read-only optical-specific pages: page 0x2A (mechanical: loading
 * mechanism, eject/lock support, the live locked bit, buffer size) and
 * page 0x01 (read error-recovery config). Decoded by a bounded page
 * walker; the device lengths (mode data, block descriptor, per-page) can
 * only shrink the trusted region, and the walk strictly advances. New
 * fields append at the END. */
struct mos_mode_caps {
    bool     have;              /* page 0x2A was present */
    uint8_t  loading_mechanism; /* page[6] 7:5 (0 caddy,1 tray,2 popup,...) */
    bool     can_eject;         /* page[6] bit 3 */
    bool     lock_supported;    /* page[6] bit 1 */
    bool     locked;            /* page[6] bit 2 (live state) */
    uint16_t buffer_kb;         /* page[12..13] BE, KB */
};

struct mos_error_recovery {
    bool    have;               /* page 0x01 was present */
    bool    awre;               /* page[2] bit 7: auto write reallocation */
    bool    arre;               /* page[2] bit 6: auto read reallocation */
    bool    per;                /* page[2] bit 2: post error */
    bool    dcr;                /* page[2] bit 0: disable correction */
    uint8_t read_retry_count;   /* page[3] */
};

/* Parse MODE SENSE(10) for page 0x2A / 0x01 into *out. True only when the
 * page is present and long enough for the fields read. Pure, bounded, no-OOB
 * — fuzz/ASan-gated. */
bool mos_internal_mode_caps_parse(const uint8_t *buf, size_t len,
                                  struct mos_mode_caps *out);
bool mos_internal_error_recovery_parse(const uint8_t *buf, size_t len,
                                       struct mos_error_recovery *out);

/* ---- SCSI task status classification (mos_pure.c) ----------------- *
 *
 * True for the four SAM-5 status values meaning "drive contended, retry
 * later." Shared with test_scsi_status.c so the test exercises the real
 * disjunction, not a mirror. */
bool mos_internal_status_is_contended(uint32_t status);

/* ---- Tray-command outcome classification (mos_pure.c) ------------- *
 *
 * Classify a tray CDB's (task status, sense triple) into mos_tray_outcome:
 * GOOD -> DONE, 5/53/02 -> REFUSED_LOCKED, any other non-GOOD ->
 * REFUSED_OTHER. Pure; pinned by tests/test_tray.c. The caller has already
 * mapped transport/lock failure to a negative mos_error — this only runs on
 * an answered command. */
mos_tray_outcome mos_internal_tray_classify(uint32_t scsi_status,
                                            uint8_t sk, uint8_t asc, uint8_t ascq);

/* ---- GET EVENT STATUS NOTIFICATION media decode (mos_pure.c) ------- *
 *
 * Pull the authoritative Door/Tray-open bit from a raw GESN (0x4A) Media-
 * class polled reply. True + *door_open only for a valid, NEA-clear,
 * full-span Media descriptor; false = no authoritative bit (caller forks
 * on sense). Validity gates at the decoder; ARCHITECTURE.md §4.2. */
bool mos_internal_gesn_media_door_open(const uint8_t *resp, size_t len,
                                       bool *door_open);

/* ---- IOReturn → mos_error mapping (mos_pure.c) -------------------- *
 *
 * Translate an IOReturn (any int32_t mach-style code) to a mos_error. Takes
 * int32_t so it stays pure; the Apple adapter casts at the call site and
 * static_asserts the SDK constants (mos_scsi.c). Every IOReturn-handling
 * site routes through here — notably NoDevice/NotAttached must map to
 * MOS_ERR_NO_DEVICE for the watch's terminal-removal path. Groupings at the
 * mapper. */
mos_error mos_internal_ioreturn_to_error(int32_t rc);

/* ---- Decision-tree core (mos_state_core.c) ------------------------- *
 *
 * mos_query_state()'s decision tree runs against this vtable: the Apple
 * adapter (mos_state.c) fills it from a real handle, tests script it. Each
 * callback returns MOS_OK and fills its out-parameter, or a negative
 * mos_error. Division of labour — TUR owns presence, the raw-GESN tray op
 * owns the tray bit (never the masking GetTrayState, ARCHITECTURE.md §9.7),
 * profile is READY-only enrichment — is at the tree. sense is fixed-format
 * 18 bytes (SPC-4 §4.5.3). */
typedef struct {
    mos_error (*test_unit_ready)    (void *ctx, uint32_t *status,
                                                uint8_t sense[18]);
    mos_error (*get_tray_state)     (void *ctx, bool *tray_open);
    mos_error (*get_current_profile)(void *ctx, uint16_t *profile);
} mos_mmc_ops_t;

/* Identification metadata propagated into mos_state_result with no IOKit
   handle involved. The Apple adapter fills these from mos_handle_t buffers;
   tests use string literals. Lifetime is the caller's — the core does not
   copy, just propagates. bsd_unit is -1 for an empty/open-tray drive (no
   resolvable name). */
typedef struct {
    const mos_mmc_ops_t *ops;
    void *ctx;
    int64_t bsd_unit;
    uint64_t registry_id;        /* drive SERVICE entry ID — the attachment
                                    identity the watch emits; 0 == unavailable,
                                    propagated verbatim into out->registry_id. */
    uint64_t media_id;           /* whole-disk IOMedia entry ID, 0 == none */
    const char *vendor;          /* may be NULL */
    const char *product;         /* may be NULL */
    const char *revision;        /* may be NULL */
} mos_state_env_t;

mos_error mos_internal_query_state_core(const mos_state_env_t *env,
                                        mos_state_result *out);

/* ---- Watch core (mos_watch_core.c) ---------------------------------- *
 *
 * Pure watch state machine: tracks prev/current state, emits event
 * decisions, schedules backoff polls. The Apple-side mos_watch.c drives it
 * with real probes; tests drive it with fake probes and a fake clock. The
 * pure core does NO IOKit notifications — that's an Apple-side optimization;
 * the pure core is correct poll-only on its own. */

typedef struct {
    /* MOS_OK + filled out on success; negative mos_error on failure (the
       core decides emit-error vs retry). MOS_ERR_NO_DEVICE means terminal
       removal — the device has gone away. */
    mos_error (*probe)(void *ctx, mos_state_result *out);

    /* MONOTONIC ms, for scheduling and latency only. MUST be monotonic
       (CLOCK_MONOTONIC / mach_absolute_time). Separate from wall_ms so a
       clock-domain mixup can't happen at wiring time — the two aren't
       interchangeable, a mixup puts the first poll decades out. */
    uint64_t  (*mono_ms)(void *ctx);

    /* WALL-CLOCK ms since Unix epoch, for the session-open timestamp and
       event ts (RFC 3339) only. MUST NOT schedule — it can jump backward on
       clock adjustments, NTP steps, or DST. */
    uint64_t  (*wall_ms)(void *ctx);
} mos_watch_ops_t;

/* Decision an event-pump loop should take. */
typedef enum {
    MOS_WATCH_DECIDE_EMIT_EVENT  = 1,
    MOS_WATCH_DECIDE_SLEEP_UNTIL = 2, /* sleep until next_poll_at_ms */
    MOS_WATCH_DECIDE_TERMINAL    = 3, /* device removed; stop pumping */
} mos_watch_decision_kind;

typedef struct {
    mos_watch_decision_kind kind;
    /* kind == EMIT_EVENT: the event to emit. vendor/product/revision are
       adapter-owned (mos_watch.c's pointer-lifetime invariant); registry_id,
       stream_open_wall_ms, bsd_unit are plain values, no lifetime constraint. */
    mos_watch_event         event;
    /* kind == SLEEP_UNTIL: the MONOTONIC-ms deadline to sleep until. */
    uint64_t                next_poll_at_mono_ms;
} mos_watch_decision;

/* The pure watch state. Tests construct one directly with their own ops +
   fake clock; the Apple-side mos_watch.c constructs it inside mos_watch_t. */
typedef struct {
    const mos_watch_ops_t *ops;
    void *ctx;

    /* Session identity: registry_id (attachment token; 0 only reachable
       from direct pure-layer callers — the adapter fails closed) and the
       wall epoch at init. bsd_unit is the whole-disk unit (-1 = no whole-disk IOMedia node (media absent)), refreshed by probes. */
    uint64_t       registry_id;
    uint64_t       stream_open_wall_ms;
    int64_t        bsd_unit;

    /* Backoff parameters. */
    uint32_t       stable_poll_ms;
    uint32_t       transition_poll_ms;

    /* State tracking. */
    mos_state last_state;
    bool           have_last_state;
    /* Error-backoff: consecutive identical probe errors widen the retry
       interval (escalation rule at the pump). */
    int32_t        last_probe_err;     /* mos_error of the previous probe, MOS_OK if it succeeded */
    uint32_t       consec_probe_errs;  /* consecutive probes returning last_probe_err */
    /* Same-state swap detection: last_media_id is the fingerprint,
       last_profile the no-id-bridge fallback. 0 == unavailable, never an
       observation. Rules at the pump. */
    uint64_t       last_media_id;
    uint16_t       last_profile;
    uint64_t       next_seq;

    /* Next probe deadline in MONOTONIC ms (vs ops->mono_ms). The field
       name carries the clock domain so a domain mixup is compile-loud. */
    uint64_t       next_poll_at_mono_ms;

    /* Removal lifecycle. terminated flips true on external removal or
       probe → NO_DEVICE. removed_event_emitted marks "terminal
       device_removed already emitted"; later pumps go straight to TERMINAL.
       Its own field (not an overloaded have_last_state) so device_removed
       survives termination before any successful probe. */
    bool           terminated;
    bool           removed_event_emitted;
} mos_watch_state;

/* Initialize a watch state.
     - start_mono_ms: monotonic ms at init; the first poll is scheduled for
       exactly this (probe immediately).
     - registry_id: the session's attachment-identity token (Apple: the
       IORegistry entry ID; tests: any value, 0 included).
     - start_wall_ms: Unix-epoch ms at init, recorded as
       stream_open_wall_ms on every event. Session identity is the
       (registry_id, stream_open_wall_ms) pair.
   stable_poll_ms == 0 → 2000ms; transition_poll_ms == 0 → 200ms. */
void mos_internal_watch_init(mos_watch_state *w,
                             const mos_watch_ops_t *ops, void *ctx,
                             int64_t bsd_unit,
                             uint64_t registry_id,
                             uint64_t start_mono_ms,
                             uint64_t start_wall_ms,
                             uint32_t stable_poll_ms,
                             uint32_t transition_poll_ms);

/* Drive one watch-pump iteration. Calls ops->mono_ms and (if a poll is
   due) ops->probe. Returns a decision:
     - EMIT_EVENT: write the event, then pump again.
     - SLEEP_UNTIL: wait until next_poll_at_ms (or an external
       notification), then pump again.
     - TERMINAL: close the watch.
   The caller owns how it sleeps (CFRunLoop on Apple, nanosleep elsewhere)
   — the pure core does not block. */
mos_watch_decision mos_internal_watch_pump(mos_watch_state *w);

/* External trigger: a device-removed notification arrived. The next pump
   returns TERMINAL with a device_removed event. */
void mos_internal_watch_notify_removed(mos_watch_state *w);

/* External trigger: a wake-up arrived (e.g. kIOGeneralInterest). The next
   pump probes immediately rather than waiting for the scheduled poll. */
void mos_internal_watch_notify_wake(mos_watch_state *w);

/* ---- Watch-all multiplexer (mos_watch_core.c) --------------------- *
 *
 * Pure fan-in over up to MOS_WATCH_ALL_CAP per-device watch cores:
 * join/leave lifecycle, stream-global seq, deterministic same-tick
 * interleave (ascending registry_id). Each slot is a full mos_watch_state
 * on its own ops/ctx — the multiplexer adds NO probing or classification,
 * it only schedules, relabels mid-stream joins (snapshot → device_appeared),
 * and makes device_removed per-slot rather than stream-terminal. */

/* Live watch-all cores. 64 matches the `mos list` / enumerate cap and is the
   HARD CEILING here: the multiplexer's per-call `visited` set is a single
   uint64_t (mos_watch_core.c), so above 64 needs a wider bitmask, not another
   bump. Each slot is a full mos_watch_state, but the runtime cost scales with
   drives actually present, not the cap — raising it only adds headroom. */
#define MOS_WATCH_ALL_CAP 64

typedef struct {
    mos_watch_state cores[MOS_WATCH_ALL_CAP];
    bool            active[MOS_WATCH_ALL_CAP];
    /* Slot joined after the stream opened: its first SNAPSHOT is relabeled
       MOS_EVENT_DEVICE_APPEARED, then the flag clears. Earlier ERROR events
       (probe failing right after hot-plug) do NOT consume the join — the
       announcement waits for the first successful probe. */
    bool            join_pending[MOS_WATCH_ALL_CAP];
    uint64_t        seq;   /* stream-global; overrides per-core seq */
} mos_watch_all_state;

void mos_internal_watch_all_init(mos_watch_all_state *a);

/* First free slot index, or -1 when full. Exposed so the adapter can point
   a slot's ctx at per-slot storage BEFORE add() initializes the core (add()
   uses the same first-free scan, single-threaded by the watch contract). */
int mos_internal_watch_all_free_slot(const mos_watch_all_state *a);

/* Add a device. Same params as mos_internal_watch_init plus mid_stream
   (true ⇒ first event is device_appeared). Returns the slot used; the
   EXISTING slot if registry_id is already active (dedupe — DR can announce
   a device the open-time snapshot already carried); -1 when full or
   registry_id == 0. */
int mos_internal_watch_all_add(mos_watch_all_state *a,
                               const mos_watch_ops_t *ops, void *ctx,
                               int64_t bsd_unit, uint64_t registry_id,
                               uint64_t start_mono_ms,
                               uint64_t start_wall_ms,
                               uint32_t stable_poll_ms,
                               uint32_t transition_poll_ms,
                               bool mid_stream);

/* Active slot index for a registry id, or -1. The adapter's Disappeared
   handler resolves the leaving device and calls
   mos_internal_watch_notify_removed on cores[i]. */
int mos_internal_watch_all_find(const mos_watch_all_state *a,
                                uint64_t registry_id);

/* One multiplexer iteration. EMIT_EVENT carries the next event with
   stream-global seq (join relabeling and per-slot removal applied);
   SLEEP_UNTIL carries the earliest deadline over active slots, or
   UINT64_MAX when none is active (empty stream: sleep until external
   add/wake). NEVER returns TERMINAL — removal is per-slot. */
mos_watch_decision mos_internal_watch_all_pump(mos_watch_all_state *a);

#endif /* MOS_PURE_H */
