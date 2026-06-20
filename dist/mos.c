/*
 * mos.c — amalgamated single-file implementation of mac-optical-state.
 *
 * Build instructions for a consuming project:
 *
 *   Compile the implementation to an object file:
 *     cc -c mos.c -mmacosx-version-min=12.0
 *
 *   Link it into an executable (frameworks needed at link time only):
 *     cc mos.o your_main.o -o yourtool \\
 *        -framework IOKit \\
 *        -framework CoreFoundation \\
 *        -framework DiscRecording \\
 *        -framework DiskArbitration \\
 *        -mmacosx-version-min=12.0
 *
 * Or add both files (mos.h and this one) to your existing build system
 * and make sure IOKit, CoreFoundation, DiscRecording, and
 * DiskArbitration are on your link line, with the deployment target
 * pinned to macOS 12.0 to match the CMake build's
 * CMAKE_OSX_DEPLOYMENT_TARGET. Skipping -framework DiscRecording
 * fails to link at the DRCopyDeviceArray reference in mos_dr.c.
 *
 * DiskArbitration is OPTIONAL: compile with -DMOS_USE_DISKARBITRATION=0
 * and you may drop -framework DiskArbitration entirely. mos_query_volume
 * then always reports unmounted (volume name/path null) — the API, CLI,
 * and JSON shapes are unchanged. With the default (flag unset), the
 * DADiskCopyDescription reference in mos_da.c requires the framework.
 *
 * See mos.h for the API.
 * See https://github.com/napieraj/mos for source, tests,
 * and the non-amalgamated layout.
 */

/* Feature-test macros for the whole amalgamated translation unit. The
 * standalone TUs define these per-file ahead of their own includes;
 * concatenation would otherwise place a later TU's defines AFTER system
 * headers have already been processed — ineffective, and fragile against
 * reordering of the weave. Hoisted here so the amalgamated build sees the
 * same SDK surface as the standalone build. The per-file blocks below are
 * #ifndef-guarded, so they become no-ops. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* mos is implemented in C11: it uses _Static_assert and _Atomic. The CMake
 * build pins -std=c11 with C_STANDARD_REQUIRED, but this amalgamation is the
 * drop-in, no-CMake path (compile mos.c by hand), where an older dialect would
 * accept _Atomic/_Static_assert as compiler extensions and silently build a
 * subtly different library. Fail loudly instead. This is in mos.c only, never
 * mos.h, so a C99 (or older) application that includes the public header and
 * links is unaffected — the C ABI is dialect-agnostic. The __cplusplus arm is
 * skipped: compiling mos.c as C++ is a separate unsupported path. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#  if !defined(__cplusplus)
#    error "mos requires a C11 (or later) compiler: mos.c uses _Static_assert and _Atomic. Compile with -std=c11 or newer."
#  endif
#endif

#include "mos.h"

/* ==== src/mos_scsi_status.h ==== */
/*
 * mos_scsi_status.h — SAM-5 §5.3 SCSI task status constants. No IOKit/CF
 * deps, so the contention classifier and its test stay SDK-free. Values
 * match Apple's kSCSITaskStatus_* enums.
 */


#define MOS_SCSI_STATUS_GOOD                  0x00
#define MOS_SCSI_STATUS_CHECK_CONDITION       0x02
#define MOS_SCSI_STATUS_CONDITION_MET         0x04
#define MOS_SCSI_STATUS_BUSY                  0x08
#define MOS_SCSI_STATUS_RESERVATION_CONFLICT  0x18
#define MOS_SCSI_STATUS_TASK_SET_FULL         0x28
#define MOS_SCSI_STATUS_ACA_ACTIVE            0x30


/* ==== src/mos_pure.h ==== */
/*
 * mos_pure.h — prototypes and layouts for the pure-data functions. No
 * IOKit / CoreFoundation, so the parsers and state mapper are testable
 * without linking IOKit (mos_internal.h re-includes this).
 */


#include <stdint.h>
#include <stdbool.h>


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
};

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
};

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

/* Drive-static facts from a full (RT=0) GET CONFIGURATION response. */
typedef struct mos_drive_caps {
    mos_drive_protection protection;
    /* Supported-profile set from the Profile List feature (0x0000), drive-
       static (the per-descriptor CurrentP bit is media-dependent, ignored).
       64 covers a conformant max (one-byte Additional Length ⇒ ≤63 codes). */
    uint8_t  profile_count;
    uint16_t profiles[64];
    /* Firmware creation timestamp from the Firmware Information feature
       (010Ch), "YYYY-MM-DDTHH:MM:SSZ" (GMT) or "" when absent. 24 holds the
       20-char ISO form + NUL. */
    char     firmware_date[24];
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
};

/* Decode a READ DISC INFORMATION (0x51, data type 000b) response into *out.
 * True only when the fixed numeric region (through byte 11) is present per
 * BOTH `len` and the reply's declared length. Address fields and
 * validity-gated identifiers are not decoded. Layout and safety contract
 * at the decoder (mos_discinfo.c). */
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
    uint16_t descriptor_count;  /* from the read-direction reply       */
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

/* ---- INQUIRY VPD page 0x80 decode (mos_vpd80.c) ------------------- *
 *
 * Decode the Unit Serial Number page into a NUL-terminated ASCII string in
 * out[0..out_cap). True only when the reply echoes page code 0x80 and a
 * non-empty serial survives the trailing space/NUL trim. Pure, bounded,
 * no-OOB — fuzz/ASan-gated. The shell (mos_serial.c) bounds len to the
 * realized transfer count before calling (O-4). */
bool mos_internal_vpd80_serial_parse(const uint8_t *buf, size_t len,
                                     char *out, size_t out_cap);

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


/* ==== src/mos_internal.h ==== */
/*
 * mos_internal.h — internal library declarations; not public ABI
 * (consumers include only <mos.h>). The IOKit-free pure-data prototypes
 * live in mos_pure.h so tests can include them without IOKit.
 */



#include <stdbool.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICmds_REQUEST_SENSE_Defs.h>

/* Transfer direction constants for mos_internal_raw_cdb(). Internal, not
   public ABI: raw CDB issuance is a library-internal mechanism (the public
   passthrough was retired — AGENTS.md). The values are pinned to the SDK's
   kSCSIDataTransfer_* enumerators by _Static_assert in mos_scsi.c, the one TU
   that sees both names. */
typedef enum {
    MOS_XFER_NONE         = 0,
    MOS_XFER_FROM_TARGET  = 2, /* drive → host (read-like) */
    MOS_XFER_TO_TARGET    = 1, /* host → drive (write-like) */
} mos_xfer_dir;

/* ---- Handle layout (opaque to public callers) ----------------------- */

struct mos_handle {
    io_service_t              svc;
    uint64_t                  drive_registry_id; /* IORegistryEntryGetRegistryEntryID(svc),
                                                    0 on failure; the attachment identity
                                                    (same value the watch emits). */
    IOCFPlugInInterface     **plugin;
    MMCDeviceInterface      **mmc;
    SCSITaskDeviceInterface **std;   /* lazy; only allocated on first raw CDB */

    bool                      have_exclusive;

    /* Whole-disk identity (the BSD unit, kIOBSDUnitKey); -1 = no whole-disk IOMedia node (media absent). The string
       buffers below back the variable-length INQUIRY fields. */
    int64_t                   bsd_unit;
    uint64_t                  media_id;        /* whole-disk IOMedia entry ID,
                                                  0 == no media; re-resolved with
                                                  bsd_unit per media-scoped query
                                                  (swap fingerprint) */
    uint64_t                  media_bytes;     /* kIOMediaSizeKey off the same
                                                  whole-disk node; 0 == absent
                                                  (query-time, like bsd_unit) */
    uint32_t                  media_block_bytes; /* kIOMediaPreferredBlockSizeKey;
                                                    0 == absent */
    const char               *media_type;      /* mos_internal_media_type_token of
                                                  the kernel "Type" key, NULL if
                                                  absent (query-time, like bsd_unit);
                                                  points to static token storage */
    signed char               writable;        /* kIOMediaWritableKey tri-state:
                                                  -1 absent, 0 read-only, 1 writable
                                                  (query-time, like bsd_unit) */
    char                      vendor_str[9];   /* 8 chars + NUL */
    char                      product_str[17]; /* 16 chars + NUL */
    char                      revision_str[5]; /* 4 chars + NUL */

    /* Handle-owned result object returned (by borrowed pointer) from
       mos_query_state. Overwritten each query; its string fields point
       into the *_str buffers above. */
    mos_state_result          result;

    /* Handle-owned disc-information result (mos_query_disc_info).
       Overwritten each query; plain values, no borrowed pointers. */
    struct mos_disc_info      disc_info;

    /* Handle-owned TOC result (mos_query_toc). Same terms. */
    struct mos_toc            toc;

    /* Handle-owned drive-caps result (mos_query_drive_caps). Same terms. */
    struct mos_drive_caps     caps;

    /* Handle-owned disc-id result (mos_query_disc_id). Same terms. */
    struct mos_disc_id        disc_id;

    /* Handle-owned CD-TEXT result (mos_query_cdtext). Same terms;
       plain values into fixed buffers, no borrowed pointers. */
    struct mos_cdtext         cdtext;

    /* Handle-owned physical structure result (mos_query_physical_structure).
       Same terms; plain values, no borrowed pointers. */
    struct mos_physical_structure physical_structure;

    /* Handle-owned track-info result (mos_query_track_info). Same terms. */
    struct mos_track_info     track_info;

    /* Handle-owned session-layout result (mos_query_session_layout): the
       per-session boundaries decoded from the kernel-cached full-TOC. Same
       terms; plain values, no borrowed pointers. */
    struct mos_session_layout session_layout;

    /* Handle-owned capacity result (mos_query_capacity). Assembled from
       the per-call-refreshed IOMedia size above + a fresh track_info read +
       (for formattable profiles) a READ FORMAT CAPACITIES convenience read. */
    struct mos_capacity       capacity;

    /* Handle-owned drive-performance result (mos_query_drive_perf). */
    struct mos_drive_perf     drive_perf;

    /* Handle-owned MODE SENSE results (mos_query_mode_caps /
       mos_query_error_recovery). Same terms. */
    struct mos_mode_caps      mode_caps;
    struct mos_error_recovery error_recovery;

    /* Handle-owned INQUIRY VPD-0x80 serial (mos_query_serial). Filled by the
       raw-INQUIRY shell, returned by borrowed pointer; 64 holds any real drive
       serial. A serial that cannot be held whole — longer than this buffer or
       carrying an interior NUL — is refused (serial stays null), never
       truncated: a partial identity key is worse than none (mos_vpd80.c). */
    char                      serial_str[64];

    /* Handle-owned standard-INQUIRY result (mos_query_drive_inquiry). */
    struct mos_drive_inquiry drive_inquiry;
};

/* Device-info records returned by the enumeration callback. Allocated on
   the stack inside the enumerator and populated per iteration; caller
   must copy anything they want to keep. */
struct mos_device_info {
    int64_t  bsd_unit;    /* whole-disk BSD unit; -1 = no whole-disk IOMedia node (media absent) */
    uint64_t registry_id; /* for stable sort */
};

/* ---- DiscRecording-linked internal prototypes (mos_dr.c) ----------- *
 *
 * The directory half: discovery, identity, addressing. Never state —
 * that stays with the MMC seam below. */

/* One enumerated device, extracted from DR's dictionaries into plain C at
   the adapter seam (no CF types cross this line). Identity buffers carry
   the SPC-4 INQUIRY field widths. */
typedef struct {
    uint64_t registry_id;     /* path → entry → ID; never 0 in a snapshot */
    int64_t  bsd_unit;        /* -1 = no whole-disk IOMedia node (media absent) */
    char     vendor[9];       /* 8 chars + NUL */
    char     product[17];     /* 16 chars + NUL */
    char     revision[5];     /* 4 chars + NUL */
} mos_internal_dr_snapshot;

/* Fill up to `cap` slots in DR device-array order (the array drutil
   enumerates — the index-provenance contract); returns the count. Devices
   whose registry path won't resolve to an entry ID are skipped (an
   un-reopenable index entry would break enumeration/index correspondence). */
size_t mos_internal_dr_copy_snapshot(mos_internal_dr_snapshot *slots,
                                     size_t cap);

/* Resolve a canonical "diskN" name to the drive's registry entry ID
   via DRDeviceCopyDeviceForBSDName; 0 when DR knows no such device. */
uint64_t mos_internal_dr_registry_id_for_bsd_name(const char *disk_name);

/* Resolve a kDRDeviceIORegistryEntryPathKey value (CFString expected;
   anything else yields 0) to the entry's uint64 registry ID. Shared by
   the snapshot builder and the watch doorbell's per-device filter. */
uint64_t mos_internal_dr_id_for_path_value(CFTypeRef path);

/* Bounded CFTypeRef-string -> C-buffer copy (mos_dr.c); "" on
   non-string, oversize, or conversion failure. Always terminates. */
void mos_internal_dr_copy_string(CFTypeRef value, char *dst, size_t cap);

/* One-shot DiskArbitration mounted-volume lookup (mos_da.c). True only
   when mounted; gate calls on bsd_unit present. No callbacks, no run
   loop — see the re-admission terms at the top of mos_da.c. */
bool mos_internal_da_volume(const char *bsd_name,
                            char *name_buf, size_t name_cap,
                            char *path_buf, size_t path_cap);

/* Force-unmount every volume on whole-disk "diskN" via DiskArbitration
   (kDADiskUnmountOptionForce | kDADiskUnmountOptionWhole). True on success.
   The SOLE DA action mos performs (async DADiskUnmount, awaited on a semaphore),
   used only by `tray eject --force`. Returns false when DA is opted out at build
   time (capability absent) — the force eject then reports the mount as BUSY.

   IDENTITY BIND (data-loss safety): bsd_name alone is NOT sufficient authority
   to destroy state — macOS reuses "diskN" unit numbers, so a stale name can
   resolve to an unrelated disk. expected_media_id is the whole-disk IOMedia
   registry entry ID the caller just resolved off the handle's stable drive
   service (h->media_id); the unmount proceeds only if the IOMedia behind diskN
   has that exact id (registry IDs are unique and not reused). A mismatch, a
   zero expected id (identity unknown), or no IOMedia behind diskN all fail
   closed — refuse rather than risk unmounting the wrong disk. */
bool mos_internal_da_unmount(const char *bsd_name, uint64_t expected_media_id);

/* Extract one device's snapshot (registry id, bsd unit, identity) from a
   DRDeviceRef passed as CFTypeRef (this header stays free of DiscRecording
   types). False when the registry path doesn't resolve — the same skip gate
   the array snapshot uses. Shared with the watch-all Appeared handler. */
bool mos_internal_dr_device_snapshot(CFTypeRef device_ref,
                                     mos_internal_dr_snapshot *s);

/* Device-static identity strings for an already-opened service, via
   DR's registry-path lookup. Best-effort: returns false (and empties
   the buffers) when DR cannot see the service (non-fatal). */
bool mos_internal_dr_copy_identity_for_service(io_service_t svc,
                                               char *vendor, size_t vcap,
                                               char *product, size_t pcap,
                                               char *revision, size_t rcap);

/* ---- IOKit-linked internal prototypes ------------------------------ *
 *
 * MMC convenience wrappers for the query path (mos_state.c). */
mos_error mos_internal_mmc_get_tray_state     (mos_handle_t *h, bool *tray_open);
mos_error mos_internal_mmc_test_unit_ready    (mos_handle_t *h,
                                               uint32_t *status,
                                               uint8_t sense[18]);
mos_error mos_internal_mmc_get_current_profile(mos_handle_t *h, uint16_t *profile);

/* Thin shim over the pure IOReturn→mos_error map (mos_scsi.c), so the typed
   query surface (mos_query.c) maps transport failures identically to the
   convenience wrappers and mos_internal_raw_cdb. CHECK CONDITION rides task
   status/sense, not IOReturn, so this maps only transport failures. */
mos_error mos_internal_ioreturn_to_mos_error(IOReturn rc);

/* Re-resolve the handle's media-scoped identity (whole-disk bsd_unit,
   media_id swap fingerprint, kernel-cached size/block bytes) from its
   stable drive service — the freshness the media-scoped queries (state,
   capacity, volume) call first so a handle held across an insert/eject
   reports current media. Local IORegistry walk off h->svc; no SCSI
   command, no exclusive access (mos_scsi.c). */
void mos_internal_refresh_media_identity(mos_handle_t *h);

/* Copy the kernel-cached full-TOC (kIOCDMediaTOCKey, a CDTOC CFData blob) off
   the drive's IOCDMedia node into `buf`, returning the byte count copied
   (clamped to `cap`), or 0 when absent — not a CD (the property is IOCDMedia-
   only), no media, or no property. Zero SCSI commands, no exclusive access: a
   pure IORegistry read like the cached capacity (mos_scsi.c). The pure parser
   mos_internal_cdtoc_parse turns the blob into the per-session layout. */
size_t mos_internal_read_cdtoc(io_service_t svc, uint8_t *buf, size_t cap);

/* Copy the kernel optical-media "Type" string (kIO{CD,DVD,BD}MediaTypeKey, all
   literally "Type") off the drive's IO{CD,DVD,BD}Media node into `buf` (NUL-
   terminated), returning the length copied or 0 when absent (no media, or no
   media-type node yet). Zero SCSI commands, no exclusive access — a pure
   IORegistry read like read_cdtoc, but media-class-agnostic. mos_internal_
   media_type_token maps the result to a token. */
size_t mos_internal_read_media_type(io_service_t svc, char *buf, size_t cap);

/* Read the kernel IOMedia Writable flag (kIOMediaWritableKey, OSBoolean) off the
   drive's IO{CD,DVD,BD}Media node. Tri-state return: -1 = no optical media node /
   key absent, 0 = read-only, 1 = writable. Zero SCSI commands, no exclusive
   access — the same optical-node walk as read_media_type, reading a CFBoolean. */
int mos_internal_read_writable(io_service_t svc);

/* Issue one 6-byte tray CDB (START STOP UNIT 0x1B / PREVENT ALLOW MEDIUM
   REMOVAL 0x1E) via mos_internal_raw_cdb and classify the result. Negative
   mos_error on transport/lock failure (BUSY, NO_DEVICE, IO); on an ANSWERED
   command, MOS_OK with *outcome (DONE / REFUSED_LOCKED / REFUSED_OTHER).
   sense_out, when non-NULL, gets {sk, asc, ascq} (zeroed on MOS_OK with no
   sense). Adds no ObtainExclusiveAccess — that stays mos_internal_raw_cdb
   (§3). Shared by the four mos_tray_* verbs. */
mos_error mos_internal_tray_cmd(mos_handle_t *h, const uint8_t cdb[6],
                                mos_tray_outcome *outcome, uint8_t sense_out[3]);

/* Issue a raw CDB against the drive — the SINGLE ObtainExclusiveAccess call
   site (ARCHITECTURE.md §3). Internal mechanism only: the public passthrough
   that exposed this was retired (AGENTS.md). Acquires and releases exclusive
   access within the single call, so it never leaves the drive blocked against
   Finder / DiskArbitration; returns MOS_ERR_BUSY / MOS_ERR_EXCLUSIVE_ACCESS
   without issuing the CDB when the drive is mounted or held by another client.

   cdb_len must be 6, 10, 12, or 16 (the lengths SCSITaskLib accepts); other
   values return MOS_ERR_INVALID_ARG. timeout_ms must be > 0 — 0 is
   SCSITaskLib's "Wait Forever", rejected at the boundary. scsi_task_status and
   sense are required; bytes_transferred may be NULL. Callers: the GESN tray
   probe (mos_scsi.c), the tray verbs (mos_tray.c), the INQUIRY reads
   (mos_serial.c / mos_drive_inquiry.c), and the diagnostic capture menu
   (cli/probe.c, MOS_CLI_PROBE). */
mos_error mos_internal_raw_cdb(mos_handle_t *h,
                               const uint8_t *cdb, size_t cdb_len,
                               void *data_buf, size_t data_len,
                               mos_xfer_dir direction,
                               uint32_t timeout_ms,
                               /* out: */
                               uint32_t *scsi_task_status,
                               uint8_t   sense[18],
                               uint64_t *bytes_transferred);



/* Open a drive by its IORegistry entry ID — the identity-stable primitive:
   IORegistryEntryIDMatching resolves atomically, returning the SAME entry
   the ID came from or NO_DEVICE if it terminated; a recycled BSD name can't
   rebind it elsewhere. The watch's authority for which drive a session
   probes. Not public (registry IDs are IOKit-specific). *err_out: NO_DEVICE
   or IO. */
mos_handle_t *mos_internal_open_by_registry_id(uint64_t id,
                                               mos_error *err_out);

/* The validated io_service_t a handle was opened against — identity
   transfer with no second BSD lookup (which would be a TOCTOU window).
   IO_OBJECT_NULL on NULL input. The caller MUST IOObjectRetain before
   mos_close(h) drops the handle's reference, then owns and releases it. */
io_service_t mos_internal_handle_get_service(mos_handle_t *h);

/* ---- Auto-cleanup helpers for IOKit / CoreFoundation refcounts ----- *
 *
 * The gcc/clang cleanup attribute runs the callback at scope exit, so
 * refcount discipline holds across early exits. To hand a reference to a
 * longer-lived owner, null the variable to disable cleanup:
 *   io_service_t consumed = local; local = IO_OBJECT_NULL;
 * The callbacks check the sentinel before releasing and clear after, so
 * manual release-and-clear and auto-cleanup coexist safely. */

static inline void mos_internal_cleanup_cftype(CFTypeRef *p)
{
    if (*p) {
        CFRelease(*p);
        *p = NULL;
    }
}

static inline void mos_internal_cleanup_io_object(io_object_t *p)
{
    if (*p != IO_OBJECT_NULL) {
        IOObjectRelease(*p);
        *p = IO_OBJECT_NULL;
    }
}

#define MOS_CF_AUTO __attribute__((cleanup(mos_internal_cleanup_cftype)))
#define MOS_IO_AUTO __attribute__((cleanup(mos_internal_cleanup_io_object)))


/* ==== src/mos_cdtext.c ==== */
/*
 * mos_cdtext.c — pure, bounds-safe decode of the disc-level (album) Title
 * and Performer from a READ TOC/PMA/ATIP format 0101b (CD-TEXT) reply. No
 * IOKit: the shell hands us a fixed zero-init buffer (filled via the
 * non-exclusive ReadTableOfContents) and its size. Every text byte is
 * disc-reported, hence hostile — the declared CD-TEXT Data Length must
 * never steer a read outside [buf, buf+len), and text is copied verbatim
 * (the CLI escapes at emit, like the volume name and INQUIRY identity).
 * No payload byte is ever used as an offset or length.
 *
 * SCOPE — the album Title/Performer (the "which album is in the drive"
 * disambiguator, parallel to the mounted volume name) plus the per-track
 * TITLES and PERFORMERS, all from the FIRST language block (block 0) in
 * single-byte charset. Field types and language blocks NOT decoded are in
 * SPEC.md; a double-byte (DBCC) field reads as absent, never mis-decoded
 * as Latin-1. This is BEST-EFFORT DISPLAY TEXT, not a fail-closed
 * fingerprint: audio-CD dedup keys ride on the TOC (mos_internal_toc_parse).
 *
 * Stream model (MMC / Red Book): within one (pack-type, block) the
 * per-track strings are NUL-separated and chopped across the 12-byte pack
 * payloads; the first pack's Track Number seeds the running index, so the
 * stream is [track S, track S+1, ...] (S = 0 is album-level). We walk the
 * block-0 packs in buffer order and dispatch each string by track number.
 *
 * Pack layout (READ TOC format 0101b, MMC-3 §6.27 / Red Book CD-TEXT;
 * libcdio cross-check in SPEC.md):
 *   [0..1] CD-TEXT Data Length (BE) — bytes available AFTER this field;
 *          the reply occupies 2 + value bytes.
 *   [2..3] reserved
 *   [4..]  a sequence of 18-byte CD-TEXT packs:
 *            [0]      Pack Type (0x80 Title, 0x81 Performer, ...)
 *            [1]      Track Number (bits 6:0; bit 7 reserved/extension)
 *            [2]      Sequence Number
 *            [3]      bit7 = double-byte (DBCC); bits 6:4 = Block Number
 *                     (language, 0..7); bits 3:0 = Character Position
 *            [4..15]  12 text bytes (NUL separates per-track strings)
 *            [16..17] CRC (present; not verified here, as in libcdio)
 *
 * No-OOB / termination gated under ASan/UBSan by tests/test_cdtext.c and
 * the fuzz_pure CD-TEXT phase.
 */


#include <string.h>

#define CDTEXT_HDR        4u    /* 2-byte data length + 2 reserved        */
#define CDTEXT_PACK_LEN  18u
#define CDTEXT_TEXT_OFF   4u    /* text bytes within a pack: [4..15]      */
#define CDTEXT_TEXT_LEN  12u

#define CDTEXT_PACK_TITLE     0x80u
#define CDTEXT_PACK_PERFORMER 0x81u

/* Bounded NUL-terminated copy into a fixed buffer (truncates past cap-1). */
static void cdtext_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* Dispatch reconstructed string `s` by track number: track 0 → the album
   field (`album_dst`); tracks 1..MAX → `tracks[track-1]` when `tracks` is
   non-NULL, bumping *max_track to the highest track with a NON-EMPTY string. */
static void cdtext_store(const char *s, uint32_t track,
                         char *album_dst, size_t album_cap,
                         char tracks[][MOS_CDTEXT_TRACK_TITLE_CAP],
                         uint8_t *max_track)
{
    if (track == 0) {
        cdtext_copy(album_dst, album_cap, s);
        return;
    }
    if (tracks && track >= 1 && track <= MOS_CDTEXT_MAX_TRACKS) {
        cdtext_copy(tracks[track - 1], MOS_CDTEXT_TRACK_TITLE_CAP, s);
        if (s[0] && (uint8_t)track > *max_track) *max_track = (uint8_t)track;
    }
}

/* Walk the block-0 packs of `want_type` in buffer order, reconstruct the
   NUL-separated per-track stream, and store each string by track number
   (the first qualifying pack's Track Number seeds the running index).
   Track 0 → `album_dst`; tracks 1.. → `tracks` when non-NULL. Single-byte
   only: a DBCC STARTING pack decodes nothing (album left ""); a mid-stream
   DBCC stops the walk, keeping the prefix already stored. */
static void cdtext_decode_type(const uint8_t *buf, size_t span,
                               uint8_t want_type,
                               char *album_dst, size_t album_cap,
                               char tracks[][MOS_CDTEXT_TRACK_TITLE_CAP],
                               uint8_t *max_track)
{
    album_dst[0] = '\0';

    char     cur[MOS_CDTEXT_STR_CAP];   /* current-string accumulator */
    size_t   n       = 0;
    uint32_t track   = 0;               /* track number of the current string */
    bool     started = false;

    for (size_t p = CDTEXT_HDR; p + CDTEXT_PACK_LEN <= span;
         p += CDTEXT_PACK_LEN) {
        if (buf[p] != want_type) continue;            /* wrong pack type   */
        uint8_t b3 = buf[p + 3];
        if (((b3 >> 4) & 0x07) != 0) continue;        /* first block only  */
        bool dbcc = (b3 & 0x80) != 0;

        if (!started) {
            if (dbcc) return;                          /* double-byte: skip */
            track   = (uint32_t)(buf[p + 1] & 0x7Fu);  /* starting track #  */
            started = true;
        } else if (dbcc) {
            break;            /* charset flip mid-stream: keep what we have */
        }

        for (size_t j = 0; j < CDTEXT_TEXT_LEN; j++) {
            uint8_t c = buf[p + CDTEXT_TEXT_OFF + j];
            if (c == 0x00) {                           /* end of one string */
                cur[n] = '\0';
                cdtext_store(cur, track, album_dst, album_cap,
                             tracks, max_track);
                n = 0;
                track++;
                continue;
            }
            if (n + 1 < sizeof cur) cur[n++] = (char)c; /* else truncate    */
        }
    }
    /* Trailing unterminated string (data clamped mid-string): keep it. */
    if (started && n > 0) {
        cur[n] = '\0';
        cdtext_store(cur, track, album_dst, album_cap, tracks, max_track);
    }
}

bool mos_internal_cdtext_parse(const uint8_t *buf, size_t len,
                               struct mos_cdtext *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!buf || len < CDTEXT_HDR) return false;

    /* CD-TEXT Data Length counts bytes AFTER its own two. 64-bit total,
       clamped by the trusted length (O-4): a lying length can only shrink
       the span, never extend a read. */
    uint64_t claimed = 2u + (uint64_t)(((uint32_t)buf[0] << 8) | buf[1]);
    size_t   span    = mos_internal_trusted_len(len, len, claimed);
    if (span < CDTEXT_HDR + CDTEXT_PACK_LEN) return false;  /* no whole pack */

    cdtext_decode_type(buf, span, CDTEXT_PACK_TITLE,
                       out->title, sizeof out->title,
                       out->track_titles, &out->track_count);
    cdtext_decode_type(buf, span, CDTEXT_PACK_PERFORMER,
                       out->performer, sizeof out->performer,
                       out->track_performers, &out->track_count);

    /* "have" gates useful identity: an empty result (no album field, no
       per-track title) isn't identity. False → the adapter reports no
       CD-TEXT (null), like the other media reads. */
    out->have = out->title[0] || out->performer[0] || out->track_count > 0;
    return out->have;
}

/* ==== src/mos_cdtoc.c ==== */
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

/* ==== src/mos_config.c ==== */
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

/* ==== src/mos_da.c ==== */
/*
 * mos_da.c — DiskArbitration: a one-shot volume lookup and the force-unmount.
 *
 * Two modalities, both confined here:
 *   1. SYNCHRONOUS volume lookup (mos_internal_da_volume) — a DADiskCopyDescription
 *      read of what the mounted-volume layer already knows: no session
 *      scheduling, no run loop, no callbacks, no commands to the drive
 *      (AGENTS.md scope doctrine). Callers gate on the media nub (bsd_unit
 *      present); with no IOMedia node nothing is mounted and DA is never consulted.
 *   2. The ASYNC force-unmount (mos_internal_da_unmount) — the single DA ACTION
 *      mos performs, used ONLY by `tray eject --force`. DADiskUnmount delivers
 *      via a callback on a dispatch queue; we block on a semaphore until it
 *      fires (see that function). Data-loss capable, strictly opt-in.
 *
 * Trust terms: the description dictionary is system-supplied but its values
 * are volume-controlled (a hostile disc names its volume), so extraction
 * goes through the same bounded, type-checked, fail-to-empty seam as the DR
 * identity copies. The CLI's output escaping guards terminal/JSON regardless.
 */


/* DiskArbitration is an OPTIONAL link dependency. Build with
   -DMOS_USE_DISKARBITRATION=0 (and drop -framework DiskArbitration) and
   mos_query_volume always reports unmounted (volume name/path null) — which
   the CLI and JSON schemas already permit, so no shape changes. Default on. */
#ifndef MOS_USE_DISKARBITRATION
#define MOS_USE_DISKARBITRATION 1
#endif

#if MOS_USE_DISKARBITRATION
#include <DiskArbitration/DiskArbitration.h>
#include <dispatch/dispatch.h>   /* semaphore wait on the async unmount callback */

/* Mounted-volume name and mount path for a whole-disk "diskN". True only
   when DA has a description AND the volume is mounted (VolumePath present);
   name may still be "" if the key is absent or hostile — the caller maps ""
   to null. Both buffers are always NUL-terminated. */
bool mos_internal_da_volume(const char *bsd_name,
                            char *name_buf, size_t name_cap,
                            char *path_buf, size_t path_cap)
{
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    if (!bsd_name || !bsd_name[0]) return false;

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return false;

    bool      mounted = false;
    DADiskRef disk    = DADiskCreateFromBSDName(kCFAllocatorDefault,
                                                session, bsd_name);
    CFDictionaryRef desc = disk ? DADiskCopyDescription(disk) : NULL;

    if (desc) {
        /* VolumePath is the mount proof: DA also describes unmounted media,
           so an absent/non-URL path means "not mounted", not an error.
           CFURLGetFileSystemRepresentation returns false when the path
           exceeds the buffer, yielding not-mounted rather than a truncated
           path a consumer might chdir into. */
        CFTypeRef path = CFDictionaryGetValue(
            desc, kDADiskDescriptionVolumePathKey);
        bool is_url = path && CFGetTypeID(path) == CFURLGetTypeID();
        if (is_url && path_buf && path_cap) {
            if (CFURLGetFileSystemRepresentation((CFURLRef)path, true,
                                                 (UInt8 *)path_buf,
                                                 (CFIndex)path_cap)) {
                mounted = true;
            } else {
                path_buf[0] = 0;
            }
        } else if (is_url) {
            /* No path buffer: VolumePath presence alone is the mount proof,
               so a name-only caller (e.g. `mos state`) still sees mounted. */
            mounted = true;
        }
        if (mounted)
            mos_internal_dr_copy_string(
                CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey),
                name_buf, name_cap);
        CFRelease(desc);
    }

    if (disk)    CFRelease(disk);
    CFRelease(session);
    return mounted;
}

