/*
 * mos_pure.h — internal prototypes for pure-data functions.
 *
 * No IOKit / CoreFoundation includes. Safe to include from:
 *   - Runtime code (via mos_internal.h which re-includes this)
 *   - Pure-data tests (tests/test_sense.c, tests/test_bsd_name.c, etc.)
 *
 * The split exists because mos_internal.h has to drag in IOKit for the
 * mos_handle / mos_device_info struct definitions, but the sense parser
 * and state mapper are pure data transformations that should be testable
 * without linking IOKit into the test binary.
 */

#ifndef MOS_PURE_H
#define MOS_PURE_H

#include <stdint.h>
#include <stdbool.h>

#include "mos.h"  /* for mos_state */

/* ---- Returned-object layouts (opaque in the public header) --------- *
 *
 * The public mos.h exposes mos_state_result and mos_watch_event only as
 * opaque typedefs plus accessor prototypes; the full layout lives here so
 * the pure core, the Apple fill paths, and the pure tests can read and
 * write fields directly. "Grow in place" means appending a field below —
 * because external callers only ever see the accessors, that is ABI-safe
 * with no size/version negotiation. Keep additions at the end. */
struct mos_state_result {
    mos_state state;
    int64_t        bsd_unit;      /* whole-disk unit; -1 = no whole-disk IOMedia node (media absent) */
    uint64_t       registry_id;   /* DRIVE service registry entry ID (the
                                     attachment identity; same value the
                                     watch stream emits). 0 == unavailable.
                                     Adapter-populated at open. */
    uint64_t       media_id;      /* whole-disk IOMedia registry entry ID;
                                     0 == no media / unavailable. Internal
                                     only: compared for equality to detect a
                                     same-state media swap, never emitted. */
    const char    *vendor;        /* INQUIRY, NULL if absent */
    const char    *product;       /* INQUIRY, NULL if absent */
    const char    *revision;      /* INQUIRY firmware revision, NULL if absent */
    uint16_t       current_profile;
    uint8_t        sense_key;
    uint8_t        asc;
    uint8_t        ascq;
};

struct mos_watch_event {
    mos_event_kind kind;
    uint64_t       seq;
    char           ts[24];        /* RFC 3339 UTC, NUL-terminated */
    /* Session identity, as two plain values: registry_id is the watch
       target's attachment identity (on the Apple adapter, the IORegistry
       entry ID — xnu guarantees real IDs >= 2^32+256);
       stream_open_wall_ms is the per-process-monotonicized wall epoch
       captured at watch open. The pair is unique per session; both are
       constant for the stream's life. JSON carries them as separate
       fields; consumers needing a single correlation key derive one. */
    uint64_t       registry_id;
    uint64_t       stream_open_wall_ms;
    int64_t        bsd_unit;
    const char    *vendor;
    const char    *product;
    const char    *revision;
    mos_state state;
    mos_state prev_state;
    uint16_t       current_profile;
    uint8_t        sense_key;
    uint8_t        asc;
    uint8_t        ascq;
    mos_error      error;
    uint32_t       latency_ms;
};

/* ---- Fixed-buffer capacities -------------------------------------- *
 *
 * Sizes for the transient BSD-name strings that appear only at I/O
 * boundaries — reading kIOBSDNameKey in the adapter, argv in the
 * probes. Drive identity itself is an int64 unit
 * (mos_*_bsd_unit), not a string; these caps bound only the short-lived
 * names that get parsed to a unit (or formatted from one) and discarded.
 *
 * MOS_BSD_NAME_CAP: a whole-disk name "diskN" — longest is "disk" + a
 *   32-bit unit ("disk4294967295", 14 chars) + NUL, so 32 is ample. */
#define MOS_BSD_NAME_CAP  32

/* ---- Sense parser (mos_sense.c) ------------------------------------ *
 *
 * Parse 18-byte fixed-format SCSI sense data into (sense_key, ASC, ASCQ).
 * Each output pointer is independently optional. */
void mos_internal_parse_sense(const uint8_t sense[18],
                              uint8_t *sk, uint8_t *asc, uint8_t *ascq);

/* Refine a tray-CLOSED, not-ready drive into its reason, from the sense
   triple. Caller has already resolved open/closed (GESN door bit, or the
   sense fork). Never returns OPEN/EMPTY_OR_OPEN — it does not decide the
   tray. UNKNOWN when the sense carries no meaning we assert. See
   mos_sense.c and ARCHITECTURE.md §5. */
mos_state mos_internal_state_from_sense_closed(uint8_t sk, uint8_t asc, uint8_t ascq);

/* ---- BSD name normalization (mos_pure.c) --------------------------- *
 *
 * Normalize any of the four accepted BSD-name forms ("disk4", "rdisk4",
 * "/dev/disk4", "/dev/rdisk4") to the IOKit canonical form ("disk4").
 * Returns a pointer into the input string. Extracted so tests can
 * verify the real rule, not a hand-copy. */