typedef struct { dispatch_semaphore_t sem; bool ok; } mos_da_unmount_ctx;

static void mos_internal_da_unmount_cb(DADiskRef disk,
                                       DADissenterRef dissenter, void *context)
{
    (void)disk;
    mos_da_unmount_ctx *c = (mos_da_unmount_ctx *)context;
    c->ok = (dissenter == NULL);     /* NULL dissenter ⇒ unmount accepted */
    dispatch_semaphore_signal(c->sem);
}

/* True when the IOMedia behind `disk` is the exact registry object identified
   by expected_media_id — the identity bind that keeps a reused "diskN" from
   sending the force-unmount to the wrong disk. DADiskCopyIOMedia returns the
   IOMedia io_service_t (owned, released here); its registry entry ID is the
   same value mos_internal_bsd_unit captured off h->svc's child. */
static bool mos_internal_da_disk_is_media(DADiskRef disk,
                                          uint64_t expected_media_id)
{
    if (expected_media_id == 0) return false;       /* identity unknown */
    io_service_t media = DADiskCopyIOMedia(disk);
    if (media == IO_OBJECT_NULL) return false;      /* diskN has no IOMedia */
    uint64_t got = 0;
    bool match = (IORegistryEntryGetRegistryEntryID(media, &got) == KERN_SUCCESS)
                 && got == expected_media_id;
    IOObjectRelease(media);
    return match;
}

/* Force-unmount EVERY volume on whole-disk "diskN"
   (kDADiskUnmountOptionForce | kDADiskUnmountOptionWhole). True on success.
   ONLY the `tray eject --force` path calls this: a forced unmount kills open
   file handles (data-loss capable) — that is the "open no matter what"
   contract, strictly opt-in behind --force. The unmount is GATED on the
   identity bind above: a stale/reused BSD name that no longer resolves to the
   caller's media is refused, never unmounted. This is the SINGLE DiskArbitration
   ACTION mos performs; unlike the synchronous description read above, DADiskUnmount
   is asynchronous (returns void, delivers via callback — verified against
   DADisk.h: takes the disk, not a session; options Force=0x00080000, Whole=0x1;
   success = NULL dissenter). We make it synchronous-from-our-side: deliver the
   callback on a background queue and block this thread on a semaphore until it
   fires. The wait is UNBOUNDED on purpose — the callback is guaranteed exactly
   once when the unmount resolves, so the context cannot outlive a late callback
   (no use-after-free), and a genuinely wedged force-unmount blocks here just as
   `diskutil` would (the I/O path itself is stuck). */
bool mos_internal_da_unmount(const char *bsd_name, uint64_t expected_media_id)
{
    if (!bsd_name || !bsd_name[0]) return false;

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return false;

    bool      ok   = false;
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault,
                                             session, bsd_name);
    /* Fail closed unless diskN still resolves to the handle's current media:
       a destructive op on a stale/reused BSD name must never touch a disk that
       is not the one the caller verified under its drive service. */
    if (disk && mos_internal_da_disk_is_media(disk, expected_media_id)) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        mos_da_unmount_ctx   ctx = { sem, false };
        DASessionSetDispatchQueue(session,
            dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
        DADiskUnmount(disk,
                      kDADiskUnmountOptionForce | kDADiskUnmountOptionWhole,
                      mos_internal_da_unmount_cb, &ctx);
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        DASessionSetDispatchQueue(session, NULL);   /* detach before release */
        ok = ctx.ok;
        dispatch_release(sem);
    }
    if (disk) CFRelease(disk);

    CFRelease(session);
    return ok;
}

#else  /* !MOS_USE_DISKARBITRATION */

/* No DiskArbitration linked: the mount layer is never consulted, so every
   disc reports unmounted — same contract as a disk DA cannot describe.
   Buffers cleared, false returned. */
bool mos_internal_da_volume(const char *bsd_name,
                            char *name_buf, size_t name_cap,
                            char *path_buf, size_t path_cap)
{
    (void)bsd_name;
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    return false;
}

/* No DiskArbitration linked: there is no unmount path, so a forced eject cannot
   clear a Finder/system mount. Returns false (capability absent), which leaves
   `tray eject --force` reporting the mount as MOS_ERR_BUSY rather than opening
   it — the honest degradation for the opt-out build (the consumer unmounts
   with `diskutil unmountDisk` first, exactly as without --force). */
bool mos_internal_da_unmount(const char *bsd_name, uint64_t expected_media_id)
{
    (void)bsd_name;
    (void)expected_media_id;
    return false;
}

#endif /* MOS_USE_DISKARBITRATION */

/* Public wrapper (contract in mos.h): the nub gate lives here, so no caller
   consults DA for a drive the kernel says holds no media. */
mos_error mos_query_volume(mos_handle_t *h, bool *mounted,
                           char *name_buf, size_t name_cap,
                           char *path_buf, size_t path_cap)
{
    if (mounted) *mounted = false;
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    if (!h) return MOS_ERR_INVALID_ARG;

    /* Held-handle freshness: re-resolve so a handle held across an insert
       sees the current disc's whole-disk node
       (mos_internal_refresh_media_identity). */
    mos_internal_refresh_media_identity(h);

    if (h->bsd_unit < 0) return MOS_OK;     /* no IOMedia node: unmounted */

    char bsd[24];
    if (!mos_bsd_name_format(h->bsd_unit, bsd, sizeof bsd)) return MOS_OK;

    bool m = mos_internal_da_volume(bsd, name_buf, name_cap,
                                    path_buf, path_cap);
    if (mounted) *mounted = m;
    return MOS_OK;
}

/* ==== src/mos_discinfo.c ==== */
/*
 * mos_discinfo.c — pure, bounds-safe decode of a READ DISC INFORMATION
 * (MMC 0x51, standard data type 000b) response. No IOKit: the shell hands
 * us a fixed zero-init buffer and its size. The Disc Information Length is
 * device-reported, hence hostile — it must never steer a read outside
 * [buf, buf+len).
 *
 * Layout (MMC-6, READ DISC INFORMATION standard response, first 12 bytes):
 *
 *   [0..1] Disc Information Length (BE) — bytes available AFTER this field;
 *          the response occupies 2 + value bytes.
 *   [2]    reserved | Erasable(bit4) | State of Last Session(bits 3:2)
 *                                     | Disc Status(bits 1:0)
 *   [3]    Number of First Track on Disc
 *   [4]    Number of Sessions (LSB)
 *   [5]    First Track Number in Last Session (LSB)
 *   [6]    Last Track Number in Last Session (LSB)
 *   [7]    DID_V DBC_V URU DAC_V .. BG Format Status
 *   [8]    Disc Type
 *   [9]    Number of Sessions (MSB)
 *   [10]   First Track Number in Last Session (MSB)
 *   [11]   Last Track Number in Last Session (MSB)
 *   (bytes 12+ : undecoded — see SPEC.md.)
 *
 * Safety contract (the device controls the length): `len` is the only
 * trusted ceiling; the Disc Information Length can only shrink the trusted
 * region, never extend it. The fixed numeric region (through byte 11) must
 * be present per both `len` and the declared length; a shorter response is
 * refused.
 */


bool mos_internal_disc_info_parse(const uint8_t *buf, size_t len,
                                  mos_disc_info *out)
{
    if (!buf || !out) return false;

    /* Fixed numeric fields run through byte 11; trusted region must reach 12. */
    if (len < 12) return false;

    /* Disc Information Length (bytes 0-1) counts bytes AFTER itself; clamp the
       trusted region to the smaller of declared+2 and len. */
    size_t declared_end = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < declared_end) ? len : declared_end;
    if (end < 12) return false;        /* device declares fewer bytes than the fields */

    uint8_t b2 = buf[2];
    out->status             = (mos_disc_status)(b2 & 0x03u);
    out->last_session_state = (uint8_t)((b2 >> 2) & 0x03u);
    out->erasable           = (b2 & 0x10u) != 0;

    out->first_track_on_disc      = buf[3];
    out->number_of_sessions       = (uint16_t)(((uint16_t)buf[9]  << 8) | buf[4]);
    out->first_track_last_session = (uint16_t)(((uint16_t)buf[10] << 8) | buf[5]);
    out->last_track_last_session  = (uint16_t)(((uint16_t)buf[11] << 8) | buf[6]);

    /* BG Format Status (byte 7 bits 1:0): background-format state of
       DVD+RW / BD-RE / Mount Rainier media. Values match Linux CDM_MRW_*. */
    out->bg_format_status = (uint8_t)(buf[7] & 0x03u);
    return true;
}

/* ==== src/mos_discstruct.c ==== */
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

/* ==== src/mos_dr.c ==== */
/*
 * mos_dr.c — DiscRecording-side adapter: the directory.
 *
 * Supplies discovery, identity, and addressing from the DiscRecording C API;
 * it never decides drive STATE — the TUR⊕GESN core in mos_state_core.c is the
 * sole authority (the §5.5 nub gate runs on TUR sense bytes DR does not
 * expose). DR is not a SCSI command author (AGENTS.md scope doctrine): it is
 * a substrate above the same kext the MMC path uses, and DR authors no raw
 * CDB itself — every raw verb (GESN, the tray opcodes, INQUIRY) lives in the
 * MMC path via mos_internal_raw_cdb (AGENTS.md tracks the running count).
 *
 * Identity resolution: DR exposes a device's IORegistry *path*
 * (kDRDeviceIORegistryEntryPathKey), not its entry ID. mos's identity
 * currency — registry_id, the reopen authority, the media-swap fingerprint —
 * is the uint64 entry ID, so each path resolves path → entry → ID here. An
 * unresolvable node is skipped, preserving the index ↔ open-by-index
 * correspondence the public API documents.
 */

#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif


#include <CoreFoundation/CoreFoundation.h>
#include <DiscRecording/DRCoreDevice.h>
#include <IOKit/IOKitLib.h>

#include <string.h>

/* Bounded CFString → C-buffer copy. CFStringGetCString fails outright (no
   usable partial-write contract) on too-small buffers, so conversion goes
   through a 256-byte temp: values up to 255 bytes convert then strlcpy to
   the SPC-4 field width; longer values fail conversion and yield "" — for
   identity fields whose real domain is ≤16 bytes, an absurdly long value is
   hostile and empty is correct. Non-string values also yield "". dst is
   always NUL-terminated. Shared with the DA volume lookup (mos_da.c), which
   reads volume-controlled strings under the same trust terms. */
void mos_internal_dr_copy_string(CFTypeRef value, char *dst, size_t cap)
{
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return;

    char tmp[256];
    if (!CFStringGetCString((CFStringRef)value, tmp, sizeof tmp,
                            kCFStringEncodingUTF8)) {
        return;
    }
    strlcpy(dst, tmp, cap);
}

/* path → IORegistry entry → uint64 entry ID; 0 on any failure (the
   documented "unavailable" sentinel, never a fabricated ID). Also used by
   the watch adapter's DR-doorbell per-device filter. */
uint64_t mos_internal_dr_id_for_path_value(CFTypeRef path)
{
    io_string_t p;
    if (!path || CFGetTypeID(path) != CFStringGetTypeID()) return 0;
    if (!CFStringGetCString((CFStringRef)path, p, sizeof(io_string_t),
                            kCFStringEncodingUTF8)) {
        return 0;
    }

    io_registry_entry_t e MOS_IO_AUTO =
        IORegistryEntryFromPath(kIOMainPortDefault, p);
    if (e == IO_OBJECT_NULL) return 0;

    uint64_t id = 0;
    if (IORegistryEntryGetRegistryEntryID(e, &id) != KERN_SUCCESS) id = 0;
    return id;
}

/* Strip trailing spaces (SPC wire padding DR may or may not have trimmed).
   Leading/interior spaces are data and stay. */
static void mos_internal_dr_strip_trailing_spaces(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}

/* The single extraction of the three identity strings; every reader funnels
   through here. Buffers keep the SPC-4 field widths. */
static void mos_internal_dr_copy_identity_from_info(CFDictionaryRef info,
                                                    char *vendor, size_t vcap,
                                                    char *product, size_t pcap,
                                                    char *revision, size_t rcap)
{
    mos_internal_dr_copy_string(
        CFDictionaryGetValue(info, kDRDeviceVendorNameKey), vendor, vcap);
    mos_internal_dr_copy_string(
        CFDictionaryGetValue(info, kDRDeviceProductNameKey), product, pcap);
    mos_internal_dr_copy_string(
        CFDictionaryGetValue(info, kDRDeviceFirmwareRevisionKey),
        revision, rcap);
    if (vendor && vcap) mos_internal_dr_strip_trailing_spaces(vendor);
    if (product && pcap) mos_internal_dr_strip_trailing_spaces(product);
    if (revision && rcap) mos_internal_dr_strip_trailing_spaces(revision);
}

/* Fill identity + registry id from one device's Info dictionary. */
static void mos_internal_dr_fill_from_info(CFDictionaryRef info,
                                           mos_internal_dr_snapshot *s)
{
    s->registry_id = mos_internal_dr_id_for_path_value(
        CFDictionaryGetValue(info, kDRDeviceIORegistryEntryPathKey));
    mos_internal_dr_copy_identity_from_info(info,
                                            s->vendor,   sizeof s->vendor,
                                            s->product,  sizeof s->product,
                                            s->revision, sizeof s->revision);
}

/* The media BSD name lives in the Status dict's media-info sub-dictionary
   (kDRDeviceMediaInfoKey → kDRDeviceMediaBSDNameKey), media-scoped: absent
   with no media loaded, hence unit -1. */
static int64_t mos_internal_dr_bsd_unit_from_status(CFDictionaryRef status)
{
    CFTypeRef mi = CFDictionaryGetValue(status, kDRDeviceMediaInfoKey);
    if (!mi || CFGetTypeID(mi) != CFDictionaryGetTypeID()) return -1;

    char name[MOS_BSD_NAME_CAP];
    mos_internal_dr_copy_string(
        CFDictionaryGetValue((CFDictionaryRef)mi, kDRDeviceMediaBSDNameKey),
        name, sizeof name);
    if (name[0] == 0) return -1;
    /* Normalizes rdisk//dev/ forms and rejects partition shapes — same
       authority as everywhere else. */
    return mos_internal_parse_bsd_unit(name);
}

bool mos_internal_dr_device_snapshot(CFTypeRef device_ref,
                                     mos_internal_dr_snapshot *s)
{
    if (!device_ref || !s) return false;
    DRDeviceRef dev = (DRDeviceRef)device_ref;

    memset(s, 0, sizeof *s);
    s->bsd_unit = -1;

    CFDictionaryRef info = DRDeviceCopyInfo(dev);
    if (info) {
        mos_internal_dr_fill_from_info(info, s);
        CFRelease(info);
    }
    /* No reopenable identity ⇒ not usable (header). */
    if (s->registry_id == 0) return false;

    CFDictionaryRef status = DRDeviceCopyStatus(dev);
    if (status) {
        s->bsd_unit = mos_internal_dr_bsd_unit_from_status(status);
        CFRelease(status);
    }
    return true;
}

size_t mos_internal_dr_copy_snapshot(mos_internal_dr_snapshot *slots,
                                     size_t cap)
{
    if (!slots || cap == 0) return 0;

    CFArrayRef arr = DRCopyDeviceArray();
    if (!arr) return 0;

    CFIndex n = CFArrayGetCount(arr);
    size_t out = 0;
    for (CFIndex i = 0; i < n && out < cap; ++i) {
        CFTypeRef dev = CFArrayGetValueAtIndex(arr, i);
        /* Skip-not-fail: an unresolvable entry must not hide its siblings.
           The index is the position among reopenable devices — DR array
           order when every device resolves, the expected case. */
        if (mos_internal_dr_device_snapshot(dev, &slots[out])) out++;
    }
    CFRelease(arr);
    return out;
}

uint64_t mos_internal_dr_registry_id_for_bsd_name(const char *disk_name)
{
    if (!disk_name || !disk_name[0]) return 0;

    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault,
                                              disk_name,
                                              kCFStringEncodingUTF8);
    if (!s) return 0;
    /* Callers pass the canonical "diskN" form (mos_bsd_name_format). */
    DRDeviceRef dev = DRDeviceCopyDeviceForBSDName(s);
    CFRelease(s);
    if (!dev) return 0;

    uint64_t id = 0;
    CFDictionaryRef info = DRDeviceCopyInfo(dev);
    if (info) {
        id = mos_internal_dr_id_for_path_value(
            CFDictionaryGetValue(info, kDRDeviceIORegistryEntryPathKey));
        CFRelease(info);
    }
    CFRelease(dev);
    return id;
}

bool mos_internal_dr_copy_identity_for_service(io_service_t svc,
                                               char *vendor, size_t vcap,
                                               char *product, size_t pcap,
                                               char *revision, size_t rcap)
{
    if (vendor && vcap) vendor[0] = 0;
    if (product && pcap) product[0] = 0;
    if (revision && rcap) revision[0] = 0;
    if (svc == IO_OBJECT_NULL) return false;

    io_string_t path;
    if (IORegistryEntryGetPath(svc, kIOServicePlane, path) != KERN_SUCCESS) {
        return false;
    }
    CFStringRef cfpath = CFStringCreateWithCString(kCFAllocatorDefault, path,
                                                   kCFStringEncodingUTF8);
    if (!cfpath) return false;

    DRDeviceRef dev = DRDeviceCopyDeviceForIORegistryEntryPath(cfpath);
    CFRelease(cfpath);
    if (!dev) return false; /* identity stays empty, non-fatal */

    bool ok = false;
    CFDictionaryRef info = DRDeviceCopyInfo(dev);
    if (info) {
        mos_internal_dr_copy_identity_from_info(info, vendor, vcap,
                                                product, pcap,
                                                revision, rcap);
        CFRelease(info);
        ok = true;
    }
    CFRelease(dev);
    return ok;
}

/* ==== src/mos_drive_inquiry.c ==== */
/*
 * mos_drive_inquiry.c — the drive-identity query (mos_query_drive_inquiry):
 * one raw STANDARD INQUIRY (EVPD=0, allocation length >= 74) on the
 * mos_internal_raw_cdb path, decoded by the pure parser in mos_inqdata.c. Surfaces the
 * drive's self-reported identity (vendor/product/revision) AND the VERSION
 * byte (SPC compliance level) + version-descriptor list — the canonical truth
 * `mos drive` prefers over the DiscRecording cache. Named for the datum (the
 * drive's INQUIRY self-report), not the generic INQUIRY command.
 *
 * Authored raw, not via the convenience Inquiry, because that method returns
 * only the 36-byte standard header (SCSICmd_INQUIRY_StandardData), so the
 * version descriptors at bytes 58-73 are structurally unreachable through it
 * — the same layer-1 raw-verb showing as the serial, the same INQUIRY opcode
 * (0x12) in a different mode: EVPD=0 here vs EVPD=1/page-0x80 in mos_serial.c
 * (AGENTS.md scope-doctrine ADR; design:
 * doc/research/2026-06-16-drive-identity-enrichment-survey.md).
 *
 * mos_internal_raw_cdb is the SINGLE ObtainExclusiveAccess call site; this file adds
 * none. Exclusive access is the gate: a mounted volume / other holder makes
 * the open fail BUSY and the CDB never issues, so the read backs off rather
 * than disturb a live nub — the same benign degradation as the serial (a
 * static drive fact, read with the tray empty). INQUIRY changes no state.
 */


/* 96-byte reply: covers the version descriptors (bytes 58-73) with margin;
   the parser bounds the decode by both this and the reply's Additional Length. */
#define MOS_INQUIRY_REPLY_BUF 96u

mos_error mos_query_drive_inquiry(mos_handle_t *h,
                                    const mos_drive_inquiry **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* STANDARD INQUIRY (SPC-4 0x12), 6-byte CDB:
         byte0   opcode 0x12
         byte1   EVPD = 0                  — standard data (not a VPD page)
         byte2   PAGE CODE = 0x00          — must be 0 when EVPD=0
         byte3-4 ALLOCATION LENGTH (BE)    — MOS_INQUIRY_REPLY_BUF (>= 74)
         byte5   CONTROL = 0 */
    const uint8_t cdb[6] = {
        0x12, 0x00, 0x00,
        (uint8_t)(MOS_INQUIRY_REPLY_BUF >> 8),
        (uint8_t)(MOS_INQUIRY_REPLY_BUF & 0xFF),
        0x00,
    };

    uint8_t  buf[MOS_INQUIRY_REPLY_BUF] = {0};
    uint32_t task_status               = 0;
    uint8_t  sense[18]                 = {0};
    uint64_t xferred                   = 0;

    mos_error e = mos_internal_raw_cdb(h, cdb, sizeof cdb, buf, sizeof buf,
                              MOS_XFER_FROM_TARGET, 2000,
                              &task_status, sense, &xferred);
    if (e != MOS_OK) return e;
    if (task_status != MOS_SCSI_STATUS_GOOD)
        return MOS_ERR_IO;

    /* Dual-length rule (O-4): bound the parse to the realized transfer count,
       not the full buffer — the parser further bounds by the reply's own
       Additional Length. */
    size_t trusted = (xferred < sizeof buf) ? (size_t)xferred : sizeof buf;
    if (!mos_internal_inqdata_parse(buf, trusted, &h->drive_inquiry))
        return MOS_ERR_IO;   /* truncated below the 5-byte fixed header */

    *out = &h->drive_inquiry;
    return MOS_OK;
}

/* ==== src/mos_formatcap.c ==== */
/*
 * mos_formatcap.c — pure, bounds-safe decode of READ FORMAT CAPACITIES (MMC
 * 0x23). The formattable view of the loaded medium: how big it is now, whether
 * it is unformatted, and the capacities the drive could format it to. This is
 * what mos_query_capacity's other two sources cannot report on a blank
 * REWRITABLE disc (DVD±RW, DVD-RAM, BD-RE): the kernel's cached READ CAPACITY
 * is 0 (nothing formatted) and there is no track for READ TRACK INFORMATION.
 *
 * No IOKit: the shell (src/mos_query.c) hands us a fixed zero-init buffer
 * filled via the ReadFormatCapacities convenience method (MMCDeviceInterface) —
 * NOT a raw CDB: the wrapper exists in SCSITaskLib.h (see the AGENTS.md ADR +
 * doc/research/2026-06-18-readformatcapacities-convenience-exists.md). Read-
 * only: mos reports formattable capacities and never issues FORMAT UNIT (0x04).
 *
 * Reply layout (MMC-6 §6.24, Format Capacities):
 *   Capacity List Header (4 bytes)
 *     [0..2]  reserved
 *     [3]     CAPACITY LIST LENGTH  — bytes that follow (= 8 + 8*N)
 *   Current/Maximum Capacity Descriptor (8 bytes) at [4..11]
 *     [4..7]  NUMBER OF BLOCKS (BE u32)
 *     [8]     bits 1:0 DESCRIPTOR TYPE (1 unformatted, 2 formatted, 3 no media)
 *     [9..11] BLOCK LENGTH (BE u24)
 *   Formattable Capacity Descriptors (8 bytes each) from [12]
 *     [0..3]  NUMBER OF BLOCKS (BE u32)
 *     [4]     bits 7:2 FORMAT TYPE
 *     [5..7]  TYPE DEPENDENT PARAMETER (BE u24) — block length for most types
 *
 * Dual-length rule (O-4): the CAPACITY LIST LENGTH is trusted only up to the
 * bytes the transport actually delivered, then floored to whole 8-byte
 * descriptors — a non-conformant USB-SATA bridge that over-claims the length
 * cannot make us read past the realized span. A capture is a falsifier per the
 * hardware ADR, not a design input.
 */


#define FC_HDR  4u   /* Capacity List Header bytes 0..3 */
#define FC_DESC 8u   /* every capacity descriptor is 8 bytes */

static uint32_t fc_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t fc_be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

bool mos_internal_format_caps_parse(const uint8_t *buf, size_t len,
                                    struct mos_format_caps *out)
{
    if (out) *out = (struct mos_format_caps){0};
    if (!buf || !out) return false;
    /* Need the 4-byte header plus the Current/Maximum Capacity Descriptor —
       a reply too short to carry even that is not a usable answer. */
    if (len < FC_HDR + FC_DESC) return false;

    /* CAPACITY LIST LENGTH (byte 3) counts the bytes after the header. Clamp to
       what was actually delivered (O-4), then floor to whole descriptors: the
       list is one Current/Max descriptor plus zero or more Formattable ones,
       so a coherent length is a positive multiple of 8. */
    size_t list_len = buf[3];
    size_t avail    = len - FC_HDR;
    if (list_len > avail) list_len = avail;
    list_len -= list_len % FC_DESC;
    if (list_len < FC_DESC) return false;   /* no Current/Max descriptor */

    /* Current/Maximum Capacity Descriptor at [4..11]. */
    const uint8_t *cur = buf + FC_HDR;
    out->cur_blocks      = fc_be32(cur);
    out->cur_type        = (uint8_t)(cur[4] & 0x03);
    out->cur_block_bytes = fc_be24(cur + 5);

    /* Formattable Capacity Descriptors follow, capped at MOS_FORMATTABLE_MAX.
       n is bounded by list_len/8 - 1, and list_len <= avail = len - FC_HDR, so
       the deepest read below — buf[FC_HDR + (1+n)*FC_DESC - 1] — stays < len. */
    size_t n = (list_len / FC_DESC) - 1u;
    if (n > MOS_FORMATTABLE_MAX) n = MOS_FORMATTABLE_MAX;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *d = buf + FC_HDR + FC_DESC + i * FC_DESC;
        out->d[i].blocks      = fc_be32(d);
        out->d[i].format_type = (uint8_t)(d[4] >> 2);
        out->d[i].param       = fc_be24(d + 5);
    }
    out->count = (uint8_t)n;
    return true;
}

/* ==== src/mos_inqdata.c ==== */
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

/* ==== src/mos_modepage.c ==== */
/*
 * mos_modepage.c — pure, bounds-safe decode of MODE SENSE(10) replies for
 * the two optical-specific pages the scope doctrine admits (read-only):
 * page 0x2A (CD/DVD Capabilities & Mechanical Status — loading mechanism,
 * eject/lock, buffer size) and page 0x01 (Read/Write Error Recovery —
 * AWRE/ARRE/PER/DCR + read-retry count). NO MODE SELECT: mos reports
 * configuration, never tunes it.
 *
 * No IOKit: the shell hands us a fixed zero-init buffer (filled via
 * ModeSense10) and its size. Every length is device-reported, hence
 * hostile — the shared page walker keeps the declared lengths from
 * steering a read outside [buf, buf+len) and cannot loop (each step
 * strictly advances).
 *
 * MODE SENSE(10) mode parameter header:
 *   [0..1] Mode Data Length (BE) — bytes AFTER this field
 *   [2]    Medium Type   [3] Device-Specific Parameter
 *   [4..5] reserved      [6..7] Block Descriptor Length (BE)
 *   [8 + block_descriptor_length ..] the mode pages, each:
 *     page[0] PS(7) SPF(6) Page Code(5:0); page[1] Page Length (n)
 *     page[2..] page data (n bytes). (Sub-page format SPF=1 has a 4-byte
 *     header with a BE16 length; pages 0x2A/0x01 are page_0 format.)
 *
 * Page 0x2A offsets (relative to page start): loading mechanism page[6]>>5
 * and eject page[6]&0x08 (sr.c cross-check in SPEC.md), buffer size
 * page[12..13] BE KB and lock bits page[6] bit1 supported / bit2 state
 * (MMC-3 page-2A). Page 0x01 is the canonical SPC Read/Write Error
 * Recovery page. A real MODE SENSE capture is a falsifier per the hardware
 * ADR, not a design input. No payload byte is ever used as an offset.
 */


#define MP_HDR  8u   /* MODE SENSE(10) parameter header */

/* Locate a page_0-format mode page by code. On success sets *poff (page
   start) and *plen (data bytes after the 2-byte page header) and returns
   true. Bounded: every iteration advances off by at least the page header,
   so a hostile page-length field cannot loop or read out of bounds. */
static bool mos_internal_mode_page_find(const uint8_t *buf, size_t len,
                                        uint8_t want,
                                        size_t *poff, size_t *plen)
{
    if (!buf || len < MP_HDR) return false;

    size_t declared = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < declared) ? len : declared;
    if (end < MP_HDR) return false;

    size_t bdl = (size_t)(((uint16_t)buf[6] << 8) | buf[7]);
    size_t off = MP_HDR + bdl;

    while (off + 2u <= end) {
        uint8_t  code = buf[off] & 0x3f;
        int      spf  = (buf[off] & 0x40) != 0;
        size_t   hdr, page_len;

        if (spf) {
            if (off + 4u > end) break;
            hdr = 4u;
            page_len = (size_t)(((uint16_t)buf[off + 2] << 8) | buf[off + 3]);
        } else {
            hdr = 2u;
            page_len = buf[off + 1];
        }

        size_t page_total = hdr + page_len;
        if (off + page_total > end) break;   /* page claims past trusted end */

        if (!spf && code == want) {
            *poff = off;
            *plen = page_len;
            return true;
        }
        /* off strictly advances: page_total = hdr + page_len and hdr is 2
           (page_0) or 4 (SPF), so page_total >= 2 — the loop always makes
           progress and terminates when off + page_total exceeds end. */
        off += page_total;
    }
    return false;
}

bool mos_internal_mode_caps_parse(const uint8_t *buf, size_t len,
                                  struct mos_mode_caps *out)
{
    if (!out) return false;
    *out = (struct mos_mode_caps){0};

    size_t poff, plen;
    if (!mos_internal_mode_page_find(buf, len, 0x2A, &poff, &plen))
        return false;
    /* Need page bytes through 13 (buffer size at page[12..13]); the walker
       already bounded poff + 2 + plen to the trusted end. */
    if (plen < 12u) return false;

    const uint8_t *p = &buf[poff];
    out->loading_mechanism = (uint8_t)(p[6] >> 5);
    out->can_eject         = (p[6] & 0x08) != 0;
    out->lock_supported    = (p[6] & 0x02) != 0;
    out->locked            = (p[6] & 0x04) != 0;
    out->buffer_kb         = (uint16_t)((p[12] << 8) | p[13]);
    out->have              = true;
    return true;
}

bool mos_internal_error_recovery_parse(const uint8_t *buf, size_t len,
                                       struct mos_error_recovery *out)
{
    if (!out) return false;
    *out = (struct mos_error_recovery){0};

    size_t poff, plen;
    if (!mos_internal_mode_page_find(buf, len, 0x01, &poff, &plen))
        return false;
    if (plen < 2u) return false;             /* need page bytes 2 and 3 */

    const uint8_t *p = &buf[poff];
    out->awre             = (p[2] & 0x80) != 0;
    out->arre             = (p[2] & 0x40) != 0;
    out->per              = (p[2] & 0x04) != 0;
    out->dcr              = (p[2] & 0x01) != 0;
    out->read_retry_count = p[3];
    out->have             = true;
    return true;
}

/* ==== src/mos_perf.c ==== */
/*
 * mos_perf.c — pure, bounds-safe decode of a GET PERFORMANCE (MMC 0xAC)
 * Performance Data response, TYPE 00h — the type Apple's GetPerformance
 * retrieves (the TYPE 03h write-speed carve-out is in SPEC.md). Direction
 * is the CDB WRITE bit, so the adapter issues this twice (WRITE=0/1); this
 * decode returns the max performance found in one reply.
 *
 * No IOKit: the shell hands us a fixed zero-init buffer (filled via
 * GetPerformance) and its size. Every length is device-reported, hence
 * hostile — the declared length must never steer a read outside
 * [buf, buf+len); only fixed offsets within each descriptor are read.
 *
 * Wire layout (MMC GET PERFORMANCE, Performance Data, TYPE 00h):
 *   [0..3]  Performance Data Length (BE) — bytes AFTER byte 3
 *   [4]     bit1 Write, bit0 Except (echo)   [5..7] reserved
 *   [8..]   Nominal Performance Descriptors, 16 bytes each:
 *     desc[0..3]   Start LBA
 *     desc[4..7]   Start Performance (kB/s, BE)
 *     desc[8..11]  End LBA
 *     desc[12..15] End Performance (kB/s, BE)
 *
 * Layout is the MMC-6 Nominal Performance Descriptor, built to spec (per
 * the hardware ADR a capture falsifies, it does not steer offsets). An
 * empty list (drive declines the direction) is data (count 0), not an
 * error. No payload byte is ever used as an offset.
 */


#define GP_HDR        8u     /* 4-byte data length + 4 reserved/echo */
#define GP_DESC       16u    /* one Nominal Performance Descriptor */
#define GP_DESC_CAP   256u   /* clamp: no real drive lists this many */

static uint32_t mos_internal_gp_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | p[3];
}

/* Decode one Performance Data reply: max performance (kB/s) across its
   descriptors and the count. True when the 8-byte header is present and
   coherent (the descriptor list may be empty). */
bool mos_internal_perf_data_parse(const uint8_t *buf, size_t len,
                                  uint32_t *max_kbps, uint16_t *count)
{
    if (max_kbps) *max_kbps = 0;
    if (count)    *count = 0;
    if (!buf || len < GP_HDR) return false;

    /* Performance Data Length counts bytes AFTER byte 3 (response = 4 +
       value). Declared can only shrink the trusted region; computed wide
       so the +4 cannot wrap. */
    size_t declared = (size_t)mos_internal_gp_be32(&buf[0]) + 4u;
    size_t end = (len < declared) ? len : declared;
    if (end < GP_HDR) return false;

    size_t n = (end - GP_HDR) / GP_DESC;
    if (n > GP_DESC_CAP) n = GP_DESC_CAP;

    uint32_t mx = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *d = &buf[GP_HDR + i * GP_DESC];
        uint32_t sp = mos_internal_gp_be32(&d[4]);    /* Start Performance */
        uint32_t ep = mos_internal_gp_be32(&d[12]);   /* End Performance   */
        if (sp > mx) mx = sp;
        if (ep > mx) mx = ep;
    }

    if (max_kbps) *max_kbps = mx;
    if (count)    *count = (uint16_t)n;
    return true;
}

/* ==== src/mos_physstruct.c ==== */
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

/* ==== src/mos_pure.c ==== */
/*
 * mos_pure.c — the IOKit-free pure surface: BSD-name predicates, the
 * SCSI-status contention test, tray-outcome and IOReturn→mos_error
 * classifiers, TOC/GESN decoders, and the dual-length helper. No IOKit or
 * CoreFoundation, so the whole pure layer is fuzz/ASan-tested without any
 * Apple framework. See ARCHITECTURE.md §3 and AGENTS.md rule 3.
 */


#include <string.h>

bool mos_internal_bsd_name_is_whole_shape(const char *bsd_name)
{
    if (!bsd_name || !*bsd_name) return false;
    const char *p = bsd_name;
    if (strncmp(p, "disk", 4) == 0)  p += 4;
    else if (strncmp(p, "rdisk", 5) == 0) p += 5;
    else return false;
    /* Require at least one digit. */
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
    /* Must end here; any trailing characters (like "s1") mean partition. */
    return *p == 0;
}

bool mos_internal_bsd_unit_matches(const char *reported, int64_t whole_unit)
{
    if (!reported || whole_unit < 0) return false;
    if (strncmp(reported, "disk", 4) != 0) return false;
    const char *p = reported + 4;
    if (*p < '0' || *p > '9') return false;        /* must carry a unit */
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return false;          /* overflow → no match */
    }
    if ((int64_t)v != whole_unit) return false;    /* disk40 vs disk4: 40 != 4 */
    /* Suffix: zero or more (s + digits) segments, nothing else. */
    while (*p == 's') {
        ++p;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') ++p;
    }
    return *p == '\0';
}

const char *mos_internal_normalize_bsd_name(const char *in)
{
    if (!in) return NULL;
    const char *bsd_name = strncmp(in, "/dev/", 5) == 0 ? in + 5 : in;
    if (strncmp(bsd_name, "rdisk", 5) == 0) bsd_name++;
    return bsd_name;
}

/* See mos_pure.h. Normalize, reject non-whole-disk shapes, parse digits to
   an int64 unit with a 32-bit overflow guard (-1 on any reject). Reuses
   normalize + is_whole_shape so the accepted-form rules live in one place. */
int64_t mos_internal_parse_bsd_unit(const char *name)
{
    const char *n = mos_internal_normalize_bsd_name(name);
    if (!mos_internal_bsd_name_is_whole_shape(n)) return -1;
    const char *p = n + 4;          /* is_whole_shape guarantees "disk" + >=1 digit */
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return -1;
    }
    return (int64_t)v;
}

/* SAM-5 §5.3: four status values that all mean "drive is contended."
     0x08 BUSY                — standard busy
     0x18 RESERVATION_CONFLICT — another initiator holds it
     0x28 TASK_SET_FULL        — drive queue is full
     0x30 ACA_ACTIVE           — auto-contingent-allegiance active
   All four surface to the caller as MOS_STATE_BUSY. Pinned by
   tests/test_scsi_status.c. */
bool mos_internal_status_is_contended(uint32_t status)
{
    return status == MOS_SCSI_STATUS_BUSY                ||
           status == MOS_SCSI_STATUS_RESERVATION_CONFLICT ||
           status == MOS_SCSI_STATUS_TASK_SET_FULL        ||
           status == MOS_SCSI_STATUS_ACA_ACTIVE;
}

/* Tray-command outcome classifier (START STOP UNIT 0x1B, PREVENT ALLOW
   MEDIUM REMOVAL 0x1E). A command that ANSWERED is a reported fact, not a
   transport error:
     GOOD                    -> DONE
     CHECK CONDITION 5/53/02 -> REFUSED_LOCKED  (eject/close hit a basic
                                Prevent lock; 04-349r1 §6.18.3.3 / Table 9)
     any other non-GOOD      -> REFUSED_OTHER   (e.g. a drive without the
                                PDTE Persistent Prevent state answering
                                0x02/0x03 with 5/24/00)
   The caller maps transport/lock failure (BUSY, NO_DEVICE, IO) to a
   negative mos_error before this runs; here only answered commands. */
mos_tray_outcome mos_internal_tray_classify(uint32_t scsi_status,
                                            uint8_t sk, uint8_t asc, uint8_t ascq)
{
    if (scsi_status == MOS_SCSI_STATUS_GOOD)        return MOS_TRAY_DONE;
    if (sk == 0x05 && asc == 0x53 && ascq == 0x02)  return MOS_TRAY_REFUSED_LOCKED;
    return MOS_TRAY_REFUSED_OTHER;
}

/* IOReturn → mos_error mapping. Pure (int32_t in); the Apple adapter casts
   at the call site, and mos_scsi.c static_asserts every SDK constant to
   the numeric value here so an SDK change fails the build loudly.

   IOReturn = 0xE0000000 | code (sys_iokit | sub_iokit_common, IOReturn.h).
   Two groupings carry weight:
     NoDevice / NotAttached → MOS_ERR_NO_DEVICE — must NOT collapse to
       MOS_ERR_IO; the watch core treats NO_DEVICE as terminal removal.
     NoMemory / NoResources → MOS_ERR_OOM — runtime exhaustion; distinct
       from MOS_ERR_DRIVER_REJECTED (mos_scsi.c, factory returned NULL).

   Pinned by tests/test_ioreturn.c — every case has a test. */
mos_error mos_internal_ioreturn_to_error(int32_t rc)
{
    /* Switch on uint32_t: the 0xE0000000-prefixed case literals would
       otherwise be negative ints (implementation-defined). */
    switch ((uint32_t)rc) {
        case 0x00000000u: return MOS_OK;                  /* kIOReturnSuccess         */
        case 0xE00002BDu: return MOS_ERR_OOM;             /* kIOReturnNoMemory        */
        case 0xE00002BEu: return MOS_ERR_OOM;             /* kIOReturnNoResources     */
        case 0xE00002C0u: return MOS_ERR_NO_DEVICE;       /* kIOReturnNoDevice        */
        case 0xE00002C2u: return MOS_ERR_INVALID_ARG;     /* kIOReturnBadArgument     */
        case 0xE00002C5u: return MOS_ERR_EXCLUSIVE_ACCESS;/* kIOReturnExclusiveAccess */
        case 0xE00002C7u: return MOS_ERR_UNSUPPORTED;     /* kIOReturnUnsupported     */
        case 0xE00002D5u: return MOS_ERR_BUSY;            /* kIOReturnBusy            */
        case 0xE00002D6u: return MOS_ERR_TIMEOUT;         /* kIOReturnTimeout         */
        case 0xE00002D9u: return MOS_ERR_NO_DEVICE;       /* kIOReturnNotAttached     */
        default:          return MOS_ERR_IO;
    }
}

/* READ TOC/PMA/ATIP format 0000b layout (MMC-6 §6.27.2.3): 2-byte TOC
   Data Length (BE, counts bytes AFTER itself), first/last track; then
   8-byte descriptors — [1]=ADR<<4|CONTROL, [2]=track (0xAA=lead-out),
   [4..7]=start LBA (BE, MSF=0). Cross-checked against Linux sr.c / cdrom.h
   and libcdio. Contract in mos_pure.h. */
bool mos_internal_toc_parse(const uint8_t *buf, size_t len, mos_toc *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!buf || len < 4) return false;

    /* TOC Data Length counts bytes AFTER its own two; 64-bit total,
       clamped by the trusted length (O-4). */
    uint64_t claimed = 2u + (uint64_t)(((uint32_t)buf[0] << 8) | buf[1]);
    size_t span = mos_internal_trusted_len(len, len, claimed);
    if (span < 4) return false;

    out->first_track = buf[2];
    out->last_track  = buf[3];

    size_t  cursor = 4;
    uint8_t prev_track = 0;
    while (cursor + 8 <= span) {
        uint8_t adr_ctrl = buf[cursor + 1];
        uint8_t track    = buf[cursor + 2];
        uint32_t lba = ((uint32_t)buf[cursor + 4] << 24)
                     | ((uint32_t)buf[cursor + 5] << 16)
                     | ((uint32_t)buf[cursor + 6] <<  8)
                     |  (uint32_t)buf[cursor + 7];

        if (track == 0xAA) {
            if (out->have_leadout) return false;   /* duplicate lead-out  */
            out->have_leadout = true;
            out->leadout_lba  = lba;
        } else if (track >= 1 && track <= 99) {
            if (out->have_leadout)   return false; /* track after lead-out */
            if (track <= prev_track) return false; /* dup / non-ascending  */
            if (out->track_count >= MOS_TOC_MAX_TRACKS) return false;
            mos_toc_entry *e = &out->tracks[out->track_count++];
            e->track     = track;
            e->adr       = (uint8_t)(adr_ctrl >> 4);
            e->control   = (uint8_t)(adr_ctrl & 0x0F);
            e->start_lba = lba;
            prev_track   = track;
        } else {
            return false;                          /* 0 / reserved range   */
        }
        cursor += 8;
    }
    /* A trailing partial descriptor in the claimed span is malformed, not
       padding. */
    if (cursor != span) return false;

    /* The header range bytes are hostile too. The walk proved the
       descriptors well-formed; identity also needs them to BE the table
       the header declares — ascending + unique + count == last-first+1 +
       matching endpoints forces exactly first..last (pigeonhole). A TOC
       that omits declared tracks or declares an inverted/out-of-range
       header is rejected whole: a fingerprint over it would be falsely
       stable across genuinely different discs. */
    if (out->first_track < 1 || out->first_track > 99) return false;
    if (out->last_track < out->first_track || out->last_track > 99) return false;
    if (out->track_count != out->last_track - out->first_track + 1) return false;
    if (out->tracks[0].track != out->first_track ||
        out->tracks[out->track_count - 1].track != out->last_track) return false;
    return true;
}

size_t mos_internal_trusted_len(size_t allocated, size_t transferred,
                                uint64_t claimed)
{
    /* Allocator and transport are both on our side of the seam; the
       smaller of the two is the largest region provably containing only
       bytes the kernel wrote this transfer. (transferred > allocated is
       itself a transport fault; min() handles it either way.) */
    size_t trusted = allocated < transferred ? allocated : transferred;

    /* The device claim is hostile, and participates only as a clamp: a
       drive honestly reporting it returned LESS is believed; one claiming
       MORE than the transfer is lying and ignored. Compared in uint64_t so
       a caller-computed total (e.g. `data_length + header`) cannot have
       wrapped on the way in. */
    if (claimed < (uint64_t)trusted)
        trusted = (size_t)claimed;

    return trusted;
}

/* Decode the Door/Tray-open bit from a GET EVENT STATUS NOTIFICATION (0x4A)
   Media-class polled reply. Byte map: ARCHITECTURE.md §4.2.

   True + *door_open ONLY for an authoritative Media event descriptor —
   ALL of:
     - >= 6 bytes present (4-byte header + >=2 descriptor bytes),
     - Event Data Length (bytes 0-1, BE, excludes itself) claims >= 6
       following bytes (full-span, not a NEA stub),
     - NEA bit (byte 2, 0x80) clear,
     - Notification Class (byte 2, low 3 bits) == Media (4).
   Otherwise false ("no authoritative bit") and the state core forks on the
   TUR sense — the honesty GetTrayState discards (it reports closed+success
   on a GESN failure).

   Bit positions per Linux sr.c media_event_desc. */
bool mos_internal_gesn_media_door_open(const uint8_t *resp, size_t len,
                                       bool *door_open)
{
    if (!resp || !door_open) return false;
    if (len < 6) return false;                       /* header(4) + ≥2 desc bytes   */

    uint16_t event_data_len = (uint16_t)((resp[0] << 8) | resp[1]);
    if (event_data_len < 6) return false;            /* device claims no full desc  */
    if (resp[2] & 0x80) return false;                /* NEA: descriptor not valid   */
    if ((resp[2] & 0x07) != 0x04) return false;      /* not the Media class         */

    /* Media Status byte (descriptor byte 1 = response byte 5): bit0 DoorOpen. */
    *door_open = (resp[5] & 0x01) != 0;
    return true;
}

bool mos_internal_value_is_registry_id(uint64_t v)
{
    return v >= MOS_REGISTRY_ID_FLOOR;
}

/* ==== src/mos_query.c ==== */
/*
 * mos_query.c — the typed MMC query surface. Each mos_query_* verb issues one
 * MMCDeviceInterface convenience command, hands the reply to a pure decoder,
 * caches the result on the handle, and returns a borrowed pointer.
 *
 * Every command here is a NON-EXCLUSIVE convenience method — including
 * mos_query_capacity's formattable view, issued via
 * MMCDeviceInterface->ReadFormatCapacities (READ FORMAT CAPACITIES, 0x23).
 * That wrapper is present in SCSITaskLib.h (verified MacOSX 10.5–11.3), so
 * 0x23 is NOT a raw verb — correcting the earlier "fifth raw CDB" call (see
 * the AGENTS.md ADR and
 * doc/research/2026-06-18-readformatcapacities-convenience-exists.md). With no
 * ObtainExclusiveAccess it works on MOUNTED media too. It is gated on the
 * current PROFILE (a cheap GET CONFIGURATION): only formattable media
 * (rewritable + BD-R) has a formattable view, so pressed / write-once
 * CD-R,DVD±R / empty media issue no read and the view stays unset there.
 */


#include <string.h>

mos_error mos_query_disc_info(mos_handle_t *h, const mos_disc_info **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* 34 bytes: the fixed numeric region plus lead-in/lead-out addresses,
       matching the readdiscinfo_*.bin fixtures. The convenience method
       reports no realized count, so sizeof buf is the trusted length
       (dual-length rule O-4); the reply's own Disc Information Length can
       only shrink the decode, never extend it. */
    uint8_t         buf[34] = {0};
    SCSITaskStatus  st      = 0;
    SCSI_Sense_Data sd      = {0};

    IOReturn rc = (*h->mmc)->ReadDiscInformation(
        h->mmc, buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        /* Transport failure maps its IOReturn; a command that reached the
           drive but gave no usable data (no medium, a unit that rejects
           0x51) is MOS_ERR_IO — out-of-band, never mistakable for a real
           all-zero answer. This convention repeats across the verbs below. */
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_disc_info_parse(buf, sizeof(buf), &h->disc_info)) {
        return MOS_ERR_IO;   /* short reply — refused whole */
    }
    *out = &h->disc_info;
    return MOS_OK;
}

/* Worst-case CDTOC blob (also reused by mos_query_session_layout below): a
   conforming CD never approaches this; the parser is bounds-safe on truncation
   regardless (dual-length rule O-4). */
#define MOS_CDTOC_REPLY_BUF 4096u

mos_error mos_query_toc(mos_handle_t *h, const mos_toc **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* PRIMARY (CD): the macOS kernel-cached full-TOC (kIOCDMediaTOCKey) — a
       superset of format-0000b, read with ZERO SCSI commands and no exclusive
       access, fresh off the current IOCDMedia node. CD-only by construction:
       the read returns 0 for DVD/BD (no IOCDMedia node) and for a just-inserted
       CD whose node isn't up yet — both fall through to the issued READ TOC
       below, which stays the universal path and the only one for DVD/BD.
       See the AGENTS.md ADR. */
    mos_internal_refresh_media_identity(h);
    uint8_t cdtoc[MOS_CDTOC_REPLY_BUF];
    size_t  clen = mos_internal_read_cdtoc(h->svc, cdtoc, sizeof cdtoc);
    if (clen && mos_internal_cdtoc_to_toc(cdtoc, clen, &h->toc)) {
        *out = &h->toc;
        return MOS_OK;
    }

    /* FALLBACK: the issued READ TOC/PMA/ATIP format 0000b. Worst case: 4-byte
       header + 100 descriptors (99 tracks + lead-out) x 8. sizeof buf is the
       trusted length (O-4); the reply's own TOC Data Length only shrinks the
       parse. MSF=0 (LBA), starting track 0 (= from first). */
    uint8_t         buf[4 + 100 * 8] = {0};
    SCSITaskStatus  st               = 0;
    SCSI_Sense_Data sd               = {0};

    IOReturn rc = (*h->mmc)->ReadTableOfContents(
        h->mmc, 0 /*LBA*/, 0x00 /*format*/, 0 /*from first track*/,
        buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_toc_parse(buf, sizeof(buf), &h->toc)) {
        return MOS_ERR_IO;   /* incoherent TOC — refused whole */
    }
    *out = &h->toc;
    return MOS_OK;
}

mos_error mos_query_cdtext(mos_handle_t *h, const mos_cdtext **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* READ TOC/PMA/ATIP format 0101b (CD-TEXT). 256 packs (4612 bytes)
       holds any real disc's album-level blocks several times over; a longer
       reply is clamped to sizeof buf (the trusted length, O-4) and the
       reply's own CD-TEXT Data Length only shrinks the parse. The
       track/session parameter is reserved here — passed 0. */
    uint8_t         buf[4 + 256 * 18] = {0};
    SCSITaskStatus  st                = 0;
    SCSI_Sense_Data sd                = {0};

    IOReturn rc = (*h->mmc)->ReadTableOfContents(
        h->mmc, 0 /*LBA*/, 0x05 /*CD-TEXT*/, 0 /*reserved*/,
        buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_cdtext_parse(buf, sizeof(buf), &h->cdtext)) {
        return MOS_ERR_IO;   /* no CD-TEXT / no usable album field */
    }
    *out = &h->cdtext;
    return MOS_OK;
}

mos_error mos_query_drive_caps(mos_handle_t *h, const mos_drive_caps **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* RT=0: header + every feature the drive implements. 1024 bytes holds
       real feature lists several times over (a loaded BD-RE runs ~400
       bytes); a longer claim is clamped to sizeof buf, and the reply's own
       lengths only shrink the walk (O-4). Feature absent (every non-BD
       drive) decodes to aacs=false — data, not an error. */
    uint8_t         buf[1024] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->GetConfiguration(
        h->mmc,
        (UInt8)0x00,             /* RT = 00b, all features              */
        (UInt16)0x0000,          /* starting feature number             */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    mos_internal_protection_from_config(buf, sizeof(buf), &h->caps);
    /* Same RT=0 reply carries the Profile List feature (0x0000); decode the
       drive-static supported-profile set from it (protection_from_config zeroed
       the struct first, so profile_count stays 0 if the feature is absent). */
    mos_internal_profile_list_from_config(buf, sizeof(buf), h->caps.profiles,
                                          MOS_DRIVE_PROFILE_CAP,
                                          &h->caps.profile_count);
    /* Firmware creation timestamp (feature 010Ch) from the same RT=0 reply. */
    mos_internal_firmware_date_from_config(buf, sizeof(buf),
                                           h->caps.firmware_date,
                                           sizeof h->caps.firmware_date);
    /* Current Profile (loaded medium) from the same RT=0 header — 0 when the
       field is absent/truncated or the tray is empty. Media-dependent; used
       only to name the loaded disc's class (e.g. speed 1x scaling). */
    if (!mos_internal_config_current_profile(buf, sizeof(buf),
                                             &h->caps.current_profile))
        h->caps.current_profile = 0;
    *out = &h->caps;
    return MOS_OK;
}

mos_error mos_enumerate_features(mos_handle_t *h,
                                 bool (*cb)(const mos_feature_info_t *f,
                                            void *ctx),
                                 void *ctx)
{
    if (!h || !h->mmc || !cb) return MOS_ERR_INVALID_ARG;

    /* Same issuance and trust terms as mos_query_drive_caps: RT=0 into a
       1024-byte buffer, sizeof buf trusted, reply lengths shrink-only (O-4). */
    uint8_t         buf[1024] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->GetConfiguration(
        h->mmc, (UInt8)0x00, (UInt16)0x0000,
        buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    size_t             cursor = 8;
    mos_config_feature feat;
    while (mos_internal_config_next_feature(buf, sizeof buf, &cursor, &feat)) {
        mos_feature_info_t info = {
            .code       = feat.feature_code,
            .current    = feat.current,
            .persistent = feat.persistent,
            .version    = feat.version,
        };
        if (!cb(&info, ctx)) break;     /* caller-requested stop, not an error */
    }
    return MOS_OK;
}

mos_error mos_query_disc_id(mos_handle_t *h, const mos_disc_id **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* One-shot read of the full BD Disc Information into a fixed buffer
       (BD DI maxes ~3588 bytes; 4096 covers it). Deliberately not
       dvd+rw-mediainfo's two-phase read-length-then-realloc: a single
       fixed buffer means no device-reported length ever drives an
       allocation or second transfer. sizeof buf is the trusted length
       (O-4); the reply's own Disc Structure Data Length only shrinks the
       parse, and an under-filled reply leaves zeros that fail the 'DI'
       gate. MEDIA_TYPE=1 (BD), FORMAT=0x00 (DI), ADDRESS/LAYER 0. */
    uint8_t         buf[4096] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->ReadDiscStructure(
        h->mmc,
        (UInt8)0x01,             /* MEDIA_TYPE = Blu-ray            */
        (UInt32)0,               /* ADDRESS                          */
        (UInt8)0,                /* LAYER_NUMBER                     */
        (UInt8)0x00,             /* FORMAT = Disc Information (DI)    */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_bd_disc_id_parse(buf, sizeof(buf), &h->disc_id)) {
        return MOS_ERR_IO;   /* not a DI reply (non-BD, or refused) */
    }
    *out = &h->disc_id;
    return MOS_OK;
}

mos_error mos_query_physical_structure(mos_handle_t *h,
                                       const mos_physical_structure **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* Two READ DISC STRUCTURE reads for DVD/HD-DVD (MEDIA_TYPE=0):
       FORMAT 0x00 (Physical Format Info) and 0x01 (Copyright Management).
       sizeof buf is the trusted length (O-4); the reply's own length only
       shrinks the parse, and an under-filled reply fails the per-format
       min-length gate. The two reads are independent (partial-readability
       ladder), so a drive that answers one format but not the other still
       yields the half it gave; both merge into one handle-owned struct. */
    struct mos_physical_structure *d = &h->physical_structure;
    *d = (struct mos_physical_structure){0};

    uint8_t         buf[2048] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->ReadDiscStructure(
        h->mmc,
        (UInt8)0x00,             /* MEDIA_TYPE = DVD / HD-DVD       */
        (UInt32)0,               /* ADDRESS                          */
        (UInt8)0,                /* LAYER_NUMBER                     */
        (UInt8)0x00,             /* FORMAT = Physical Format Info    */
        buf, (UInt16)sizeof(buf),
        &st, &sd);
    if (rc == kIOReturnSuccess && st == kSCSITaskStatus_GOOD)
        (void)mos_internal_physical_format_parse(buf, sizeof(buf), d);

    memset(buf, 0, sizeof buf);
    st = 0;
    sd = (SCSI_Sense_Data){0};
    rc = (*h->mmc)->ReadDiscStructure(
        h->mmc,
        (UInt8)0x00,             /* MEDIA_TYPE = DVD / HD-DVD       */
        (UInt32)0,               /* ADDRESS                          */
        (UInt8)0,                /* LAYER_NUMBER                     */
        (UInt8)0x01,             /* FORMAT = Copyright Management    */
        buf, (UInt16)sizeof(buf),
        &st, &sd);
    if (rc == kIOReturnSuccess && st == kSCSITaskStatus_GOOD)
        (void)mos_internal_copyright_mgmt_parse(buf, sizeof(buf), d);

    if (!d->have_physical && !d->have_copyright) {
        return MOS_ERR_IO;   /* neither format answered (non-DVD or refused) */
    }
    *out = d;
    return MOS_OK;
}

mos_error mos_query_track_info(mos_handle_t *h, const mos_track_info **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* READ TRACK INFORMATION (0x52), first track. ADDRESS_TYPE = 01b
       (logical track number), ADDRESS = 1 — well-defined on any media with
       a track. 64 bytes covers the Track Information Block (core 36 bytes;
       MMC-6 extends it slightly). sizeof buf is the trusted length (O-4);
       the reply's Track Information Length only shrinks the parse.
       Signature confirmed against SCSITaskLib.h (ADDRESS_NUMBER_TYPE,
       LBA/track/session, buffer, bufferSize, taskStatus, senseData). */
    uint8_t         buf[64] = {0};
    SCSITaskStatus  st      = 0;
    SCSI_Sense_Data sd      = {0};

    IOReturn rc = (*h->mmc)->ReadTrackInformation(
        h->mmc,
        (UInt8)0x01,             /* ADDRESS_TYPE = logical track number */
        (UInt32)1,               /* ADDRESS = first track               */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_track_info_parse(buf, sizeof(buf), &h->track_info)) {
        return MOS_ERR_IO;   /* short reply — refused whole */
    }
    *out = &h->track_info;
    return MOS_OK;
}

mos_error mos_query_session_layout(mos_handle_t *h,
                                   const mos_session_layout **out)
{
    if (out) *out = NULL;
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    /* The IOCDMedia node is media-scoped: re-resolve so a handle held across a
       media change reads the CURRENT disc's cached TOC, not the open-time one
       (same freshness contract as capacity/state). No SCSI command. */
    mos_internal_refresh_media_identity(h);

    uint8_t buf[MOS_CDTOC_REPLY_BUF];
    size_t len = mos_internal_read_cdtoc(h->svc, buf, sizeof buf);
    if (len == 0) return MOS_ERR_IO;        /* not a CD, no media, no property */

    if (!mos_internal_cdtoc_parse(buf, len, &h->session_layout)) {
        return MOS_ERR_IO;                  /* unparseable / no boundaries */
    }
    *out = &h->session_layout;
    return MOS_OK;
}

/* READ FORMAT CAPACITIES (0x23) via the ReadFormatCapacities convenience
   method — the same non-exclusive MMCDeviceInterface wrapper class as the
   verbs above (SCSITaskLib.h, verified MacOSX 10.5–11.3). Fills the formattable
   view: how big the medium is now, whether it is unformatted, and the
   capacities it could be formatted to (the blank-rewritable gap the other two
   capacity sources can't reach). No ObtainExclusiveAccess, so it also works on
   MOUNTED media. Read-only: never FORMAT UNIT. */
#define MOS_FORMATCAP_REPLY_BUF 260u   /* 4-byte header + up to 32 * 8 desc */

static bool mos_internal_read_format_caps(mos_handle_t *h,
                                          struct mos_format_caps *out)
{
    if (out) *out = (struct mos_format_caps){0};
    if (!h || !h->mmc || !out) return false;

    uint8_t         buf[MOS_FORMATCAP_REPLY_BUF] = {0};
    SCSITaskStatus  st                           = 0;
    SCSI_Sense_Data sd                           = {0};

    IOReturn rc = (*h->mmc)->ReadFormatCapacities(
        h->mmc, buf, (UInt16)sizeof(buf), &st, &sd);
    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD)
        return false;   /* no medium / unit rejects 0x23 → formattable unset */

    /* The convenience method reports no realized count, so sizeof buf is the
       trusted length (dual-length rule O-4); the reply's own Capacity List
       Length can only shrink the decode. */
    return mos_internal_format_caps_parse(buf, sizeof buf, out);
}

mos_error mos_query_capacity(mos_handle_t *h, const mos_capacity **out)
{
    if (out) *out = NULL;
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    struct mos_capacity *c = &h->capacity;
    *c = (struct mos_capacity){0};

    /* Held-handle freshness: re-resolve so the size reflects the current
       disc, not the open-time one (mos_internal_refresh_media_identity). */
    mos_internal_refresh_media_identity(h);

    /* (a) Whole-disk byte capacity from the kernel's attach-time READ
       CAPACITY, cached on the IOMedia node (no command, works on mounted
       media). 0 == absent: a blank/absent disc has no whole-disk node. */
    c->media_bytes = h->media_bytes;
    c->block_bytes = h->media_block_bytes;

    /* (b) Recordable / append-state via a fresh READ TRACK INFORMATION.
       Best-effort and independent of (a): a drive that rejects 0x52 just
       leaves have_recordable false. Guard on the MMC interface so a handle
       lacking it still returns the media-size half. */
    if (h->mmc) {
        const mos_track_info *t = NULL;
        if (mos_query_track_info(h, &t) == MOS_OK && t) {
            c->have_recordable = true;
            c->nwa_valid     = mos_track_info_nwa_valid(t);
            c->free_blocks   = mos_track_info_free_blocks(t);
            c->next_writable = mos_track_info_next_writable(t);
            c->track_size    = mos_track_info_track_size(t);
        }

        /* (c) Formattable view via READ FORMAT CAPACITIES (0x23), issued
           through the non-exclusive ReadFormatCapacities convenience method —
           the blank-rewritable gap (a)/(b) can't fill (no whole-disk node, no
           track). Gated on the current profile (a cheap, non-exclusive GET
           CONFIGURATION): only formattable media (rewritable + BD-R) has a
           formattable view, so for pressed / write-once CD-R,DVD±R / empty
           media we issue no read. No lock, so it also works on a MOUNTED
           formattable disc (a mounted BD-RE/DVD-RAM still reports its view). */
        uint16_t profile = 0;
        if (mos_internal_mmc_get_current_profile(h, &profile) == MOS_OK &&
            mos_internal_profile_is_formattable(profile))
            c->have_formattable =
                mos_internal_read_format_caps(h, &c->formattable);
    }

    *out = c;
    return MOS_OK;
}

/* One GET PERFORMANCE (0xAC) Performance Data read in the given direction
   (WRITE=0 read, WRITE=1 write). TOLERANCE=10b nominal, EXCEPT=0,
   STARTING_LBA=0. Returns the decoded max kB/s + descriptor count. */
static mos_error mos_internal_get_perf(mos_handle_t *h, uint8_t write,
                                       uint32_t *max_kbps, uint16_t *count)
{
    uint8_t         buf[2048] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->GetPerformance(
        h->mmc,
        (UInt8)0x02,             /* TOLERANCE = 10b (nominal)      */
        (UInt8)write,            /* WRITE                          */
        (UInt8)0x00,             /* EXCEPT = 0 (nominal perf)      */
        (UInt32)0,               /* STARTING_LBA                   */
        (UInt16)64,              /* MAXIMUM_NUMBER_OF_DESCRIPTORS  */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }
    if (!mos_internal_perf_data_parse(buf, sizeof(buf), max_kbps, count)) {
        return MOS_ERR_IO;   /* short/incoherent header */
    }
    return MOS_OK;
}

mos_error mos_query_drive_perf(mos_handle_t *h, const mos_drive_perf **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* Two Performance Data reads assembled into one result. The read
       direction is the gate (defines `have`); the write direction is
       best-effort (read-only drive or non-writable medium leaves
       max_write_kbps 0). */
    struct mos_drive_perf *p = &h->drive_perf;
    *p = (struct mos_drive_perf){0};

    uint32_t rd_max = 0, wr_max = 0;
    uint16_t rd_cnt = 0, wr_cnt = 0;

    mos_error e = mos_internal_get_perf(h, 0, &rd_max, &rd_cnt);
    if (e != MOS_OK) return e;

    (void)mos_internal_get_perf(h, 1, &wr_max, &wr_cnt);  /* best-effort */

    p->descriptor_count = rd_cnt;
    p->max_read_kbps    = rd_max;
    p->max_write_kbps   = wr_max;
    p->have             = (rd_cnt > 0);   /* read direction is the gate */
    *out = p;
    return MOS_OK;
}

/* Shared MODE SENSE(10) issuance for the two read-only optical pages
   (AGENTS.md scope doctrine, layer 2). Signature confirmed against
   SCSITaskLib.h (LLBAA, DBD, PC, PAGE_CODE, buffer, bufferSize, taskStatus,
   senseData). PC = 00b (current values); DBD=1 (no block descriptor) keeps
   the reply compact, though the walker tolerates a descriptor either way. */
static mos_error mos_internal_mode_sense10(mos_handle_t *h, uint8_t page,
                                           uint8_t *buf, size_t buf_len)
{
    SCSITaskStatus  st = 0;
    SCSI_Sense_Data sd = {0};
    IOReturn rc = (*h->mmc)->ModeSense10(
        h->mmc,
        (UInt8)0,                /* LLBAA = 0                           */
        (UInt8)1,                /* DBD = 1 (disable block descriptor)  */
        (UInt8)0x00,             /* PC = current values                 */
        (UInt8)page,             /* PAGE_CODE                           */
        buf, (UInt16)buf_len,
        &st, &sd);
    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }
    return MOS_OK;
}