const char *mos_internal_normalize_bsd_name(const char *in);

/* True if the BSD name looks like a whole-disk entry (diskN or rdiskN)
   rather than a partition (diskNsM). */
bool mos_internal_bsd_name_is_whole_shape(const char *bsd_name);

/* Parse any accepted whole-disk name form ("disk4", "rdisk4",
   "/dev/disk4", "/dev/rdisk4") to its unit number (the N), as an
   int64_t in [0, UINT32_MAX]. Returns -1 for NULL/empty, a non-whole
   shape (partition "diskNsM", garbage), or a unit that overflows 32
   bits. The one place a BSD-name string becomes the stored integer
   identity. Pinned by tests/test_bsd_name.c. */
int64_t mos_internal_parse_bsd_unit(const char *name);

/* True if `reported` (a raw IOKit-reported BSD name, e.g. "disk4" or
   "disk4s1") names whole-disk unit `whole_unit` itself or one of its
   partition children. The unit is compared numerically (disk40 vs unit 4
   is 40 != 4) and the suffix validated as `(s<digits>)*`; false for NULL,
   whole_unit < 0, a non-"disk" prefix, or a malformed suffix. Pinned by
   tests/test_bsd_name.c. No in-tree consumers since the DA retirement
   (2026-06-11): its call sites were the DiskArbitration event filters
   (the watch's, retired in DR pivot Phase 2a, then the probe's, retired
   with the probe consolidation). Kept as the pinned partition-child
   matching rule for any future BSD-name event filtering. */
bool mos_internal_bsd_unit_matches(const char *reported, int64_t whole_unit);

/* xnu mints IORegistry entry IDs from a never-reused monotone counter
   starting at 2^32+256; CLI indexes are 1..MOS_CLI_LIST_CAP. The two
   all-digit selector spaces are disjoint by kernel construction, so a
   parsed value classifies deterministically. Pinned by
   tests/test_bsd_name.c. */
#define MOS_REGISTRY_ID_FLOOR ((1ULL << 32) + 256)
bool mos_internal_value_is_registry_id(uint64_t v);

/* ---- GET CONFIGURATION feature walk (mos_config.c) --------------- *
 *
 * One decoded MMC feature descriptor. `data` borrows into the caller's
 * response buffer (valid only while that buffer is) and `data_len` is the
 * descriptor's Additional Length, already proven to fit the buffer. */
typedef struct {
    uint16_t       feature_code;
    bool           current;     /* byte 2, bit 0 */
    bool           persistent;  /* byte 2, bit 1 */
    uint8_t        version;     /* byte 2, bits 2..5 */
    const uint8_t *data;        /* feature payload, borrowed; NULL if none */
    uint8_t        data_len;    /* Additional Length, clamped to the buffer */
} mos_config_feature;

/* Bounds-safe pull-iterator over a GET CONFIGURATION response. `buf`/`len`
 * are the response and the byte count you trust (sizeof your zero-init
 * buffer; the MMC convenience GetConfiguration reports no realized count).
 * Initialize *cursor = 8 to skip the feature header. Returns true and fills
 * *out for each in-bounds descriptor (advancing *cursor by >= 4); false at
 * end-of-data or on the first descriptor that would read past the trusted
 * region. Device-reported lengths can only shorten the walk — no call ever
 * reads outside [buf, buf+len). Pure, so it is ASan/fuzz-checked headless. */
/* READ TOC/PMA/ATIP format 0000b response — the normalized table of
 * contents, THE disc-identity primitive (MusicBrainz/CDDB ids are pure
 * functions of exactly these fields), read unprivileged via the
 * ReadTableOfContents convenience method. FAIL-CLOSED: an out-of-range,
 * duplicate, or non-ascending track rejects the whole TOC — identity
 * from a half-parsed hostile TOC would be a falsely-stable fingerprint.
 * The header's declared range is held to the same standard: first/last
 * must be coherent (1..99, not inverted) and the descriptor list must
 * cover exactly first..last — a claimed span that truncates the table
 * mid-range is the half-parsed case again, not padding.
 * A TOC without a lead-out parses (have_leadout=false); identity
 * consumers must require it. Byte layout at the decoder (mos_pure.c). */
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