mos_error mos_query_mode_caps(mos_handle_t *h, const mos_mode_caps **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    uint8_t   buf[96] = {0};
    mos_error e = mos_internal_mode_sense10(h, 0x2A, buf, sizeof buf);
    if (e != MOS_OK) return e;

    if (!mos_internal_mode_caps_parse(buf, sizeof(buf), &h->mode_caps)) {
        return MOS_ERR_IO;   /* page 0x2A absent or short — refused whole */
    }
    *out = &h->mode_caps;
    return MOS_OK;
}

mos_error mos_query_error_recovery(mos_handle_t *h,
                                   const mos_error_recovery **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    uint8_t   buf[64] = {0};
    mos_error e = mos_internal_mode_sense10(h, 0x01, buf, sizeof buf);
    if (e != MOS_OK) return e;

    if (!mos_internal_error_recovery_parse(buf, sizeof(buf),
                                           &h->error_recovery)) {
        return MOS_ERR_IO;   /* page 0x01 absent or short — refused whole */
    }
    *out = &h->error_recovery;
    return MOS_OK;
}

/* ==== src/mos_result.c ==== */
/*
 * mos_result.c — accessors for the opaque query-result objects (layout in
 * mos_pure.h, may grow appended fields without breaking ABI). Pure, no IOKit.
 * Every accessor tolerates a NULL object, returning a benign zero/NULL.
 */


/* ---- mos_state_result --------------------------------------------- */

mos_state mos_state_result_state(const mos_state_result *r)
{
    return r ? r->state : MOS_STATE_UNKNOWN;
}

uint64_t mos_state_result_registry_id(const mos_state_result *r)
{
    return r ? r->registry_id : 0;
}

int64_t mos_state_result_bsd_unit(const mos_state_result *r)
{
    return r ? r->bsd_unit : -1;
}

const char *mos_state_result_vendor(const mos_state_result *r)
{
    return r ? r->vendor : NULL;
}

const char *mos_state_result_product(const mos_state_result *r)
{
    return r ? r->product : NULL;
}

const char *mos_state_result_revision(const mos_state_result *r)
{
    return r ? r->revision : NULL;
}

const char *mos_state_result_media_type(const mos_state_result *r)
{
    return r ? r->media_type : NULL;
}

int mos_state_result_writable(const mos_state_result *r)
{
    return r ? r->writable : -1;
}

uint16_t mos_state_result_current_profile(const mos_state_result *r)
{
    return r ? r->current_profile : 0;
}

void mos_state_result_sense(const mos_state_result *r,
                            uint8_t *sense_key, uint8_t *asc, uint8_t *ascq)
{
    if (sense_key) *sense_key = r ? r->sense_key : 0;
    if (asc)       *asc       = r ? r->asc       : 0;
    if (ascq)      *ascq      = r ? r->ascq      : 0;
}

/* ---- mos_watch_event ---------------------------------------------- */

mos_event_kind mos_watch_event_kind(const mos_watch_event *e)
{
    return e ? e->kind : MOS_EVENT_SNAPSHOT;
}

uint64_t mos_watch_event_seq(const mos_watch_event *e)
{
    return e ? e->seq : 0;
}

const char *mos_watch_event_ts(const mos_watch_event *e)
{
    return e ? e->ts : NULL;
}

uint64_t mos_watch_event_registry_id(const mos_watch_event *e)
{
    return e ? e->registry_id : 0;
}

uint64_t mos_watch_event_stream_open_ms(const mos_watch_event *e)
{
    return e ? e->stream_open_wall_ms : 0;
}

int64_t mos_watch_event_bsd_unit(const mos_watch_event *e)
{
    return e ? e->bsd_unit : -1;
}

const char *mos_watch_event_vendor(const mos_watch_event *e)
{
    return e ? e->vendor : NULL;
}

const char *mos_watch_event_product(const mos_watch_event *e)
{
    return e ? e->product : NULL;
}

const char *mos_watch_event_revision(const mos_watch_event *e)
{
    return e ? e->revision : NULL;
}

const char *mos_watch_event_serial(const mos_watch_event *e)
{
    return e ? e->serial : NULL;
}

const char *mos_watch_event_media_type(const mos_watch_event *e)
{
    return e ? e->media_type : NULL;
}

int mos_watch_event_writable(const mos_watch_event *e)
{
    return e ? e->writable : -1;
}

mos_state mos_watch_event_state(const mos_watch_event *e)
{
    return e ? e->state : MOS_STATE_UNKNOWN;
}

mos_state mos_watch_event_prev_state(const mos_watch_event *e)
{
    return e ? e->prev_state : MOS_STATE_UNKNOWN;
}

uint16_t mos_watch_event_current_profile(const mos_watch_event *e)
{
    return e ? e->current_profile : 0;
}

void mos_watch_event_sense(const mos_watch_event *e,
                           uint8_t *sense_key, uint8_t *asc, uint8_t *ascq)
{
    if (sense_key) *sense_key = e ? e->sense_key : 0;
    if (asc)       *asc       = e ? e->asc       : 0;
    if (ascq)      *ascq      = e ? e->ascq      : 0;
}

mos_error mos_watch_event_error(const mos_watch_event *e)
{
    return e ? e->error : MOS_OK;
}

uint32_t mos_watch_event_latency_ms(const mos_watch_event *e)
{
    return e ? e->latency_ms : 0;
}

/* ---- mos_disc_info -------------------------------------------------- */

mos_disc_status mos_disc_info_status(const mos_disc_info *d)
{
    return d ? d->status : MOS_DISC_OTHER;
}

bool mos_disc_info_erasable(const mos_disc_info *d)
{
    return d ? d->erasable : false;
}

uint8_t mos_disc_info_first_track(const mos_disc_info *d)
{
    return d ? d->first_track_on_disc : 0;
}

uint16_t mos_disc_info_session_count(const mos_disc_info *d)
{
    return d ? d->number_of_sessions : 0;
}

uint16_t mos_disc_info_first_track_last_session(const mos_disc_info *d)
{
    return d ? d->first_track_last_session : 0;
}

uint16_t mos_disc_info_last_track_last_session(const mos_disc_info *d)
{
    return d ? d->last_track_last_session : 0;
}

uint8_t mos_disc_info_last_session_state(const mos_disc_info *d)
{
    return d ? d->last_session_state : 0;
}

uint8_t mos_disc_info_bg_format_status(const mos_disc_info *d)
{
    return d ? d->bg_format_status : 0;
}

/* ---- mos_toc accessors (mos_query_toc) ------------------------------- *
 * NULL- and range-tolerant; the entry index is bounded by track_count,
 * which the fail-closed parser proved covers exactly first..last. */

uint8_t mos_toc_first_track(const mos_toc *t) { return t ? t->first_track : 0; }
uint8_t mos_toc_last_track(const mos_toc *t)  { return t ? t->last_track  : 0; }

size_t mos_toc_track_count(const mos_toc *t)
{
    return t ? (size_t)t->track_count : 0;
}

bool mos_toc_have_leadout(const mos_toc *t)
{
    return t ? t->have_leadout : false;
}

uint32_t mos_toc_leadout_lba(const mos_toc *t)
{
    return (t && t->have_leadout) ? t->leadout_lba : 0;
}

uint8_t mos_toc_track_number(const mos_toc *t, size_t i)
{
    return (t && i < t->track_count) ? t->tracks[i].track : 0;
}

uint8_t mos_toc_track_adr(const mos_toc *t, size_t i)
{
    return (t && i < t->track_count) ? t->tracks[i].adr : 0;
}

uint8_t mos_toc_track_control(const mos_toc *t, size_t i)
{
    return (t && i < t->track_count) ? t->tracks[i].control : 0;
}

uint32_t mos_toc_track_start_lba(const mos_toc *t, size_t i)
{
    return (t && i < t->track_count) ? t->tracks[i].start_lba : 0;
}

/* ---- mos_drive_caps accessors (mos_query_drive_caps) ----------------- */

bool mos_drive_caps_css(const mos_drive_caps *c)
{
    return c ? c->protection.css : false;
}

uint8_t mos_drive_caps_css_version(const mos_drive_caps *c)
{
    return c ? c->protection.css_version : 0;
}

bool mos_drive_caps_cprm(const mos_drive_caps *c)
{
    return c ? c->protection.cprm : false;
}

uint8_t mos_drive_caps_cprm_version(const mos_drive_caps *c)
{
    return c ? c->protection.cprm_version : 0;
}

bool mos_drive_caps_aacs(const mos_drive_caps *c)
{
    return c ? c->protection.aacs : false;
}

uint8_t mos_drive_caps_aacs_version(const mos_drive_caps *c)
{
    return c ? c->protection.aacs_version : 0;
}

bool mos_drive_caps_bus_encryption(const mos_drive_caps *c)
{
    return c ? c->protection.bus_encryption : false;
}

bool mos_drive_caps_write_bus_encryption(const mos_drive_caps *c)
{
    return c ? c->protection.write_bus_encryption : false;
}

bool mos_drive_caps_securdisc(const mos_drive_caps *c)
{
    return c ? c->protection.securdisc : false;
}

bool mos_drive_caps_vcps(const mos_drive_caps *c)
{
    return c ? c->protection.vcps : false;
}

uint8_t mos_drive_caps_profile_count(const mos_drive_caps *c)
{
    return c ? c->profile_count : 0;
}

uint16_t mos_drive_caps_profile_code(const mos_drive_caps *c, uint8_t i)
{
    return (c && i < c->profile_count) ? c->profiles[i] : 0;
}

const char *mos_drive_caps_firmware_date(const mos_drive_caps *c)
{
    return (c && c->firmware_date[0]) ? c->firmware_date : NULL;
}

uint16_t mos_drive_caps_current_profile(const mos_drive_caps *c)
{
    return c ? c->current_profile : 0;
}

/* ---- mos_drive_inquiry accessors (mos_query_drive_inquiry) ------- */

const char *mos_drive_inquiry_vendor(const mos_drive_inquiry *s)
{
    return (s && s->vendor[0]) ? s->vendor : NULL;
}

const char *mos_drive_inquiry_product(const mos_drive_inquiry *s)
{
    return (s && s->product[0]) ? s->product : NULL;
}

const char *mos_drive_inquiry_revision(const mos_drive_inquiry *s)
{
    return (s && s->revision[0]) ? s->revision : NULL;
}

uint8_t mos_drive_inquiry_spc_version(const mos_drive_inquiry *s)
{
    return s ? s->spc_version : 0;
}

uint8_t mos_drive_inquiry_descriptor_count(const mos_drive_inquiry *s)
{
    return s ? s->descriptor_count : 0;
}

uint16_t mos_drive_inquiry_descriptor_code(const mos_drive_inquiry *s,
                                             uint8_t i)
{
    return (s && i < s->descriptor_count) ? s->descriptors[i] : 0;
}

/* ---- mos_feature_info accessors (mos_enumerate_features) ------------- */

uint16_t mos_feature_info_code(const mos_feature_info_t *f)
{
    return f ? f->code : 0;
}

bool mos_feature_info_current(const mos_feature_info_t *f)
{
    return f ? f->current : false;
}

bool mos_feature_info_persistent(const mos_feature_info_t *f)
{
    return f ? f->persistent : false;
}

uint8_t mos_feature_info_version(const mos_feature_info_t *f)
{
    return f ? f->version : 0;
}

/* ---- mos_disc_id accessors (mos_query_disc_id) ---------------------- *
 * Borrowed strings into the handle-owned result; "" reads as NULL so the
 * emitters suppress empty fields. Disc-controlled bytes — the CLI escapes. */

const char *mos_disc_id_disc_type(const mos_disc_id *d)
{
    return (d && d->disc_type[0]) ? d->disc_type : NULL;
}

const char *mos_disc_id_manufacturer(const mos_disc_id *d)
{
    return (d && d->manufacturer[0]) ? d->manufacturer : NULL;
}

const char *mos_disc_id_media_type(const mos_disc_id *d)
{
    return (d && d->media_type[0]) ? d->media_type : NULL;
}

const char *mos_disc_id_revision(const mos_disc_id *d)
{
    return (d && d->revision[0]) ? d->revision : NULL;
}

/* ---- mos_cdtext accessors (mos_query_cdtext) ----------------------- *
 * Borrowed strings into the handle-owned result; "" reads as NULL so the
 * emitters suppress empty fields. Disc-controlled bytes — the CLI escapes. */

const char *mos_cdtext_title(const mos_cdtext *c)
{
    return (c && c->title[0]) ? c->title : NULL;
}

const char *mos_cdtext_performer(const mos_cdtext *c)
{
    return (c && c->performer[0]) ? c->performer : NULL;
}

uint8_t mos_cdtext_track_count(const mos_cdtext *c)
{
    return c ? c->track_count : 0;
}

const char *mos_cdtext_track_title(const mos_cdtext *c, uint8_t track)
{
    if (!c || track < 1 || track > MOS_CDTEXT_MAX_TRACKS) return NULL;
    const char *t = c->track_titles[track - 1];
    return t[0] ? t : NULL;
}

const char *mos_cdtext_track_performer(const mos_cdtext *c, uint8_t track)
{
    if (!c || track < 1 || track > MOS_CDTEXT_MAX_TRACKS) return NULL;
    const char *p = c->track_performers[track - 1];
    return p[0] ? p : NULL;
}

/* ---- mos_physical_structure accessors (mos_query_physical_structure) - *
 * Plain values, NULL-tolerant. Physical/copyright fields are meaningful
 * only when have_physical/have_copyright — the emitters gate on those. */

bool mos_physical_structure_have_physical(const mos_physical_structure *d)
{
    return d ? d->have_physical : false;
}

uint8_t mos_physical_structure_book_type(const mos_physical_structure *d)
{
    return d ? d->book_type : 0;
}

uint8_t mos_physical_structure_part_version(const mos_physical_structure *d)
{
    return d ? d->part_version : 0;
}

uint8_t mos_physical_structure_disc_size(const mos_physical_structure *d)
{
    return d ? d->disc_size : 0;
}

uint8_t mos_physical_structure_max_rate(const mos_physical_structure *d)
{
    return d ? d->max_rate : 0;
}

uint8_t mos_physical_structure_layer_type(const mos_physical_structure *d)
{
    return d ? d->layer_type : 0;
}

uint8_t mos_physical_structure_track_path(const mos_physical_structure *d)
{
    return d ? d->track_path : 0;
}

uint8_t mos_physical_structure_num_layers(const mos_physical_structure *d)
{
    return d ? d->num_layers : 0;
}

uint8_t mos_physical_structure_linear_density(const mos_physical_structure *d)
{
    return d ? d->linear_density : 0;
}

uint8_t mos_physical_structure_track_density(const mos_physical_structure *d)
{
    return d ? d->track_density : 0;
}

bool mos_physical_structure_bca(const mos_physical_structure *d)
{
    return d ? d->bca : false;
}

uint32_t mos_physical_structure_start_sector(const mos_physical_structure *d)
{
    return d ? d->start_sector : 0;
}

uint32_t mos_physical_structure_end_sector(const mos_physical_structure *d)
{
    return d ? d->end_sector : 0;
}

uint32_t mos_physical_structure_end_sector_l0(const mos_physical_structure *d)
{
    return d ? d->end_sector_l0 : 0;
}

bool mos_physical_structure_have_copyright(const mos_physical_structure *d)
{
    return d ? d->have_copyright : false;
}

uint8_t mos_physical_structure_protection(const mos_physical_structure *d)
{
    return d ? d->protection : 0;
}

uint8_t mos_physical_structure_region(const mos_physical_structure *d)
{
    return d ? d->region : 0;
}

/* ---- mos_track_info accessors (mos_query_track_info) ---------------- *
 * Plain values, NULL-tolerant. next_writable/last_recorded are valid only
 * when nwa_valid/lra_valid — the emitter gates on those. */

uint16_t mos_track_info_track_number(const mos_track_info *t)
{
    return t ? t->track_number : 0;
}

uint16_t mos_track_info_session_number(const mos_track_info *t)
{
    return t ? t->session_number : 0;
}

uint8_t mos_track_info_track_mode(const mos_track_info *t)
{
    return t ? t->track_mode : 0;
}

uint8_t mos_track_info_data_mode(const mos_track_info *t)
{
    return t ? t->data_mode : 0;
}

bool mos_track_info_blank(const mos_track_info *t)
{
    return t ? t->blank : false;
}

bool mos_track_info_damage(const mos_track_info *t)
{
    return t ? t->damage : false;
}

bool mos_track_info_nwa_valid(const mos_track_info *t)
{
    return t ? t->nwa_valid : false;
}

bool mos_track_info_lra_valid(const mos_track_info *t)
{
    return t ? t->lra_valid : false;
}

uint32_t mos_track_info_track_start(const mos_track_info *t)
{
    return t ? t->track_start : 0;
}

uint32_t mos_track_info_next_writable(const mos_track_info *t)
{
    return t ? t->next_writable : 0;
}

uint32_t mos_track_info_free_blocks(const mos_track_info *t)
{
    return t ? t->free_blocks : 0;
}

uint32_t mos_track_info_track_size(const mos_track_info *t)
{
    return t ? t->track_size : 0;
}

uint32_t mos_track_info_last_recorded(const mos_track_info *t)
{
    return t ? t->last_recorded : 0;
}

/* ---- mos_session_layout accessors (mos_query_session_layout) -------- *
 * Plain values, NULL-tolerant. i is bounds-checked against count; an
 * out-of-range index reads as 0/false. first_track/last_track use 0 as the
 * "session carried no POINT 0xA0/0xA1" sentinel (tracks are 1..99); the
 * emitter renders 0 as JSON null. leadout_lba needs have_leadout. */

static const mos_session_entry *mos_internal_session_at(
    const mos_session_layout *s, uint8_t i)
{
    return (s && i < s->count) ? &s->sessions[i] : NULL;
}

uint8_t mos_session_layout_count(const mos_session_layout *s)
{
    return s ? s->count : 0;
}

uint8_t mos_session_layout_session(const mos_session_layout *s, uint8_t i)
{
    const mos_session_entry *e = mos_internal_session_at(s, i);
    return e ? e->session : 0;
}

uint8_t mos_session_layout_first_track(const mos_session_layout *s, uint8_t i)
{
    const mos_session_entry *e = mos_internal_session_at(s, i);
    return (e && e->have_first) ? e->first_track : 0;
}

uint8_t mos_session_layout_last_track(const mos_session_layout *s, uint8_t i)
{
    const mos_session_entry *e = mos_internal_session_at(s, i);
    return (e && e->have_last) ? e->last_track : 0;
}

bool mos_session_layout_have_leadout(const mos_session_layout *s, uint8_t i)
{
    const mos_session_entry *e = mos_internal_session_at(s, i);
    return e ? e->have_leadout : false;
}

uint32_t mos_session_layout_leadout_lba(const mos_session_layout *s, uint8_t i)
{
    const mos_session_entry *e = mos_internal_session_at(s, i);
    return (e && e->have_leadout) ? e->leadout_lba : 0;
}

/* ---- mos_capacity accessors (mos_query_capacity) ------------------- *
 * Plain values, NULL-tolerant. Two independent halves: have_media_size
 * gates the kernel IOMedia size; have_recordable gates the READ TRACK
 * INFORMATION view, within which next_writable needs nwa_valid.
 * media_blocks is derived, never stored. */

bool mos_capacity_have_media_size(const mos_capacity *c)
{
    /* media_bytes 0 is the "no whole-disk node" sentinel (blank/absent). */
    return c ? (c->media_bytes != 0) : false;
}

uint64_t mos_capacity_media_bytes(const mos_capacity *c)
{
    return c ? c->media_bytes : 0;
}

uint32_t mos_capacity_block_bytes(const mos_capacity *c)
{
    return c ? c->block_bytes : 0;
}

uint64_t mos_capacity_media_blocks(const mos_capacity *c)
{
    /* bytes / block size; 0 when either is absent (no divide-by-zero). */
    if (!c || c->media_bytes == 0 || c->block_bytes == 0) return 0;
    return c->media_bytes / c->block_bytes;
}

bool mos_capacity_have_recordable(const mos_capacity *c)
{
    return c ? c->have_recordable : false;
}

bool mos_capacity_nwa_valid(const mos_capacity *c)
{
    return c ? c->nwa_valid : false;
}

uint32_t mos_capacity_free_blocks(const mos_capacity *c)
{
    return c ? c->free_blocks : 0;
}

uint32_t mos_capacity_next_writable(const mos_capacity *c)
{
    return c ? c->next_writable : 0;
}

uint32_t mos_capacity_track_size(const mos_capacity *c)
{
    return c ? c->track_size : 0;
}

/* Formattable view (READ FORMAT CAPACITIES). All gated by have_formattable;
   the indexed accessors clamp to the stored count and read 0 out of range. */

bool mos_capacity_have_formattable(const mos_capacity *c)
{
    return c ? c->have_formattable : false;
}

uint8_t mos_capacity_format_type(const mos_capacity *c)
{
    return c ? c->formattable.cur_type : 0;
}

uint32_t mos_capacity_formattable_blocks(const mos_capacity *c)
{
    return c ? c->formattable.cur_blocks : 0;
}

uint32_t mos_capacity_formattable_block_bytes(const mos_capacity *c)
{
    return c ? c->formattable.cur_block_bytes : 0;
}

uint8_t mos_capacity_formattable_descriptor_count(const mos_capacity *c)
{
    return c ? c->formattable.count : 0;
}

uint32_t mos_capacity_formattable_descriptor_blocks(const mos_capacity *c,
                                                    uint8_t i)
{
    if (!c || i >= c->formattable.count) return 0;
    return c->formattable.d[i].blocks;
}

uint8_t mos_capacity_formattable_descriptor_type(const mos_capacity *c,
                                                 uint8_t i)
{
    if (!c || i >= c->formattable.count) return 0;
    return c->formattable.d[i].format_type;
}

uint32_t mos_capacity_formattable_descriptor_param(const mos_capacity *c,
                                                   uint8_t i)
{
    if (!c || i >= c->formattable.count) return 0;
    return c->formattable.d[i].param;
}

/* ---- mos_drive_perf accessors (mos_query_drive_perf) ---------------- *
 * Plain values, NULL-tolerant. Speeds meaningful only when have. */

bool mos_drive_perf_have(const mos_drive_perf *p)
{
    return p ? p->have : false;
}

uint16_t mos_drive_perf_descriptor_count(const mos_drive_perf *p)
{
    return p ? p->descriptor_count : 0;
}

uint32_t mos_drive_perf_max_read_kbps(const mos_drive_perf *p)
{
    return p ? p->max_read_kbps : 0;
}

uint32_t mos_drive_perf_max_write_kbps(const mos_drive_perf *p)
{
    return p ? p->max_write_kbps : 0;
}

/* ---- mos_mode_caps accessors (mos_query_mode_caps) ----------------- */

uint8_t mos_mode_caps_loading_mechanism(const mos_mode_caps *m)
{
    return m ? m->loading_mechanism : 0;
}

bool mos_mode_caps_can_eject(const mos_mode_caps *m)
{
    return m ? m->can_eject : false;
}

bool mos_mode_caps_lock_supported(const mos_mode_caps *m)
{
    return m ? m->lock_supported : false;
}

bool mos_mode_caps_locked(const mos_mode_caps *m)
{
    return m ? m->locked : false;
}

uint16_t mos_mode_caps_buffer_kb(const mos_mode_caps *m)
{
    return m ? m->buffer_kb : 0;
}

/* ---- mos_error_recovery accessors (mos_query_error_recovery) -------- */

bool mos_error_recovery_awre(const mos_error_recovery *e)
{
    return e ? e->awre : false;
}

bool mos_error_recovery_arre(const mos_error_recovery *e)
{
    return e ? e->arre : false;
}

bool mos_error_recovery_per(const mos_error_recovery *e)
{
    return e ? e->per : false;
}

bool mos_error_recovery_dcr(const mos_error_recovery *e)
{
    return e ? e->dcr : false;
}

uint8_t mos_error_recovery_read_retry_count(const mos_error_recovery *e)
{
    return e ? e->read_retry_count : 0;
}

/* ==== src/mos_scsi.c ==== */
/*
 * mos_scsi.c — IOKit lifecycle, enumeration, the MMC convenience
 * primitives the state core uses (TUR / current-profile / tray-state),
 * and the one raw-CDB path (mos_internal_raw_cdb — the sole ObtainExclusiveAccess
 * site). The typed mos_query_* verb surface lives in mos_query.c.
 *
 * All internal functions must be `static` or `mos_internal_`-prefixed;
 * this library is statically linked into downstream projects (HandBrake
 * etc.) and cannot collide on generic names.
 */


#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOBSD.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICommandOperationCodes.h>
#include <IOKit/scsi/SCSICmds_REQUEST_SENSE_Defs.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <IOKit/storage/IOMedia.h>   /* kIOMediaSizeKey / kIOMediaPreferredBlockSizeKey */
#include <IOKit/storage/IOCDMedia.h> /* kIOCDMediaTOCKey (CD-only cached full-TOC) */

#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- Wire-format compile-time invariants -------------------------- */
/* Pin the 18-byte sense assumption two ways: (1) Apple's struct matches
   their own kSenseDefaultSize; (2) that constant is 18, the literal baked
   into the sense[18] signatures in mos.h / mos_internal.h. A (1) failure
   means Apple broke an internal contract; (2) means the public API literal
   needs updating. */
_Static_assert(sizeof(SCSI_Sense_Data) == kSenseDefaultSize,
               "SCSI_Sense_Data size no longer matches kSenseDefaultSize");
_Static_assert(kSenseDefaultSize == 18,
               "kSenseDefaultSize changed — update sense[18] in mos.h and mos_internal.h");

/* ---- Registry helpers --------------------------------------------- */

/* The pure BSD-name helpers live in src/mos_pure.c (declared in mos_pure.h,
   re-included via mos_internal.h) so they link into the pure-only tests. */

/* Read an OSNumber registry property as uint64, or 0 (the "absent"
   sentinel) when missing, non-numeric, or non-positive. Used for the
   whole-disk IOMedia size/block-size — the kernel's cached READ CAPACITY,
   no command. */
static uint64_t mos_internal_cf_number_u64(io_registry_entry_t node,
                                           CFStringRef key)
{
    CFTypeRef v MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
            node, key, kCFAllocatorDefault, 0);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return 0;
    long long out = 0;
    if (!CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &out) ||
        out <= 0) {
        return 0;
    }
    return (uint64_t)out;
}

/* Resolve the whole-disk BSD unit (N in "diskN") for an IOKit service, or
   -1 if none (empty/open-tray drive, no IOMedia child). Captures the
   whole-disk IOMedia registry entry ID into *media_id_out (0 if none), and,
   when non-NULL, the node's kernel-cached size/block-size into
   *media_bytes_out / *block_bytes_out (kIOMediaSizeKey /
   kIOMediaPreferredBlockSizeKey — the kernel's attach-time READ CAPACITY;
   0 if absent). The transient "diskN" name buffers never escape — only the
   parsed integer identity does. */
static int64_t mos_internal_bsd_unit(io_service_t svc, uint64_t *media_id_out,
                                     uint64_t *media_bytes_out,
                                     uint32_t *block_bytes_out)
{
    if (media_id_out)    *media_id_out = 0;
    if (media_bytes_out) *media_bytes_out = 0;
    if (block_bytes_out) *block_bytes_out = 0;
    io_iterator_t it MOS_IO_AUTO = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(svc, kIOServicePlane,
            kIORegistryIterateRecursively, &it) != KERN_SUCCESS) {
        return -1;
    }

    /* Two-pass selection:
       1. Prefer a whole-disk IOMedia child — the "Whole" property cleanly
          separates "disk4" from partitions ("disk4s1").
       2. Fallback: some USB bridges expose a BSD name on a node with no
          "Whole" property. Accept any whole-disk-shaped name (disk\d+ /
          rdisk\d+, no partition suffix).
       A Whole node is authoritative and wins. Refcounts auto-release via
       MOS_IO_AUTO / MOS_CF_AUTO. */
    char whole_name[MOS_BSD_NAME_CAP] = {0};
    char fallback_name[MOS_BSD_NAME_CAP] = {0};
    uint64_t whole_id = 0;
    uint64_t whole_bytes = 0;   /* kIOMediaSizeKey off the Whole node */
    uint32_t whole_block = 0;   /* kIOMediaPreferredBlockSizeKey */

    for (;;) {
        io_object_t child MOS_IO_AUTO = IOIteratorNext(it);
        if (child == IO_OBJECT_NULL) break;

        char this_name[MOS_BSD_NAME_CAP] = {0};

        CFTypeRef bsd MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                child, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
        if (bsd && CFGetTypeID(bsd) == CFStringGetTypeID()) {
            /* CFStringGetCString may write partial bytes before failing
               (Apple spec); clear so the empty check below stays valid. */
            if (!CFStringGetCString((CFStringRef)bsd, this_name,
                                    sizeof(this_name),
                                    kCFStringEncodingUTF8)) {
                this_name[0] = 0;
            }
        }

        if (this_name[0] == 0) continue;

        /* Pass 1: the authoritative Whole node. Require IOMedia conformance
           — some descendants carry a "Whole" boolean without being media.
           (String-name IOObjectConformsTo avoids including IOMedia.h.) The
           fallback still takes whole-disk-shaped names on non-IOMedia
           bridge nodes. */
        CFTypeRef whole MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                child, CFSTR("Whole"), kCFAllocatorDefault, 0);
        bool is_whole = (whole &&
                         CFGetTypeID(whole) == CFBooleanGetTypeID() &&
                         CFBooleanGetValue((CFBooleanRef)whole) &&
                         IOObjectConformsTo(child, "IOMedia"));

        if (is_whole && whole_name[0] == 0) {
            strlcpy(whole_name, this_name, sizeof(whole_name));
            /* Capture the IOMedia registry entry ID — the media-swap
               fingerprint — while we hold the node. On failure it stays 0
               (the "unavailable" sentinel; the watch core won't infer a
               swap from it). */
            if (IORegistryEntryGetRegistryEntryID(child, &whole_id) != KERN_SUCCESS) {
                whole_id = 0;
            }
            /* Kernel-cached capacity off the same node, captured now rather
               than re-resolved by media_id later. */
            whole_bytes = mos_internal_cf_number_u64(child,
                              CFSTR(kIOMediaSizeKey));
            /* Block size is reported as u64 but stored u32. Optical block
               sizes are tiny (512/2048/4096); a value that does not fit u32 is
               a malformed/hostile registry property, so fail closed to 0
               (absent) rather than silently truncate the high bits. */
            uint64_t blk = mos_internal_cf_number_u64(child,
                              CFSTR(kIOMediaPreferredBlockSizeKey));
            whole_block = (blk <= UINT32_MAX) ? (uint32_t)blk : 0u;
        } else if (!is_whole && fallback_name[0] == 0 &&
                   mos_internal_bsd_name_is_whole_shape(this_name)) {
            strlcpy(fallback_name, this_name, sizeof(fallback_name));
            /* No id capture here. media_id must be the whole-disk IOMedia
               entry ID (mos_pure.h), re-minted on a swap; a bridge node's
               ID identifies the BRIDGE and may survive a swap, so a stale
               non-zero value would disarm both swap detectors at once
               (mos_watch_core.c). Leaving it 0 keeps the no-id fallback
               armed. Whether real bridges hit this branch is a rig
               question. */
        }

        if (whole_name[0] != 0) break; /* Whole found, done */
    }

    const char *chosen = whole_name[0] ? whole_name
                       : fallback_name[0] ? fallback_name
                       : NULL;
    if (!chosen) return -1;
    if (media_id_out) *media_id_out = whole_name[0] ? whole_id : 0;
    /* Size and id ride the Whole node only — the fallback bridge node's are
       not the medium's (see the no-id reasoning above). */
    if (media_bytes_out) *media_bytes_out = whole_name[0] ? whole_bytes : 0;
    if (block_bytes_out) *block_bytes_out = whole_name[0] ? whole_block : 0;
    /* parse_bsd_unit normalizes any rdisk/ /dev/ prefix, rejects
       partition/non-whole shapes, and returns the unit or -1. Identity is
       an integer from here; "diskN" is reconstructed only at output. */
    return mos_internal_parse_bsd_unit(chosen);
}

/* Re-resolve the handle's media-scoped identity (bsd_unit, media_id swap
   fingerprint, cached size/block bytes) off its stable drive service.
   h->svc is fixed for the handle's life; the IOMedia child under it is not
   — it appears on insert, vanishes on eject — so a single open-time capture
   goes stale across a media change. Media-scoped queries call this first to
   report CURRENT media: a local IORegistry walk, no SCSI command, no
   exclusive access. Single-threaded like every handle mutation. */
void mos_internal_refresh_media_identity(mos_handle_t *h)
{
    if (!h) return;
    h->bsd_unit = mos_internal_bsd_unit(h->svc, &h->media_id,
                                        &h->media_bytes,
                                        &h->media_block_bytes);  /* -1 if no media */
    /* Media-type token off the IO{CD,DVD,BD}Media node — zero-MMC, present even
       when not READY (so the profile, and thus media_class, is suppressed).
       NULL when no media node carries a Type, or the string is unknown
       (fail-closed map). */
    char type[16] = {0};
    h->media_type = (mos_internal_read_media_type(h->svc, type, sizeof type) > 0)
                        ? mos_internal_media_type_token(type)
                        : NULL;
    /* Kernel IOMedia Writable flag off the same node — tri-state -1/0/1. A
       mechanism fact (the kernel's bit), not a blank/appendable assertion. */
    h->writable = (signed char)mos_internal_read_writable(h->svc);
}

/* Copy the kernel-cached full-TOC (kIOCDMediaTOCKey, a CDTOC CFData blob) off
   the drive's IOCDMedia node. CD-only: the property exists only on IOCDMedia
   (no DVD/BD equivalent — those expose only a media-type string). A pure
   IORegistry read, no SCSI command and no exclusive access, like the cached
   kIOMediaSize capacity — so it works on mounted media. Returns the bytes
   copied (clamped to cap), 0 when absent. */
size_t mos_internal_read_cdtoc(io_service_t svc, uint8_t *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    io_iterator_t it MOS_IO_AUTO = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(svc, kIOServicePlane,
            kIORegistryIterateRecursively, &it) != KERN_SUCCESS) {
        return 0;
    }
    for (;;) {
        io_object_t child MOS_IO_AUTO = IOIteratorNext(it);
        if (child == IO_OBJECT_NULL) break;
        /* IOCDMedia is the CD whole-media node carrying the TOC; the string
           conformance check avoids a hard IOCDMedia.h class dependency in the
           walk and naturally excludes DVD/BD media and partitions. */
        if (!IOObjectConformsTo(child, "IOCDMedia")) continue;

        CFTypeRef v MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                child, CFSTR(kIOCDMediaTOCKey), kCFAllocatorDefault, 0);
        if (!v || CFGetTypeID(v) != CFDataGetTypeID()) continue;

        CFIndex n = CFDataGetLength((CFDataRef)v);
        if (n <= 0) continue;
        size_t copy = ((size_t)n < cap) ? (size_t)n : cap;
        CFDataGetBytes((CFDataRef)v, CFRangeMake(0, (CFIndex)copy), buf);
        return copy;   /* MOS_CF_AUTO / MOS_IO_AUTO release on this return */
    }
    return 0;
}

/* Copy the kernel optical-media "Type" string off the drive's IO{CD,DVD,BD}Media
   node (kIO{CD,DVD,BD}MediaTypeKey are all the literal "Type"). Media-class-
   agnostic sibling of read_cdtoc: same recursive walk, but matches any of the
   three optical media classes and reads a CFString. Pure IORegistry read, no
   SCSI command, no exclusive access — present even when the drive is not READY.
   Returns the NUL-terminated length copied, 0 when absent. */
size_t mos_internal_read_media_type(io_service_t svc, char *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    io_iterator_t it MOS_IO_AUTO = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(svc, kIOServicePlane,
            kIORegistryIterateRecursively, &it) != KERN_SUCCESS) {
        return 0;
    }
    for (;;) {
        io_object_t child MOS_IO_AUTO = IOIteratorNext(it);
        if (child == IO_OBJECT_NULL) break;
        /* String conformance (like read_cdtoc) avoids a hard class-header
           dependency; "Type" is the same key string on all three classes. */
        if (!IOObjectConformsTo(child, "IOCDMedia")  &&
            !IOObjectConformsTo(child, "IODVDMedia") &&
            !IOObjectConformsTo(child, "IOBDMedia")) continue;

        CFTypeRef v MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                child, CFSTR("Type"), kCFAllocatorDefault, 0);
        if (!v || CFGetTypeID(v) != CFStringGetTypeID()) continue;

        /* CFStringGetCString NUL-terminates and returns false on overflow; the
           buffer is sized for the longest token ("HD DVD-ROM" = 10 + NUL). */
        if (!CFStringGetCString((CFStringRef)v, buf, (CFIndex)cap,
                                kCFStringEncodingUTF8)) {
            buf[0] = '\0';
            continue;
        }
        return strlen(buf);   /* MOS_CF_AUTO / MOS_IO_AUTO release on return */
    }
    return 0;
}

/* Read the kernel IOMedia Writable flag off the drive's optical media node. Same
   walk as read_media_type (so it tracks the same whole-disk optical node, not a
   partition), but reads kIOMediaWritableKey as a CFBoolean. -1 when no optical
   media node carries the key (no media / not published yet), else 0/1. */
int mos_internal_read_writable(io_service_t svc)
{
    io_iterator_t it MOS_IO_AUTO = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(svc, kIOServicePlane,
            kIORegistryIterateRecursively, &it) != KERN_SUCCESS) {
        return -1;
    }
    for (;;) {
        io_object_t child MOS_IO_AUTO = IOIteratorNext(it);
        if (child == IO_OBJECT_NULL) break;
        if (!IOObjectConformsTo(child, "IOCDMedia")  &&
            !IOObjectConformsTo(child, "IODVDMedia") &&
            !IOObjectConformsTo(child, "IOBDMedia")) continue;

        CFTypeRef v MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                child, CFSTR(kIOMediaWritableKey), kCFAllocatorDefault, 0);
        if (!v || CFGetTypeID(v) != CFBooleanGetTypeID()) continue;

        return CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
    }
    return -1;
}

/* ---- Enumeration ---------------------------------------------------- */

#define MOS_ENUM_CAP 64

void mos_enumerate_devices(mos_enumerate_cb cb, void *ctx)
{
    if (!cb) return;

    /* DR-backed (mos_dr.c): the snapshot arrives in DR device-array order,
       the public ordering contract (same array drutil enumerates), already
       deduped to one DRDevice per drive. Identity strings ride the snapshot
       but mos_device_info_t doesn't surface them — identity is handle data,
       not enumeration data. */
    mos_internal_dr_snapshot snap[MOS_ENUM_CAP];
    size_t n = mos_internal_dr_copy_snapshot(snap, MOS_ENUM_CAP);

    for (size_t i = 0; i < n; ++i) {
        struct mos_device_info info = {
            .bsd_unit    = snap[i].bsd_unit,
            .registry_id = snap[i].registry_id,
        };
        if (!cb(&info, ctx)) break;
    }
}

/* Enumeration yields bsd_unit + registry_id only; -1 for an empty drive. */
int64_t mos_device_info_bsd_unit(const mos_device_info_t *i) { return i ? i->bsd_unit : -1; }
uint64_t mos_device_info_registry_id(const mos_device_info_t *i) { return i ? i->registry_id : 0; }

/* The bsd_unit a handle was opened against, for partial-failure paths where
   open succeeded but a later call errored. -1 for an empty drive. */
int64_t mos_handle_bsd_unit(const mos_handle_t *h)
{
    return h ? h->bsd_unit : -1;
}

io_service_t mos_internal_handle_get_service(mos_handle_t *h) {
    /* No retain taken — caller must IOObjectRetain before mos_close(h)
       (contract in mos_internal.h). */
    return h ? h->svc : IO_OBJECT_NULL;
}

/* ---- Open / close --------------------------------------------------- */

static mos_handle_t *mos_internal_open_service(io_service_t svc, mos_error *err)
{
    mos_handle_t *h = calloc(1, sizeof(*h));
    if (!h) { IOObjectRelease(svc); if (err) *err = MOS_ERR_OOM; return NULL; }
    h->svc = svc;
    /* Attachment identity, captured once. Best-effort: a failed call leaves
       the calloc'd 0, documented as "unavailable", never fabricated. */
    if (IORegistryEntryGetRegistryEntryID(svc, &h->drive_registry_id)
            != KERN_SUCCESS) {
        h->drive_registry_id = 0;
    }
    /* Best-effort, not a gate: a nameless empty drive opens fine (plug-in
       and queries run off svc). Media-scoped queries re-run this per call
       so a handle held across insert/eject stays current. */
    mos_internal_refresh_media_identity(h);   /* sets bsd_unit/-1, media_id, size */

    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        svc,
        kIOMMCDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID,
        &h->plugin, &score);
    if (kr != KERN_SUCCESS || !h->plugin) {
        /* Apple's kext declined to attach SCSITaskUserClient (match
           behavior has shifted across macOS releases — see KNOWN UNKNOWNS). */
        if (err) *err = MOS_ERR_DRIVER_REJECTED;
        mos_close(h);
        return NULL;
    }

    HRESULT hr = (*h->plugin)->QueryInterface(
        h->plugin,
        CFUUIDGetUUIDBytes(kIOMMCDeviceInterfaceID),
        (LPVOID *)&h->mmc);
    if (hr != S_OK || !h->mmc) {
        /* COM leaves the out pointer untouched on failure (calloc-NULL
           here); NULL it anyway so mos_close never Releases garbage if a
           plug-in violates that. */
        h->mmc = NULL;
        if (err) *err = MOS_ERR_DRIVER_REJECTED;
        mos_close(h);
        return NULL;
    }

    /* Identity from the DR directory (device-static). Non-fatal on
       failure — empty strings. */
    (void)mos_internal_dr_copy_identity_for_service(
        h->svc,
        h->vendor_str,   sizeof h->vendor_str,
        h->product_str,  sizeof h->product_str,
        h->revision_str, sizeof h->revision_str);

    if (err) *err = MOS_OK;
    return h;
}

mos_handle_t *mos_open_by_bsd_name(const char *want, mos_error *err_out)
{
    if (err_out) *err_out = MOS_ERR_INVALID_ARG;
    if (!want) return NULL;

    /* parse_bsd_unit accepts disk4 / rdisk4 / /dev/ forms, -1 otherwise.
       Empty drives have no unit, so this never resolves a nameless drive —
       they're index/enumeration-only. Preserves the CLI contract: malformed
       name = invalid_arg/64, well-formed-but-absent = no_device/66. */
    int64_t want_unit = mos_internal_parse_bsd_unit(want);
    if (want_unit < 0) return NULL;

    /* Reconstruct canonical "diskN" before asking DR — it documents the
       plain /dev entry name, and normalization stays with parse_bsd_unit,
       not the caller's prefix. The resulting registry ID reopens atomically
       below (same TOCTOU posture as open-by-index): a name DR resolved but
       whose entry then terminated returns NO_DEVICE, never another drive. */
    char disk_name[MOS_BSD_NAME_CAP];
    if (!mos_bsd_name_format(want_unit, disk_name, sizeof disk_name)) {
        return NULL;
    }
    uint64_t id = mos_internal_dr_registry_id_for_bsd_name(disk_name);
    if (id == 0) {
        if (err_out) *err_out = MOS_ERR_NO_DEVICE;
        return NULL;
    }
    return mos_internal_open_by_registry_id(id, err_out);
}

mos_handle_t *mos_open_by_registry_id(uint64_t registry_id,
                                      mos_error *err_out)
{
    if (registry_id == 0) {
        if (err_out) *err_out = MOS_ERR_INVALID_ARG;
        return NULL;
    }
    return mos_internal_open_by_registry_id(registry_id, err_out);
}

/* Collects registry IDs (not BSD names) for by-index reopen: names can
   shift on hot-plug between enumerate and reopen (TOCTOU), while a registry
   entry ID is stable and reopens atomically via IORegistryEntryIDMatching. */
typedef struct {
    uint64_t ids[MOS_ENUM_CAP];
    size_t   count;
} mos_internal_id_collect;

static bool mos_internal_collect_cb(const mos_device_info_t *info, void *ctx)
{
    mos_internal_id_collect *c = (mos_internal_id_collect *)ctx;
    if (c->count >= MOS_ENUM_CAP) return false;
    uint64_t id = mos_device_info_registry_id(info);
    if (id != 0) {
        c->ids[c->count++] = id;
    }
    return true;
}

/* Reopen by registry entry ID — race-free (kernel resolves the match
   atomically). Used by mos_open_by_index; non-static for the watch
   adapter's per-probe reopen. Contract in mos_internal.h. */
mos_handle_t *mos_internal_open_by_registry_id(uint64_t id,
                                               mos_error *err_out)
{
    if (err_out) *err_out = MOS_ERR_INVALID_ARG;
    if (id == 0) return NULL;

    CFMutableDictionaryRef m = IORegistryEntryIDMatching(id);
    if (!m) {
        if (err_out) *err_out = MOS_ERR_IO;
        return NULL;
    }

    /* IOServiceGetMatchingService consumes the dictionary reference
       (IOKit ownership rule) whether or not it matches — no CFRelease here. */
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (svc == IO_OBJECT_NULL) {
        /* Enumerated earlier but gone now — hot-unplugged between
           enumeration and open. NO_DEVICE distinguishes this from a bad
           index. */
        if (err_out) *err_out = MOS_ERR_NO_DEVICE;
        return NULL;
    }

    return mos_internal_open_service(svc, err_out);
}

mos_handle_t *mos_open_by_index(int one_based, mos_error *err_out)
{
    if (err_out) *err_out = MOS_ERR_INVALID_ARG;
    if (one_based < 1) return NULL;

    /* Two-stage but race-free: enumerate to collect registry IDs in DR
       order (the index contract), then reopen the captured ID atomically.
       A hot-plug between passes either succeeds or returns NO_DEVICE, never
       opens a different drive that inherited the BSD name. */

    mos_internal_id_collect c = { {0}, 0 };
    mos_enumerate_devices(mos_internal_collect_cb, &c);
    if ((size_t)one_based > c.count) {
        if (err_out) *err_out = MOS_ERR_NO_DEVICE;
        return NULL;
    }
    return mos_internal_open_by_registry_id(c.ids[one_based - 1], err_out);
}

mos_handle_t *mos_open_device(const mos_device_info_t *info,
                              mos_error *err_out)
{
    if (err_out) *err_out = MOS_ERR_INVALID_ARG;
    if (!info || info->registry_id == 0) return NULL;

    /* The registry ID reopens atomically, so opening from inside the
       enumeration callback has no TOCTOU window and no re-enumeration —
       the one-snapshot path CLI list/status use. */
    return mos_internal_open_by_registry_id(info->registry_id, err_out);
}

void mos_close(mos_handle_t *h)
{
    if (!h) return;
    if (h->std) {
        if (h->have_exclusive) (*h->std)->ReleaseExclusiveAccess(h->std);
        (*h->std)->Release(h->std);
    }
    if (h->mmc)    (*h->mmc)->Release(h->mmc);
    if (h->plugin) IODestroyPlugInInterface(h->plugin);
    if (h->svc)    IOObjectRelease(h->svc);
    free(h);
}

/* ---- MMC convenience wrappers ------------------------------------- *
 *
 * No exclusive access required (the point of MMC vs raw
 * SCSITaskDeviceInterface). Can still return kIOReturnExclusiveAccess when
 * another client holds the drive — that's a caller-sees-BUSY.
 *
 * Signatures verified against the macOS 26.5 SDK SCSITaskLib.h.
 */

/* Pin the IOKit constants to the literals the pure mapping expects, so an
   SDK renumbering breaks the build loudly instead of miscategorizing
   IOReturn codes. */
_Static_assert((uint32_t)kIOReturnSuccess         == 0x00000000u,
               "kIOReturnSuccess mapping in mos_pure.c is out of date");
_Static_assert((uint32_t)kIOReturnNoMemory        == 0xE00002BDu,
               "kIOReturnNoMemory mapping in mos_pure.c is out of date");
_Static_assert((uint32_t)kIOReturnNoResources     == 0xE00002BEu,
               "kIOReturnNoResources mapping in mos_pure.c is out of date");
_Static_assert((uint32_t)kIOReturnNoDevice        == 0xE00002C0u,
               "kIOReturnNoDevice mapping in mos_pure.c is out of date — "
               "the watch core's terminal-removal path depends on this");
_Static_assert((uint32_t)kIOReturnBadArgument     == 0xE00002C2u,
               "kIOReturnBadArgument mapping in mos_pure.c is out of date");
_Static_assert((uint32_t)kIOReturnExclusiveAccess == 0xE00002C5u,
               "kIOReturnExclusiveAccess mapping in mos_pure.c is out of date");
_Static_assert((uint32_t)kIOReturnUnsupported     == 0xE00002C7u,
               "kIOReturnUnsupported mapping in mos_pure.c is out of date");
_Static_assert((uint32_t)kIOReturnBusy            == 0xE00002D5u,
               "kIOReturnBusy mapping in mos_pure.c is out of date");
_Static_assert((uint32_t)kIOReturnTimeout         == 0xE00002D6u,
               "kIOReturnTimeout mapping in mos_pure.c is out of date");
_Static_assert((uint32_t)kIOReturnNotAttached     == 0xE00002D9u,
               "kIOReturnNotAttached mapping in mos_pure.c is out of date");

/* mos_internal_raw_cdb passes mos_xfer_dir straight to SetScatterGatherEntries,
   so the values MUST equal the SDK's kSCSIDataTransfer_* enumerators —
   a renumbering would send CDBs in the wrong direction. Pin them. */
_Static_assert((int)MOS_XFER_NONE        == (int)kSCSIDataTransfer_NoDataTransfer,
               "MOS_XFER_NONE no longer matches kSCSIDataTransfer_NoDataTransfer "
               "— mos_internal_raw_cdb passes mos_xfer_dir directly to "
               "SetScatterGatherEntries; the values MUST be identical.");
_Static_assert((int)MOS_XFER_TO_TARGET   == (int)kSCSIDataTransfer_FromInitiatorToTarget,
               "MOS_XFER_TO_TARGET no longer matches "
               "kSCSIDataTransfer_FromInitiatorToTarget — see MOS_XFER_NONE "
               "assertion above for the rationale.");
_Static_assert((int)MOS_XFER_FROM_TARGET == (int)kSCSIDataTransfer_FromTargetToInitiator,
               "MOS_XFER_FROM_TARGET no longer matches "
               "kSCSIDataTransfer_FromTargetToInitiator — see MOS_XFER_NONE "
               "assertion above for the rationale.");

/* The pure tray classifier (mos_pure.c) compares mos_internal_raw_cdb's task
   status against MOS_SCSI_STATUS_GOOD but can't include the SDK to check they
   agree — pin it here, the one TU that sees both names. */
_Static_assert((int)MOS_SCSI_STATUS_GOOD == (int)kSCSITaskStatus_GOOD,
               "MOS_SCSI_STATUS_GOOD no longer matches kSCSITaskStatus_GOOD "
               "— the pure tray classifier would misclassify GOOD status.");

/* Thin shim over the pure mapping in mos_pure.c (int32_t in, fixture-tested
   without IOKit). Maps transport-layer failures only — CHECK CONDITION
   rides taskStatus/sense, not IOReturn. */
mos_error mos_internal_ioreturn_to_mos_error(IOReturn rc)
{
    return mos_internal_ioreturn_to_error((int32_t)rc);
}

mos_error mos_internal_mmc_get_tray_state(mos_handle_t *h, bool *tray_open)
{
    if (!h || !tray_open) return MOS_ERR_INVALID_ARG;

    /* Raw GET EVENT STATUS NOTIFICATION (0x4A), Media class, Polled. We do
       NOT use the GetTrayState convenience method: on a GESN failure it
       hard-codes (closed, success) (ARCHITECTURE.md §4.2, §9.7), masking the
       failure. Issuing the CDB ourselves keeps a failure a failure so the
       state core can fork on the TUR sense.

       Reached only on the not-ready path (TUR already proved reachable and
       not mounted), so exclusive access is free of mount conflict.
       mos_internal_raw_cdb acquires and releases the lock per call —
       single-shot, never held.

       CDB and response byte map: ARCHITECTURE.md §4.2. */
    const uint8_t cdb[10] = {
        0x4A,        /* GET EVENT STATUS NOTIFICATION              */
        0x01,        /* byte1 bit0 IMMED = 1 → Polled mode         */
        0x00, 0x00,
        0x10,        /* byte4 Notification Class Request = Media   */
        0x00, 0x00,
        0x00, 0x08,  /* bytes7-8 Allocation Length = 8 (BE)        */
        0x00,        /* control                                    */
    };

    uint8_t  resp[8]     = {0};
    uint32_t task_status = 0;
    uint8_t  sense[18]   = {0};

    mos_error e = mos_internal_raw_cdb(h, cdb, sizeof cdb,
                              resp, sizeof resp,
                              MOS_XFER_FROM_TARGET,
                              2000,                 /* ms, ARCHITECTURE.md §4.2 */
                              &task_status, sense, NULL);
    if (e != MOS_OK) return e;                      /* transport/lock: honest fail */
    if (task_status != kSCSITaskStatus_GOOD)        /* CHECK CONDITION etc.        */
        return MOS_ERR_IO;

    /* Validity gates (NEA, Media class, full span) live in the pure decoder
       so they're fuzz/ASan-checked headless. The buffer span is the bound;
       the decoder trusts the reply's own Event Data Length, not the
       transport's realized count (some USB bridges under-report it). A false
       return means "no authoritative bit" — fork on the TUR sense. */
    if (!mos_internal_gesn_media_door_open(resp, sizeof resp, tray_open))
        return MOS_ERR_IO;

    return MOS_OK;
}

mos_error mos_internal_mmc_test_unit_ready(mos_handle_t *h,
                                           uint32_t *status,
                                           uint8_t sense[18])
{
    if (!h || !h->mmc || !status || !sense) return MOS_ERR_INVALID_ARG;

    /* A non-IOKit failure surfaces as outTaskStatus == CHECK CONDITION with
       valid sense; IOReturn covers only transport errors. */
    SCSITaskStatus  task_status  = 0;
    SCSI_Sense_Data sense_struct = {0};

    IOReturn rc = (*h->mmc)->TestUnitReady(h->mmc, &task_status, &sense_struct);
    if (rc != kIOReturnSuccess) {
        /* Transport failure: zero outputs, return the mapped error.
           *status is meaningful only on MOS_OK. */
        *status = 0;
        memset(sense, 0, 18);
        return mos_internal_ioreturn_to_mos_error(rc);
    }

    *status = (uint32_t)task_status;
    memcpy(sense, &sense_struct, 18);
    return MOS_OK;
}

/* RETURN-CONVENTION: returns MOS_ERR_IO for reached-but-unusable replies
   (non-GOOD status, truncated GOOD) rather than clearing the out-param.
   Load-bearing: 0x0000 is a REAL answer ("no current profile"), so a
   malformed reply must be distinguishable out-of-band or it masquerades as
   no-media. (Identity strings have no such collision, so that seam clears
   in-band.) The caller treats both shapes as non-fatal enrichment skips. */
mos_error mos_internal_mmc_get_current_profile(mos_handle_t *h, uint16_t *profile)
{
    if (!h || !h->mmc || !profile) return MOS_ERR_INVALID_ARG;

    /* RT=2 ("only this feature") with starting feature 0x0000 returns just
       the 8-byte feature header. */
    uint8_t          buf[16] = {0};
    SCSITaskStatus   st      = 0;
    SCSI_Sense_Data  sd      = {0};

    IOReturn rc = (*h->mmc)->GetConfiguration(
        h->mmc,
        (UInt8)0x02,             /* RT = 10b, only the feature we asked for */
        (UInt16)0x0000,          /* starting feature: Profile List           */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        *profile = 0x0000;
        /* Distinguish transport failure (map the IOReturn) from non-GOOD
           status (reached the drive, no usable data) — report I/O failure,
           not a misleading "no profile". */
        return (rc != kIOReturnSuccess) ? mos_internal_ioreturn_to_mos_error(rc)
                                        : MOS_ERR_IO;
    }

    /* Gate extraction on the reply's Feature Header Data Length (pure
       decoder, fuzz-checked): a GOOD-but-short transfer must surface as "no
       profile", not a silent 0x0000 that reads like no-media. Buffer is the
       bound; the reply's own Data Length is the validity check (convenience
       GetConfiguration reports no realized count). */
    uint16_t parsed = 0x0000;
    if (!mos_internal_config_current_profile(buf, sizeof(buf), &parsed)) {
        *profile = 0x0000;
        return MOS_ERR_IO;          /* truncated/short reply — enrichment skips it */
    }
    *profile = parsed;
    return MOS_OK;
}

/* Open-time directory identity for the drive verb: zero commands, borrowed
   strings pointing into the handle's buffers (same as the state result). */
const char *mos_handle_vendor(const mos_handle_t *h)
{
    return (h && h->vendor_str[0]) ? h->vendor_str : NULL;
}

const char *mos_handle_product(const mos_handle_t *h)
{
    return (h && h->product_str[0]) ? h->product_str : NULL;
}

const char *mos_handle_revision(const mos_handle_t *h)
{
    return (h && h->revision_str[0]) ? h->revision_str : NULL;
}

uint64_t mos_handle_registry_id(const mos_handle_t *h)
{
    return h ? h->drive_registry_id : 0;
}

/* Identity is directory data from DRDeviceCopyInfo (framework-parsed
   INQUIRY bytes), copied through the bounded truncating seam in mos_dr.c.
   Hostile bytes are escaped at the output layer (mos_cli_json_str /
   mos_cli_safe_ascii). */

/* ---- Raw CDB (diagnostic only) ------------------------------------- */

mos_error mos_internal_raw_cdb(mos_handle_t *h,
                      const uint8_t *cdb, size_t cdb_len,
                      void *data_buf, size_t data_len,
                      mos_xfer_dir direction,
                      uint32_t timeout_ms,
                      uint32_t *scsi_task_status,
                      uint8_t   sense[18],
                      uint64_t *bytes_transferred)
{
    /* SetCommandDescriptorBlock accepts only 6/10/12/16 (kSCSICDBSize_*).
       Reject other lengths here so callers get MOS_ERR_INVALID_ARG, not an
       opaque execute-time failure. */
    if (!h || !h->mmc || !cdb || !scsi_task_status || !sense)
        return MOS_ERR_INVALID_ARG;
    if (cdb_len != 6 && cdb_len != 10 && cdb_len != 12 && cdb_len != 16)
        return MOS_ERR_INVALID_ARG;
    if (direction != MOS_XFER_NONE &&
        direction != MOS_XFER_FROM_TARGET &&
        direction != MOS_XFER_TO_TARGET)
        return MOS_ERR_INVALID_ARG;
    if (direction == MOS_XFER_NONE) {
        if (data_len != 0) return MOS_ERR_INVALID_ARG;
    } else {
        if (data_len == 0 || data_buf == NULL) return MOS_ERR_INVALID_ARG;
    }
    /* Timeout 0 means "Wait Forever" to SetTimeoutDuration — reject it so a
       non-responsive command can't hang. */
    if (timeout_ms == 0)
        return MOS_ERR_INVALID_ARG;

    /* Zero outputs after arg validation so error paths leave a deterministic
       zero state for consumers who skip the return check. Validation
       failures above leave outputs untouched ("never started"). */
    *scsi_task_status = 0;
    memset(sense, 0, 18);
    if (bytes_transferred) *bytes_transferred = 0;

    /* Lazily acquire the SCSITaskDeviceInterface. */
    if (!h->std) {
        h->std = (*h->mmc)->GetSCSITaskDeviceInterface(h->mmc);
        if (!h->std) return MOS_ERR_DRIVER_REJECTED;
    }

    /* Invariant pin (debug): every mos_internal_raw_cdb call releases exclusive access on
       every exit path, so the lock is never held across calls — it must be
       free on entry. A future early return that forgot to clear
       have_exclusive would otherwise skip the acquire below and run the CDB
       believing it holds a lock it doesn't; assert catches that in debug. */
    assert(!h->have_exclusive);

    /* Raw CDB requires exclusive access. */
    if (!h->have_exclusive) {
        IOReturn rx = (*h->std)->ObtainExclusiveAccess(h->std);
        if (rx != kIOReturnSuccess)
            return mos_internal_ioreturn_to_mos_error(rx);
        h->have_exclusive = true;
    }

    SCSITaskInterface **t = (*h->std)->CreateSCSITask(h->std);
    if (!t) {
        /* Release the lock acquired above (every exit clears
           have_exclusive, so it's always false on entry) — holding it is
           pointless without a task. */
        (*h->std)->ReleaseExclusiveAccess(h->std);
        h->have_exclusive = false;
        return MOS_ERR_IO;
    }

    /* Check each Set* IOReturn — ignoring one ships a malformed task and a
       baffling execute-time error. Identical cleanup, so all converge on
       one label. */
    IOReturn sr;
    sr = (*t)->SetCommandDescriptorBlock(t, (UInt8 *)cdb, (UInt8)cdb_len);
    if (sr != kIOReturnSuccess) goto setup_failed;

    IOVirtualRange sg = { (IOVirtualAddress)data_buf, (IOByteCount)data_len };
    sr = (*t)->SetScatterGatherEntries(t,
        data_len ? &sg : NULL,
        (UInt8)(data_len ? 1 : 0),
        (UInt64)data_len,
        (UInt8)direction);
    if (sr != kIOReturnSuccess) goto setup_failed;

    sr = (*t)->SetTimeoutDuration(t, timeout_ms);
    if (sr != kIOReturnSuccess) goto setup_failed;

    /* sense was zeroed in the zero-outputs block; nothing writes it since. */
    SCSI_Sense_Data sense_struct = {0};
    SCSITaskStatus   st           = 0;
    UInt64           xferred      = 0;

    IOReturn er = (*t)->ExecuteTaskSync(t, &sense_struct, &st, &xferred);
    (*t)->Release(t);

    /* Release before returning: a diagnostic command must not hold the lock
       for the handle's lifetime (would block Finder / MakeMKV / DA mounts).
       Released on success and IO-failure; only the InvalidArg exits skip it,
       never having acquired. */
    (*h->std)->ReleaseExclusiveAccess(h->std);
    h->have_exclusive = false;

    /* Transport failure: outputs stay at the zeros above (defined, not
       garbage); st/sense/xferred aren't copied. */
    if (er != kIOReturnSuccess) {
        return mos_internal_ioreturn_to_mos_error(er);
    }

    *scsi_task_status = (uint32_t)st;
    memcpy(sense, &sense_struct, 18);
    if (bytes_transferred) *bytes_transferred = (uint64_t)xferred;

    return MOS_OK;

setup_failed:
    (*t)->Release(t);
    (*h->std)->ReleaseExclusiveAccess(h->std);
    h->have_exclusive = false;
    return mos_internal_ioreturn_to_mos_error(sr);
}

/* ==== src/mos_sense.c ==== */
/*
 * mos_sense.c — SCSI sense-data parsing and state mapping. No IOKit;
 * unit-testable with fixture bytes.
 *
 * Fixed-format (response code 0x70 / 0x71): SPC-4 §4.5.3
 *   byte 0:  response code + valid bit
 *   byte 2:  bits 3:0 = sense key
 *   byte 12: ASC      byte 13: ASCQ
 *
 * Descriptor-format (response code 0x72 / 0x73): SPC-4 §4.5.2
 *   byte 0: response code   byte 1: bits 3:0 = sense key
 *   byte 2: ASC             byte 3: ASCQ
 *
 * Optical drives return fixed format in practice; the descriptor path is
 * here for correctness.
 */

#include <string.h>

void mos_internal_parse_sense(const uint8_t sense[18],
                              uint8_t *sk, uint8_t *asc, uint8_t *ascq)
{
    if (sk)   *sk   = 0;
    if (asc)  *asc  = 0;
    if (ascq) *ascq = 0;
    if (!sense) return;

    uint8_t rc = sense[0] & 0x7F;

    if (rc == 0x70 || rc == 0x71) {
        /* Fixed format */
        if (sk)   *sk   = sense[2] & 0x0F;
        if (asc)  *asc  = sense[12];
        if (ascq) *ascq = sense[13];
    } else if (rc == 0x72 || rc == 0x73) {
        /* Descriptor format */
        if (sk)   *sk   = sense[1] & 0x0F;
        if (asc)  *asc  = sense[2];
        if (ascq) *ascq = sense[3];
    }
}

/*
 * Map (sense_key, asc, ascq) → state, GIVEN THE TRAY IS CLOSED.
 *
 * Enrichment, not tray detection: open/closed is already settled (GESN
 * door bit, or the sense fork in mos_state_core.c), so this never returns
 * OPEN/EMPTY_OR_OPEN — it names the *reason* a closed tray isn't ready. A
 * 3A/02 ("medium not present, tray open") reaching here has its tray hint
 * discarded (enrich, don't invalidate): 0x3A means EMPTY, any other
 * not-ready sense means a disc is engaged, unrecognized → UNKNOWN.
 *
 * T10 ASC/ASCQ list: https://www.t10.org/lists/asc-num.htm
 */