/* THE DUAL-LENGTH RULE (seam contract O-4; AGENTS scope doctrine layer 3).
 *
 * Every variable-size transfer has three lengths from three authorities:
 *   allocated   — bytes the CALLER allocated / requested (ours);
 *   transferred — bytes the TRANSPORT reports it actually delivered
 *                 (the kernel's realizedByteCount; ours-adjacent);
 *   claimed     — bytes the DEVICE's own header says exist (hostile).
 *
 * The trusted parse region is min(allocated, transferred), computed ONCE
 * at the seam; the device claim is DATA that may only shrink that bound,
 * never set or grow it. This is the generalization of the sense buffer's
 * fixed-18 rule to drive-sized replies, and it forecloses the classic
 * SCSI allocation-length overread (header claims 0xFFFF over an 8-byte
 * transfer) by construction. `claimed` is uint64_t so callers compute
 * header-derived totals (e.g. GET CONFIGURATION's `Data Length + 4`) in
 * a width that cannot wrap before the clamp. Pure, total, no failure
 * mode: pathological inputs simply yield a smaller (possibly zero)
 * trusted length. v0.4 RT=0 enrichment MUST derive its parse bound from
 * this function — see doc/seam-contract.md O-4. */
size_t mos_internal_trusted_len(size_t allocated, size_t transferred,
                                uint64_t claimed);

bool mos_internal_config_next_feature(const uint8_t *buf, size_t len,
                                      size_t *cursor, mos_config_feature *out);

/* First feature matching `feature_code`, via the walker (same trust
   bounds). False when absent or the walk fails closed. The v0.4 drive
   verb reads AACS (0x010D) presence/version through this. */
bool mos_internal_config_find_feature(const uint8_t *buf, size_t len,
                                      uint16_t feature_code,
                                      mos_config_feature *out);

/* AACS capability facts from a full (RT=0) GET CONFIGURATION response —
   the spec-grounded subset of what MakeMKV's drive dump shows, WITHOUT
   the LibreDrive synthesis (design doc 2026-06-10 + 06-12 addendum).
   bus_encryption is the DRIVE-REPORTED support bit (feature 0x010D
   payload byte 0 bit 1, per libaacs/UDFclient agreement; the
   authoritative signed BEC bit lives in the AACS drive certificate
   behind REPORT KEY, out of scope). Feature present but payload
   truncated (< 4 bytes) is malformed and reads as absent — fail
   closed, same rule as the walker. */
typedef struct mos_drive_caps {
    bool    aacs;            /* feature 0x010D present in the walk      */
    uint8_t aacs_version;    /* payload byte 3; 0 when aacs is false    */
    bool    bus_encryption;  /* payload byte 0 bit 1; false when absent */
} mos_drive_caps;

void mos_internal_aacs_caps_from_config(const uint8_t *buf, size_t len,
                                        mos_drive_caps *out);

/* One feature for the public enumeration (mos_enumerate_features) —
   the descriptor header facts only. The payload bytes stay internal:
   exposing a borrowed slice across the public ABI buys lifetime rules
   no current consumer needs; a typed decode (the AACS caps above) is
   how payload facts go public. Tagged, no internal typedef alias: mos.h
   owns the sole typedef (mos_feature_info_t), exactly as struct
   mos_device_info is defined here but typedef'd only across the ABI. */
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
 * Disc status from the MMC READ DISC INFORMATION (0x51) standard response —
 * the disc-completion signal: a Complete disc has finalized, readable content; a
 * Blank one has nothing to rip. byte 2 carries the status, last-session, and
 * erasable bits; the session/track counts are split LSB/MSB across the fixed
 * header (bytes 4..11). */
/* mos_disc_status (the enum) is public — defined in mos.h with the
   other ABI-pinned enums; the v0.4 typed accessor surfaces it. The
   struct layout below stays internal (mos.h sees only the opaque
   typedef; accessors in mos_result.c are the supported read path). */
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

/* Decode a READ DISC INFORMATION (0x51, data type 000b) response into
 * *out. True only when the fixed numeric region (through byte 11) is
 * present per BOTH `len` and the reply's own declared length. Address
 * fields and validity-gated identifiers are deliberately not decoded
 * (v0.4+). Layout and safety contract at the decoder (mos_discinfo.c). */
bool mos_internal_disc_info_parse(const uint8_t *buf, size_t len,
                                  mos_disc_info *out);

/* ---- READ DISC STRUCTURE / BD Disc Information decode (mos_discstruct.c) -- *
 *
 * The disc's REGISTERED identity from a Blu-ray Disc Information (DI)
 * reply (READ DISC STRUCTURE 0xAD, BD media type, format 0x00): the
 * Disc Manufacturer ID, Media Type ID, and Product Revision. Fixed-
 * width ASCII fields read at CONSTANT offsets inside the first DI unit;
 * no device-supplied value is ever used as an offset or length (the
 * only device length, the structure-data-length header, can only shrink
 * the trusted region), and no payload byte is dereferenced — bytes are
 * copied verbatim into these fixed buffers and the CLI layer escapes
 * them, same as INQUIRY identity. Classification (e.g. manufacturer
 * "MILLEN" => M-DISC) is the consumer's, not mos's. Strings are NUL-
 * terminated with fixed-width space padding stripped; "" when the DI is
 * absent. New fields append at the END (ABI-safe; accessors are the
 * contract). */
struct mos_disc_id {
    char disc_type[4];      /* DI+8,   3 bytes + NUL: "BDR"/"BDW"/"BDO" */
    char manufacturer[7];   /* DI+100, 6 bytes + NUL */
    char media_type[4];     /* DI+106, 3 bytes + NUL */
    char revision[2];       /* DI+111, 1 byte  + NUL */
};

/* Parse a BD DI reply into *out. True only when the 'DI' signature is
 * present AND the trusted region (min of `len` and the reply's declared
 * length) reaches the product-revision byte; false (and *out emptied)
 * otherwise. Pure, fixed-offset, no-OOB — fuzz/ASan-gated by
 * tests/fuzz_pure.c and tests/test_discstruct.c. */
bool mos_internal_bd_disc_id_parse(const uint8_t *buf, size_t len,
                                   struct mos_disc_id *out);

/* ---- READ TOC/PMA/ATIP format 0101b (CD-TEXT) decode (mos_cdtext.c) --- *
 *
 * The disc-level (album) Title and Performer from a CD-TEXT reply — the
 * "which album is in the drive" disambiguator, parallel to the mounted
 * volume name for data discs. Decoded from the FIRST language block
 * (block 0), single-byte charset; a double-byte (DBCC) album field reads
 * as "" (absent), never mis-decoded. Disc-controlled bytes copied
 * verbatim into fixed buffers (CLI escapes at emit); the only device
 * length (CD-TEXT Data Length) can only shrink the trusted span. This is
 * BEST-EFFORT DISPLAY TEXT, not a fingerprint — audio-CD dedup keys ride
 * on the fail-closed TOC. Per-track titles, the other field types, and
 * additional language blocks are deferred (design doc 2026-06-14). New
 * fields append at the END (ABI-safe; accessors are the contract). */
#define MOS_CDTEXT_STR_CAP        160u
#define MOS_CDTEXT_MAX_TRACKS      99u
#define MOS_CDTEXT_TRACK_TITLE_CAP 64u
struct mos_cdtext {
    bool    have;                       /* a non-empty album field present */
    char    title[MOS_CDTEXT_STR_CAP];     /* album Title (track 0, block 0); "" if absent */
    char    performer[MOS_CDTEXT_STR_CAP]; /* album Performer; "" if absent   */
    /* Per-track titles (pack 0x80) and performers (pack 0x81), tracks
       1..N, block 0, indexed by track number: track_titles[n-1] /
       track_performers[n-1] are track n's strings ("" if that track had
       none of that field — the arrays are independently sparse, e.g. a
       various-artists disc carries per-track performers). track_count is
       the highest track number carrying EITHER a non-empty title or
       performer; entries above it are unset. */
    uint8_t track_count;
    char    track_titles[MOS_CDTEXT_MAX_TRACKS][MOS_CDTEXT_TRACK_TITLE_CAP];
    char    track_performers[MOS_CDTEXT_MAX_TRACKS][MOS_CDTEXT_TRACK_TITLE_CAP];
};

/* Parse a CD-TEXT (format 0101b) reply into *out. True only when at
 * least one non-empty album-level field (Title or Performer) was decoded
 * within the trusted region (min of `len` and the reply's declared
 * length); false (and *out emptied) otherwise. Pure, no-OOB, every
 * string NUL-terminated — fuzz/ASan-gated by tests/test_cdtext.c. */
bool mos_internal_cdtext_parse(const uint8_t *buf, size_t len,
                               struct mos_cdtext *out);

/* ---- READ DISC STRUCTURE / physical structure decode (mos_physstruct.c) --- *
 *
 * Physical Format Information (READ DISC STRUCTURE 0xAD, DVD/HD-DVD media
 * type, format 0x00) and Copyright Management Information (format 0x01).
 * "Physical structure" rather than "DVD": the media-type-0 reply carries
 * HD-DVD book types (0x4..0x6) as well as DVD ones. The physical fields
 * are geometry the disc reports (book type, layer layout, data-area
 * sector boundaries — end_sector_l0 is the layer break); the copyright
 * fields are the protection-system type and the region mask. All read at
 * CONSTANT offsets inside a fixed buffer; the only device length (the
 * structure-data-length header) can only shrink the trusted region.
 * Classification (book_type => media name, cpst => "CSS-protected") is
 * the consumer's. The two halves share one struct: have_physical /
 * have_copyright say which the adapter merged in. New fields append at
 * the END (ABI-safe; accessors are the contract). */
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