mos_state mos_internal_state_from_sense_closed(uint8_t sk, uint8_t asc, uint8_t ascq)
{
    /* HARDWARE ERROR (key 0x04): the drive faulted — outranks any
       medium/not-ready detail also set. */
    if (sk == 0x04) return MOS_STATE_DEVICE_FAULT;

    /* MEDIUM ERROR (key 0x03), or 57/00 UNABLE TO RECOVER TOC: disc loaded
       but unreadable. Not self-resolving, so not loading. */
    if (sk == 0x03 || (asc == 0x57 && ascq == 0x00))
        return MOS_STATE_MEDIA_UNREADABLE;

    /* 3A/xx MEDIUM NOT PRESENT: no medium (the ASCQ tray flavor is GESN's). */
    if (asc == 0x3A) return MOS_STATE_EMPTY;

    /* 04/xx LOGICAL UNIT NOT READY: a disc is engaged; the qualifier says
       why it isn't ready yet. */
    if (asc == 0x04) {
        switch (ascq) {
            case 0x01:  /* becoming ready                 */
            case 0x02:  /* initialize command required    */
            case 0x07:  /* operation in progress          */
                return MOS_STATE_LOADING;   /* self-resolving by waiting */
            case 0x04:  /* format in progress             */
                return MOS_STATE_FORMATTING;
            case 0x08:  /* long write in progress         */
                return MOS_STATE_BUSY;      /* actively writing; back off */
            default:
                return MOS_STATE_UNKNOWN;
        }
    }

    return MOS_STATE_UNKNOWN;
}

/* ==== src/mos_serial.c ==== */
/*
 * mos_serial.c — the drive-serial query (mos_query_serial): one raw INQUIRY
 * (EVPD=1, PAGE CODE=0x80, Unit Serial Number) on the mos_internal_raw_cdb path,
 * decoded by the pure parser in mos_vpd80.c. The serial is the durable
 * drive-inventory key that survives replug and machine moves (registry_id is
 * attachment-scoped). Named for the datum it produces, not the generic
 * INQUIRY command set: this file owns the serial verb only — any future VPD
 * page is its own argument, not a fold into "the inquiry file".
 *
 * Authored raw, not via the convenience Inquiry, because MMCDeviceInterface's
 * Inquiry takes only SCSICmd_INQUIRY_StandardData* — no EVPD / PAGE CODE
 * parameter — so VPD page 0x80 is structurally unreachable through it
 * (contrast ModeSense10's PC/PAGE_CODE, GetConfiguration's RT). That is the
 * layer-1 "no convenience method carries the information" showing (AGENTS.md
 * scope doctrine; design + full derivation:
 * doc/research/2026-06-16-serial-vpd-0x80-feasibility.md).
 *
 * Never disturbs another consumer. mos_internal_raw_cdb is the SINGLE
 * ObtainExclusiveAccess call site (ARCHITECTURE.md §3); this file adds none.
 * Exclusive access is the gate: if anyone else holds the drive — a mounted
 * IOMedia nub, Finder, MakeMKV, another initiator — ObtainExclusiveAccess
 * fails (kIOReturnBusy / kIOReturnExclusiveAccess) and mos_internal_raw_cdb returns
 * the mapped error WITHOUT issuing the CDB, so the serial read backs off
 * cleanly rather than contending. On success the lock is held only for the
 * single INQUIRY and released immediately (mos_internal_raw_cdb releases per call —
 * never held across the handle's life). This is the same BUSY-on-mounted
 * guard the §5.5 nub invariant relies on. The degradation is benign: the
 * serial is a static drive fact, equally readable with the tray empty (the
 * natural time to inventory a drive), and any non-OK leaves serial null —
 * the field's existing default. INQUIRY changes no drive state, so there is
 * no lock-lifetime question (unlike the tray PREVENT verbs).
 */


/* 252-byte reply buffer: the serial fits in serial_str (64) many times over;
   the SPC PAGE LENGTH is a single byte (max 255), so this receives any
   conforming page and the parser truncates into serial_str. */
#define MOS_SERIAL_REPLY_BUF 252u

mos_error mos_query_serial(mos_handle_t *h, const char **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* INQUIRY (SPC-4 0x12), 6-byte CDB:
         byte0   opcode 0x12
         byte1   bit0 EVPD = 1            — request a vital-product-data page
         byte2   PAGE CODE = 0x80         — Unit Serial Number
         byte3-4 ALLOCATION LENGTH (BE)   — MOS_SERIAL_REPLY_BUF
         byte5   CONTROL = 0
       IMMED has no meaning for INQUIRY; the call waits for final status. */
    const uint8_t cdb[6] = {
        0x12, 0x01, 0x80,
        (uint8_t)(MOS_SERIAL_REPLY_BUF >> 8),
        (uint8_t)(MOS_SERIAL_REPLY_BUF & 0xFF),
        0x00,
    };

    uint8_t  buf[MOS_SERIAL_REPLY_BUF] = {0};
    uint32_t task_status               = 0;
    uint8_t  sense[18]                 = {0};
    uint64_t xferred                   = 0;

    /* Exclusive access unavailable (mounted media, another holder) →
       kIOReturnBusy/ExclusiveAccess → MOS_ERR_BUSY, the CDB never issues;
       any transport error surfaces honestly. The caller treats every non-OK
       as "serial stays null". */
    mos_error e = mos_internal_raw_cdb(h, cdb, sizeof cdb, buf, sizeof buf,
                              MOS_XFER_FROM_TARGET, 2000,
                              &task_status, sense, &xferred);
    if (e != MOS_OK) return e;
    if (task_status != MOS_SCSI_STATUS_GOOD)   /* CHECK CONDITION etc. */
        return MOS_ERR_IO;

    /* Dual-length rule (O-4): bound the parse to the bytes the transport
       actually delivered, not the full buffer — some USB bridges under-fill.
       The page's own PAGE LENGTH only shrinks the serial within that span. */
    size_t trusted = (xferred < sizeof buf) ? (size_t)xferred : sizeof buf;
    if (!mos_internal_vpd80_serial_parse(buf, trusted,
                                         h->serial_str, sizeof h->serial_str))
        return MOS_ERR_IO;   /* page absent / wrong page / no serial → null */

    *out = h->serial_str;
    return MOS_OK;
}

/* ==== src/mos_state.c ==== */
/*
 * mos_state.c — Apple-side adapter for the pure decision-tree core.
 *
 * Fills mos_state_env_t from a mos_handle_t and calls the pure core. The
 * split lets tests/test_state_core.c drive the tree with scripted MMC
 * responses. Contract: mos_internal_query_state_core in mos_pure.h.
 */


/* vtable trampolines; static so only this file binds the Apple ops table. */

static mos_error adapter_get_tray_state(void *ctx, bool *tray_open)
{
    return mos_internal_mmc_get_tray_state((mos_handle_t *)ctx, tray_open);
}

static mos_error adapter_test_unit_ready(void *ctx,
                                         uint32_t *status,
                                         uint8_t sense[18])
{
    return mos_internal_mmc_test_unit_ready((mos_handle_t *)ctx, status, sense);
}

static mos_error adapter_get_current_profile(void *ctx, uint16_t *profile)
{
    return mos_internal_mmc_get_current_profile((mos_handle_t *)ctx, profile);
}

static const mos_mmc_ops_t apple_mmc_ops = {
    .get_tray_state      = adapter_get_tray_state,
    .test_unit_ready     = adapter_test_unit_ready,
    .get_current_profile = adapter_get_current_profile,
};

mos_error mos_query_state(mos_handle_t *h, const mos_state_result **out)
{
    if (out) *out = NULL;
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    /* Re-resolve whole-disk identity off the pinned drive service before
       querying, so a handle opened on an empty drive reports an inserted
       disc's bsd_unit/media_id rather than the open-time -1. Only the
       IOMedia child changes with the media. */
    mos_internal_refresh_media_identity(h);

    mos_state_env_t env = {
        .ops                 = &apple_mmc_ops,
        .ctx                 = h,
        .bsd_unit            = h->bsd_unit,
        .registry_id         = h->drive_registry_id,
        .media_id            = h->media_id,
        .vendor              = h->vendor_str[0]  ? h->vendor_str  : NULL,
        .product             = h->product_str[0] ? h->product_str : NULL,
        .revision            = h->revision_str[0] ? h->revision_str : NULL,
    };

    /* Disc-completion (blank vs finalized) is not enriched here — no state
       decision needs it; it ships as an on-demand typed query
       (mos_query_disc_info; ARCHITECTURE.md §4.4). */

    mos_error rc = mos_internal_query_state_core(&env, &h->result);
    /* Media-type token read zero-MMC off the media node in
       refresh_media_identity above (NULL if absent). The core classifies state
       and does not touch it; set it here. Unlike media_class (derived from the
       profile, which is suppressed off the not-ready branch), this is present
       whenever the kernel publishes a Type — so a not-ready result can still
       name the disc. */
    h->result.media_type = h->media_type;
    /* Kernel IOMedia Writable flag, same zero-MMC origin and not-ready
       availability as media_type (-1 absent / 0 read-only / 1 writable). */
    h->result.writable = h->writable;
    if (rc == MOS_OK) *out = &h->result;
    return rc;
}

/* ==== src/mos_state_core.c ==== */
/*
 * mos_state_core.c — pure decision tree for mos_query_state().
 *
 * No IOKit. The single-shot convenience-TUR presence probe, the raw-GESN
 * tray fork, and the closed-branch sense enrichment run against a small
 * vtable (mos_mmc_ops_t). mos_state.c fills mos_state_env_t from a real
 * handle; mos_scsi.c implements the ops; tests/test_state_core.c drives it
 * with scripted ops and no hardware.
 *
 * Shape: the convenience TUR is trusted for PRESENCE and short-circuits
 * READY without a lock; on not-ready we take exclusive access (free, since
 * not-ready ⇒ not-mounted) and fire a RAW GESN for the one bit it owns,
 * tray open/closed; the TUR sense then refines the closed side into a
 * reason without overturning GESN's verdict. A negative return ("couldn't
 * reach the drive") stays categorically distinct from out->state.
 */


#include <string.h>

mos_error mos_internal_query_state_core(const mos_state_env_t *env,
                                        mos_state_result *out)
{
    if (!env || !env->ops || !out) return MOS_ERR_INVALID_ARG;

    /* All three callbacks are dispatched below; a NULL one would crash, and
       there's no degraded mode for classifying without TEST UNIT READY.
       Production tables are fully populated; this just gives fixture/fuzz
       paths a clean failure. */
    if (!env->ops->test_unit_ready ||
        !env->ops->get_tray_state ||
        !env->ops->get_current_profile) {
        return MOS_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->state    = MOS_STATE_UNKNOWN;
    /* Identity passes through verbatim: bsd_unit == -1 means "no media"
       (no IOMedia child), media_id carries the same-state swap fingerprint. */
    out->bsd_unit    = env->bsd_unit;
    out->registry_id = env->registry_id;
    out->media_id    = env->media_id;
    out->vendor   = env->vendor;
    out->product  = env->product;
    out->revision = env->revision;

    /* Declared above the first `goto enrich` so no jump skips an
       initializer (C11 §6.2.4). */
    uint32_t  status     = 0;
    uint8_t   sense[18]  = {0};
    uint8_t   sk = 0, asc = 0, ascq = 0;
    mos_error tur_err    = MOS_OK;
    bool      door_open  = false;
    bool      tray_open  = false;

    /* ---- 1. Convenience TUR — non-exclusive — ONE shot — PRESENCE ---- *
     * GOOD answers the whole question (closed + present + ready); otherwise
     * the sense feeds steps 2–3.
     *
     * Issued exactly once, like the macOS peers, with no UNIT ATTENTION
     * drain: by the time mos holds a handle the kernel's device init has
     * already consumed the power-on / reset / media-change UA, so one TUR
     * sees a settled drive. 0x3A signals "no medium" before GESN. */
    tur_err = env->ops->test_unit_ready(env->ctx, &status, sense);

    /* Expected, not defensive: the kernel user client refuses a convenience
       TUR while another client holds exclusivity — SCSITaskUserClient::
       TestUnitReady presets status to kIOReturnExclusiveAccess and gates on
       GetUserClientExclusivityState() (apple-oss-distributions/
       IOSCSIArchitectureModelFamily, UserClient/SCSITaskUserClient.cpp). So
       a contended drive is a real answer, and BUSY (not a negative error)
       is the truthful state. */
    if (tur_err == MOS_ERR_EXCLUSIVE_ACCESS || tur_err == MOS_ERR_BUSY) {
        out->state = MOS_STATE_BUSY;
        goto enrich;
    }
    /* Transport/IOKit failure reaching TUR: no state observed, surface the
       negative code rather than any state. */
    if (tur_err != MOS_OK) return tur_err;

    if (status == MOS_SCSI_STATUS_GOOD) {
        out->state = MOS_STATE_READY;   /* closed + disc present + ready */
        goto enrich;
    }
    if (mos_internal_status_is_contended(status)) {
        out->state = MOS_STATE_BUSY;
        goto enrich;
    }
    if (status == MOS_SCSI_STATUS_CHECK_CONDITION) {
        mos_internal_parse_sense(sense, &sk, &asc, &ascq);
        out->sense_key = sk; out->asc = asc; out->ascq = ascq;
    }
    /* NUB GATE — must equal the kernel's nub predicate exactly (an
       approximate `sk==0 && asc==0 && ascq==0` gate diverges; see
       tests/audit/nub_invariant_check.c).

       PollForMedia sets mediaFound on CC + 00/00 independent of the sense
       key (IOSCSIMultimediaCommandsDevice.cpp:3890-3894, before the
       SENSE_KEY switch), resets it when the switch set shouldEjectMedia
       (4012-4029), and creates the IOMedia nub only if it survives (4052).
       At 00/00 the eject set is keys {NOT_READY, MEDIUM_ERROR,
       HARDWARE_ERROR, BLANK_CHECK} (their keep-lists can't match 00/00). So:

         - CC + 00/00 + key OUTSIDE {0x2,0x3,0x4,0x8}: kernel KEEPS the nub,
           so mos must NOT lock — classify UNKNOWN. Stray UNIT ATTENTION
           (06/00/00) or RECOVERED ERROR (01/00/00) land here.
         - CC + 00/00 + key IN {0x2,0x3,0x4,0x8}: kernel EJECTS, no nub, the
           lock is free, and the GESN probe below turns HARDWARE ERROR into
           device_fault rather than UNKNOWN.

       Non-zero ASC/ASCQ never sets the flag, so the lock is always safe. */
    if (status != MOS_SCSI_STATUS_CHECK_CONDITION ||
        (asc == 0 && ascq == 0 &&
         sk != 0x02 && sk != 0x03 && sk != 0x04 && sk != 0x08)) {
        /* Kernel keeps the nub (or it isn't a CHECK CONDITION) — mos may
           not probe. */
        out->state = MOS_STATE_UNKNOWN;
        goto enrich;
    }

    /* ---- 2. Not ready ⇒ not mounted ⇒ lock is free. Tray bit. ---- *
     * get_tray_state issues a RAW GESN under exclusive access. MOS_OK ⇒
     * door_open is authoritative; any failure ⇒ fall back to the TUR sense.
     * The sense never overturns a GESN open/closed verdict. */
    if (env->ops->get_tray_state(env->ctx, &door_open) == MOS_OK) {
        tray_open = door_open;                     /* authoritative */
    } else if (asc == 0x3A && ascq == 0x02) {
        tray_open = true;                          /* sense fork: tray open */
    } else if (asc == 0x3A && ascq == 0x01) {
        tray_open = false;                         /* sense fork: tray closed */
    } else if (asc == 0x3A) {                       /* 3A/00 + no GESN */
        out->state = MOS_STATE_EMPTY_OR_OPEN;       /* no medium, tray unknowable */
        goto enrich;
    } else {
        tray_open = false;                          /* non-3A not-ready ⇒ disc engaged ⇒ closed */
    }

    /* ---- 3. Fork ---- */
    if (tray_open) {
        out->state = MOS_STATE_OPEN;                /* tray's out — nothing to enrich */
        goto enrich;
    }

    /* Tray CLOSED: the TUR sense refines the not-ready reason (may be
       UNKNOWN — still closed, raw sense rides on out->sense_*). */
    out->state = mos_internal_state_from_sense_closed(sk, asc, ascq);

enrich:
    /* ---- Enrichment (metadata only, never changes state) ---- *
     * current_profile only on READY: some firmwares (notably LG) keep
     * reporting the last disc's profile for minutes after eject
     * (ARCHITECTURE.md §9), so surfacing it on a not-present state would
     * imply a disc. Else it stays at the memset(0) default. */
    if (out->state == MOS_STATE_READY) {
        uint16_t profile = 0x0000;
        if (env->ops->get_current_profile(env->ctx, &profile) == MOS_OK) {
            out->current_profile = profile;
        }
    }

    return MOS_OK;
}

/* ==== src/mos_strings.c ==== */
/*
 * mos_strings.c — pure string tables, escapers, and version. Separate TU so
 * mos_scsi.c stays exclusively IOKit-linked. No IOKit.
 */

/* mos_pure.h (which re-includes mos.h) declares the mos_internal_* helpers
   defined in this file, so the definitions are checked against their
   prototypes (-Wmissing-prototypes). Kept on its own line above the include:
   the amalgamator drops library-local #include lines wholesale, so a trailing
   block comment here would be orphaned into dist/mos.c (scripts/amalgamate.sh). */
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

/* ==== src/mos_trackinfo.c ==== */
/*
 * mos_trackinfo.c — pure, bounds-safe decode of a READ TRACK INFORMATION
 * (MMC 0x52) Track Information Block: the capacity / append-state surface
 * (track start, next writable address, free blocks, track size, last
 * recorded address) plus track/data mode and blank/damage bits. No IOKit:
 * the shell hands us a fixed zero-init buffer (filled via
 * ReadTrackInformation) and its size. Every length is device-reported,
 * hence hostile — the declared length must never steer a read outside
 * [buf, buf+len); only fixed offsets are read.
 *
 * Wire layout (the MMC Track Information Block; cdrom.h cross-check in
 * SPEC.md):
 *   [0..1]  Track Information Length (BE) — bytes AFTER this field
 *   [2]     Track Number (LSB)
 *   [3]     Session Number (LSB)
 *   [4]     reserved
 *   [5]     reserved(7:6) | damage(5) | copy(4) | track_mode(3:0)
 *   [6]     rt(7) | blank(6) | packet(5) | fp(4) | data_mode(3:0)
 *   [7]     reserved(7:2) | lra_v(1) | nwa_v(0)
 *   [8..11]  Track Start Address (BE)
 *   [12..15] Next Writable Address (BE)   — valid iff nwa_v
 *   [16..19] Free Blocks (BE)
 *   [20..23] Fixed Packet Size (BE)
 *   [24..27] Track Size (BE)
 *   [28..31] Last Recorded Address (BE)   — valid iff lra_v
 *   [32..33] Track / Session Number MSB   — MMC-6 longer reply, optional
 *
 * NWA and LRA are meaningful only when their *_v validity bit is set; the
 * consumer must check the *_valid accessor. No payload byte is ever used
 * as an offset.
 */


#define TI_MIN_LEN  32u   /* through Last Recorded Address (byte 31) */
#define TI_MSB_LEN  34u   /* track/session MSB present (byte 33) */

static uint32_t mos_internal_ti_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | p[3];
}

bool mos_internal_track_info_parse(const uint8_t *buf, size_t len,
                                   struct mos_track_info *out)
{
    if (!out) return false;
    *out = (struct mos_track_info){0};
    if (!buf || len < TI_MIN_LEN) return false;

    size_t declared = (size_t)(((uint16_t)buf[0] << 8) | buf[1]) + 2u;
    size_t end = (len < declared) ? len : declared;
    if (end < TI_MIN_LEN) return false;

    out->track_number   = buf[2];
    out->session_number = buf[3];
    out->track_mode     = (uint8_t)(buf[5] & 0x0f);
    out->damage         = (buf[5] >> 5) & 0x01;
    out->data_mode      = (uint8_t)(buf[6] & 0x0f);
    out->blank          = (buf[6] >> 6) & 0x01;
    out->lra_valid      = (buf[7] >> 1) & 0x01;
    out->nwa_valid      = buf[7] & 0x01;
    out->track_start    = mos_internal_ti_be32(&buf[8]);
    out->next_writable  = mos_internal_ti_be32(&buf[12]);
    out->free_blocks    = mos_internal_ti_be32(&buf[16]);
    out->track_size     = mos_internal_ti_be32(&buf[24]);
    out->last_recorded  = mos_internal_ti_be32(&buf[28]);

    /* MMC-6 longer reply carries the track/session high byte at 32/33;
       only when the trusted region reaches them. */
    if (end >= TI_MSB_LEN) {
        out->track_number   = (uint16_t)(out->track_number   | (buf[32] << 8));
        out->session_number = (uint16_t)(out->session_number | (buf[33] << 8));
    }
    return true;
}

/* ==== src/mos_tray.c ==== */
/*
 * mos_tray.c — tray-control verbs that CHANGE drive state: eject/close
 * (START STOP UNIT 0x1B) and lock/unlock (PREVENT ALLOW MEDIUM REMOVAL
 * 0x1E). Each verb is one raw 6-byte CDB on the mos_internal_raw_cdb path,
 * with a sense check in place of a payload decode.
 *
 * mos_internal_raw_cdb is the SINGLE ObtainExclusiveAccess call site
 * (ARCHITECTURE.md §3); this file adds none, so the BUSY-on-mounted guard the
 * §5.5 nub invariant relies on also covers the tray verbs: a user-initiated
 * lock/eject on a MOUNTED volume returns MOS_ERR_BUSY rather than disturbing
 * a live IOMedia nub.
 *
 * Authored raw, not via convenience methods, because MMCDeviceInterface's
 * SetTrayState cannot surface a 5/53/02 locked-eject refusal (ARCHITECTURE.md
 * §9.7/§9.9) — the layer-1 "no convenience method carries the information"
 * showing (AGENTS.md scope doctrine).
 *
 * Lock lifetime: the PREVENT state is per-I_T-nexus and survives a handle
 * close / process exit (T10 04-349r1 §6.18; the SCSITaskUserClient close is
 * none of the SPC-4 clearing events, and Apple's
 * IOSCSIMultimediaCommandsDevice issues no voluntary ALLOW on exclusive-
 * access release). mos holds nothing for the lock window — the verbs are
 * fire-and-forget, recovery is a later mos_tray_unlock on the same single
 * initiator. No atexit ALLOW on the lock path: a single-shot lock that
 * released itself on return would be a no-op, and a persistent lock is
 * exactly what a ripping-robot orchestrator wants to outlive the process.
 */


/* The CDBs as fixed 6-byte arrays. IMMED (byte1 bit0) is 0 on all, so the
   call WAITS for the honest final status instead of an immediate "accepted"
   — a locked eject's 5/53/02 must arrive on the sense channel.

   START STOP UNIT (SPC-4 0x1B): byte4 = PWRCND(7:4) 0 | NO_FLUSH(bit2) 0 |
   LoEj(bit1) | START(bit0). eject = LoEj 1, START 0 -> 0x02;
   close/load = LoEj 1, START 1 -> 0x03.

   PREVENT ALLOW MEDIUM REMOVAL (SPC-4 0x1E): byte4 PREVENT field
   {PERSISTENT(bit1), PREVENT(bit0)} (T10 04-349r1 Table 8):
     0x00 clear basic Prevent (unlock)        0x01 set basic Prevent (lock)
     0x02 clear Persistent Prevent (p-allow)  0x03 set Persistent Prevent (p-lock)
   The two states are INDEPENDENT — 0x00 does not clear a 0x03 lock, 0x02 does
   (04-349r1 §6.18.2 / §6.18.3.2). */