/* Parse the Physical Format Information (format 0x00) / Copyright
 * Management Information (format 0x01) halves into *out. Each sets its
 * own have_* flag and fills its own fields; the adapter zero-inits the
 * struct once and calls both. True only when the trusted region (min of
 * `len` and the reply's declared length) reaches the last needed byte.
 * Pure, fixed-offset, no-OOB — fuzz/ASan-gated. */
bool mos_internal_physical_format_parse(const uint8_t *buf, size_t len,
                                        struct mos_physical_structure *out);
bool mos_internal_copyright_mgmt_parse(const uint8_t *buf, size_t len,
                                       struct mos_physical_structure *out);

/* ---- READ TRACK INFORMATION decode (mos_trackinfo.c) --------------- *
 *
 * The capacity / append-state surface of one track from READ TRACK
 * INFORMATION (0x52): track start, next writable address, free blocks,
 * track size, last recorded address, plus the track/data mode and
 * blank/damage bits. next_writable and last_recorded are meaningful only
 * when nwa_valid / lra_valid (the reply's own validity bits) are set.
 * Read at CONSTANT offsets; the only device length (the Track
 * Information Length header) can only shrink the trusted region. New
 * fields append at the END. */
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
 * region (min of `len` and the reply's declared length) reaches the Last
 * Recorded Address (byte 31). Pure, fixed-offset, no-OOB — fuzz/ASan-
 * gated. */
bool mos_internal_track_info_parse(const uint8_t *buf, size_t len,
                                   struct mos_track_info *out);

/* ---- Disc capacity (assembled, no command) ------------------------- *
 *
 * Unlike every struct above, mos_capacity has NO pure decoder: there is
 * no capacity reply to parse. The IOKit shell assembles it from two
 * sources it already holds (doc/research/2026-06-13-read-capacity-
 * feasibility.md):
 *   - the whole-disk IOMedia node's kernel-cached byte size and natural
 *     block size (kIOMediaSizeKey / kIOMediaPreferredBlockSizeKey) — the
 *     result of the kernel's own attach-time READ CAPACITY, a registry
 *     property read with no SCSI command and no exclusive access (so it
 *     works on MOUNTED media, where a raw READ CAPACITY would BUSY); and
 *   - the recordable / append-state view from READ TRACK INFORMATION
 *     (mos_track_info), the same non-exclusive convenience read.
 * media_bytes / block_bytes are 0 when the whole-disk node carries no
 * size (blank or absent media — there is no node until the disc is
 * formatted/recorded); have_recordable is false when READ TRACK
 * INFORMATION did not answer (e.g. an empty drive). New fields append at
 * the END. */
struct mos_capacity {
    uint64_t media_bytes;     /* kIOMediaSizeKey; 0 == no whole-disk size  */
    uint32_t block_bytes;     /* kIOMediaPreferredBlockSizeKey; 0 == none  */
    bool     have_recordable; /* READ TRACK INFORMATION answered           */
    bool     nwa_valid;       /* next_writable meaningful (TI reply bit)    */
    uint32_t free_blocks;     /* recordable free space (blocks)            */
    uint32_t next_writable;   /* append point (valid iff nwa_valid)        */
    uint32_t track_size;      /* first-track size (blocks); pressed-disc
                                 capacity for single-track media           */
};

/* ---- GET PERFORMANCE performance-data decode (mos_perf.c) ---------- *
 *
 * The drive's read/write performance from GET PERFORMANCE (0xAC, Type
 * 00h Performance Data — the type the Apple convenience method exposes),
 * summarized: max read and max write performance (kB/s) and the
 * descriptor count. The read/write split is the CDB WRITE bit, so the
 * adapter issues the command twice and fills this struct from the two
 * replies. have is false when neither direction returned a descriptor
 * (media-dependent — data, not error). Spec-derived layout (no in-repo
 * capture yet); a real capture is a falsifier per the hardware ADR. New
 * fields append at the END. */
struct mos_drive_perf {
    bool     have;              /* >= 1 descriptor in either direction */
    uint16_t descriptor_count;  /* from the read-direction reply       */
    uint32_t max_read_kbps;     /* max performance, WRITE=0 reply       */
    uint32_t max_write_kbps;    /* max performance, WRITE=1 reply       */
};

/* Decode one Performance Data reply: the max performance (kB/s) across
 * its Nominal Performance Descriptors and the descriptor count. True
 * when the 8-byte header is present and coherent (list may be empty).
 * Pure, fixed-offset, no-OOB — fuzz/ASan-gated. The adapter assembles
 * the struct above from two calls (WRITE=0 / WRITE=1). */
bool mos_internal_perf_data_parse(const uint8_t *buf, size_t len,
                                  uint32_t *max_kbps, uint16_t *count);

/* ---- MODE SENSE(10) page decode (mos_modepage.c) ------------------- *
 *
 * Two read-only optical-specific pages (AGENTS 2026-06-13 addendum):
 * page 0x2A (mechanical: loading mechanism, eject/lock support, the live
 * locked bit, buffer size) and page 0x01 (read error-recovery config).
 * Decoded by a bounded page walker; the only device lengths (mode data
 * length, block descriptor length, per-page length) can only shrink the
 * trusted region, and the walk strictly advances. New fields append at
 * the END. */
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
 * page is present and long enough for the fields read. Pure, bounded,
 * no-OOB — fuzz/ASan-gated. */
bool mos_internal_mode_caps_parse(const uint8_t *buf, size_t len,
                                  struct mos_mode_caps *out);
bool mos_internal_error_recovery_parse(const uint8_t *buf, size_t len,
                                       struct mos_error_recovery *out);

/* ---- SCSI task status classification (mos_pure.c) ----------------- *
 *
 * True for the four SAM-5 status values that mean "drive contended,
 * try again later." Shared with test_scsi_status.c so the test
 * exercises the real disjunction, not a mirror. */
bool mos_internal_status_is_contended(uint32_t status);

/* ---- Tray-command outcome classification (mos_pure.c) ------------- *
 *
 * Classify a tray CDB's (task status, sense triple) into the public
 * mos_tray_outcome. GOOD -> DONE, 5/53/02 -> REFUSED_LOCKED, any other
 * non-GOOD -> REFUSED_OTHER. Pure; pinned by tests/test_tray.c. The
 * caller has already mapped transport/lock failure to a negative
 * mos_error — this only runs on a command the drive answered. */
mos_tray_outcome mos_internal_tray_classify(uint32_t scsi_status,
                                            uint8_t sk, uint8_t asc, uint8_t ascq);

/* ---- GET EVENT STATUS NOTIFICATION media decode (mos_pure.c) ------- *
 *
 * Pull the authoritative Door/Tray-open bit out of a raw GESN (0x4A)
 * Media-class polled reply. True + *door_open only for a valid,
 * NEA-clear, full-span Media descriptor; false = no authoritative bit
 * (caller forks on sense). Validity gates at the decoder (mos_pure.c);
 * ARCHITECTURE.md §4.2. */
bool mos_internal_gesn_media_door_open(const uint8_t *resp, size_t len,
                                       bool *door_open);

/* ---- IOReturn → mos_error mapping (mos_pure.c) -------------------- *
 *
 * Translate an IOReturn (any int32_t mach-style code) to a mos_error.
 * Takes int32_t so it lives in the pure layer; the Apple adapter casts
 * at the call site and static_asserts the SDK constants (mos_scsi.c).
 * Every IOReturn-handling adapter site must route through here —
 * notably NoDevice/NotAttached must map to MOS_ERR_NO_DEVICE for the
 * watch core's terminal-removal path. Groupings at the mapper. */
mos_error mos_internal_ioreturn_to_error(int32_t rc);

/* ---- Decision-tree core (mos_state_core.c) ------------------------- *
 *
 * mos_query_state()'s decision tree runs against this vtable: the Apple
 * adapter (mos_state.c) fills it from a real handle, tests script it.
 * Each callback receives the caller's ctx and either returns MOS_OK and
 * fills its out-parameter, or returns a negative mos_error. Division of
 * labour — TUR owns presence, the raw-GESN tray op owns the one tray
 * bit (never the masking GetTrayState convenience, ARCHITECTURE.md
 * §9.7), profile is READY-only enrichment — is documented at the tree
 * (mos_state_core.c). sense is fixed-format 18 bytes (SPC-4 §4.5.3). */
typedef struct {
    mos_error (*test_unit_ready)    (void *ctx, uint32_t *status,
                                                uint8_t sense[18]);
    mos_error (*get_tray_state)     (void *ctx, bool *tray_open);
    mos_error (*get_current_profile)(void *ctx, uint16_t *profile);
} mos_mmc_ops_t;

/* Identification metadata that propagates to mos_state_result without
   any IOKit handle being involved. The Apple adapter populates these
   from mos_handle_t internal buffers; tests populate them from string
   literals. The pointer lifetime contract is the caller's: the core
   does not copy these, just propagates them into out->bsd_unit etc.
   bsd_unit is -1 for an empty/open-tray drive (no resolvable name). */
typedef struct {
    const mos_mmc_ops_t *ops;
    void *ctx;
    int64_t bsd_unit;
    uint64_t registry_id;        /* drive SERVICE registry entry ID — the
                                    attachment identity the watch stream
                                    emits; 0 == unavailable. Propagated
                                    verbatim into out->registry_id. */
    uint64_t media_id;           /* whole-disk IOMedia registry entry ID, 0 == none/unavailable */
    const char *vendor;          /* may be NULL */
    const char *product;         /* may be NULL */
    const char *revision;        /* may be NULL */
} mos_state_env_t;

mos_error mos_internal_query_state_core(const mos_state_env_t *env,
                                        mos_state_result *out);

/* ---- Watch core (mos_watch_core.c) ---------------------------------- *
 *
 * Pure watch state machine: tracks prev/current state, emits event
 * decisions, schedules backoff polls. The Apple-side mos_watch.c
 * drives this with real probes; tests drive it with fake probes and
 * a fake clock.
 *
 * The pure core does NOT do IOKit notifications — that's an
 * Apple-side optimization layered on top. The pure core is correct
 * on its own with poll-only behavior. */

typedef struct {
    /* Returns MOS_OK and fills out on successful probe. Returns
       negative mos_error on failure; the watch core decides whether
       to emit an error event or to retry. MOS_ERR_NO_DEVICE is
       interpreted by the core as terminal removal — the probe is
       indicating the underlying device has gone away. */
    mos_error (*probe)(void *ctx, mos_state_result *out);

    /* Returns current MONOTONIC time in milliseconds. Used only for
       deadline scheduling and latency measurement. MUST be from a
       monotonic source (CLOCK_MONOTONIC / mach_absolute_time on
       Apple, equivalent elsewhere). Tests inject a fake clock with
       small integer values; production uses real uptime ms.

       Splitting mono_ms and wall_ms into two callbacks at the type
       level makes a clock-domain mixup impossible at adapter wiring
       time: scheduling code calls mono_ms, ts-emission calls wall_ms.
       The two values are not interchangeable — a mixup puts the
       first-poll deadline decades in the future. */
    uint64_t  (*mono_ms)(void *ctx);

    /* Returns current WALL-CLOCK time in milliseconds since Unix
       epoch. Used only for the session-open timestamp and event ts
       formatting (RFC 3339). MUST NOT be used for scheduling — it
       can jump backward on clock adjustments, NTP steps, or DST. */
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
    /* When kind == EMIT_EVENT, this is the event the caller should emit.
       The vendor/product/revision pointers are adapter-owned (see
       mos_watch.c's pointer-lifetime invariant); registry_id,
       stream_open_wall_ms, and bsd_unit are plain values with no
       lifetime constraint. */
    mos_watch_event         event;
    /* When kind == SLEEP_UNTIL, this is the MONOTONIC-ms deadline
       the caller should sleep until. */
    uint64_t                next_poll_at_mono_ms;
} mos_watch_decision;

/* The pure watch state. Tests construct one directly with their own ops +
   fake clock; the Apple-side mos_watch.c constructs it inside mos_watch_t. */
typedef struct {
    const mos_watch_ops_t *ops;
    void *ctx;

    /* Session identity: registry_id (attachment identity token; 0 only
       reachable from direct pure-layer callers — the adapter fails
       closed) and the wall epoch captured at init. bsd_unit is the
       whole-disk unit (-1 = no whole-disk IOMedia node (media absent)), event-time refreshed by probes. */
    uint64_t       registry_id;
    uint64_t       stream_open_wall_ms;
    int64_t        bsd_unit;

    /* Backoff parameters. */
    uint32_t       stable_poll_ms;
    uint32_t       transition_poll_ms;

    /* State tracking. */
    mos_state last_state;
    bool           have_last_state;
    /* Error-backoff tracking: consecutive identical probe errors widen
       the retry interval (escalation rule at the pump, mos_watch_core.c). */
    int32_t        last_probe_err;     /* mos_error of the previous probe, MOS_OK if it succeeded */
    uint32_t       consec_probe_errs;  /* consecutive probes returning last_probe_err */
    /* Media identity for same-state swap detection (F1): last_media_id is
       the fingerprint, last_profile the no-id-bridge fallback. 0 means
       "unavailable", never an observation. Rules at the pump. */
    uint64_t       last_media_id;
    uint16_t       last_profile;
    uint64_t       next_seq;

    /* Next scheduled probe deadline in MONOTONIC ms (compared against
       ops->mono_ms). Field name carries the clock domain in the type
       to make domain-mixup compile-loud rather than silent. */
    uint64_t       next_poll_at_mono_ms;

    /* Removal lifecycle. terminated flips true on external removal
       notification or probe → NO_DEVICE. removed_event_emitted is the
       sentinel for "we've already emitted the terminal device_removed
       event"; subsequent pumps go straight to TERMINAL. Keeping this
       as its own field (rather than overloading have_last_state as a
       dual-purpose sentinel) preserves the device_removed event when
       termination happens before any successful probe. */
    bool           terminated;
    bool           removed_event_emitted;
} mos_watch_state;

/* Initialize a watch state.
     - start_mono_ms: monotonic ms at init time. The first poll is
       scheduled for exactly this value (i.e. probe immediately).
     - registry_id: the session's attachment-identity token (Apple
       adapter: the IORegistry entry ID; tests: any value, 0 included).
     - start_wall_ms: Unix-epoch ms at init time, recorded as
       stream_open_wall_ms on every event. Session identity is the
       (registry_id, stream_open_wall_ms) pair.
   stable_poll_ms == 0 → 2000ms default; transition_poll_ms == 0 →
   200ms default. */
void mos_internal_watch_init(mos_watch_state *w,
                             const mos_watch_ops_t *ops, void *ctx,
                             int64_t bsd_unit,
                             uint64_t registry_id,
                             uint64_t start_mono_ms,
                             uint64_t start_wall_ms,
                             uint32_t stable_poll_ms,
                             uint32_t transition_poll_ms);

/* Drive one iteration of the watch pump. Internally calls ops->mono_ms
   and (if a poll is due) ops->probe. Returns a decision the caller
   acts on:
     - EMIT_EVENT: caller writes the event, then calls pump again.
     - SLEEP_UNTIL: caller waits until next_poll_at_ms (or until an
       external notification interrupts), then calls pump again.
     - TERMINAL: caller closes the watch.
   The caller controls how it sleeps (CFRunLoop on Apple, simple
   nanosleep elsewhere) — the pure core does not block. */
mos_watch_decision mos_internal_watch_pump(mos_watch_state *w);

/* External trigger: tell the watch state that a device-removed
   notification arrived. Next pump call will return TERMINAL with a
   device_removed event. */
void mos_internal_watch_notify_removed(mos_watch_state *w);

/* External trigger: tell the watch state that a wake-up arrived
   (e.g., a kIOGeneralInterest notification). The next pump call will
   probe immediately rather than waiting for the scheduled poll. */
void mos_internal_watch_notify_wake(mos_watch_state *w);

/* ---- Watch-all multiplexer (DR pivot Phase 2b) --------------------- *
 *
 * Pure fan-in over up to MOS_WATCH_ALL_CAP per-device watch cores:
 * join/leave lifecycle, stream-global seq, deterministic same-tick
 * interleave (ascending registry_id). Each slot is a full
 * mos_watch_state driven through its own ops/ctx — the multiplexer
 * adds NO probing or classification of its own, it only schedules,
 * relabels mid-stream joins (snapshot → device_appeared), and makes
 * device_removed per-slot instead of stream-terminal. */

#define MOS_WATCH_ALL_CAP 16

typedef struct {
    mos_watch_state cores[MOS_WATCH_ALL_CAP];
    bool            active[MOS_WATCH_ALL_CAP];
    /* Slot joined after the stream opened: its first SNAPSHOT is
       relabeled MOS_EVENT_DEVICE_APPEARED, then the flag clears.
       Earlier ERROR events (probe failing right after hot-plug) do
       NOT consume the join — the announcement waits for the first
       successful probe. */
    bool            join_pending[MOS_WATCH_ALL_CAP];
    uint64_t        seq;   /* stream-global; overrides per-core seq */
} mos_watch_all_state;

void mos_internal_watch_all_init(mos_watch_all_state *a);

/* First free slot index, or -1 when full. Exposed so the adapter can
   point a slot's ctx at per-slot storage BEFORE add() initializes the
   core with it (add() uses the same first-free scan, single-threaded
   by the watch contract). */
int mos_internal_watch_all_free_slot(const mos_watch_all_state *a);

/* Add a device. Same parameters as mos_internal_watch_init, plus
   mid_stream (true ⇒ first event is device_appeared). Returns the slot
   index used; the index of the EXISTING slot if registry_id is already
   active (dedupe — DR can announce a device the open-time snapshot
   already carried); -1 when full or registry_id == 0. */
int mos_internal_watch_all_add(mos_watch_all_state *a,
                               const mos_watch_ops_t *ops, void *ctx,
                               int64_t bsd_unit, uint64_t registry_id,
                               uint64_t start_mono_ms,
                               uint64_t start_wall_ms,
                               uint32_t stable_poll_ms,
                               uint32_t transition_poll_ms,
                               bool mid_stream);

/* Active slot index for a registry id, or -1. The adapter's
   Disappeared handler resolves the leaving device with this and calls
   mos_internal_watch_notify_removed on cores[i]. */
int mos_internal_watch_all_find(const mos_watch_all_state *a,
                                uint64_t registry_id);

/* One multiplexer iteration. EMIT_EVENT carries the next event with
   stream-global seq (join relabeling and per-slot removal applied);
   SLEEP_UNTIL carries the earliest deadline over active slots, or
   UINT64_MAX when no slot is active (empty stream: sleep until an
   external add/wake). NEVER returns TERMINAL — removal is per-slot. */
mos_watch_decision mos_internal_watch_all_pump(mos_watch_all_state *a);

#endif /* MOS_PURE_H */