static const uint8_t cdb_eject [6] = { 0x1B, 0x00, 0x00, 0x00, 0x02, 0x00 };
static const uint8_t cdb_close [6] = { 0x1B, 0x00, 0x00, 0x00, 0x03, 0x00 };
static const uint8_t cdb_unlock        [6] = { 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t cdb_lock          [6] = { 0x1E, 0x00, 0x00, 0x00, 0x01, 0x00 };
static const uint8_t cdb_unlock_persist[6] = { 0x1E, 0x00, 0x00, 0x00, 0x02, 0x00 };
static const uint8_t cdb_lock_persist  [6] = { 0x1E, 0x00, 0x00, 0x00, 0x03, 0x00 };

/* Prevent/allow is electronic (instant); eject/close drives the tray motor
   and needs time for mechanical travel. GESN uses 2000 ms; eject/close start
   at 5000 ms (a fixture refines it if a slow loader appears). */
#define MOS_TRAY_PREVENT_TIMEOUT_MS 2000u
#define MOS_TRAY_MOTION_TIMEOUT_MS  5000u

mos_error mos_internal_tray_cmd(mos_handle_t *h, const uint8_t cdb[6],
                                mos_tray_outcome *outcome, uint8_t sense_out[3])
{
    if (!h || !cdb || !outcome) return MOS_ERR_INVALID_ARG;
    if (sense_out) { sense_out[0] = sense_out[1] = sense_out[2] = 0; }

    /* 0x1B gets the mechanical timeout, 0x1E the short one — opcode is the
       only discriminator the wrapper needs. */
    uint32_t timeout = (cdb[0] == 0x1B) ? MOS_TRAY_MOTION_TIMEOUT_MS
                                        : MOS_TRAY_PREVENT_TIMEOUT_MS;

    uint32_t task_status = 0;
    uint8_t  sense[18]   = {0};
    mos_error e = mos_internal_raw_cdb(h, cdb, 6, NULL, 0, MOS_XFER_NONE,
                              timeout, &task_status, sense, NULL);
    if (e != MOS_OK) return e;          /* transport/lock: honest failure */

    uint8_t sk = 0, asc = 0, ascq = 0;
    mos_internal_parse_sense(sense, &sk, &asc, &ascq);
    *outcome = mos_internal_tray_classify(task_status, sk, asc, ascq);
    if (sense_out) { sense_out[0] = sk; sense_out[1] = asc; sense_out[2] = ascq; }
    return MOS_OK;
}

mos_error mos_tray_eject(mos_handle_t *h, bool force,
                         mos_tray_outcome *out, uint8_t sense[3])
{
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    /* ONE flow. Every eject grabs exclusive access (in mos_internal_raw_cdb) and issues
       the CDB; a plain eject reports the result verbatim. --force diverges only
       on a CLEARABLE failure and then RECONVERGES on the same eject CDB:

         - MOS_ERR_BUSY = a Finder/system mount holds exclusive access
           (SCSITaskLib: "media is still mounted"; mos_pure.c maps it,
           mos_scsi.c static-asserts the constant) -> force-unmount, re-eject.
         - REFUSED_LOCKED = a basic Prevent lock refused the eject CDB -> clear
           BOTH Prevent states (basic then persistent, so nothing is left
           locked when a lock was in the way), re-eject.

       The one failure --force cannot clear is MOS_ERR_EXCLUSIVE_ACCESS = another
       userland client (no SCSI preempt exists): it falls through and surfaces,
       tray shut. At most two blockers (mount, lock) stack, so the loop is
       bounded at two passes. Each CDB grabs/releases exclusive access per call
       — mos_internal_raw_cdb stays the sole §3 lock site; no second one is introduced.
       (A drive with ONLY a Persistent Prevent and no mount/basic-lock ejects on
       the first CDB — an initiator eject succeeds under Persistent Prevent by
       spec — so it opens without a speculative clear; --force clears persistent
       only when a lock actually blocked, never issuing a command it can't know
       it needs.) */
    mos_error e = mos_internal_tray_cmd(h, cdb_eject, out, sense);
    if (!force) return e;

    for (int pass = 0; pass < 2; pass++) {
        if (e == MOS_ERR_BUSY) {                          /* Finder/system mount */
            /* Re-resolve the CURRENT media under h->svc before unmounting: the
               cached h->bsd_unit can be stale (opened empty, or a swap since
               the last media query), and a forced unmount is data-loss-capable.
               The refresh derives bsd_unit + media_id from h->svc's live child,
               so the name we format and the identity we bind both describe the
               disc actually in THIS drive now. media gone (bsd_unit < 0) → fail
               closed; the bind in mos_internal_da_unmount closes the residual
               BSD-reuse race. */
            mos_internal_refresh_media_identity(h);
            char name[24];
            if (h->bsd_unit < 0 ||
                !mos_bsd_name_format(h->bsd_unit, name, sizeof name) ||
                !mos_internal_da_unmount(name, h->media_id))
                break;                                    /* mount uncleared */
        } else if (e == MOS_OK && *out == MOS_TRAY_REFUSED_LOCKED) {  /* basic Prevent */
            mos_tray_outcome ignored = MOS_TRAY_DONE;
            (void)mos_internal_tray_cmd(h, cdb_unlock,         &ignored, NULL);
            (void)mos_internal_tray_cmd(h, cdb_unlock_persist, &ignored, NULL);
        } else {
            break;   /* DONE, EXCLUSIVE_ACCESS (peer client), or transport — stop */
        }
        e = mos_internal_tray_cmd(h, cdb_eject, out, sense);   /* reconverge */
    }
    return e;
}

mos_error mos_tray_close(mos_handle_t *h, mos_tray_outcome *out, uint8_t sense[3])
{
    if (!h || !out) return MOS_ERR_INVALID_ARG;
    return mos_internal_tray_cmd(h, cdb_close, out, sense);
}

mos_error mos_tray_lock(mos_handle_t *h, bool persistent,
                        mos_tray_outcome *out, uint8_t sense[3])
{
    if (!h || !out) return MOS_ERR_INVALID_ARG;
    return mos_internal_tray_cmd(h, persistent ? cdb_lock_persist : cdb_lock,
                                 out, sense);
}

mos_error mos_tray_unlock(mos_handle_t *h, bool persistent,
                          mos_tray_outcome *out, uint8_t sense[3])
{
    if (!h || !out) return MOS_ERR_INVALID_ARG;
    return mos_internal_tray_cmd(h,
                                 persistent ? cdb_unlock_persist : cdb_unlock,
                                 out, sense);
}

/* ==== src/mos_vpd80.c ==== */
/*
 * mos_vpd80.c — pure, bounds-safe decode of INQUIRY VPD page 0x80 (Unit
 * Serial Number). The one identity field DiscRecording's directory does not
 * cache and no convenience method can carry: MMCDeviceInterface's Inquiry
 * issues only a standard INQUIRY (no EVPD / PAGE CODE), so page 0x80 needs a
 * raw INQUIRY (mos_serial.c). Design + layer-1 raw-verb showing:
 * doc/research/2026-06-16-serial-vpd-0x80-feasibility.md.
 *
 * No IOKit: the shell hands us a fixed zero-init buffer (filled via
 * mos_internal_raw_cdb) bounded to the bytes the transport actually returned
 * (dual-length rule O-4 — the realized count, not the device-claimed
 * length, is the trusted span). The page's own PAGE LENGTH never extends
 * past that span; if it exceeds it (the transport under-delivered) the
 * reply is refused rather than emitted as a prefix — a durable identity key
 * is complete or nothing (the canonical-data corollary of O-4).
 *
 * Page 0x80 layout (SPC-4 §7.7.13, Unit Serial Number VPD page):
 *   [0]      PERIPHERAL QUALIFIER (7:5) | PERIPHERAL DEVICE TYPE (4:0)
 *   [1]      PAGE CODE = 80h        — the drive echoes the page we asked for
 *   [2]      reserved
 *   [3]      PAGE LENGTH (n-3)      — serial byte count
 *   [4..n]   PRODUCT SERIAL NUMBER  — ASCII, left-justified, space-padded
 * Byte 2 stays reserved for this page (it is NOT the high byte of a 2-byte
 * length — that generalization is page 0x83's, not 0x80's). A real page-0x80
 * capture is a falsifier per the hardware ADR, not a design input.
 */


#define VPD_HDR 4u   /* bytes 0..3 before the serial */

/* Decode page 0x80 into out[0..out_cap). True only when the reply echoes
   page code 0x80 and carries a complete, non-empty serial (trailing spaces /
   NULs trimmed) that can be represented WHOLE as a C string. out is always
   NUL-terminated. Returns false → the caller leaves serial null (never an
   empty, truncated, or NUL-severed string) when the drive does not implement
   the page (it is optional), has none programmed (all-spaces), under-delivers
   the reply (PAGE LENGTH > bytes received), or the serial cannot be held whole
   — longer than out_cap, or containing an interior NUL (which would sever the
   C string invisibly at the NUL). This is the complete-or-unavailable rule for
   a durable identity key: a prefix is an indistinguishable, wrong key (two
   drives can share it), so it is refused, not marked. Non-ASCII bytes are
   copied verbatim and escaped at the output sink (mos_cli_json_str /
   mos_safe_ascii), as with vendor/product/revision. */
bool mos_internal_vpd80_serial_parse(const uint8_t *buf, size_t len,
                                     char *out, size_t out_cap)
{
    if (out && out_cap) out[0] = 0;
    if (!buf || !out || out_cap == 0) return false;
    if (len < VPD_HDR) return false;          /* no room for the VPD header */
    if (buf[1] != 0x80) return false;         /* wrong page echoed — refuse */

    /* PAGE LENGTH (byte 3) is the serial byte count the drive CLAIMS. The
       reply must actually carry all of it. If PAGE LENGTH exceeds the bytes
       delivered (avail — the realized-transfer span, O-4), the transport
       under-filled (a non-conformant USB-SATA bridge, a short transfer) and we
       hold only a PREFIX of the serial. REFUSE rather than emit it: this value
       is a DURABLE IDENTITY KEY the caller caches sticky (mos watch grabs it
       once per session), and a silent prefix can collide with another drive or
       misidentify this one — an incomplete key is worse than none. Complete or
       nothing. (A conforming drive, given mos_serial.c's ample 252-byte
       allocation, returns the whole serial, so page_len <= avail holds.) This
       still bounds the read: refusing happens before any serial byte is read,
       so a hostile over-long PAGE LENGTH cannot read past the trusted span. */
    size_t page_len = buf[3];
    size_t avail    = len - VPD_HDR;
    if (page_len > avail) return false;
    size_t serial_len = page_len;

    /* Trim trailing wire padding — spaces (SPC pad) and NULs. Leading and
       interior bytes are data and stay (mirrors mos_dr.c identity trim). */
    while (serial_len > 0) {
        uint8_t c = buf[VPD_HDR + serial_len - 1];
        if (c != ' ' && c != 0x00) break;
        serial_len--;
    }
    if (serial_len == 0) return false;        /* page present, no serial */

    /* Complete-or-unavailable. The serial is a durable identity key the caller
       caches sticky, so it must be representable WHOLE as a C string or refused
       — a prefix is a different-but-equally-wrong key two drives can share.
       Two ways the whole serial cannot be held, both REFUSE (return false), the
       same disposition as the transport-under-delivery case above:

         - an interior NUL: trailing NULs were trimmed, but a NUL among the
           remaining bytes would sever the C string invisibly at the NUL,
           hiding everything after it — so two serials differing only past
           the NUL would collide. It is non-ASCII for a SPC serial; treat it
           as an unrepresentable key, not data to copy.
         - serial_len > out_cap - 1: the whole serial does not fit. (Real
           serials sit far below mos_serial.c's 64-byte sink, so this fires only
           on a pathological/hostile over-long serial.)

       Refusing precedes any copy, so nothing partial is ever emitted. */
    for (size_t i = 0; i < serial_len; i++)
        if (buf[VPD_HDR + i] == 0x00) return false;   /* interior NUL */
    if (serial_len > out_cap - 1u) return false;       /* would not fit whole */

    for (size_t i = 0; i < serial_len; i++) out[i] = (char)buf[VPD_HDR + i];
    out[serial_len] = 0;
    return true;
}

/* ==== src/mos_watch.c ==== */
/*
 * mos_watch.c — Apple-side adapter for the pure watch state machine
 * (src/mos_watch_core.c). Single-threaded by contract: notification
 * callbacks fire on the same run-loop thread that calls
 * mos_watch_next_event, so no locking.
 */

/* Before any system header so BSD extensions stay visible: strlcpy (the
   identity/serial re-home below) needs _DARWIN_C_SOURCE, and the amalgamation
   merges adapter TUs into one feature-macro environment, so this also keeps
   standalone and amalgamated builds from diverging. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

/* CLOCK_MONOTONIC / clock_gettime require POSIX.1-2008. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif


#include <CoreFoundation/CoreFoundation.h>
#include <DiscRecording/DRCoreDevice.h>
#include <DiscRecording/DRCoreNotifications.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Private run-loop mode for the watch's sources — never
   kCFRunLoopDefaultMode — so the host's default-mode work can't dispatch
   our callbacks and our CFRunLoopStop can't halt a host-owned run-loop
   invocation. The pump runs this mode, so our sources fire only while
   mos_watch_next_event waits. */
#define MOS_WATCH_RUN_LOOP_MODE CFSTR("io.github.napieraj.mos.watch")

/* ---- Public opaque type --------------------------------------------- */

struct mos_watch {
    /* Pure state machine; owns session identity / bsd_unit / seq. */
    mos_watch_state core;

    /* Whole-disk unit (N in "diskN"), or -1 for an empty/open-tray drive.
       Tags events and feeds mos_watch_bsd_unit — NOT the probe authority;
       that's registry_id below. */
    int64_t bsd_unit;

    /* IORegistry entry ID of the bound drive, captured at construction.
       watch_probe reopens the SAME drive each poll, immune to BSD-name
       reassignment: bound to drive A, keeps probing A even if A's name is
       recycled. When A terminates the reopen returns NO_DEVICE → terminal
       device_removed. The single probe-identity authority. */
    uint64_t registry_id;

    /* Retained IOKit reference for the notification; released in close. */
    io_service_t svc;

    /* Notification plumbing; fires on the run loop this port is scheduled
       on (the caller's, set in watch_open). Token released in close. */
    IONotificationPortRef notify_port;
    io_object_t           notify_token;       /* kIOGeneralInterest */
    CFRunLoopSourceRef    notify_source;
    CFRunLoopRef          run_loop;

    /* DiscRecording doorbell for media/tray-change wake-up. DR's
       StatusChanged is device-scoped, so it also wakes tray-open/no-media
       drives. The callback pulls the next poll forward and CFRunLoopStop()s
       the pump. Both NULL on poll-only fallback (creation failed) — polling
       is the correctness floor, the doorbell latency only. SINGLE-TARGET
       ONLY: in all mode discovery rides the doorbell with no poll floor, so
       mos_watch_open_all fails instead of falling back. */
    DRNotificationCenterRef dr_center;
    CFRunLoopSourceRef      dr_source;

    /* Holds the most recent event so mos_watch_next_event can return
       borrowed pointers valid until the next call. Identity values are
       plain; vendor/product/revision point into the buffers below. */
    mos_watch_event last_event;

    /* Device-static identity, captured ONCE from the validated open handle
       (DR-directory strings), owned for the watch's life. Events point
       here; per-probe handles never contribute identity. Widths are the
       SPC-4 INQUIRY field widths the directory data parses from:
         vendor[9]    VENDOR_IDENTIFICATION   ( 8 + NUL)
         product[17]  PRODUCT_IDENTIFICATION  (16 + NUL)
         revision[5]  PRODUCT_REVISION_LEVEL  ( 4 + NUL, SPC-4 §6.4.2) */
    char vendor[9];
    char product[17];
    char revision[5];

    /* Drive Unit Serial Number (INQUIRY VPD 0x80), grabbed ONCE per session
       on a probe handle and cached for the watch's life. serial_grabbed flips
       true on the first successful read so later probes stop trying. serial[64]
       matches mos_handle's serial_str width (SPC max is 255; 64 truncates
       safely — the chosen buffer everywhere). Empty (serial[0]==0) until the
       first free/not-ready poll lands the read; events carry NULL until then,
       the cached string after. */
    char serial[64];
    bool serial_grabbed;

    /* ---- Watch-all mode -------------------------------------------- *
     * all_mode selects the multiplexer: `all` is the pure fan-in over
     * per-slot cores, `slots` is the per-device probe context (registry id
     * + identity) each core's ctx points at. The single-target fields above
     * are unused in all mode; bsd_unit stays -1. Poll rates kept for
     * mid-stream joins. */
    bool                 all_mode;
    mos_watch_all_state  all;
    struct mos_watch_slot {
        uint64_t registry_id;
        char     vendor[9];
        char     product[17];
        char     revision[5];
        /* Per-slot serial: same grab-once-per-session contract as the
           single-target fields above. Slots are RECYCLED — a removed device
           frees its slot and watch_all_add_device reclaims any inactive one —
           so these are NOT fresh per device by themselves; that function
           memsets the slot on claim, which is what resets serial_grabbed and
           prevents the prior device's serial from carrying over. */
        char     serial[64];
        bool     serial_grabbed;
    }                    slots[MOS_WATCH_ALL_CAP];
    uint32_t             stable_poll_ms;
    uint32_t             transition_poll_ms;
    /* The all-watch's ONE stream-open timestamp, minted once and given to
       every slot (open-time drives and later joiners alike), so
       stream_open_ms is constant across the stream (mos.h, mos.event.v1).
       Per-event time rides ts; (registry_id, stream_open_ms) stays unique
       because a replug re-mints registry_id. 0 in single-target mode. */
    uint64_t             all_stream_open_wall_ms;

    /* One-shot reconciliation flag (all-mode). A DR Appeared whose snapshot
       fails — transient: DRDeviceCopyInfo returned NULL, or the IORegistry
       path was not yet resolvable at that instant — cannot join its device,
       and Appeared is edge-triggered, so it never re-fires; all-mode also has
       no discovery poll floor, so the device would stay invisible for the
       session. A failed Appeared therefore ARMS this flag, and the next pump
       entry clears it and re-copies the directory ONCE, joining anything not
       already active (watch_all_add_device dedupes). Bounded: a single flag,
       not a periodic rescan, so it recovers the drop WITHOUT reintroducing the
       poll floor the all-watch deliberately omits. Set/read only on the single
       run-loop thread (the watch's threading contract), so no synchronization.
       Residual (strictly narrower than the bug it closes): if the device is
       STILL unresolvable when the reconciliation runs, the flag is already
       cleared and it is missed — two consecutive resolution failures, vs. the
       one the bug needed. */
    bool                 all_rescan_pending;
};

/* ---- Time --------------------------------------------------------- */

/* Monotonic ms. CLOCK_MONOTONIC is unconditional at our 12.0 floor. */
static uint64_t monotonic_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Wall-clock ms for the session-open timestamp (stream_open_ms is real
   epoch ms). Used only at open, via the monotonicized wrapper below. */
static uint64_t wall_clock_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Per-process monotonicized wall ms for the session-open timestamp. Two
   watches opened on the same drive in the same wall ms would otherwise
   share a (registry_id, stream_open_ms) pair. Bumping a same-or-earlier
   reading to last+1 keeps it epoch-ms-shaped while guaranteeing per-process
   uniqueness across NTP step-backs. Event ts is unaffected — it reads
   wall_clock_ms() fresh per emit. */
static uint64_t stream_epoch_wall_ms(void)
{
    static _Atomic uint64_t last = 0;
    uint64_t now  = wall_clock_ms();
    uint64_t prev = atomic_load_explicit(&last, memory_order_relaxed);
    for (;;) {
        uint64_t next = (now > prev) ? now : prev + 1;
        if (atomic_compare_exchange_weak_explicit(&last, &prev, next,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            return next;
        }
        /* failed CAS reloaded prev; recompute and retry. */
    }
}

/* ---- vtable callbacks -------------------------------------------- */

/* probe: reopen a fresh handle by the watch's registry id, query state,
   close. Handle-per-probe lets a transient driver detach recover next poll.

   POINTER-LIFETIME INVARIANT (adapter-scoped — the pure layer forwards
   `const char *` fields verbatim, blind to one being borrowed from a handle
   we're about to close, so its tests can't catch a violation): before any
   mos_close(h), every handle-borrowed pointer field of the escaping struct
   must be REPLACED with a watch-static buffer (w->vendor / w->product /
   w->revision / w->serial) or NULLed. The footgun is `*out = *qr;` — it copies every
   pointer, so "forgot one" is the default. A new borrowed pointer on
   mos_watch_event / mos_state_result needs watch-lifetime backing and a
   replacement below. (bsd_unit is a value, never replaced.) */
static mos_error watch_probe(void *ctx, mos_state_result *out)
{
    mos_watch_t *w = (mos_watch_t *)ctx;
    if (!w || !out) return MOS_ERR_INVALID_ARG;

    /* Reopen by registry ID: either the SAME drive back, or NO_DEVICE if it
       terminated (core treats that as terminal removal). */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_internal_open_by_registry_id(w->registry_id, &err);
    if (!h) {
        /* Contract: NULL iff err != MOS_OK; force non-OK if violated. */
        return err != MOS_OK ? err : MOS_ERR_IO;
    }

    const mos_state_result *qr = NULL;
    mos_error qerr = mos_query_state(h, &qr);
    if (qerr != MOS_OK || !qr) {
        mos_close(h);
        return qerr != MOS_OK ? qerr : MOS_ERR_IO;
    }

    /* Copy the handle-owned result so its identity strings can be re-homed
       below and survive mos_close(h). */
    *out = *qr;

    /* The drive is pinned, but the media's BSD unit isn't (-1 empty,
       changes across eject/reinsert). Refresh the adapter's copy (feeds
       mos_watch_bsd_unit). The media_id fingerprint rides the copy. */
    w->bsd_unit = out->bsd_unit;
    /* Re-home the three identity pointers to watch-static buffers (lifetime
       invariant above) so they don't survive the mos_close below. Identity
       is device-static, so this loses nothing. */
    out->vendor   = w->vendor[0]   ? w->vendor   : NULL;
    out->product  = w->product[0]  ? w->product  : NULL;
    out->revision = w->revision[0] ? w->revision : NULL;

    /* Grab the serial ONCE per session, piggybacked on this same handle (no
       extra open). mos_query_serial self-gates on exclusive access, so a
       mounted/ready disc makes it BUSY and the CDB never issues — leave it
       ungrabbed and retry next poll; the first empty/not-ready poll lands it
       (the walk's lock is already free then and the serial needs no disc).
       Re-home into watch-static storage like the identity strings (the
       returned pointer borrows the handle we're about to close). */
    if (!w->serial_grabbed) {
        const char *sn = NULL;
        if (mos_query_serial(h, &sn) == MOS_OK && sn && sn[0]) {
            strlcpy(w->serial, sn, sizeof w->serial);
            w->serial_grabbed = true;
        }
    }
    out->serial = w->serial[0] ? w->serial : NULL;

    mos_close(h);
    return MOS_OK;
}

/* Monotonic-ms callback: poll scheduling and latency only. */
static uint64_t watch_mono_ms(void *ctx)
{
    (void)ctx;
    return monotonic_ms();
}

/* Wall-clock ms callback: event ts formatting only. Never scheduling —
   the clock can jump backward. */
static uint64_t watch_wall_ms(void *ctx)
{
    (void)ctx;
    return wall_clock_ms();
}

static const mos_watch_ops_t apple_watch_ops = {
    .probe   = watch_probe,
    .mono_ms = watch_mono_ms,
    .wall_ms = watch_wall_ms,
};

/* Per-slot probe for watch-all: same contract as watch_probe, but ctx is
   the slot (its own registry id + identity). Same pointer-lifetime
   invariant — identity AND serial repointed at slot storage before close. */
static mos_error watch_slot_probe(void *ctx, mos_state_result *out)
{
    struct mos_watch_slot *s = (struct mos_watch_slot *)ctx;
    if (!s || !out) return MOS_ERR_INVALID_ARG;

    mos_error err = MOS_OK;
    mos_handle_t *h = mos_internal_open_by_registry_id(s->registry_id, &err);
    if (!h) return err != MOS_OK ? err : MOS_ERR_IO;

    const mos_state_result *qr = NULL;
    mos_error qerr = mos_query_state(h, &qr);
    if (qerr != MOS_OK || !qr) {
        mos_close(h);
        return qerr != MOS_OK ? qerr : MOS_ERR_IO;
    }

    *out = *qr;
    out->vendor   = s->vendor[0]   ? s->vendor   : NULL;
    out->product  = s->product[0]  ? s->product  : NULL;
    out->revision = s->revision[0] ? s->revision : NULL;

    /* Grab the serial once per slot (per session), same contract as
       watch_probe above — piggyback the open handle, BUSY-back-off, re-home
       into slot storage before close. */
    if (!s->serial_grabbed) {
        const char *sn = NULL;
        if (mos_query_serial(h, &sn) == MOS_OK && sn && sn[0]) {
            strlcpy(s->serial, sn, sizeof s->serial);
            s->serial_grabbed = true;
        }
    }
    out->serial = s->serial[0] ? s->serial : NULL;

    mos_close(h);
    return MOS_OK;
}

static const mos_watch_ops_t apple_watch_slot_ops = {
    .probe   = watch_slot_probe,
    .mono_ms = watch_mono_ms,
    .wall_ms = watch_wall_ms,
};

/* Add one device from a DR snapshot. Dedupe by registry_id before touching
   slot storage; the slot is claimed by the same first-free scan add() uses
   (single-thread contract keeps the scans agreeing). */
static void watch_all_add_device(mos_watch_t *w,
                                 const mos_internal_dr_snapshot *snap,
                                 bool mid_stream)
{
    if (!w || !snap || snap->registry_id == 0) return;

    if (mos_internal_watch_all_find(&w->all, snap->registry_id) >= 0) {
        return; /* duplicate Appeared — already streaming in a slot */
    }
    int i = mos_internal_watch_all_free_slot(&w->all);
    if (i < 0) {
        return; /* full and genuinely new — documented drop until a slot frees */
    }
    /* free_slot returns ANY inactive slot, so this one may be RECYCLED from a
       device that was removed (the core freed it: mos_watch_core.c active[best]
       = false on DEVICE_REMOVED). Reset every cached per-device field before
       claiming it. The identity strings below are unconditionally overwritten,
       but the grab-once serial cache (serial / serial_grabbed) is NOT — a
       recycled slot with serial_grabbed still true would skip the re-query and
       emit the PRIOR device's serial (durable-identity corruption). memset
       clears all of it, and stays correct if another cached slot field is
       added later. (Initial-snapshot slots are already zero from the calloc'd
       handle, so this is a no-op there.) */
    memset(&w->slots[i], 0, sizeof w->slots[i]);
    /* Source and destination share the SPC-4 identity widths, so these
       copies can't truncate. */
    _Static_assert(sizeof w->slots[i].vendor   == sizeof snap->vendor,
                   "slot vendor width must match the DR snapshot's");
    _Static_assert(sizeof w->slots[i].product  == sizeof snap->product,
                   "slot product width must match the DR snapshot's");
    _Static_assert(sizeof w->slots[i].revision == sizeof snap->revision,
                   "slot revision width must match the DR snapshot's");
    w->slots[i].registry_id = snap->registry_id;
    strlcpy(w->slots[i].vendor,   snap->vendor,   sizeof w->slots[i].vendor);
    strlcpy(w->slots[i].product,  snap->product,  sizeof w->slots[i].product);
    strlcpy(w->slots[i].revision, snap->revision, sizeof w->slots[i].revision);

    (void)mos_internal_watch_all_add(&w->all, &apple_watch_slot_ops,
                                     &w->slots[i],
                                     snap->bsd_unit, snap->registry_id,
                                     monotonic_ms(),
                                     w->all_stream_open_wall_ms,
                                     w->stable_poll_ms, w->transition_poll_ms,
                                     mid_stream);
}

/* ---- Notification handler ---------------------------------------- *
 *
 * Fires on the run-loop thread for kIOGeneralInterest on the matched
 * io_service_t. Do NOT also subscribe to kIOBusyInterest: a probe changes
 * the drive's busy state, which would fire and schedule another probe — a
 * live loop.
 *
 * Message handling:
 *   1. kIOMessageServiceIsTerminated → TERMINAL (notify_removed).
 *   2. kIOMessageServicePropertyChange → WAKE (notify_wake). Tracks drive,
 *      not client, state, so it does NOT fire on our own probe
 *      open/close — safe to wake on.
 *   3. Everything else IGNORED — including IsAttemptingOpen / WasClosed /
 *      BusyStateChange, which fire on any user-client open/close (our probes
 *      included) and would self-trigger.
 *
 * messageType is natural_t here vs uint32_t in the SDK typedef; both are
 * `unsigned int`, so the function-pointer types stay compatible under
 * -Werror. */
static void watch_interest_callback(void *refcon,
                                    io_service_t service,
                                    natural_t messageType,
                                    void *messageArgument)
{
    (void)service;
    (void)messageArgument;
    mos_watch_t *w = (mos_watch_t *)refcon;
    if (!w) return;

    switch (messageType) {
    case kIOMessageServiceIsTerminated:
        /* TERMINAL: drive went away. */
        mos_internal_watch_notify_removed(&w->core);
        break;

    case kIOMessageServicePropertyChange:
        /* WAKE: a registry property changed (maybe a media-state
           transition). Pull the next poll forward; false wakes are cheap. */
        mos_internal_watch_notify_wake(&w->core);
        break;

    default:
        /* IGNORED (see header): power/sleep messages and the
           IsAttemptingOpen/WasClosed/BusyStateChange family that
           self-trigger on our own probe handles. */
        return;
    }

    /* TERMINAL and WAKE: break the pump's CFRunLoopRunInMode sleep so the
       next mos_watch_next_event returns promptly. */
    if (w->run_loop) {
        CFRunLoopStop(w->run_loop);
    }
}

/* ---- Wake source: DiscRecording doorbell --------------------------- *
 *
 * kDRDeviceStatusChangedNotification fires on a device status-dict change
 * (media in/out, tray, busy), collapsing worst-case insert→event latency
 * from stable_poll_ms to DR's delivery time. Polling is the correctness
 * floor; the doorbell is latency only, so any setup failure falls back to
 * poll-only (dr_center/dr_source NULL; close treats NULL as no-op).
 *
 * DR's notification is DEVICE-scoped, so it also rings for tray-open /
 * no-media drives.
 *
 * The callback filters by registry ID — a parameter, so watch-all widens
 * the filter rather than rewiring the pump. Fail-OPEN: an unresolvable
 * device wakes anyway (a false wake is one cheap probe; a missed wake is
 * stable_poll_ms of latency). DR data never decides state — the wake only
 * schedules the MMC probe.
 */

static void dr_status_changed_callback(DRNotificationCenterRef center,
                                       void *observer, CFStringRef name,
                                       DRTypeRef object,
                                       CFDictionaryRef info)
{
    /* Fires on the run loop the DR source is scheduled on (the caller's). */
    (void)center; (void)name; (void)info;

    mos_watch_t *w = (mos_watch_t *)observer;
    if (!w) return;

    /* Filter by registry ID: resolve the changed DRDeviceRef's path → entry
       ID and compare. Any resolution failure wakes anyway (fail-open). In
       all mode the filter routes instead of rejects: wake the matching
       slot, or every slot when unresolved. */
    uint64_t id = 0;
    if (object) {
        CFDictionaryRef dev_info = DRDeviceCopyInfo((DRDeviceRef)object);
        if (dev_info) {
            id = mos_internal_dr_id_for_path_value(
                CFDictionaryGetValue(dev_info,
                                     kDRDeviceIORegistryEntryPathKey));
            CFRelease(dev_info);
        }
    }

    if (w->all_mode) {
        bool woke = false;
        int slot = (id != 0) ? mos_internal_watch_all_find(&w->all, id) : -1;
        if (slot >= 0) {
            mos_internal_watch_notify_wake(&w->all.cores[slot]);
            woke = true;
        } else if (id == 0) {
            for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
                if (w->all.active[i]) {
                    mos_internal_watch_notify_wake(&w->all.cores[i]);
                    woke = true;
                }
            }
        }
        /* id resolved but unknown: a device we're not watching (cap
           overflow) or one whose Appeared hasn't delivered — the Appeared
           handler owns joins, nothing to wake. Only stop the pump when a
           poll was actually pulled forward. */
        if (woke && w->run_loop) CFRunLoopStop(w->run_loop);
        return;
    }

    if (id != 0 && w->registry_id != 0 && id != w->registry_id) {
        return; /* another drive */
    }

    mos_internal_watch_notify_wake(&w->core);
    /* Break the pump's sleep so the next call re-probes immediately. */
    if (w->run_loop) {
        CFRunLoopStop(w->run_loop);
    }
}

/* Set up the kIOGeneralInterest notification for service termination. Each
   failing step tears down what it created and leaves every field NULL —
   caller falls back to poll-only for this mechanism (the DR doorbell is
   independent). Invariant the pump's run-loop gate depends on: on return,
   w->notify_port is non-NULL iff w->notify_source is too AND a notification
   is registered. */
static void setup_iokit_interest_wake(mos_watch_t *w)
{
    if (!w || !w->run_loop || w->svc == IO_OBJECT_NULL) return;

    w->notify_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!w->notify_port) return;

    w->notify_source = IONotificationPortGetRunLoopSource(w->notify_port);
    if (!w->notify_source) {
        /* No source: tear the port down. A set notify_port without a live
           source would make the pump's gate run CFRunLoopRunInMode in a
           source-less mode, returning instantly and tight-looping to
           timeout. */
        IONotificationPortDestroy(w->notify_port);
        w->notify_port = NULL;
        return;
    }

    CFRunLoopAddSource(w->run_loop, w->notify_source,
                       MOS_WATCH_RUN_LOOP_MODE);

    kern_return_t kr = IOServiceAddInterestNotification(
            w->notify_port, w->svc, kIOGeneralInterest,
            watch_interest_callback, w, &w->notify_token);
    if (kr != KERN_SUCCESS) {
        /* Registration failed: remove the source and tear down the port. */
        CFRunLoopRemoveSource(w->run_loop, w->notify_source,
                              MOS_WATCH_RUN_LOOP_MODE);
        w->notify_source = NULL;
        IONotificationPortDestroy(w->notify_port);
        w->notify_port   = NULL;
        return;
    }

    /* kIOBusyInterest deliberately NOT registered: BusyStateChange fires on
       every user-client open/close (our own probes included), so it would
       self-trigger a tight probe loop. */
}

/* Tear down the IOKit interest notification in reverse order: remove
   source, release token, destroy port. Safe on NULL/poll-only state. */
static void teardown_iokit_interest_wake(mos_watch_t *w)
{
    if (!w) return;

    if (w->run_loop && w->notify_source) {
        CFRunLoopRemoveSource(w->run_loop, w->notify_source,
                              MOS_WATCH_RUN_LOOP_MODE);
        w->notify_source = NULL;
    }
    if (w->notify_token != IO_OBJECT_NULL) {
        IOObjectRelease(w->notify_token);
        w->notify_token = IO_OBJECT_NULL;
    }
    if (w->notify_port) {
        IONotificationPortDestroy(w->notify_port);
        w->notify_port = NULL;
    }
}

/* All-mode lifecycle: Appeared joins a device (its first event is relabeled
   device_appeared by the multiplexer); Disappeared wakes the matching slot
   so its reopen confirms removal. All-mode only — single-target watches use
   kIOGeneralInterest for terminal removal. */
static void dr_device_appeared_callback(DRNotificationCenterRef center,
                                        void *observer, CFStringRef name,
                                        DRTypeRef object,
                                        CFDictionaryRef info)
{
    (void)center; (void)name; (void)info;
    mos_watch_t *w = (mos_watch_t *)observer;
    if (!w || !w->all_mode || !object) return;

    mos_internal_dr_snapshot snap;
    if (mos_internal_dr_device_snapshot((CFTypeRef)object, &snap)) {
        watch_all_add_device(w, &snap, /*mid_stream=*/true);
    } else {
        /* Not resolvable at this Appeared edge (transient). Appeared won't
           re-fire and all-mode has no discovery floor, so arm the one-shot
           reconciliation (struct field doc): the next pump re-copies the
           directory and joins anything now resolvable. */
        w->all_rescan_pending = true;
    }
    if (w->run_loop) CFRunLoopStop(w->run_loop);
}

static void dr_device_disappeared_callback(DRNotificationCenterRef center,
                                           void *observer, CFStringRef name,
                                           DRTypeRef object,
                                           CFDictionaryRef info)
{
    (void)center; (void)name; (void)info;
    mos_watch_t *w = (mos_watch_t *)observer;
    if (!w || !w->all_mode || !object) return;

    uint64_t id = 0;
    CFDictionaryRef dev_info = DRDeviceCopyInfo((DRDeviceRef)object);
    if (dev_info) {
        id = mos_internal_dr_id_for_path_value(
            CFDictionaryGetValue(dev_info, kDRDeviceIORegistryEntryPathKey));
        CFRelease(dev_info);
    }
    int slot = (id != 0) ? mos_internal_watch_all_find(&w->all, id) : -1;
    if (slot >= 0) {
        /* Wake, not removal authority: the woken reopen confirms (NO_DEVICE
           → terminal), and a spurious Disappeared costs one probe instead
           of a permanent eviction (DR never decides state). */
        mos_internal_watch_notify_wake(&w->all.cores[slot]);
    }
    /* Unresolved id: the probe floor catches it — the next reopen returns
       NO_DEVICE. */
    if (w->run_loop) CFRunLoopStop(w->run_loop);
}

/* Set up the DR notification center and register the StatusChanged
   observer. Independent of the IOKit wake — either may fail soft to
   poll-only. Stores center + source on success, both NULL on failure. */
static void setup_dr_doorbell_wake(mos_watch_t *w)
{
    if (!w || !w->run_loop) return;

    DRNotificationCenterRef center = DRNotificationCenterCreate();
    if (!center) return;

    CFRunLoopSourceRef source = DRNotificationCenterCreateRunLoopSource(center);
    if (!source) {
        CFRelease(center);
        return;
    }
    CFRunLoopAddSource(w->run_loop, source, MOS_WATCH_RUN_LOOP_MODE);

    /* Register LAST: once observed, callbacks can fire, so every prior step
       must already be safe. object=NULL observes all devices; the callback
       filters by registry ID (fail-open). All mode adds the
       Appeared/Disappeared observers that make the bus stream live. */
    DRNotificationCenterAddObserver(center, w, dr_status_changed_callback,
                                    kDRDeviceStatusChangedNotification,
                                    NULL);
    if (w->all_mode) {
        DRNotificationCenterAddObserver(center, w,
                                        dr_device_appeared_callback,
                                        kDRDeviceAppearedNotification, NULL);
        DRNotificationCenterAddObserver(center, w,
                                        dr_device_disappeared_callback,
                                        kDRDeviceDisappearedNotification,
                                        NULL);
    }

    w->dr_center = center;
    w->dr_source = source;
}

/* Tear down the DR doorbell in reverse order: remove observer, remove
   source, release both. Safe on NULL/poll-only state. */
static void teardown_dr_doorbell_wake(mos_watch_t *w)
{
    if (!w || !w->dr_center) return;

    DRNotificationCenterRemoveObserver(w->dr_center, w,
                                       kDRDeviceStatusChangedNotification,
                                       NULL);
    if (w->all_mode) {
        DRNotificationCenterRemoveObserver(w->dr_center, w,
                                           kDRDeviceAppearedNotification,
                                           NULL);
        DRNotificationCenterRemoveObserver(w->dr_center, w,
                                           kDRDeviceDisappearedNotification,
                                           NULL);
    }
    if (w->run_loop && w->dr_source) {
        CFRunLoopRemoveSource(w->run_loop, w->dr_source,
                              MOS_WATCH_RUN_LOOP_MODE);
    }
    if (w->dr_source) {
        CFRelease(w->dr_source);
        w->dr_source = NULL;
    }
    CFRelease(w->dr_center);
    w->dr_center = NULL;
}

/* ---- Open / close ------------------------------------------------ */

/* Takes ownership of `h`; builds a watch or closes h on failure. The single
   funnel all public open entry points go through — none re-resolve by name.
   bsd_unit comes from the handle's resolved unit, not a caller string, so
   unit and service identity stay consistent. */
static mos_watch_t *watch_open_from_validated_handle(
        mos_handle_t *h,
        uint32_t stable_poll_ms,
        uint32_t transition_poll_ms,
        mos_error *err_out)
{
    if (!h) {
        if (err_out) *err_out = MOS_ERR_INVALID_ARG;
        return NULL;
    }

    /* An empty drive has no unit (-1). Not a failure — identity is the
       registry_id below, and the device-scoped doorbell covers a nameless
       drive like a named one. */
    const int64_t bsd_unit = mos_handle_bsd_unit(h);   /* -1 if empty */

    mos_watch_t *w = (mos_watch_t *)calloc(1, sizeof(*w));
    if (!w) {
        mos_close(h);
        if (err_out) *err_out = MOS_ERR_OOM;
        return NULL;
    }

    w->bsd_unit = bsd_unit;   /* -1 (empty) carries through */

    /* Capture the validated io_service_t before mos_close — the identity
       the watch preserves (registry_id below comes from it). handle_get_
       service returns it without a retain, so retain here (mos_close drops
       the handle's own) and release in mos_watch_close. On retain failure
       leave w->svc NULL and fall back to poll-only. */
    io_service_t validated_svc = mos_internal_handle_get_service(h);
    if (validated_svc != IO_OBJECT_NULL &&
        IOObjectRetain(validated_svc) == KERN_SUCCESS) {
        w->svc = validated_svc;
    }

    /* Capture the registry-entry ID — the authority watch_probe reopens by.
       Fail closed if absent: a BSD-name fallback would reintroduce the
       two-identity bug this path closes. */
    uint64_t entry_id = 0;
    if (validated_svc == IO_OBJECT_NULL ||
        IORegistryEntryGetRegistryEntryID(validated_svc, &entry_id) != KERN_SUCCESS ||
        entry_id == 0) {
        if (w->svc != IO_OBJECT_NULL) IOObjectRelease(w->svc);
        free(w);
        mos_close(h);
        if (err_out) *err_out = MOS_ERR_IO;
        return NULL;
    }
    w->registry_id = entry_id;

    /* Capture device-static identity ONCE before the handle closes (DR
       directory). Events point at these buffers for the watch's life;
       per-probe handles never contribute identity. strlcpy can't truncate —
       pinned at build time: */
    _Static_assert(sizeof w->vendor   == sizeof h->vendor_str,
                   "watch vendor width must match the handle's");
    _Static_assert(sizeof w->product  == sizeof h->product_str,
                   "watch product width must match the handle's");
    _Static_assert(sizeof w->revision == sizeof h->revision_str,
                   "watch revision width must match the handle's");
    strlcpy(w->vendor,   h->vendor_str,   sizeof w->vendor);
    strlcpy(w->product,  h->product_str,  sizeof w->product);
    strlcpy(w->revision, h->revision_str, sizeof w->revision);

    mos_close(h);

    /* Init the pure state machine BEFORE registering callbacks that mutate
       it: init can't fail and depends on nothing but the clocks, so doing
       it first makes early callback delivery harmless. */
    mos_internal_watch_init(&w->core, &apple_watch_ops, w,
                            w->bsd_unit,
                            /*registry_id=*/w->registry_id,
                            /*start_mono_ms=*/monotonic_ms(),
                            /*start_wall_ms=*/stream_epoch_wall_ms(),
                            stable_poll_ms,
                            transition_poll_ms);

    /* Capture the caller's run loop once for both wake sources (best-effort,
       independent, either can fail to poll-only).

       CFRunLoopGetCurrent() is a BORROWED ref (Get rule), and a thread's run
       loop is freed when the thread exits. If the handle outlives its origin
       thread (embedder behind a dispatch queue / thread pool / actor), the
       unconditional derefs in callbacks, teardown, and the pump gate become
       use-after-free. Retain here / release in mos_watch_close so it lives
       for the handle's life. This makes off-thread misuse a safe no-op — it
       does NOT make cross-thread use functional; the single-thread contract
       stands. */
    w->run_loop = CFRunLoopGetCurrent();
    CFRetain(w->run_loop);

    setup_iokit_interest_wake(w);
    setup_dr_doorbell_wake(w);

    if (err_out) *err_out = MOS_OK;
    return w;
}

mos_watch_t *mos_watch_open_by_bsd_name(const char *bsd_name,
                                        uint32_t stable_poll_ms,
                                        uint32_t transition_poll_ms,
                                        mos_error *err_out)
{
    if (!bsd_name || !*bsd_name) {
        if (err_out) *err_out = MOS_ERR_INVALID_ARG;
        return NULL;
    }
    /* Resolve once via BSD name (this entry point's contract). The
       validated handle's service identity is then preserved by the funnel —
       no second BSD-name resolution. */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_bsd_name(bsd_name, &err);
    if (!h) {
        if (err_out) *err_out = (err != MOS_OK) ? err : MOS_ERR_IO;
        return NULL;
    }
    return watch_open_from_validated_handle(h, stable_poll_ms,
                                            transition_poll_ms, err_out);
}

mos_watch_t *mos_watch_open_by_index(int one_based,
                                     uint32_t stable_poll_ms,
                                     uint32_t transition_poll_ms,
                                     mos_error *err_out)
{
    /* Resolve once via index. mos_open_by_index uses
       IORegistryEntryIDMatching, so the handle is the exact peripheral
       selected at enumeration time; the funnel preserves that identity. */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_index(one_based, &err);
    if (!h) {
        if (err_out) *err_out = (err != MOS_OK) ? err : MOS_ERR_IO;
        return NULL;
    }
    return watch_open_from_validated_handle(h, stable_poll_ms,
                                            transition_poll_ms, err_out);
}

mos_watch_t *mos_watch_open_by_registry_id(uint64_t registry_id,
                                           uint32_t stable_poll_ms,
                                           uint32_t transition_poll_ms,
                                           mos_error *err_out)
{
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_registry_id(registry_id, &err);
    if (!h) {
        if (err_out) *err_out = (err != MOS_OK) ? err : MOS_ERR_IO;
        return NULL;
    }
    return watch_open_from_validated_handle(h, stable_poll_ms,
                                            transition_poll_ms, err_out);
}

mos_watch_t *mos_watch_open_all(uint32_t stable_poll_ms,
                                uint32_t transition_poll_ms,
                                mos_error *err_out)
{
    mos_watch_t *w = (mos_watch_t *)calloc(1, sizeof(*w));
    if (!w) {
        if (err_out) *err_out = MOS_ERR_OOM;
        return NULL;
    }
    w->all_mode           = true;
    w->bsd_unit           = -1;   /* no single unit */
    /* Raw caller values (possibly 0); each per-core watch_init substitutes
       defaults, so these are NOT effective rates. */
    w->stable_poll_ms     = stable_poll_ms;
    w->transition_poll_ms = transition_poll_ms;
    /* One stream, one open time, minted before any slot exists so every
       event carries the same stream_open_ms (contract in the struct). */
    w->all_stream_open_wall_ms = stream_epoch_wall_ms();
    mos_internal_watch_all_init(&w->all);

    /* Observers BEFORE the snapshot: a device arriving in the gap is caught
       by the queued Appeared (callback runs only inside the pump), and one
       landing in both dedupes by registry_id. The reverse order leaves an
       unwatchable window — all-mode discovery has no poll floor. */
    w->run_loop = CFRunLoopGetCurrent();
    CFRetain(w->run_loop);   /* borrowed Get-rule ref; released in close */
    setup_dr_doorbell_wake(w);
    /* No kIOGeneralInterest in all mode: removal rides DR Disappeared
       (wake) + per-probe NO_DEVICE (floor). */

    /* Unlike single mode the doorbell is NOT latency-only: arrivals are
       discovered by the Appeared observer, with no periodic poll floor (the
       pump re-scans the directory ONLY when a failed Appeared snapshot arms
       the bounded reconciliation — never on a timer). A doorbell-less all-watch
       would "succeed" while unable to honor the hot-plug-joins contract,
       undetectably. Fail honestly instead. */
    if (!w->dr_center) {
        mos_watch_close(w);
        if (err_out) *err_out = MOS_ERR_IO;
        return NULL;
    }

    /* Initial population from ONE directory snapshot — no per-device opens
       (the first probe validates each; a vanished one yields device_removed
       normally). Zero devices is a valid empty stream. */
    mos_internal_dr_snapshot snap[MOS_WATCH_ALL_CAP];
    size_t n = mos_internal_dr_copy_snapshot(snap, MOS_WATCH_ALL_CAP);
    for (size_t i = 0; i < n; ++i) {
        watch_all_add_device(w, &snap[i], /*mid_stream=*/false);
    }

    if (err_out) *err_out = MOS_OK;
    return w;
}

/* Safe on NULL; not idempotent (mos_close convention). Order is
   load-bearing — stop callbacks before freeing what they reference: DR
   doorbell, IOKit wake, then the retained io_service_t. */
void mos_watch_close(mos_watch_t *w)
{
    if (!w) return;

    teardown_dr_doorbell_wake(w);
    teardown_iokit_interest_wake(w);

    /* Release the run loop AFTER both teardowns — they call
       CFRunLoopRemoveSource(w->run_loop, …), so it must still be alive.
       NULL only on a watch that never reached the capture sites. */
    if (w->run_loop) {
        CFRelease(w->run_loop);
    }

    if (w->svc != IO_OBJECT_NULL) {
        IOObjectRelease(w->svc);
    }
    free(w);
}

int64_t mos_watch_bsd_unit(const mos_watch_t *w)
{
    /* -1 for NULL, a media-less single-target watch, and always for an
       all-watch (no single unit — demux per event). */
    if (!w) return -1;
    return w->bsd_unit;
}

/* ---- Pump --------------------------------------------------------- */

mos_error mos_watch_next_event(mos_watch_t *w, const mos_watch_event **out,
                               int timeout_ms)
{
    if (out) *out = NULL;
    if (!w || !out) return MOS_ERR_INVALID_ARG;

    /* The pump may spin a few times (pump → SLEEP_UNTIL → wait → pump →
       EMIT) within one call, bounded by timeout_ms. On exhaustion return
       MOS_ERR_TIMEOUT; the caller decides whether to call again. */
    uint64_t start = monotonic_ms();
    uint64_t deadline = (timeout_ms < 0)
        ? UINT64_MAX
        : start + (uint64_t)timeout_ms;

    for (;;) {
        /* Bounded reconciliation: a DR Appeared that couldn't snapshot its
           device armed all_rescan_pending (struct field doc). Drain it BEFORE
           pumping so a re-resolved device is joined and its slot is included in
           this pump's fan-in (its first event is relabeled device_appeared).
           One directory re-copy per armed flag — watch_all_add_device dedupes
           the already-active devices, so this only ADDS the missed one(s). */
        if (w->all_mode && w->all_rescan_pending) {
            w->all_rescan_pending = false;
            mos_internal_dr_snapshot snap[MOS_WATCH_ALL_CAP];
            size_t rn = mos_internal_dr_copy_snapshot(snap, MOS_WATCH_ALL_CAP);
            for (size_t i = 0; i < rn; ++i)
                watch_all_add_device(w, &snap[i], /*mid_stream=*/true);
        }

        mos_watch_decision d = w->all_mode
            ? mos_internal_watch_all_pump(&w->all)
            : mos_internal_watch_pump(&w->core);

        if (d.kind == MOS_WATCH_DECIDE_EMIT_EVENT) {
            w->last_event = d.event;
            *out = &w->last_event;
            return MOS_OK;
        }

        if (d.kind == MOS_WATCH_DECIDE_TERMINAL) {
            return MOS_ERR_NO_DEVICE;
        }

        /* SLEEP_UNTIL: block until next_poll_at_mono_ms or a notification
           fires (CFRunLoopStop). The deadline is a monotonic value,
           compared like the pump does. */
        uint64_t now = monotonic_ms();
        if (now >= deadline) {
            return MOS_ERR_TIMEOUT;   /* caller timeout; pump again next call */
        }

        uint64_t sleep_until_ms = d.next_poll_at_mono_ms;
        if (sleep_until_ms > deadline) sleep_until_ms = deadline;
        if (sleep_until_ms < now) sleep_until_ms = now;
        double interval_sec = (double)(sleep_until_ms - now) / 1000.0;

        /* Wait on the run loop only if a source is scheduled (an empty mode
           returns instantly and tight-loops) AND we're on the owning thread.
           Off the owning thread, CFRunLoopRunInMode would run a source-less
           mode on the wrong loop and burn CPU to timeout; the thread check
           degrades that misuse to honest nanosleep instead of a busy-spin
           (it does not make cross-thread wakes work). */
        if (w->run_loop && (w->notify_source || w->dr_source) &&
            CFRunLoopGetCurrent() == w->run_loop) {
            /* Returns on timer, CFRunLoopStop, or a handled source — any is
               a wake; loop back to pump. Private mode keeps us off the
               host's default-mode sources. */
            CFRunLoopRunInMode(MOS_WATCH_RUN_LOOP_MODE, interval_sec, false);
        } else {
            /* No source scheduled: nanosleep. Taken in poll-only mode (no
               validated io_service_t) or when both registration paths
               failed at open. */
            struct timespec ts;
            ts.tv_sec  = (time_t)(sleep_until_ms - now) / 1000;
            ts.tv_nsec = (long)((sleep_until_ms - now) % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
        /* Loop and re-pump. */
    }
}

/* ==== src/mos_watch_core.c ==== */
/*
 * mos_watch_core.c — pure watch state machine.
 *
 * Drives the poll loop:
 *
 *   - When to probe (next_poll_at_mono_ms, two backoff rates by whether the
 *     last state was transitional or stable).
 *   - What to emit (snapshot on first probe; state_changed on a state
 *     delta; media_changed on a same-state READY disc swap; error on
 *     transient probe failure; device_removed on notify_removed or a probe
 *     returning MOS_ERR_NO_DEVICE).
 *   - How to label it (monotonic seq; RFC 3339 ts from ops->wall_ms;
 *     registry_id + stream_open_wall_ms; bsd_unit).
 *   - When to stop (terminated flag, set by notify_removed or probe →
 *     NO_DEVICE).
 *
 * Two time domains, distinct at the type level so a mixup can't slip in:
 *   - ops->mono_ms() — MONOTONIC. Scheduling/latency only.
 *   - ops->wall_ms() — WALL-CLOCK ms since epoch. stream_open_wall_ms and
 *     event ts only; can jump backward, never used for scheduling.
 *
 * The caller's pump owns blocking — this core never sleeps. It returns a
 * decision: EMIT_EVENT (write and re-pump), SLEEP_UNTIL (block then
 * re-pump), TERMINAL (close). mos_watch.c maps SLEEP_UNTIL to
 * CFRunLoopRunInMode; the test driver advances a fake clock. Every
 * transition is testable without IOKit.
 */

/* Mirrors mos_watch.c: no BSD-only helper here yet, but the define keeps
   the amalgamated feature-macro environment consistent. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

/* gmtime_r needs POSIX.1-2008 on glibc (Apple exposes it unconditionally).
   Define before any header. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif


#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- Defaults ----------------------------------------------------- */

/* Two backoff rates. "stable" (open/empty/ready) isn't a transition window
   — changes arrive via OS notifications. "transition" (loading/busy/
   unknown) polls fast to catch the resolution. Defaults (2s / 200ms) are
   conservative: stable still notices an insert within a couple seconds
   without notifications. Both configurable per watch. */
#define MOS_WATCH_DEFAULT_STABLE_MS     2000U
#define MOS_WATCH_DEFAULT_TRANSITION_MS  200U

/* ---- Time formatting --------------------------------------------- */

/* Format wall-clock ms-since-epoch as RFC 3339 UTC (YYYY-MM-DDTHH:MM:SSZ),
   21 bytes incl NUL (cap must be >= 21). Integer seconds; no sub-second
   precision. Feeding monotonic ms produces nonsense like
   1970-01-01T00:00:12Z.

   SATURATING: the clock is hostile INPUT to this pure layer, so an insane
   host clock, NTP step, or fuzzed ops table must not emit a schema-invalid
   line (the schema requires a 4-digit year). Values past
   9999-12-31T23:59:59Z clamp to that instant; post-clamp gmtime_r/strftime
   cannot fail, and the fallback writes the clamp constant anyway, so the
   contract holds unconditionally. */
#define MOS_TS_MAX_SECS 253402300799ULL   /* 9999-12-31T23:59:59Z */
static void format_rfc3339(uint64_t wall_ms, char *out, size_t cap)
{
    if (cap < 21) {
        if (cap > 0) out[0] = 0;
        return;
    }
    uint64_t s64 = wall_ms / 1000U;
    if (s64 > MOS_TS_MAX_SECS) s64 = MOS_TS_MAX_SECS;
    time_t secs = (time_t)s64;
    struct tm tm;
    /* gmtime_r: POSIX.1-2008, present on every platform we build/test on.
       No _WIN32/gmtime_s branch — Windows isn't a target. */
    if (gmtime_r(&secs, &tm) != NULL) {
        /* The numeric strftime specifiers used here are POSIX locale-
           independent. strftime also sidesteps a -Wformat-truncation false
           positive a hand-rolled snprintf hits under -O2 (GCC sees tm_year
           as unbounded). */
        if (strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 20) {
            return;
        }
    }
    /* Unreachable post-clamp on a conforming libc; holds the contract if
       one misbehaves. */
    memcpy(out, "9999-12-31T23:59:59Z", 21);
}
/* ---- Public-via-mos_pure.h init / pump --------------------------- */

void mos_internal_watch_init(mos_watch_state *w,
                             const mos_watch_ops_t *ops, void *ctx,
                             int64_t bsd_unit,
                             uint64_t registry_id,
                             uint64_t start_mono_ms,
                             uint64_t start_wall_ms,
                             uint32_t stable_poll_ms,
                             uint32_t transition_poll_ms)
{
    memset(w, 0, sizeof(*w));
    w->ops                    = ops;
    w->ctx                    = ctx;
    w->stable_poll_ms         = stable_poll_ms     ? stable_poll_ms
                                                   : MOS_WATCH_DEFAULT_STABLE_MS;
    w->transition_poll_ms     = transition_poll_ms ? transition_poll_ms
                                                   : MOS_WATCH_DEFAULT_TRANSITION_MS;
    w->last_state             = MOS_STATE_UNKNOWN;
    w->have_last_state        = false;
    w->last_media_id          = 0;
    w->last_profile           = 0;
    w->next_seq               = 1;
    /* First probe at start_mono_ms (immediately). Monotonic value. */
    w->next_poll_at_mono_ms   = start_mono_ms;
    w->terminated             = false;
    w->removed_event_emitted  = false;
    w->bsd_unit               = bsd_unit;   /* -1 == no media */

    /* Session identity: two plain values. registry_id is the attachment
       identity (xnu mints non-reused IDs >= 2^32+256; 0 only from direct
       pure-layer callers). start_wall_ms rides every event as
       stream_open_wall_ms; the adapter monotonicizes it per process so
       (registry_id, stream_open_wall_ms) is unique per session even for
       same-ms reopens of the same drive. */
    w->registry_id         = registry_id;
    w->stream_open_wall_ms = start_wall_ms;
}

void mos_internal_watch_notify_removed(mos_watch_state *w)
{
    if (!w) return;
    w->terminated = true;
}

void mos_internal_watch_notify_wake(mos_watch_state *w)
{
    if (!w) return;
    /* Pull the next poll to "now" without reading the clock here: setting
       0 guarantees now >= next_poll_at_mono_ms on the next pump, whatever
       the monotonic clock's origin. */
    w->next_poll_at_mono_ms = 0;
}

/* Transition vs stable, for backoff selection. */
static bool watch_state_is_transitional(mos_state s)
{
    switch (s) {
        /* In-progress or degraded — poll fast to converge. */
        case MOS_STATE_LOADING:
        case MOS_STATE_BUSY:
        case MOS_STATE_FORMATTING:      /* long op, still progressing to ready/empty */
        case MOS_STATE_EMPTY_OR_OPEN:   /* degraded: GESN was unavailable; re-probe to resolve */
        case MOS_STATE_UNKNOWN:
            return true;
        /* Settled physical states and stable error conditions. */
        case MOS_STATE_OPEN:
        case MOS_STATE_EMPTY:
        case MOS_STATE_READY:
        case MOS_STATE_MEDIA_UNREADABLE:  /* bad disc; not self-resolving */
        case MOS_STATE_DEVICE_FAULT:      /* drive fault; not self-resolving */
            return false;
    }
    /* No default: a new mos_state value must trip -Wswitch so its poll
       class is chosen deliberately. This trailing return only covers an
       out-of-range value (the enum is int32-wide). */
    return false;
}

/* Build a base event: session identity, seq, ts (read fresh from
   ops->wall_ms, not the scheduling clock), bsd_unit. Caller fills the
   kind-specific fields. */
static void fill_event_base(mos_watch_state *w, mos_watch_event *e)
{
    memset(e, 0, sizeof(*e));
    e->seq                 = w->next_seq++;
    e->registry_id         = w->registry_id;
    e->stream_open_wall_ms = w->stream_open_wall_ms;
    e->bsd_unit            = w->bsd_unit; /* -1 == no media; copied verbatim */

    uint64_t wall = w->ops->wall_ms ? w->ops->wall_ms(w->ctx) : 0;
    format_rfc3339(wall, e->ts, sizeof(e->ts));

    e->error     = MOS_OK;
}

/* Copy probe-result fields (all but prev_state) into an event. Shared by
   snapshot/state_changed/media_changed so a new field has one assignment
   site, not three. */
static void fill_event_state_fields(mos_watch_event *e,
                                    const mos_state_result *r,
                                    uint32_t latency_ms)
{
    e->state           = r->state;
    /* BSD unit from THIS probe, not the open-time value, so a disc in a
       drive empty at open (or a unit changed across eject/reinsert) shows
       in the event. fill_event_base seeds w->bsd_unit for events with no
       fresh probe (error / device_removed). */
    e->bsd_unit        = r->bsd_unit;
    e->current_profile = r->current_profile;
    e->vendor          = r->vendor;
    e->product         = r->product;
    e->revision        = r->revision;
    e->serial          = r->serial;   /* NULL until a free poll grabs it (mos_watch.c) */
    e->media_type      = r->media_type;  /* static token storage or NULL — no re-home */
    e->writable        = r->writable;    /* tri-state -1/0/1, plain scalar */
    e->sense_key       = r->sense_key;
    e->asc             = r->asc;
    e->ascq            = r->ascq;
    e->latency_ms      = latency_ms;
}

/* Saturating monotonic delta in ms: clamps end < start to 0 and a >49.7-day
   span to UINT32_MAX, either of which would otherwise underflow/truncate. */
static uint32_t mos_watch_latency_ms(uint64_t start, uint64_t end)
{
    uint64_t delta = end >= start ? end - start : 0;
    return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}

/* Per-state poll interval: transition_poll_ms for transitional states,
   stable_poll_ms otherwise. One site for the policy. */
static uint32_t poll_ms_for_state(const mos_watch_state *w,
                                  mos_state state)
{
    return watch_state_is_transitional(state)
        ? w->transition_poll_ms
        : w->stable_poll_ms;
}

mos_watch_decision mos_internal_watch_pump(mos_watch_state *w)
{
    mos_watch_decision d;
    memset(&d, 0, sizeof(d));

    if (!w || !w->ops) {
        d.kind = MOS_WATCH_DECIDE_TERMINAL;
        return d;
    }

    /* Terminal (notify_removed or a probe → NO_DEVICE): emit one final
       device_removed, then refuse further pumps. The removed_event_emitted
       sentinel guarantees exactly once, even if termination precedes any
       observation (prev_state then unknown). */
    if (w->terminated) {
        if (!w->removed_event_emitted) {
            fill_event_base(w, &d.event);
            d.event.kind       = MOS_EVENT_DEVICE_REMOVED;
            d.event.state      = MOS_STATE_UNKNOWN;
            d.event.prev_state = w->have_last_state ? w->last_state
                                                    : MOS_STATE_UNKNOWN;
            w->removed_event_emitted = true;
            d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
            return d;
        }
        d.kind = MOS_WATCH_DECIDE_TERMINAL;
        return d;
    }

    if (!w->ops->probe || !w->ops->mono_ms) {
        d.kind = MOS_WATCH_DECIDE_TERMINAL;
        return d;
    }

    uint64_t now_mono = w->ops->mono_ms(w->ctx);

    /* Not yet time to probe → sleep. */
    if (now_mono < w->next_poll_at_mono_ms) {
        d.kind                 = MOS_WATCH_DECIDE_SLEEP_UNTIL;
        d.next_poll_at_mono_ms = w->next_poll_at_mono_ms;
        return d;
    }

    mos_state_result r;
    memset(&r, 0, sizeof(r));
    uint64_t  probe_start_mono = now_mono;
    mos_error perr             = w->ops->probe(w->ctx, &r);
    uint64_t  probe_end_mono   = w->ops->mono_ms(w->ctx);

    /* NO_DEVICE means the drive went away under the watch: terminal
       removal. Set the flag and let the terminated path above emit
       device_removed. Without this, poll-only mode (no notifications) would
       spin emitting error events forever for an unplugged drive. */
    if (perr == MOS_ERR_NO_DEVICE) {
        w->terminated = true;
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_DEVICE_REMOVED;
        d.event.state      = MOS_STATE_UNKNOWN;
        d.event.prev_state = w->have_last_state ? w->last_state
                                                : MOS_STATE_UNKNOWN;
        d.event.latency_ms = mos_watch_latency_ms(probe_start_mono, probe_end_mono);
        w->removed_event_emitted = true;
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    if (perr != MOS_OK) {
        /* Other error: no observation this round. Non-terminal — emit error
           and reschedule. */
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_ERROR;
        d.event.error      = perr;
        d.event.state      = MOS_STATE_UNKNOWN;
        d.event.prev_state = w->have_last_state ? w->last_state : MOS_STATE_UNKNOWN;
        d.event.latency_ms = mos_watch_latency_ms(probe_start_mono, probe_end_mono);

        /* Don't update last_state — there's no observation, just its
           absence.

           Retry cadence: a first (or changed) error retries at transition
           rate; each further identical error doubles the interval, capped
           at stable_poll_ms. A persistent failure converges to the stable
           cadence instead of flooding; a notify_wake still pulls the next
           poll forward on a real event. */
        if (perr == (mos_error)w->last_probe_err && w->consec_probe_errs > 0) {
            if (w->consec_probe_errs < UINT32_MAX) w->consec_probe_errs++;
        } else {
            w->last_probe_err    = (int32_t)perr;
            w->consec_probe_errs = 1;
        }
        {
            uint64_t interval = w->transition_poll_ms;
            uint32_t doublings = w->consec_probe_errs - 1u;
            while (doublings-- > 0u && interval < w->stable_poll_ms) {
                interval <<= 1;
            }
            if (interval > w->stable_poll_ms) interval = w->stable_poll_ms;
            w->next_poll_at_mono_ms = probe_end_mono + interval;
        }
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    /* Successful observation: any error streak is over. */
    w->last_probe_err    = (int32_t)MOS_OK;
    w->consec_probe_errs = 0;
    /* Adopt the probe's unit. Fresh-probe events carry r.bsd_unit directly,
       but the error/device_removed fallback reads w->bsd_unit, which must
       track the last OBSERVED unit (not open-time). The core owns this so
       pure-only behavior is correct without each adapter rediscovering it. */
    w->bsd_unit = r.bsd_unit;


    /* First successful probe → snapshot. */
    if (!w->have_last_state) {
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_SNAPSHOT;
        d.event.prev_state = MOS_STATE_UNKNOWN;
        fill_event_state_fields(&d.event, &r,
            mos_watch_latency_ms(probe_start_mono, probe_end_mono));

        w->last_state           = r.state;
        w->have_last_state      = true;
        w->last_media_id        = r.media_id;
        w->last_profile         = r.current_profile;
        w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    /* State change → emit delta. */
    if (r.state != w->last_state) {
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_STATE_CHANGED;
        d.event.prev_state = w->last_state;
        fill_event_state_fields(&d.event, &r,
            mos_watch_latency_ms(probe_start_mono, probe_end_mono));

        w->last_state           = r.state;
        w->last_media_id        = r.media_id;
        w->last_profile         = r.current_profile;
        w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    /* Same-state media swap → media_changed while the drive stays READY
       across two probes (a fast slot-load swap, or eject/reinsert between
       polls). Two independent signals:

       1. Registry-identity change — the whole-disk IOMedia entry ID re-mints
          on a physical swap even when the profile is unchanged (the
          same-profile DVD→DVD case). Strong signal; both ids must be
          non-zero (0 = unavailable, never inferred from).

       2. Profile change with NO usable identity — some USB-ATAPI bridges
          never expose a media_id (both 0). Any non-zero profile change
          fires: a different profile with no identity means a different disc.
          A same-PROFILE swap (DVD-R → DVD-R) is invisible here. */
    bool id_changed =
        r.media_id != 0 && w->last_media_id != 0 &&
        r.media_id != w->last_media_id;
    bool profile_changed_without_id =
        r.media_id == 0 && w->last_media_id == 0 &&
        r.current_profile != 0 && w->last_profile != 0 &&
        r.current_profile != w->last_profile;

    if (r.state == MOS_STATE_READY && w->last_state == MOS_STATE_READY &&
        (id_changed || profile_changed_without_id)) {
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_MEDIA_CHANGED;
        d.event.prev_state = w->last_state;   /* READY → READY */
        fill_event_state_fields(&d.event, &r,
            mos_watch_latency_ms(probe_start_mono, probe_end_mono));

        w->last_media_id        = r.media_id;
        w->last_profile         = r.current_profile;
        w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
        d.kind = MOS_WATCH_DECIDE_EMIT_EVENT;
        return d;
    }

    /* No change → reschedule and sleep, no event.

       Adopt any *informative* identity first. media_id / current_profile
       unavailable (0) at the last event often arrive a probe later (the
       IOMedia child registers a beat after TUR goes GOOD; profile
       enrichment can fail transiently). A non-zero value is adopted, a zero
       never overwrites one, so the fingerprint survives the gap and a swap
       across it is still detected. Without this, a 0→non-zero arrival would
       pin the snapshot fingerprint and disarm swap detection for the
       session. */
    if (r.media_id != 0)        w->last_media_id = r.media_id;
    if (r.current_profile != 0) w->last_profile  = r.current_profile;
    w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
    d.kind                 = MOS_WATCH_DECIDE_SLEEP_UNTIL;
    d.next_poll_at_mono_ms = w->next_poll_at_mono_ms;
    return d;
}

/* ---- Watch-all multiplexer ----------------------------------------- *
 *
 * Contract in mos_pure.h. Slots are visited in ascending registry_id on
 * every entry, so same-tick interleave is deterministic and independent of
 * slot-assignment history. */

void mos_internal_watch_all_init(mos_watch_all_state *a)
{
    if (!a) return;
    memset(a, 0, sizeof *a);
}

int mos_internal_watch_all_free_slot(const mos_watch_all_state *a)
{
    if (!a) return -1;
    for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
        if (!a->active[i]) return i;
    }
    return -1;
}

int mos_internal_watch_all_find(const mos_watch_all_state *a,
                                uint64_t registry_id)
{
    if (!a || registry_id == 0) return -1;
    for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
        if (a->active[i] && a->cores[i].registry_id == registry_id) return i;
    }
    return -1;
}

int mos_internal_watch_all_add(mos_watch_all_state *a,
                               const mos_watch_ops_t *ops, void *ctx,
                               int64_t bsd_unit, uint64_t registry_id,
                               uint64_t start_mono_ms,
                               uint64_t start_wall_ms,
                               uint32_t stable_poll_ms,
                               uint32_t transition_poll_ms,
                               bool mid_stream)
{
    if (!a || registry_id == 0) return -1;

    /* Dedupe by attachment identity: DR Appeared can re-announce a device
       the snapshot already had (or fire twice on a bus rescan). Same id ⇒
       same slot; a replug gets a fresh id and a new slot. */
    int existing = mos_internal_watch_all_find(a, registry_id);
    if (existing >= 0) return existing;

    int i = mos_internal_watch_all_free_slot(a);
    if (i < 0) return -1;

    mos_internal_watch_init(&a->cores[i], ops, ctx, bsd_unit, registry_id,
                            start_mono_ms, start_wall_ms,
                            stable_poll_ms, transition_poll_ms);
    a->active[i]       = true;
    a->join_pending[i] = mid_stream;
    return i;
}

mos_watch_decision mos_internal_watch_all_pump(mos_watch_all_state *a)
{
    mos_watch_decision out;
    memset(&out, 0, sizeof out);
    out.kind                 = MOS_WATCH_DECIDE_SLEEP_UNTIL;
    out.next_poll_at_mono_ms = UINT64_MAX;
    if (!a) return out;

    /* Visit active slots in ascending registry_id (selection scan; the CAP is
       small, a sort would be ceremony). Returning on the first EMIT bounds per-call
       work; the next call re-enters at the lowest id, draining same-tick
       events in id order. */
    _Static_assert(MOS_WATCH_ALL_CAP <= 64, "visited bitmask is 64-wide");
    uint64_t visited = 0; /* bitmask of slots already pumped this call */
    for (;;) {
        int best = -1;
        for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
            if (!a->active[i] || (visited & (1ull << i))) continue;
            if (best < 0 ||
                a->cores[i].registry_id < a->cores[best].registry_id) {
                best = i;
            }
        }
        if (best < 0) break;
        visited |= (1ull << best);

        mos_watch_decision d = mos_internal_watch_pump(&a->cores[best]);

        if (d.kind == MOS_WATCH_DECIDE_EMIT_EVENT) {
            d.event.seq = ++a->seq;            /* stream-global numbering */
            /* Relabel only the join's SNAPSHOT. A mid-stream device whose
               first pumps yield ERROR keeps its pending join, so its first
               successful probe still announces device_appeared; clearing on
               any first event would demote it (contract: every joining
               drive emits device_appeared). */
            if (a->join_pending[best] &&
                d.event.kind == MOS_EVENT_SNAPSHOT) {
                d.event.kind = MOS_EVENT_DEVICE_APPEARED;
                a->join_pending[best] = false;
            }
            if (d.event.kind == MOS_EVENT_DEVICE_REMOVED) {
                /* Per-slot, not stream-terminal: free after taking the
                   event. A replug arrives as a new id. */
                a->active[best] = false;
            }
            return d;
        }
        if (d.kind == MOS_WATCH_DECIDE_TERMINAL) {
            /* device_removed already emitted on an earlier call; this slot
               pumped again (external notify after emission). Just free it. */
            a->active[best] = false;
            continue;
        }
        /* SLEEP_UNTIL: fold the earliest deadline. */
        if (d.next_poll_at_mono_ms < out.next_poll_at_mono_ms) {
            out.next_poll_at_mono_ms = d.next_poll_at_mono_ms;
        }
    }
    return out;
}

