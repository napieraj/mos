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
 *        -mmacosx-version-min=12.0
 *
 * Or add both files (mos.h and this one) to your existing build system
 * and make sure IOKit, CoreFoundation, and DiscRecording are on your
 * link line, with the deployment target pinned to macOS 12.0 to match
 * the CMake build's CMAKE_OSX_DEPLOYMENT_TARGET. Skipping
 * -framework DiscRecording fails to link at the DRCopyDeviceArray
 * reference in mos_dr.c.
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

#include "mos.h"

/* ==== src/mos_scsi_status.h ==== */
/*
 * mos_scsi_status.h — SAM-5 §5.3 SCSI task status constants.
 *
 * Pure-data header with no IOKit / CoreFoundation dependencies. Safe
 * to include from:
 *   - Runtime source that needs to classify task status (mos_state_core.c)
 *   - Pure-data test code that must compile without the SDK
 *     (tests/test_scsi_status.c)
 *
 * This is the single source of truth for these values. Do NOT redeclare
 * them elsewhere — a second copy drifts the first time only one is updated.
 *
 * We define our own constants rather than using Apple's kSCSITaskStatus_*
 * enums from SCSITask.h because the Apple set is incomplete: it omits
 * RESERVATION_CONFLICT, TASK_SET_FULL, and ACA_ACTIVE, which our state
 * machine treats uniformly as "drive contended." Having our own set lets
 * the contention classifier (and its test) work from one consistent
 * vocabulary.
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


#include <stdint.h>
#include <stdbool.h>


/* ---- Returned-object layouts (opaque in the public header) --------- *
 *
 * The public mos.h exposes mos_state_result and mos_watch_event only as
 * opaque typedefs plus accessor prototypes; the full layout lives here so
 * the pure core, the Apple fill paths, and the pure tests can read and
 * write fields directly. "Grow in place" means appending a field below —
 * because external callers only ever see the accessors, that is ABI-safe
 * with no size/version negotiation. Keep additions at the end. */
struct mos_state_result {
    mos_state_enum state;
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
    mos_state_enum state;
    mos_state_enum prev_state;
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
 * boundaries — reading kIOBSDNameKey in the adapter, the DA reported
 * name, argv in the probes. Drive identity itself is an int64 unit
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
mos_state_enum mos_internal_state_from_sense_closed(uint8_t sk, uint8_t asc, uint8_t ascq);

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

/* True if `reported` (a raw DA/IOKit BSD name, e.g. "disk4" or
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

typedef struct {
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
};

/* Decode a READ DISC INFORMATION (0x51, data type 000b) response into
 * *out. True only when the fixed numeric region (through byte 11) is
 * present per BOTH `len` and the reply's own declared length. Address
 * fields and validity-gated identifiers are deliberately not decoded
 * (v0.4+). Layout and safety contract at the decoder (mos_discinfo.c). */
bool mos_internal_disc_info_parse(const uint8_t *buf, size_t len,
                                  mos_disc_info *out);

/* ---- SCSI task status classification (mos_pure.c) ----------------- *
 *
 * True for the four SAM-5 status values that mean "drive contended,
 * try again later." Shared with test_scsi_status.c so the test
 * exercises the real disjunction, not a mirror. */
bool mos_internal_status_is_contended(uint32_t status);

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
    mos_state_enum last_state;
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


/* ==== src/mos_internal.h ==== */
/*
 * mos_internal.h — internal library declarations. Not part of the
 * public ABI. Consumers should include only <mos.h>.
 *
 * Pure-data prototypes (sense parser, BSD-name normalization,
 * status classifier, IOReturn mapper, watch-core state machine)
 * live in mos_pure.h so tests can include them without pulling in
 * IOKit.
 */



#include <stdbool.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICmds_REQUEST_SENSE_Defs.h>

/* ---- Handle layout (opaque to public callers) ----------------------- */

struct mos_handle {
    io_service_t              svc;
    uint64_t                  drive_registry_id; /* IORegistryEntryGetRegistryEntryID(svc);
                                                    0 if the call failed. The attachment
                                                    identity (same value the watch emits). */
    IOCFPlugInInterface     **plugin;
    MMCDeviceInterface      **mmc;
    SCSITaskDeviceInterface **std;   /* lazy; only allocated on first raw CDB */

    bool                      have_exclusive;

    /* Whole-disk identity (the BSD unit, kIOBSDUnitKey); -1 = no whole-disk IOMedia node (media absent). The string
       buffers below back the variable-length INQUIRY fields. */
    int64_t                   bsd_unit;
    uint64_t                  media_id;        /* whole-disk IOMedia registry
                                                  entry ID, 0 == no media;
                                                  captured at open alongside
                                                  bsd_unit (F1 swap fingerprint) */
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
 * The directory half of the DR pivot: discovery, identity, addressing
 * (doc/research/2026-06-10-dr-pivot-implementation-plan.md). Never
 * state — that stays with the MMC seam below. */

/* One enumerated device, extracted from DR's dictionaries into plain C
   at the adapter seam (no CF types cross this line). Identity buffers
   carry the SPC-4 INQUIRY field widths — DR pre-parses the same bytes. */
typedef struct {
    uint64_t registry_id;     /* path → entry → ID; never 0 in a snapshot */
    int64_t  bsd_unit;        /* -1 = no whole-disk IOMedia node (media absent) */
    char     vendor[9];       /* 8 chars + NUL */
    char     product[17];     /* 16 chars + NUL */
    char     revision[5];     /* 4 chars + NUL */
} mos_internal_dr_snapshot;

/* Fill up to `cap` slots in DR device-array order (the same array
   drutil enumerates — the index provenance contract). Returns the
   count. Devices whose registry path cannot be resolved to an entry
   ID are skipped (an un-reopenable index entry would violate the
   enumeration/index correspondence). */
size_t mos_internal_dr_copy_snapshot(mos_internal_dr_snapshot *slots,
                                     size_t cap);

/* Resolve a canonical "diskN" name to the drive's registry entry ID
   via DRDeviceCopyDeviceForBSDName; 0 when DR knows no such device. */
uint64_t mos_internal_dr_registry_id_for_bsd_name(const char *disk_name);

/* Resolve a kDRDeviceIORegistryEntryPathKey value (CFString expected;
   anything else yields 0) to the entry's uint64 registry ID. Shared by
   the snapshot builder and the watch doorbell's per-device filter. */
uint64_t mos_internal_dr_id_for_path_value(CFTypeRef path);

/* Extract one device's snapshot (registry id, bsd unit, identity) from
   a DRDeviceRef passed as CFTypeRef (mos_internal.h stays free of
   DiscRecording types). False when the device's registry path doesn't
   resolve — the same skip gate the array snapshot applies. Used by the
   snapshot builder and the watch-all Appeared handler. */
bool mos_internal_dr_device_snapshot(CFTypeRef device_ref,
                                     mos_internal_dr_snapshot *s);

/* Device-static identity strings for an already-opened service, via
   DR's registry-path lookup. Best-effort: returns false (and empties
   the buffers) when DR cannot see the service — the same non-fatal
   contract the retired open-time INQUIRY had. */
bool mos_internal_dr_copy_identity_for_service(io_service_t svc,
                                               char *vendor, size_t vcap,
                                               char *product, size_t pcap,
                                               char *revision, size_t rcap);

/* ---- IOKit-linked internal prototypes ------------------------------ *
 *
 * MMC convenience wrappers for the query path (mos_state.c). The
 * open-time INQUIRY wrapper retired with the DR pivot — identity is
 * directory data now. */
mos_error mos_internal_mmc_get_tray_state     (mos_handle_t *h, bool *tray_open);
mos_error mos_internal_mmc_test_unit_ready    (mos_handle_t *h,
                                               uint32_t *status,
                                               uint8_t sense[18]);
mos_error mos_internal_mmc_get_current_profile(mos_handle_t *h, uint16_t *profile);

/* STUB (v0.4, hardware-gated): the IOKit half of GET CONFIGURATION's feature
 * surface. Returns MOS_ERR_UNSUPPORTED until the RT=0 issuance is written and
 * HW-validated. See mos_scsi.c for the walker seam. */
mos_error mos_internal_mmc_get_features       (mos_handle_t *h);

/* Internal-only accessor for the registry entry ID captured during
   enumeration. Used by mos_open_by_index to reopen by stable ID rather than
   racing on BSD-name re-resolution. */
uint64_t mos_internal_device_info_registry_id(const mos_device_info_t *i);

/* Open a drive by its IORegistry entry ID — the identity-stable
   primitive: the kernel resolves IORegistryEntryIDMatching atomically,
   so this returns the SAME entry the ID came from or NO_DEVICE if it
   terminated; a recycled BSD name cannot rebind it to a different
   drive. The watch's authority for which drive a session probes. Not
   public: registry IDs are IOKit-specific. *err_out: NO_DEVICE or IO. */
mos_handle_t *mos_internal_open_by_registry_id(uint64_t id,
                                               mos_error *err_out);

/* The validated io_service_t a handle was opened against — identity
   transfer without a second BSD lookup (which would be a TOCTOU window).
   IO_OBJECT_NULL on NULL input. The caller MUST IOObjectRetain the
   result before mos_close(h) drops the handle's own reference; the
   caller then owns the extra retain and must IOObjectRelease it. */
io_service_t mos_internal_handle_get_service(mos_handle_t *h);

/* ---- Auto-cleanup helpers for IOKit / CoreFoundation refcounts ----- *
 *
 * The cleanup attribute is a gcc/clang extension that runs the named
 * callback when the variable goes out of scope. We use it to make
 * refcount discipline automatic in functions with multiple early-exit
 * paths (iterator loops, two-pass property lookups) where an explicit
 * release is easy to miss on one branch.
 *
 * Usage:
 *   io_object_t child MOS_IO_AUTO = IOIteratorNext(it);
 *   CFTypeRef prop  MOS_CF_AUTO = IORegistryEntryCreateCFProperty(...);
 *   // ... use, no explicit release ...
 *   // child and prop are released at scope exit
 *
 * Ownership transfer (when handing off to a longer-lived owner):
 *   io_service_t local MOS_IO_AUTO = IOIteratorNext(it);
 *   // ... validate ...
 *   io_service_t consumed = local;
 *   local = IO_OBJECT_NULL;   // disable cleanup; consumer now owns it
 *   return some_consumer(consumed);
 *
 * The cleanup callbacks check for the sentinel value before releasing
 * and clear the variable after, so manual release-and-clear and
 * auto-cleanup coexist safely. */

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


/* ==== src/mos_sense.c ==== */
/*
 * mos_sense.c — SCSI sense-data parsing and state mapping.
 *
 * No IOKit dependency; trivially unit-testable with fixture bytes.
 *
 * Fixed-format (response code 0x70 / 0x71): SPC-4 §4.5.3
 *   byte 0: response code + valid bit
 *   byte 2: bits 3:0 = sense key
 *   byte 12: Additional Sense Code (ASC)
 *   byte 13: Additional Sense Code Qualifier (ASCQ)
 *
 * Descriptor-format (response code 0x72 / 0x73): SPC-4 §4.5.2
 *   byte 0: response code
 *   byte 1: bits 3:0 = sense key
 *   byte 2: ASC
 *   byte 3: ASCQ
 *
 * Optical drives in practice always return fixed format. The descriptor
 * path is here for correctness, not because we've seen it in the wild.
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
 * This is the closed-branch enrichment, not a tray detector. By the time
 * it runs, the open/closed verdict already belongs to GESN's door bit (or,
 * when GESN was silent, to the sense fork in mos_state_core.c). So this
 * function never returns OPEN or EMPTY_OR_OPEN: it only refines a closed
 * tray into the *reason* the unit isn't ready. A 3A/02 ("medium not
 * present, tray open") reaching here means GESN already said closed and the
 * ASCQ's tray hint is deliberately discarded — enrich, don't invalidate.
 *
 * Presence is the hoist: asc == 0x3A means no medium → EMPTY; any other
 * not-ready sense means a disc is engaged and we classify why.
 *
 * Returns MOS_STATE_UNKNOWN for sense we won't assert a meaning for; the
 * tray is still known-closed, the raw sense still rides along on the result.
 *
 * References:
 *   T10 ASC/ASCQ public list: https://www.t10.org/lists/asc-num.htm
 *   MMC-6 / SBC-4 sense usage is consistent with the generic SCSI table.
 */
mos_state_enum mos_internal_state_from_sense_closed(uint8_t sk, uint8_t asc, uint8_t ascq)
{
    /* HARDWARE ERROR (key 0x04): the drive itself faulted — outranks any
       medium/not-ready detail that might also be set. */
    if (sk == 0x04) return MOS_STATE_DEVICE_FAULT;

    /* MEDIUM ERROR (key 0x03), or 57/00 UNABLE TO RECOVER TABLE-OF-CONTENTS:
       a disc is loaded but the drive can't read it. Not self-resolving, so
       it is NOT loading. */
    if (sk == 0x03 || (asc == 0x57 && ascq == 0x00))
        return MOS_STATE_MEDIA_UNREADABLE;

    /* 3A/xx MEDIUM NOT PRESENT, tray closed (per the hoist: no medium). The
       ASCQ open/closed flavor is moot here — GESN owns that. */
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

/* ==== src/mos_pure.c ==== */
/*
 * mos_pure.c — the IOKit-free pure surface: BSD-name predicates, the
 * SCSI-status contention test, identity-string rehoming, and the
 * IOReturn→mos_error mapping. No IOKit or CoreFoundation, so the whole
 * pure layer compiles, links, and is fuzz/ASan-tested without any Apple
 * framework. See ARCHITECTURE.md §3 and AGENTS.md rule 3.
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
   normalize + is_whole_shape so the accepted-form rules stay defined once. */
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

/* IOReturn → mos_error mapping. Pure: takes int32_t, returns mos_error.
   The Apple adapter casts IOReturn to int32_t at the call site; the
   src/mos_scsi.c adapter file contains static_asserts that pin every
   symbolic constant to the numeric value this function expects, so a
   future SDK change would fail the build loudly.

   IOReturn values are built from IOKit/IOReturn.h as:
     iokit_common_err(code) = sys_iokit | sub_iokit_common | code
                            = (0x38 << 26) | (0 << 14) | code
                            = 0xE0000000 | code
   The codes below are the ones the convenience-method documentation
   surfaces, plus a few others useful for diagnostic CDB. Notable
   groupings:
     kIOReturnNoDevice / kIOReturnNotAttached → MOS_ERR_NO_DEVICE
       — "device went away"; the watch core treats NO_DEVICE as terminal
         removal, so these must not collapse to generic MOS_ERR_IO.
     kIOReturnNoMemory / kIOReturnNoResources → MOS_ERR_OOM
       — runtime resource exhaustion in a convenience method. (Distinct
         from MOS_ERR_DRIVER_REJECTED, which mos_scsi.c produces only when
         the SCSITask/MMC interface factory returns NULL — the true "driver
         did not attach" case.)

   Pinned by tests/test_ioreturn.c — every case below has a test. */
mos_error mos_internal_ioreturn_to_error(int32_t rc)
{
    /* Switch on uint32_t to dodge implementation-defined behavior for
       large positive constants overflowing signed int. IOReturn values
       use the high bit (0xE0000000 prefix), so the literals would
       otherwise be negative ints. */
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
   [4..7]=start LBA (BE, MSF=0). Cross-checked against Linux sr.c /
   cdrom.h TOC ioctls and libcdio. Contract in mos_pure.h. */
bool mos_internal_toc_parse(const uint8_t *buf, size_t len, mos_toc *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!buf || len < 4) return false;

    /* Device claim: TOC Data Length counts bytes AFTER its own two.
       64-bit total, clamped by the trusted length (O-4). */
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
    /* A trailing partial descriptor inside the claimed span is a
       malformed TOC, not padding. */
    if (cursor != span) return false;

    /* The header's range bytes are hostile input too. The walk above
       proves the descriptors well-formed; identity additionally needs
       them to BE the table the header declares — ascending + unique +
       count == last-first+1 + matching endpoints forces exactly
       first..last (pigeonhole). A TOC that omits declared tracks, or
       declares an inverted or out-of-range header, is rejected whole:
       a fingerprint hashed over it would be falsely stable across
       genuinely different discs. */
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
       smaller of the two is the largest region that provably contains
       only bytes the kernel wrote this transfer. (transferred >
       allocated would itself be a transport fault; min() handles it
       without needing to decide whose bug it is.) */
    size_t trusted = allocated < transferred ? allocated : transferred;

    /* The device claim is hostile input. It participates only as a
       clamp: a drive may honestly tell us it returned LESS than we
       asked for, and we believe that; a drive claiming MORE than the
       transfer is lying or broken, and the claim is ignored. The
       comparison is performed in uint64_t so a caller-computed total
       like `data_length + header` cannot have wrapped on the way in. */
    if (claimed < (uint64_t)trusted)
        trusted = (size_t)claimed;

    return trusted;
}

/* Decode the Door/Tray-open bit from a GET EVENT STATUS NOTIFICATION (0x4A)
   Media-class polled reply. See ARCHITECTURE.md §4.2 for the byte map and
   research/2026-05-29-gesn-single-poll.md for the validity discipline.

   Returns true and sets *door_open ONLY when the reply carries an
   authoritative Media event descriptor — ALL of:
     - at least 6 bytes present (4-byte header + ≥2 descriptor bytes),
     - the device's own Event Data Length (bytes 0-1, big-endian, excludes
       itself) claims ≥6 following bytes (full-span, not a NEA stub),
     - the NEA "No Event Available" bit (byte 2, 0x80) is clear,
     - the header Notification Class (byte 2, low 3 bits) is Media (4).
   Otherwise returns false — "no authoritative bit" — and the state core
   forks on the TUR sense instead of trusting a fabricated verdict. This is
   the honesty the GetTrayState convenience wrapper throws away (it reports
   closed+success on a GESN failure).

   Bit positions per Linux drivers/scsi/sr.c media_event_desc. Pure and
   bounds-checked, so the offsets are fuzz/ASan-verifiable headless. */
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

/* ==== src/mos_config.c ==== */
/*
 * mos_config.c — pure, bounds-safe iteration over a GET CONFIGURATION
 * (MMC) response buffer.
 *
 * No IOKit. The IOKit shell issues GET_CONFIGURATION into a fixed,
 * zero-initialized buffer and hands that buffer plus the byte count it
 * trusts (sizeof the buffer — the MMC convenience GetConfiguration
 * returns no realized-transfer count) to the walker below. Every length
 * in the payload is device-reported and therefore hostile; this file is
 * the choke point that keeps those lengths from steering a read outside
 * [buf, buf+len).
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
 *     Callers walk this as a plain `while (next(...))` and never need to
 *     tell the two apart (libcdio's long-standing model); the malformed
 *     branch is unreachable on conformant hardware.
 *
 * No-OOB property gated headless under ASan/UBSan by tests/test_config.c.
 */


bool mos_internal_config_next_feature(const uint8_t *buf, size_t len,
                                      size_t *cursor, mos_config_feature *out)
{
    if (!buf || !cursor || !out) return false;

    /* Trusted end of the walk. Start at the hard buffer ceiling, then let
       the device's Data Length pull it IN if (and only if) it claims
       less. Computed in 64-bit so the +4 cannot wrap before the compare;
       a wrapped or oversized claim simply fails to shrink and `len`
       stands — device length only ever shortens the walk. */
    size_t end = len;
    if (len >= 4) {
        uint64_t dlen = ((uint64_t)buf[0] << 24) | ((uint64_t)buf[1] << 16)
                      | ((uint64_t)buf[2] << 8)  |  (uint64_t)buf[3];
        uint64_t declared = dlen + 4u;            /* total incl. length field */
        if (declared < (uint64_t)end) end = (size_t)declared;
    }

    size_t c = *cursor;

    /* Descriptor header must fit. `c > end` also catches a cursor already
       past the trusted region; `end - c` is computed only once c <= end,
       so it cannot wrap. */
    if (c > end || end - c < 4) return false;

    uint8_t add  = buf[c + 3];                    /* Additional Length, 0..255 */

    /* MMC requires Additional Length to be a multiple of 4. A value that
       is not is malformed; tolerating it would let a hostile device
       desync the walk so later descriptors decode from misaligned bytes
       (in-bounds, but attacker-chosen feature codes). Fail closed: end
       the walk at the first malformed descriptor. */
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

/* Current Profile = feature-header bytes 6-7, gated on the header's own
   Data Length (bytes 0-3, counting bytes that FOLLOW it): the profile
   field exists only when the drive claims >= 4 following bytes. The gate
   is what keeps a truncated reply from being read as profile 0x0000
   (= "no media"). Header layout above; contract in mos_pure.h. */
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

/* ==== src/mos_discinfo.c ==== */
/*
 * mos_discinfo.c — pure, bounds-safe decode of a READ DISC INFORMATION
 * (MMC 0x51, standard data type 000b) response.
 *
 * No IOKit. The IOKit shell issues READ DISC INFORMATION into a fixed,
 * zero-initialized buffer and hands that buffer plus its size here. The
 * Disc Information Length is device-reported and therefore hostile; this
 * file keeps it from steering a read outside [buf, buf+len).
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
 *   (bytes 12+ : Disc Identification, lead-in / lead-out addresses, bar
 *    code, OPC table — not decoded; informational, not the status.)
 *
 * Safety contract (the device controls the length here):
 *   - `len` is the only trusted ceiling; the Disc Information Length can
 *     only shrink the trusted region (clamped under `len`), never extend it.
 *   - The fixed numeric region (through byte 11) must be present per both
 *     `len` and the declared length; a shorter response is refused.
 *
 * No-OOB property gated headless under ASan/UBSan by tests/test_discinfo.c.
 */


bool mos_internal_disc_info_parse(const uint8_t *buf, size_t len,
                                  mos_disc_info *out)
{
    if (!buf || !out) return false;

    /* The fixed numeric fields this decode promises run through byte 11, so
       the trusted region must reach at least byte 12. */
    if (len < 12) return false;

    /* Disc Information Length (bytes 0-1) counts bytes AFTER itself, so the
       response occupies declared + 2 bytes. Clamp the trusted region to the
       smaller of that and len — a device length only ever shrinks it. */
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
    return true;
}

/* ==== src/mos_result.c ==== */
/*
 * mos_result.c — accessors for the opaque mos_state_result and
 * mos_watch_event objects.
 *
 * The public header exposes these objects only as opaque typedefs; their
 * layout (in mos_pure.h) is internal and may grow by appended fields
 * without breaking ABI. These accessors are the supported read path. Pure
 * (no IOKit), so they build and are unit-tested headless on any platform.
 *
 * Every accessor tolerates a NULL object, returning a benign zero/NULL —
 * a caller that ignored a failed query's NULL *out gets a defined answer
 * rather than a crash.
 */


/* ---- mos_state_result --------------------------------------------- */

mos_state_enum mos_state_result_state(const mos_state_result *r)
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

mos_state_enum mos_watch_event_state(const mos_watch_event *e)
{
    return e ? e->state : MOS_STATE_UNKNOWN;
}

mos_state_enum mos_watch_event_prev_state(const mos_watch_event *e)
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

/* ==== src/mos_state_core.c ==== */
/*
 * mos_state_core.c — pure decision tree for mos_query_state().
 *
 * No IOKit. The convenience-TUR-first presence probe (single shot), the
 * raw-GESN tray fork, and the closed-branch sense enrichment all live here
 * against a small vtable (mos_mmc_ops_t).
 * mos_state.c fills mos_state_env_t from a real handle; mos_scsi.c
 * implements the ops; tests/test_state_core.c drives it with scripted
 * ops and no hardware.
 *
 * The shape, in one breath: the convenience TUR is trusted for PRESENCE
 * and short-circuits READY without a lock; only when it is NOT ready do we
 * take exclusive access (free, because not-ready ⇒ not-mounted) and fire a
 * RAW GESN for the one bit it owns — tray open or closed; then the TUR
 * sense refines the closed side into the reason, never overturning GESN's
 * open/closed verdict. "Couldn't reach the drive" (a negative return) is
 * kept categorically distinct from "here is the state" (out->state).
 */


#include <string.h>

mos_error mos_internal_query_state_core(const mos_state_env_t *env,
                                        mos_state_result *out)
{
    if (!env || !env->ops || !out) return MOS_ERR_INVALID_ARG;

    /* All three callbacks are dispatched below; a NULL one would crash on
       first use, and there is no defensible degraded mode for "classify a
       drive without TEST UNIT READY." Production tables are static const and
       fully populated; this guards fixture/fuzz paths with a clean failure. */
    if (!env->ops->test_unit_ready ||
        !env->ops->get_tray_state ||
        !env->ops->get_current_profile) {
        return MOS_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->state    = MOS_STATE_UNKNOWN;
    /* Identity propagates verbatim: bsd_unit == -1 is the "no media" signal
       end to end (an empty/open-tray drive has no IOMedia child, hence no
       unit), and media_id carries the F1 same-state swap fingerprint. */
    out->bsd_unit    = env->bsd_unit;
    out->registry_id = env->registry_id;
    out->media_id    = env->media_id;
    out->vendor   = env->vendor;
    out->product  = env->product;
    out->revision = env->revision;

    /* Hoisted above the first `goto enrich` so no jump skips an initializer
       (C11 §6.2.4). Nothing under enrich: reads the tray temporaries, but
       hoisting keeps a future edit there from silently becoming UB. */
    uint32_t  status     = 0;
    uint8_t   sense[18]  = {0};
    uint8_t   sk = 0, asc = 0, ascq = 0;
    mos_error tur_err    = MOS_OK;
    bool      door_open  = false;
    bool      tray_open  = false;

    /* ---- 1. Convenience TUR — non-exclusive — ONE shot — PRESENCE ---- *
     * "Do I have a disc, and am I ready?" GOOD answers the whole question
     * (closed + present + ready); otherwise the sense feeds steps 2–3.
     *
     * Issued exactly once, like the macOS peers. We do NOT drain UNIT
     * ATTENTION: unlike the Linux first-toucher pattern, by the time mos holds
     * a handle the kernel's own device initialization has already consumed the
     * power-on / reset / media-change UA, so a single TUR sees a settled
     * drive. Pending events are not our concern — presence is, and 0x3A tells
     * us "no medium" before we ever reach GESN. */
    tur_err = env->ops->test_unit_ready(env->ctx, &status, sense);

    /* Load-bearing, not defensive: the kernel's user client REFUSES a
       convenience TUR while another client holds exclusivity —
       SCSITaskUserClient::TestUnitReady initializes its status to
       kIOReturnExclusiveAccess and gates on
       GetUserClientExclusivityState() (apple-oss-distributions/
       IOSCSIArchitectureModelFamily, UserClient/SCSITaskUserClient.cpp).
       A contended drive is therefore a real, expected transport answer
       here, and BUSY — not a negative error — is the truthful state. */
    if (tur_err == MOS_ERR_EXCLUSIVE_ACCESS || tur_err == MOS_ERR_BUSY) {
        out->state = MOS_STATE_BUSY;
        goto enrich;
    }
    /* COMMS_FAIL: a transport/IOKit failure reaching TUR. We cannot observe
       state at all — surface the negative code, distinct from any state. */
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
    /* NUB GATE — must equal the kernel's nub predicate, not approximate
       it: the plausible `sk==0 && asc==0 && ascq==0` gate diverges on
       exactly 11 inputs, mechanically proven by exhaustive enumeration
       in tests/audit/nub_invariant_check.c.

       PollForMedia sets mediaFound on CC + ASC/ASCQ 00/00 INDEPENDENT of
       the sense key (IOSCSIMultimediaCommandsDevice.cpp:3890-3894 — the
       check runs before the SENSE_KEY switch), then RESETS it whenever
       the switch set shouldEjectMedia (4012-4029), and only creates the
       IOMedia nub if it survives (require_quiet at 4052). At 00/00 the
       eject set is exactly keys {NOT_READY, MEDIUM_ERROR, HARDWARE_ERROR,
       BLANK_CHECK}: NOT_READY's keep-list (04/00, 04/01, 3A/xx, 57/00,
       04/04) cannot match 00/00, and BLANK_CHECK keeps only 64/00. So:

         - CC + 00/00 + key OUTSIDE {0x2,0x3,0x4,0x8}: the kernel KEEPS
           the nub (default switch arm). mos must NOT take the exclusive
           lock — classify UNKNOWN from here. This is where a stray
           UNIT ATTENTION (06/00/00) or RECOVERED ERROR (01/00/00) lands.
         - CC + 00/00 + key IN {0x2,0x3,0x4,0x8}: the kernel EJECTS — no
           nub exists, the lock is free, and the GESN probe below is what
           turns HARDWARE ERROR into device_fault instead of UNKNOWN.

       Non-zero ASC/ASCQ never sets the kernel flag, so the lock is
       always safe there. */
    if (status != MOS_SCSI_STATUS_CHECK_CONDITION ||
        (asc == 0 && ascq == 0 &&
         sk != 0x02 && sk != 0x03 && sk != 0x04 && sk != 0x08)) {
        /* Not GOOD, not contended, and either not a CHECK CONDITION or a
           kernel-nub-preserving 00/00 sense — nothing mos may probe. */
        out->state = MOS_STATE_UNKNOWN;
        goto enrich;
    }

    /* ---- 2. Not ready ⇒ not mounted ⇒ the lock is free. Tray bit. ---- *
     * get_tray_state issues a RAW GESN under exclusive access. MOS_OK ⇒
     * *door_open is authoritative. ANY failure (no lock, or GESN silent) ⇒
     * no authoritative bit, so the TUR sense becomes the fork. GESN's
     * open/closed is never overturned by the sense. */
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

    /* Tray CLOSED: the TUR sense refines the not-ready reason. This may be
       UNKNOWN (closed, unclassified) — still a closed drive, raw sense rides
       along on out->sense_*. */
    out->state = mos_internal_state_from_sense_closed(sk, asc, ascq);

enrich:
    /* ---- Enrichment (metadata only, never changes state) ---- *
     * current_profile only on READY: many firmwares (notably LG) keep
     * reporting the last disc's profile for minutes after the tray empties
     * (ARCHITECTURE.md §9), so exposing it on any not-present state would
     * imply a disc. Otherwise it stays at the memset(0) default. */
    if (out->state == MOS_STATE_READY) {
        uint16_t profile = 0x0000;
        if (env->ops->get_current_profile(env->ctx, &profile) == MOS_OK) {
            out->current_profile = profile;
        }
    }

    return MOS_OK;
}

/* ==== src/mos_watch_core.c ==== */
/*
 * mos_watch_core.c — pure watch state machine.
 *
 * Drives the poll loop:
 *
 *   - When to probe (next_poll_at_mono_ms, with two backoff rates by
 *     whether the last state was "transitional" or "stable").
 *   - What to emit (snapshot on first probe; state_changed when state
 *     differs from last; media_changed on a same-state ready disc swap;
 *     error on transient probe failure; device_removed when notify_removed
 *     fires OR a probe returns MOS_ERR_NO_DEVICE).
 *   - How to label it (monotonic seq; RFC 3339 ts from ops->wall_ms;
 *     registry_id + stream_open_wall_ms values; bsd_unit int).
 *   - When to stop (terminated flag from notify_removed, or auto-set on
 *     probe → NO_DEVICE).
 *
 * Two time domains, separated at the type level so a mixup cannot be
 * introduced silently — the signatures distinguish them at every callsite:
 *
 *   - ops->mono_ms() — MONOTONIC. Poll scheduling and latency only; never
 *     human-readable output.
 *   - ops->wall_ms() — WALL-CLOCK ms since Unix epoch. stream_open_wall_ms
 *     and event ts only; can jump backward on NTP, so never used for
 *     scheduling.
 *
 * The caller's pump loop owns blocking — this core never sleeps or calls
 * the OS. It returns a decision: EMIT_EVENT (write and re-pump),
 * SLEEP_UNTIL (block, then re-pump), TERMINAL (close). mos_watch.c maps
 * SLEEP_UNTIL to CFRunLoopRunInMode so a notification can wake early; the
 * test driver maps it to advancing a fake clock. Mirrors the
 * mos_state_core.c pattern: every transition testable without IOKit. See
 * tests/test_watch_core.c — including the fixture that pins the two-clock
 * contract by running mono_ms in the thousands and wall_ms in the trillions.
 */

/* Precautionary, mirroring mos_watch.c: this file uses only POSIX time
   interfaces today, but the define keeps BSD extensions visible if a
   BSD-only helper is added later. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

/* gmtime_r requires POSIX.1-2008 (200809L) on glibc; Apple's time.h
   exposes it unconditionally. Define before any header inclusion. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif


#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- Defaults ----------------------------------------------------- */

/* The two backoff rates. "stable" (open / empty / ready) is not a
   transition window — changes arrive via OS notifications (insert,
   eject). "transition" (loading / busy / unknown) is mid-transition, so
   short polls catch the resolution faster than waiting for one. Defaults
   are conservative (2s / 200ms): stable still notices an insert within a
   couple of seconds without notifications, transition observes ready
   resolution with no perceptible delay. Both configurable per watch. */
#define MOS_WATCH_DEFAULT_STABLE_MS     2000U
#define MOS_WATCH_DEFAULT_TRANSITION_MS  200U

/* ---- Time formatting --------------------------------------------- */

/* Format milliseconds-since-epoch as RFC 3339 UTC in YYYY-MM-DDTHH:MM:SSZ.
   Writes 21 bytes including NUL into the buffer (size must be >= 21).
   The seconds component is integer; sub-second precision is not
   surfaced in events. Input is WALL-CLOCK ms — feeding monotonic ms
   would produce nonsense like 1970-01-01T00:00:12Z.

   SATURATING: the schema pattern requires a 4-digit year, and the clock
   is an INPUT to this pure layer — the hostile-input discipline applies
   to ops->wall_ms exactly as it does to drive-controlled bytes; an
   insane host clock, NTP step, or fuzzed ops table must not produce a
   schema-invalid line. Values past
   9999-12-31T23:59:59Z clamp to that instant; a 5-digit year from
   strftime (21 chars) and an empty string from a gmtime_r failure are
   both schema violations, so neither can escape. Post-clamp, gmtime_r
   and strftime cannot fail for any uint64 input; the fallback writes
   the clamp constant anyway so the contract holds unconditionally. */
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
    /* gmtime_r: POSIX.1-2008, present on every platform this project
       compiles on (macOS targets, Linux pure-test CI). Deliberately no
       _WIN32/gmtime_s branch — Windows is neither a build nor a test
       target, and untestable code is unverifiable. */
    if (gmtime_r(&secs, &tm) != NULL) {
        /* All-numeric strftime specifiers (%Y %m %d %H %M %S) are
           POSIX-defined as locale-independent — locale only affects textual
           ones (%A, %B, %p, %c/%x/%X) we don't use. strftime also sidesteps a
           -Wformat-truncation false positive that a hand-rolled snprintf
           hits under -O2 (GCC sees tm_year as unbounded int). */
        if (strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 20) {
            return;
        }
    }
    /* Unreachable post-clamp on a conforming libc; the contract holds
       even if a libc misbehaves. */
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
    /* Schedule the first probe at start_mono_ms (i.e. immediately).
       This is a MONOTONIC value; the pump compares against ops->mono_ms. */
    w->next_poll_at_mono_ms   = start_mono_ms;
    w->terminated             = false;
    w->removed_event_emitted  = false;
    w->bsd_unit               = bsd_unit;   /* -1 == no media */

    /* Session identity: two plain values, no composite token. The
       registry_id is the attachment identity (xnu mints real IDs from a
       never-reused monotone counter >= 2^32+256; 0 is only reachable
       from direct pure-layer callers). start_wall_ms is recorded as
       stream_open_wall_ms on every event; the adapter monotonicizes it
       per process so the (registry_id, stream_open_wall_ms) pair is
       unique per session even for same-millisecond reopens of the same
       drive. Consumers needing a single correlation key derive one —
       the data layer stays normalized. */
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
    /* Pull the next poll forward to "right now" without inspecting
       the clock here — the caller's pump call will compare against
       ops->mono_ms() and probe immediately. Setting to 0 (zero
       monotonic ms) guarantees `now >= next_poll_at_mono_ms` on the
       next pump regardless of how the monotonic clock started. */
    w->next_poll_at_mono_ms = 0;
}

/* Whether the state is a "transition" state for backoff purposes.
   Loading / busy / unknown are transitional; the others are stable. */
static bool watch_state_is_transitional(mos_state_enum s)
{
    switch (s) {
        /* In-progress or degraded observations — poll fast to converge. */
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
    /* No default: a new mos_state_enum value trips -Wswitch under -Werror, so
       its poll class must be chosen deliberately rather than silently
       inheriting "stable." This trailing return only handles an out-of-range
       value (the enum is int32-wide). */
    return false;
}

/* Build a base event: session identity, seq, ts, bsd_unit. The ts is read
   fresh from ops->wall_ms each time, *not* derived from the
   monotonic clock used for scheduling. The caller fills in
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

/* Copy probe-result fields (everything except prev_state) into an event.
   Shared by the snapshot, state_changed, and media_changed branches, so a
   field added to mos_watch_event has one assignment site, not three. */
static void fill_event_state_fields(mos_watch_event *e,
                                    const mos_state_result *r,
                                    uint32_t latency_ms)
{
    e->state           = r->state;
    /* Event-time media identity: take the BSD unit from THIS probe rather
       than the watch's open-time value, so a disc appearing in a drive
       that was empty at open (or a unit that changed across eject/reinsert)
       is reflected in the event. fill_event_base seeds w->bsd_unit as the
       fallback for events with no fresh probe (error / device_removed). */
    e->bsd_unit        = r->bsd_unit;
    e->current_profile = r->current_profile;
    e->vendor          = r->vendor;
    e->product         = r->product;
    e->revision        = r->revision;
    e->sense_key       = r->sense_key;
    e->asc             = r->asc;
    e->ascq            = r->ascq;
    e->latency_ms      = latency_ms;
}

/* Saturating monotonic delta in milliseconds. Guards against a
   non-monotonic clock source (end < start -> 0) and against a probe that
   somehow spans more than ~49.7 days (delta > UINT32_MAX -> clamped),
   either of which would otherwise underflow or truncate on the cast. */
static uint32_t mos_watch_latency_ms(uint64_t start, uint64_t end)
{
    uint64_t delta = end >= start ? end - start : 0;
    return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}

/* Per-state poll interval: transitional states re-probe at the faster
   transition_poll_ms, stable states at stable_poll_ms. One site so the
   policy is audited in one place. */
static uint32_t poll_ms_for_state(const mos_watch_state *w,
                                  mos_state_enum state)
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

    /* Terminal: caller was told the device went away (either via
       notify_removed or via a probe that returned NO_DEVICE). Emit a
       final device_removed event then refuse further pumps. The
       removed_event_emitted sentinel ensures we emit exactly once,
       even when termination happens before any successful observation
       (in which case prev_state is reported as unknown). */
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

    /* Not yet time to probe → tell the caller to sleep. */
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

    /* MOS_ERR_NO_DEVICE from a probe means the device went away
       underneath the watch. Treat as terminal removal: flip the
       termination flag and let the next pump emit the device_removed
       event through the terminated path above. This handles the case
       where notifications didn't register (or aren't supported on
       the OS): without it, poll-only mode would spin emitting error
       events forever for an unplugged drive. */
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
        /* Other error: we couldn't observe state this round. Treat
           as non-terminal, emit the error event, and reschedule. */
        fill_event_base(w, &d.event);
        d.event.kind       = MOS_EVENT_ERROR;
        d.event.error      = perr;
        d.event.state      = MOS_STATE_UNKNOWN;
        d.event.prev_state = w->have_last_state ? w->last_state : MOS_STATE_UNKNOWN;
        d.event.latency_ms = mos_watch_latency_ms(probe_start_mono, probe_end_mono);

        /* Don't update last_state on an error — we don't have an
           observation, just an absence of one.

           Retry cadence: the first error (or a different error code)
           reschedules at transition rate for a prompt retry; each further
           consecutive identical error doubles the interval, capped at
           stable_poll_ms. A persistent failure thus converges to the
           stable cadence instead of flooding at the transition rate,
           while a notify_wake still pulls the next poll forward
           immediately on a real event. */
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
    /* Adopt the probe's unit as the core's own. The media's BSD unit is
       not stable (-1 when empty at open; changes across eject/reinsert);
       events with a fresh probe carry r.bsd_unit directly, but the
       error/device_removed fallback in fill_event_base reads w->bsd_unit
       — which must therefore track the last OBSERVED unit, not the
       open-time one. The CORE owns this update: pushing it to adapters
       would leave pure-only behavior wrong and make every adapter
       rediscover the obligation. */
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

    /* Same-state media swap (F1) → media_changed, while the drive stays READY
       across two probes (a fast slot-load swap, or eject/reinsert that fell
       entirely between polls). Two independent signals:

       1. Registry-identity change — the whole-disk IOMedia registry entry ID
          re-mints on a physical swap even when the MMC profile is unchanged
          (the same-profile DVD→DVD case a profile compare would miss). This is
          the strong signal; both ids must be non-zero (0 = identity
          unavailable, never inferred from).

       2. Profile-class change with NO usable identity — some USB-ATAPI bridges
          never expose a media_id (both 0). There a changed *non-zero* current
          profile (e.g. CD-ROM 0x08 → DVD-ROM 0x10) is the only evidence a swap
          happened, so we use last_profile as the fallback fingerprint. This
          still cannot see a same-class swap (DVD-R → DVD-R) on such bridges —
          no signal exists there — but it catches cross-class swaps that would
          otherwise be silent. */
    bool id_changed =
        r.media_id != 0 && w->last_media_id != 0 &&
        r.media_id != w->last_media_id;
    bool profile_class_changed_without_id =
        r.media_id == 0 && w->last_media_id == 0 &&
        r.current_profile != 0 && w->last_profile != 0 &&
        r.current_profile != w->last_profile;

    if (r.state == MOS_STATE_READY && w->last_state == MOS_STATE_READY &&
        (id_changed || profile_class_changed_without_id)) {
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

    /* No change → reschedule and sleep, no event. The poll-rate
       choice still uses the (unchanged) current state.

       Adopt any *informative* identity the probe carried before sleeping.
       media_id / current_profile that were unavailable (0) at the last
       event often arrive one probe later — the whole-disk IOMedia child
       registers a beat after TUR goes GOOD, and profile enrichment can
       fail transiently. Zero means "unavailable", never an observation:
       a non-zero value is adopted, a zero never overwrites one, so the
       fingerprint survives an unavailability gap and an identity change
       observed across the gap is still detected as a swap. Without this,
       a 0→non-zero arrival would pin the snapshot-era fingerprint for the
       whole session and permanently disarm same-state swap detection. */
    if (r.media_id != 0)        w->last_media_id = r.media_id;
    if (r.current_profile != 0) w->last_profile  = r.current_profile;
    w->next_poll_at_mono_ms = probe_end_mono + poll_ms_for_state(w, r.state);
    d.kind                 = MOS_WATCH_DECIDE_SLEEP_UNTIL;
    d.next_poll_at_mono_ms = w->next_poll_at_mono_ms;
    return d;
}

/* ---- Watch-all multiplexer (DR pivot Phase 2b) --------------------- *
 *
 * See mos_pure.h for the contract. Iteration order is ascending
 * registry_id over active slots on EVERY entry, so same-tick event
 * interleave is deterministic and independent of slot assignment
 * history — the property the fixture tests pin. */

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

    /* Dedupe by attachment identity: the DR Appeared notification can
       announce a device the open-time snapshot already carried (or
       fire twice across a bus rescan). Same id ⇒ same plug session ⇒
       same slot; a REPLUG has a fresh id by xnu construction and lands
       in a new slot. */
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

    /* Visit active slots in ascending registry_id (selection scan; CAP
       is 16, an index sort would be ceremony). Returning on the first
       EMIT keeps per-call work bounded; the next call re-enters at the
       lowest id, so same-tick events drain in id order. */
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
            /* Relabel the join's SNAPSHOT — and only the snapshot. A
               mid-stream device whose first pumps yield ERROR events
               (probe failing right after hot-plug) keeps its pending
               join, so the eventual first successful probe still
               announces it as device_appeared; clearing on any first
               event would silently demote it to a mid-stream snapshot
               (contract: every joining drive emits device_appeared). */
            if (a->join_pending[best] &&
                d.event.kind == MOS_EVENT_SNAPSHOT) {
                d.event.kind = MOS_EVENT_DEVICE_APPEARED;
                a->join_pending[best] = false;
            }
            if (d.event.kind == MOS_EVENT_DEVICE_REMOVED) {
                /* Per-slot, not stream-terminal: free the slot AFTER
                   taking the event. A replug arrives as a new id. */
                a->active[best] = false;
            }
            return d;
        }
        if (d.kind == MOS_WATCH_DECIDE_TERMINAL) {
            /* The core's device_removed was emitted on an earlier call
               and the slot somehow pumped again (external notify after
               emission). Nothing to emit — just free the slot. */
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

/* ==== src/mos_state.c ==== */
/*
 * mos_state.c — Apple-side adapter for the pure decision-tree core.
 *
 * Fills mos_state_env_t from a mos_handle_t and calls the pure core; the
 * split lets tests/test_state_core.c drive the tree with scripted MMC
 * responses instead of real hardware. Contract:
 * mos_internal_query_state_core in mos_pure.h.
 */


/* vtable trampolines, static — only this file binds the Apple ops table. */

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

    /* Disc-completion data (blank vs finalized) is deliberately NOT an
       enrichment branch here: no state decision needs it, so it ships as
       the on-demand typed query instead (mos_query_disc_info, mos_scsi.c;
       ARCHITECTURE.md §4.4). */

    mos_error rc = mos_internal_query_state_core(&env, &h->result);
    if (rc == MOS_OK) *out = &h->result;
    return rc;
}

/* ==== src/mos_watch.c ==== */
/*
 * mos_watch.c — Apple-side adapter for the pure watch state machine.
 *
 * The state machine lives in src/mos_watch_core.c (pure, no IOKit).
 * This file does three things:
 *
 *   1. Implements the public watch API (mos_watch_open_by_bsd_name,
 *      mos_watch_open_by_index, mos_watch_next_event, mos_watch_close).
 *
 *   2. Wires the mos_watch_ops_t vtable to real implementations:
 *      - probe()    → open mos_handle_t, query state, close handle
 *      - mono_ms()  → CLOCK_MONOTONIC in milliseconds (scheduling)
 *      - wall_ms()  → CLOCK_REALTIME in milliseconds (stream_open_ms / ts)
 *
 *   3. Registers kIOGeneralInterest notifications on the watched drive
 *      so device removal wakes the run loop and triggers a clean
 *      terminal event without waiting for the next scheduled poll.
 *
 * The per-probe open/close cycle is deliberate: a held handle keeps the
 * drive reserved for the whole watch, conflicting with DiskArbitration,
 * Finder, and other tools. A fresh handle per probe also tolerates a
 * transient driver detach without poisoning later polls. The retained
 * io_service_t we hold for the notification is just an IOKit reference,
 * not an active client connection.
 *
 * Threading: single-threaded by contract. The notification callback
 * fires on the run loop thread, which is the same thread calling
 * mos_watch_next_event. No locking needed.
 */

/* Must precede any system header so BSD extensions stay visible on
   Apple's SDK. The strlcpy call sites this originally served moved to
   mos_scsi.c during the string-copy normalization; the define stays
   because the amalgamation concatenates the adapter TUs into one
   feature-macro environment (scripts/amalgamate.sh adds a prologue copy of these (the per-TU defines stay as #ifndef no-ops) for its
   prologue), and dropping it here would make the standalone-TU and
   amalgamated builds see different SDK surfaces. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

/* CLOCK_MONOTONIC and clock_gettime require POSIX.1-2008. */
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

/* Private run-loop mode for the watch's IOKit and DA sources — never
   kCFRunLoopDefaultMode — so a host's default-mode work can't dispatch our
   callbacks and our CFRunLoopStop can't halt a run-loop invocation the host
   owns. The pump runs this same mode, so our sources fire only while
   mos_watch_next_event is waiting. (Caller-facing contract in mos.h.) */
#define MOS_WATCH_RUN_LOOP_MODE CFSTR("io.github.napieraj.mac-optical-state.watch")

/* ---- Public opaque type --------------------------------------------- */

struct mos_watch {
    /* Pure state machine. Owns the session identity / bsd_unit / seq state. */
    mos_watch_state core;

    /* What we're watching: the whole-disk unit (N in "diskN"), or -1 for
       an empty/open-tray drive. Tags emitted events and drives the Disk
       Arbitration wake filter — NOT the authority for which physical
       drive to probe. The actual probe identity is `registry_id` below. */
    int64_t bsd_unit;

    /* IORegistry entry ID of the physical drive this watch is bound to,
       captured at construction. watch_probe reopens the SAME drive every
       poll via mos_internal_open_by_registry_id, regardless of any BSD-name
       reassignment — so a session bound to drive A keeps probing A even if
       A's name is later recycled to a drive B on the same port. When A is
       terminated the reopen returns NO_DEVICE → terminal device_removed.
       registry_id (not the BSD name) is the single probe-identity authority. */
    uint64_t registry_id;

    /* Retained IOKit reference for the notification. Released in close. */
    io_service_t svc;

    /* Notification plumbing. The notification fires on the run loop
       this port is scheduled on (which is the caller's run loop, set
       up in watch_open). The token must be released in close. */
    IONotificationPortRef notify_port;
    io_object_t           notify_token;       /* kIOGeneralInterest */
    CFRunLoopSourceRef    notify_source;
    CFRunLoopRef          run_loop;

    /* DiscRecording doorbell for media/tray-change wake-up (Phase 2a of
       the DR pivot: replaced the DiskArbitration session — DR's
       StatusChanged is device-scoped, so it also wakes on tray-open /
       no-media drives where DA's media-scoped, bsd_unit-filtered wake
       matched nothing). The callback calls
       mos_internal_watch_notify_wake() to pull the next poll forward
       and CFRunLoopStop() to break the pump's current sleep. Both
       fields NULL on poll-only fallback (center or run-loop source
       creation failed at open time) — polling is the correctness
       floor, the doorbell is latency only. */
    DRNotificationCenterRef dr_center;
    CFRunLoopSourceRef      dr_source;

    /* Storage for the most recent event so mos_watch_next_event can
       return borrowed pointers that remain valid until the next call.
       Session identity (registry_id, stream_open_wall_ms) and bsd_unit
       are plain values with no pointer lifetime; vendor / product /
       revision point into the watch-owned buffers below. */
    mos_watch_event last_event;

    /* Device-static identity, captured ONCE from the validated open
       handle (whose strings come from the DR directory) and owned by
       the watch for its whole life. Events point here; per-probe
       handles never contribute identity (the per-probe re-home this
       replaced — and the v0.3.2 use-after-free class it existed to
       contain — retired with DR pivot Phase 2a). Widths are the SPC-4
       INQUIRY field widths the directory data is parsed from:
         vendor[9]    VENDOR_IDENTIFICATION   ( 8 + NUL)
         product[17]  PRODUCT_IDENTIFICATION  (16 + NUL)
         revision[5]  PRODUCT_REVISION_LEVEL  ( 4 + NUL, SPC-4 §6.4.2) */
    char vendor[9];
    char product[17];
    char revision[5];

    /* ---- Watch-all mode (DR pivot Phase 2b) ------------------------ *
     * all_mode selects the multiplexer path: `all` is the pure fan-in
     * over per-slot cores, `slots` is the adapter-side per-device probe
     * context (registry id + watch-static identity) each core's ctx
     * points at. Single-target fields above (core, svc, notify_*,
     * registry_id, identity buffers) are unused in all mode; bsd_unit
     * stays -1. Poll rates are kept for mid-stream joins. */
    bool                 all_mode;
    mos_watch_all_state  all;
    struct mos_watch_slot {
        uint64_t registry_id;
        char     vendor[9];
        char     product[17];
        char     revision[5];
    }                    slots[MOS_WATCH_ALL_CAP];
    uint32_t             stable_poll_ms;
    uint32_t             transition_poll_ms;
};

/* ---- Time --------------------------------------------------------- */

/* Monotonic milliseconds. CLOCK_MONOTONIC is available on macOS 10.12+;
   we're already floor 12.0 (Monterey) so this is unconditional. */
static uint64_t monotonic_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Wall-clock milliseconds for the session-open timestamp
   (stream_open_ms is documented as real epoch ms, not monotonic ms).
   Used only at watch open, via the monotonicized wrapper below. */
static uint64_t wall_clock_ms(void)
{
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Per-process monotonicized wall ms for the session-open timestamp
   (P4). Two watches opened on the same drive in the same wall-clock
   millisecond would otherwise share a (registry_id, stream_open_ms)
   pair, since per-drive uniqueness rides on the wall component.
   Bumping a same-or-earlier reading to last+1 keeps the value
   epoch-ms-shaped — rough cross-run orderability preserved — while
   guaranteeing per-process uniqueness even across NTP step-backs.
   Event `ts` is unaffected: it reads wall_clock_ms() fresh at every
   emit. */
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
        /* prev was reloaded by the failed CAS; recompute and retry. */
    }
}

/* ---- vtable callbacks -------------------------------------------- */

/* probe: reopen a fresh handle by the watch's registry id (not BSD name —
   see the registry_id field), query state, close. Handle-per-probe lets a
   transient driver detach recover on the next poll.

   POINTER-LIFETIME INVARIANT (adapter-scoped, and it must be — the pure
   layer forwards `const char *` fields verbatim and is structurally blind
   to the fact that one is borrowed from a handle this adapter is about to
   close; the pure tests/fuzzers therefore cannot catch a violation):
   before any mos_close(h), every handle-borrowed pointer field of the
   escaping struct must be REPLACED — identity fields point at the
   watch-static buffers captured at open (w->vendor / w->product /
   w->revision; device-static data, so per-probe refresh carried no
   information) — or set NULL. The footgun is `*out = *qr;` — it copies
   every pointer verbatim, so "forgot one" is the default, not the
   exception (the v0.3.2 revision use-after-free was exactly this: it
   rode the struct copy un-replaced). Any NEW borrowed pointer added to
   mos_watch_event / mos_state_result needs a watch-lifetime backing
   store and a replacement below. (bsd_unit is a value, never replaced.) */
static mos_error watch_probe(void *ctx, mos_state_result *out)
{
    mos_watch_t *w = (mos_watch_t *)ctx;
    if (!w || !out) return MOS_ERR_INVALID_ARG;

    /* Reopen by registry ID, not BSD name (see registry_id field): the
       original entry still exists and we get the SAME drive back, or it has
       been terminated and we get NO_DEVICE — which the core treats as
       terminal removal. */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_internal_open_by_registry_id(w->registry_id, &err);
    if (!h) {
        /* Contract is NULL iff err != MOS_OK; force non-OK if ever violated,
           so the core never reads garbage state on a NULL handle. */
        return err != MOS_OK ? err : MOS_ERR_IO;
    }

    const mos_state_result *qr = NULL;
    mos_error qerr = mos_query_state(h, &qr);
    if (qerr != MOS_OK || !qr) {
        mos_close(h);
        return qerr != MOS_OK ? qerr : MOS_ERR_IO;
    }

    /* Copy the handle-owned result into the caller's struct so its
       identity strings can be re-homed below and survive mos_close(h). */
    *out = *qr;

    /* The drive is pinned by registry ID, but the media's BSD unit is not
       stable (-1 when empty at open; changes across eject/reinsert). Refresh
       the ADAPTER's copy for the DA wake filter; the pure core adopts the
       probe's unit itself on every successful pump (mos_watch_core.c), so
       the error/device_removed fallback no longer depends on this adapter
       — a layering obligation retired by the third review. media_id (the
       F1 swap fingerprint) needs no manual tracking — it rides the
       *out = *qr copy and the core reads it from the result. */
    w->bsd_unit = out->bsd_unit;
    /* Replace the three handle-borrowed identity pointers with the
       watch-static identity captured at open (the lifetime invariant
       above): they must not survive the mos_close(h) below. Identity is
       device-static directory data, so the per-probe handle's copy is
       byte-identical to the open-time capture — repointing loses
       nothing and removes the per-probe re-home entirely. */
    out->vendor   = w->vendor[0]   ? w->vendor   : NULL;
    out->product  = w->product[0]  ? w->product  : NULL;
    out->revision = w->revision[0] ? w->revision : NULL;

    mos_close(h);
    return MOS_OK;
}

/* Monotonic ms callback for the watch core. Used for poll deadline
   scheduling and latency measurement. CLOCK_MONOTONIC only. */
static uint64_t watch_mono_ms(void *ctx)
{
    (void)ctx;
    return monotonic_ms();
}

/* Wall-clock ms callback for the watch core. Used only for event ts
   formatting. CLOCK_REALTIME (Unix epoch ms). MUST NOT be used for
   scheduling — clock can jump backward. */
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

/* Per-slot probe for watch-all: identical contract to watch_probe, but
   ctx is the slot (its own registry id + watch-static identity). The
   same pointer-lifetime invariant applies: identity fields are
   repointed at slot-lifetime storage before the handle closes. */
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

    mos_close(h);
    return MOS_OK;
}

static const mos_watch_ops_t apple_watch_slot_ops = {
    .probe   = watch_slot_probe,
    .mono_ms = watch_mono_ms,
    .wall_ms = watch_wall_ms,
};

/* Add one device to an all-watch from a DR snapshot record. The slot's
   ctx storage is claimed via the same first-free scan add() uses (the
   single-thread contract makes the two scans agree); on dedupe the
   pre-filled slot entry is simply unused. */
static void watch_all_add_device(mos_watch_t *w,
                                 const mos_internal_dr_snapshot *snap,
                                 bool mid_stream)
{
    if (!w || !snap || snap->registry_id == 0) return;

    int i = mos_internal_watch_all_free_slot(&w->all);
    if (i < 0 && mos_internal_watch_all_find(&w->all, snap->registry_id) < 0) {
        return; /* full and genuinely new — documented drop until a slot frees */
    }
    if (i >= 0) {
        /* Width-agreement pins: source and destination both carry the
           SPC-4 identity widths, so these copies can never truncate.
           Successor of the retired INQUIRY path's per-site asserts. */
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
    }
    (void)mos_internal_watch_all_add(&w->all, &apple_watch_slot_ops,
                                     i >= 0 ? &w->slots[i] : NULL,
                                     snap->bsd_unit, snap->registry_id,
                                     monotonic_ms(), stream_epoch_wall_ms(),
                                     w->stable_poll_ms, w->transition_poll_ms,
                                     mid_stream);
}

/* ---- Notification handler ---------------------------------------- *
 *
 * Fires on the run loop thread for kIOGeneralInterest messages on the
 * matched io_service_t. Do NOT also subscribe to kIOBusyInterest: issuing
 * a probe changes the drive's busy state, which would fire the notification
 * and schedule another probe — a live loop.
 *
 * Message handling:
 *   1. kIOMessageServiceIsTerminated → TERMINAL: notify_removed; pump emits
 *      device_removed.
 *   2. kIOMessageServicePropertyChange → WAKE: notify_wake; pump re-probes.
 *      Tracks drive state, not client state, so it does NOT fire on our own
 *      per-probe MMC user-client open/close — safe to wake on.
 *   3. Everything else IGNORED — including IsAttemptingOpen / WasClosed /
 *      BusyStateChange, which fire on ANY user-client open/close (our own
 *      probes included) and would self-trigger. Whether they can be used
 *      safely is deferred to v0.4 pending the empirical probe.
 *
 * messageType is natural_t here vs uint32_t in the SDK's
 * IOServiceInterestCallback typedef; both are `unsigned int`, so the
 * function-pointer types are compatible and the -Werror check is satisfied. */
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
        /* TERMINAL. Drive went away. */
        mos_internal_watch_notify_removed(&w->core);
        break;

    case kIOMessageServicePropertyChange:
        /* WAKE. A registry property changed — possibly a media-state
           transition from the kernel's MMC stack. Pull the next-poll
           deadline forward so the pump probes now. False wakes are cheap
           (one probe, no state mutation if unchanged). */
        mos_internal_watch_notify_wake(&w->core);
        break;

    default:
        /* IGNORED. Other messages (IsRequestingClose, power-state
           transitions, system sleep notifications, AND the
           IsAttemptingOpen/WasClosed/BusyStateChange family that
           self-trigger on our own probe handles — see header
           comment for the deferred-v0.4 plan). */
        return;
    }

    /* For both TERMINAL and WAKE: break the pump's CFRunLoopRunInMode
       sleep so the next mos_watch_next_event call returns promptly. */
    if (w->run_loop) {
        CFRunLoopStop(w->run_loop);
    }
}

/* ---- Wake source: DiscRecording doorbell --------------------------- *
 *
 * kDRDeviceStatusChangedNotification fires when a device's status dict
 * changes (media in/out, tray, busy), collapsing worst-case
 * insert→event latency from stable_poll_ms (default 2s) to however
 * long DR takes. Polling is the correctness floor; the doorbell is
 * latency only, so any setup failure falls back to poll-only
 * (dr_center/dr_source stay NULL; close treats NULL as a no-op).
 *
 * DR's notification is DEVICE-scoped (not media-scoped), so this
 * doorbell also rings for tray-open / no-media drives (DR pivot
 * Phase 2a, doc/research/2026-06-10-dr-pivot-implementation-plan.md).
 *
 * The callback filters by registry ID — a parameter, not a structural
 * assumption, so a future watch-all mode widens the filter rather than
 * rewiring the pump (plan, Phase 2b). Filtering is fail-OPEN: if the
 * event's device can't be resolved to an ID, wake anyway — a false
 * wake is one cheap probe, a missed wake is stable_poll_ms of latency.
 * DR data never decides state; the wake only schedules the MMC probe.
 */

static void dr_status_changed_callback(DRNotificationCenterRef center,
                                       void *observer, CFStringRef name,
                                       DRTypeRef object,
                                       CFDictionaryRef info)
{
    /* Fires on the run loop the DR source is scheduled on (the caller's
       run loop, same as our IONotificationPort source). */
    (void)center; (void)name; (void)info;

    mos_watch_t *w = (mos_watch_t *)observer;
    if (!w) return;

    /* Per-device filter by registry ID (the watch's one identity
       authority). object is the DRDeviceRef that changed; resolve its
       registry path → entry ID and compare. Any resolution failure
       wakes anyway (fail-open, see design block). In all mode the
       filter routes instead of rejects: wake the matching slot, or
       every slot when unresolved. */
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
        int slot = (id != 0) ? mos_internal_watch_all_find(&w->all, id) : -1;
        if (slot >= 0) {
            mos_internal_watch_notify_wake(&w->all.cores[slot]);
        } else if (id == 0) {
            for (int i = 0; i < MOS_WATCH_ALL_CAP; ++i) {
                if (w->all.active[i]) {
                    mos_internal_watch_notify_wake(&w->all.cores[i]);
                }
            }
        }
        /* id resolved but unknown: a device we are not watching (cap
           overflow) or one Appeared hasn't delivered yet — the
           Appeared handler owns joins; nothing to wake. */
        if (w->run_loop) CFRunLoopStop(w->run_loop);
        return;
    }

    if (id != 0 && w->registry_id != 0 && id != w->registry_id) {
        return; /* another drive */
    }

    mos_internal_watch_notify_wake(&w->core);
    /* Break the pump's CFRunLoopRunInMode sleep — same pattern as
       watch_interest_callback's termination handling. The next
       mos_watch_next_event call re-probes immediately. */
    if (w->run_loop) {
        CFRunLoopStop(w->run_loop);
    }
}

/* Set up an IOKit interest notification (kIOGeneralInterest) for
   service termination. Called from watch_open_common after the pure
   watch core is initialized. Each step that fails tears down what
   it created and returns leaving every field NULL — caller falls
   back to poll-only for this mechanism (the DA path below is
   independent). The invariant this maintains: after this function
   returns, w->notify_port is non-NULL iff w->notify_source is also
   non-NULL AND a kIOGeneralInterest notification is registered.
   That invariant is what the pump's run-loop gate depends on. */
static void setup_iokit_interest_wake(mos_watch_t *w)
{
    if (!w || !w->run_loop || w->svc == IO_OBJECT_NULL) return;

    w->notify_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!w->notify_port) return;

    w->notify_source = IONotificationPortGetRunLoopSource(w->notify_port);
    if (!w->notify_source) {
        /* Port created but cannot acquire its run-loop source — tear
           the port down immediately. Leaving w->notify_port set
           without a live source would make the pump's run-loop gate
           enter CFRunLoopRunInMode in a mode with no sources, which
           returns instantly and tight-loops until the caller's
           timeout fires. */
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
        /* Notification registration failed; remove the source and
           tear down the port. DA below is unaffected. */
        CFRunLoopRemoveSource(w->run_loop, w->notify_source,
                              MOS_WATCH_RUN_LOOP_MODE);
        w->notify_source = NULL;
        IONotificationPortDestroy(w->notify_port);
        w->notify_port   = NULL;
        return;
    }

    /* kIOBusyInterest deliberately NOT registered: BusyStateChange fires on
       every user-client open/close — including our own per-probe MMC
       user-clients — so dispatching it would self-trigger a tight probe
       loop. Revisiting it is a v0.4 question for the empirical probe. */
}

/* Tear down the IOKit interest notification in reverse order:
   remove source from run loop (no more callbacks), release the
   notification token, destroy the port. Safe to call with NULL /
   poll-only state. Called from mos_watch_close. */
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

/* All-mode lifecycle (Phase 2b): Appeared joins a device to the
   stream (its first event is relabeled device_appeared by the pure
   multiplexer), Disappeared marks the slot's core removed so its next
   pump emits a per-device device_removed. Both are load-bearing in
   all mode only — single-target watches keep kIOGeneralInterest as
   their terminal-removal source and never register these. */
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
        mos_internal_watch_notify_removed(&w->all.cores[slot]);
    }
    /* Unresolved id: the probe floor catches it — the slot's next
       reopen returns NO_DEVICE, which the core treats as removal. */
    if (w->run_loop) CFRunLoopStop(w->run_loop);
}

/* Set up the DR notification center and register the StatusChanged
   observer. Independent of the IOKit interest wake — either or both
   may fail soft to poll-only. Stores center + source on success;
   leaves both NULL on any failure. */
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

    /* Register LAST: once observed, callbacks can fire, so every prior
       step must already be safe to be live. object=NULL observes all
       devices; the callback filters by registry ID (fail-open). In all
       mode the Appeared/Disappeared lifecycle observers join here —
       they are what makes the bus stream live. */
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

/* Tear down the DR doorbell in reverse order: remove the observer (no
   more callbacks), remove the source from the run loop, release both.
   Safe to call with NULL/poll-only state. Called from mos_watch_close. */
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

/* Takes ownership of `h` and either builds a watch around it or closes it
   on failure. The single funnel both public open entry points go through —
   neither re-resolves the drive by name internally. The watch's bsd_unit
   comes from the handle's resolved unit (mos_handle_bsd_unit), not a
   caller-supplied string, so unit and service identity stay consistent. */
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

    /* An empty/open-tray drive has no unit (mos_handle_bsd_unit returns -1).
       Not a failure for the watch — identity is the registry_id captured
       below, and the DR doorbell is device-scoped, so a nameless drive
       gets the same wake coverage as a named one. */
    const int64_t bsd_unit = mos_handle_bsd_unit(h);   /* -1 if empty */

    mos_watch_t *w = (mos_watch_t *)calloc(1, sizeof(*w));
    if (!w) {
        mos_close(h);
        if (err_out) *err_out = MOS_ERR_OOM;
        return NULL;
    }

    /* Plain value copy; -1 (empty drive) carries through unchanged. */
    w->bsd_unit = bsd_unit;

    /* Capture the validated io_service_t before mos_close — this is the
       identity the watch preserves (the registry_id below is taken from it,
       and per-poll reopen targets that, immune to BSD-name reassignment).
       Refcount: handle_get_service returns it without a retain, so retain
       here (mos_close drops the handle's own) and release in
       mos_watch_close. If the retain fails, leave w->svc NULL and fall back
       to poll-only — safer than holding a service we don't own. */
    io_service_t validated_svc = mos_internal_handle_get_service(h);
    if (validated_svc != IO_OBJECT_NULL &&
        IOObjectRetain(validated_svc) == KERN_SUCCESS) {
        w->svc = validated_svc;
    }

    /* Capture the registry-entry ID — the identity authority watch_probe
       reopens by each poll. Fail closed if it can't be captured: a BSD-name
       fallback would reintroduce the two-identity bug this path closes. */
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

    /* Capture the device-static identity strings ONCE, before the
       validation handle closes — they came from the DR directory at
       open. Events point at these watch-owned buffers for the watch's
       whole life; per-probe handles never contribute identity (see the
       buffer comment in struct mos_watch). strlcpy truncation cannot
       trigger — pinned at build time: */
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

    /* Initialize the pure state machine BEFORE registering any
       callbacks that mutate it. The pure init has no failure path
       and depends on nothing beyond monotonic_ms() / wall_clock_ms(),
       so doing it first costs nothing and makes early callback
       delivery harmless by construction. */
    mos_internal_watch_init(&w->core, &apple_watch_ops, w,
                            w->bsd_unit,
                            /*registry_id=*/w->registry_id,
                            /*start_mono_ms=*/monotonic_ms(),
                            /*start_wall_ms=*/stream_epoch_wall_ms(),
                            stable_poll_ms,
                            transition_poll_ms);

    /* Capture the caller's run loop once so the IOKit and DiscRecording
       wake sources can be scheduled independently. Both are best-effort;
       either can succeed without the other, and both can fail to
       poll-only without affecting correctness. */
    w->run_loop = CFRunLoopGetCurrent();

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
    /* Resolve once via BSD name (this entry point's contract is "find
       me whatever drive currently has this BSD name"). The handle that
       comes back is validated against the storage-device class
       hierarchy; watch_open_from_validated_handle then preserves THAT
       service identity — no second BSD-name resolution. */
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
       IORegistryEntryIDMatching internally, so the handle carries the exact
       peripheral selected at enumeration time, not whatever currently holds
       its BSD name; watch_open_from_validated_handle preserves that identity
       (no second resolution). */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_index(one_based, &err);
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
    w->bsd_unit           = -1;   /* no single unit; accessor contract */
    w->stable_poll_ms     = stable_poll_ms;
    w->transition_poll_ms = transition_poll_ms;
    mos_internal_watch_all_init(&w->all);

    /* Initial population from ONE directory snapshot — no per-device
       opens (the first probe validates each device; a vanished one
       yields its device_removed through the normal path). Zero devices
       is a valid empty stream: the Appeared observer below is what
       makes it live. */
    mos_internal_dr_snapshot snap[MOS_WATCH_ALL_CAP];
    size_t n = mos_internal_dr_copy_snapshot(snap, MOS_WATCH_ALL_CAP);
    for (size_t i = 0; i < n; ++i) {
        watch_all_add_device(w, &snap[i], /*mid_stream=*/false);
    }

    w->run_loop = CFRunLoopGetCurrent();
    setup_dr_doorbell_wake(w);
    /* No kIOGeneralInterest in all mode: removal rides DR Disappeared
       (fast path) + per-probe NO_DEVICE (floor). Poll-only fallback if
       DR setup failed, exactly like single mode. */

    if (err_out) *err_out = MOS_OK;
    return w;
}

/* Safe to call on NULL; do not call twice (mos_close convention). Order is
   load-bearing — stop callbacks before releasing the memory they reference:
   DR doorbell, then IOKit interest wake, then the retained io_service_t.
   (Each teardown helper enforces its own internal stop-before-free order.) */
void mos_watch_close(mos_watch_t *w)
{
    if (!w) return;

    teardown_dr_doorbell_wake(w);
    teardown_iokit_interest_wake(w);

    if (w->svc != IO_OBJECT_NULL) {
        IOObjectRelease(w->svc);
    }
    free(w);
}

int64_t mos_watch_bsd_unit(const mos_watch_t *w)
{
    /* -1 for NULL, for a media-less single-target watch, and always
       for an all-watch (no single unit; demux per event instead). */
    if (!w) return -1;
    return w->bsd_unit;
}

/* ---- Pump --------------------------------------------------------- */

mos_error mos_watch_next_event(mos_watch_t *w, const mos_watch_event **out,
                               int timeout_ms)
{
    if (out) *out = NULL;
    if (!w || !out) return MOS_ERR_INVALID_ARG;

    /* The pump may need to spin a couple of times — pump → SLEEP_UNTIL →
       wait → pump → EMIT_EVENT — within a single user-visible call.
       We bound that with the user's timeout_ms; if we exhaust it without
       producing an event, return MOS_ERR_TIMEOUT and the caller's outer
       loop decides whether to call again. */
    uint64_t start = monotonic_ms();
    uint64_t deadline = (timeout_ms < 0)
        ? UINT64_MAX
        : start + (uint64_t)timeout_ms;

    for (;;) {
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

        /* SLEEP_UNTIL: block until next_poll_at_mono_ms or until a
           notification fires (which calls CFRunLoopStop). Use
           CFRunLoopRunInMode with a bounded interval. The deadline
           is a monotonic-clock value, compared against monotonic_ms()
           the same way the pump compares. */
        uint64_t now = monotonic_ms();
        if (now >= deadline) {
            /* Caller-side timeout elapsed without producing an event.
               Pump again next time the caller asks. */
            return MOS_ERR_TIMEOUT;
        }

        uint64_t sleep_until_ms = d.next_poll_at_mono_ms;
        if (sleep_until_ms > deadline) sleep_until_ms = deadline;
        if (sleep_until_ms < now) sleep_until_ms = now;
        double interval_sec = (double)(sleep_until_ms - now) / 1000.0;

        /* Only wait on the run loop if a source is actually scheduled
           (notify_source or dr_source) — an empty mode returns instantly
           and tight-loops — AND we are on the thread that owns it. The
           documented contract is open/pump/close on one thread; if a
           caller violates it anyway, CFRunLoopRunInMode here would run
           the WRONG thread's loop, where our private mode has no sources,
           returning instantly and burning CPU until timeout_ms. The
           misuse stays misuse (notification wakes can't reach a foreign
           thread's loop), but it degrades to honest nanosleep polling
           instead of a busy-spin. */
        if (w->run_loop && (w->notify_source || w->dr_source) &&
            CFRunLoopGetCurrent() == w->run_loop) {
            /* Returns on timer, CFRunLoopStop (a notification callback), or
               a handled source — any is a wake; loop back to pump. Private
               mode keeps us off host-app default-mode sources. */
            CFRunLoopRunInMode(MOS_WATCH_RUN_LOOP_MODE, interval_sec, false);
        } else {
            /* No notification source set up; fall back to nanosleep.
               This path is taken when the handle didn't surface a
               validated io_service_t (poll-only mode) or when BOTH
               notification registration paths failed at open time. */
            struct timespec ts;
            ts.tv_sec  = (time_t)(sleep_until_ms - now) / 1000;
            ts.tv_nsec = (long)((sleep_until_ms - now) % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
        /* Loop and re-pump. */
    }
}

/* ==== src/mos_strings.c ==== */
/*
 * mos_strings.c — pure string tables, escapers, and version. Separate TU
 * so mos_scsi.c stays exclusively IOKit-linked. No IOKit.
 */

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

/* ==== src/mos_dr.c ==== */
/*
 * mos_dr.c — DiscRecording-side adapter: the directory.
 *
 * Doctrine (doc/research/2026-06-10-dr-pivot-implementation-plan.md):
 * DR is the doorbell and the directory; MMC is the inspector. This TU
 * supplies discovery, identity, and addressing from the DiscRecording
 * C API; it never decides drive STATE — the TUR⊕GESN core in
 * mos_state_core.c remains the sole authority (the §5.5 nub gate runs
 * on TUR sense bytes DR does not expose).
 *
 * Command-surface note (AGENTS.md scope doctrine): DR is not a SCSI
 * command author from mos's point of view — it is a substrate above
 * the same kext the MMC path uses. mos still authors exactly one raw
 * CDB (GESN, mos_scsi.c).
 *
 * The one surviving IOKit step (dr-field-mapping §identity): DR
 * exposes a device's IORegistry *path* (kDRDeviceIORegistryEntryPathKey),
 * not its entry ID. mos's identity currency — registry_id in events,
 * the reopen authority, the F1 fingerprint — is the uint64 entry ID,
 * so each path is resolved path → entry → ID here. A node that cannot
 * be resolved is skipped, preserving the enumeration/index ↔
 * open-by-index correspondence the public API documents (same gate
 * the pre-pivot visit_collect applied).
 *
 * KNOWN UNKNOWN (hardware falsification target, plan §coexistence):
 * whether DR's registry path lands on the same IO*BlockStorageDevice
 * node mos's IOKit matching used to produce, or on a neighbor in the
 * stack (e.g. the SCSI peripheral nub). If it's a neighbor, the MMC
 * plug-in attach in mos_internal_open_service fails DRIVER_REJECTED
 * and the Phase 0 probe's Info dumps will show the path shape — fix
 * lands as a path normalization HERE, never as a caller workaround.
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

/* Bounded CFString-ish → C-buffer copy. CFStringGetCString FAILS
   outright (no partial-write contract we may rely on) when the buffer
   is too small, so conversion goes through a generous temp: values up
   to 255 bytes convert and then strlcpy-truncate to the SPC-4 field
   width; anything larger fails the conversion and yields "" — for
   identity fields whose real domain is ≤16 bytes, an absurdly long
   value is hostile data and empty is the right answer. dst is always
   NUL-terminated. Non-string values (a hostile or surprising
   dictionary) also yield "". */
static void mos_internal_dr_copy_string(CFTypeRef value,
                                        char *dst, size_t cap)
{
    if (cap == 0) return;
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
   documented "unavailable" sentinel, never a fabricated ID). Exported
   to the watch adapter: the DR doorbell's per-device filter resolves
   the notifying device the same way (decl in mos_internal.h). */
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

/* The ONE extraction of the three identity strings from an Info
   dictionary — every reader (snapshot builder, open-time identity for
   a service) funnels through here so a future gate on the extraction
   (encoding validation, width policy) has a single home. Buffers keep
   the SPC-4 field widths; bounding per mos_internal_dr_copy_string. */
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

/* The media BSD name lives in the Status dictionary's media-info
   sub-dictionary (kDRDeviceMediaInfoKey → kDRDeviceMediaBSDNameKey),
   media-scoped exactly like the pre-pivot IOMedia walk: absent when
   no media is loaded, hence unit -1. */
static int64_t mos_internal_dr_bsd_unit_from_status(CFDictionaryRef status)
{
    CFTypeRef mi = CFDictionaryGetValue(status, kDRDeviceMediaInfoKey);
    if (!mi || CFGetTypeID(mi) != CFDictionaryGetTypeID()) return -1;

    char name[MOS_BSD_NAME_CAP];
    mos_internal_dr_copy_string(
        CFDictionaryGetValue((CFDictionaryRef)mi, kDRDeviceMediaBSDNameKey),
        name, sizeof name);
    if (name[0] == 0) return -1;
    /* parse_bsd_unit normalizes rdisk/ /dev/ forms and rejects
       partition shapes — same authority as everywhere else. */
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
    /* No reopenable identity ⇒ not usable (see header comment). */
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
        /* Skip-not-fail per device: an unresolvable entry must not hide
           its siblings. The index is the position among reopenable
           devices — DR array order whenever every device resolves, the
           expected case. */
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
    /* The header documents the plain "diskN" form for this call —
       callers pass the canonical mos_bsd_name_format rendering. */
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
    if (vcap) vendor[0] = 0;
    if (pcap) product[0] = 0;
    if (rcap) revision[0] = 0;
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
    if (!dev) return false; /* identity stays empty — same non-fatal
                               contract the INQUIRY failure path had */

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

/* ==== src/mos_scsi.c ==== */
/*
 * mos_scsi.c — IOKit lifecycle, enumeration, MMC convenience wrappers.
 * The only IOKit-linked file in the library.
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

#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- Wire-format compile-time invariants -------------------------- */
/* Pin the 18-byte assumption at compile time, two ways:
   (1) sizeof(SCSI_Sense_Data) == kSenseDefaultSize — Apple's own
       documented size matches their struct layout.
   (2) kSenseDefaultSize == 18 — Apple's constant matches the literal
       18 baked into mos.h and mos_internal.h's sense[18] signatures.
   A failure of (1) means Apple broke their internal contract; of (2)
   means the public API's sense[18] literal needs updating. */
_Static_assert(sizeof(SCSI_Sense_Data) == kSenseDefaultSize,
               "SCSI_Sense_Data size no longer matches kSenseDefaultSize");
_Static_assert(kSenseDefaultSize == 18,
               "kSenseDefaultSize changed — update sense[18] in mos.h and mos_internal.h");

/* ---- Registry helpers --------------------------------------------- */

/* The pure BSD-name helpers live in src/mos_pure.c (declared in mos_pure.h,
   re-included via mos_internal.h) so they link into the pure-only tests. */

/* Resolve the whole-disk BSD unit (the N in "diskN") for an IOKit service,
   or -1 if none (e.g. an empty/open-tray drive with no IOMedia child), and
   capture the whole-disk IOMedia registry entry ID into *media_id_out (0 if
   none). The transient "diskN" name buffers below never escape this
   function — only the parsed integer identity does. */
static int64_t mos_internal_bsd_unit(io_service_t svc, uint64_t *media_id_out)
{
    if (media_id_out) *media_id_out = 0;
    io_iterator_t it MOS_IO_AUTO = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(svc, kIOServicePlane,
            kIORegistryIterateRecursively, &it) != KERN_SUCCESS) {
        return -1;
    }

    /* Two-pass selection:
       1. Normal: an IOMedia whole-disk child exists. Prefer it —
          kIOMediaWholeKey reliably discriminates "disk4" from partition
          children ("disk4s1") on mounted media.
       2. Fallback: some USB bridges expose a BSD name on a non-standard
          IOMedia node with no "Whole" property. Accept any whole-disk-shaped
          name (disk\d+ / rdisk\d+, no partition suffix).
       Pass 1 wins if it finds anything — a Whole node is authoritative.
       Refcounts auto-release via MOS_IO_AUTO / MOS_CF_AUTO (see
       mos_internal.h). */
    char whole_name[MOS_BSD_NAME_CAP] = {0};
    char fallback_name[MOS_BSD_NAME_CAP] = {0};
    uint64_t whole_id = 0;
    uint64_t fallback_id = 0;

    for (;;) {
        io_object_t child MOS_IO_AUTO = IOIteratorNext(it);
        if (child == IO_OBJECT_NULL) break;

        char this_name[MOS_BSD_NAME_CAP] = {0};

        CFTypeRef bsd MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                child, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
        if (bsd && CFGetTypeID(bsd) == CFStringGetTypeID()) {
            /* CFStringGetCString may write partial bytes before returning
               false (Apple spec); clear so the empty check below stays
               authoritative. */
            if (!CFStringGetCString((CFStringRef)bsd, this_name,
                                    sizeof(this_name),
                                    kCFStringEncodingUTF8)) {
                this_name[0] = 0;
            }
        }

        if (this_name[0] == 0) continue;

        /* Pass 1: the authoritative Whole node. Require IOMedia conformance
           — some descendant nodes carry a "Whole" boolean without being
           media. (The string-name IOObjectConformsTo avoids needing
           <IOKit/storage/IOMedia.h>.) The fallback below still takes
           whole-disk-shaped names on non-IOMedia nodes for USB bridges. */
        CFTypeRef whole MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                child, CFSTR("Whole"), kCFAllocatorDefault, 0);
        bool is_whole = (whole &&
                         CFGetTypeID(whole) == CFBooleanGetTypeID() &&
                         CFBooleanGetValue((CFBooleanRef)whole) &&
                         IOObjectConformsTo(child, "IOMedia"));

        if (is_whole && whole_name[0] == 0) {
            strlcpy(whole_name, this_name, sizeof(whole_name));
            /* Capture the whole-disk IOMedia registry entry ID while we
               hold the node — this is the F1 media-swap fingerprint. On
               failure the id stays 0 (the "unavailable" sentinel), which
               the watch core treats as "don't infer a swap". */
            if (IORegistryEntryGetRegistryEntryID(child, &whole_id) != KERN_SUCCESS) {
                whole_id = 0;
            }
        } else if (!is_whole && fallback_name[0] == 0 &&
                   mos_internal_bsd_name_is_whole_shape(this_name)) {
            strlcpy(fallback_name, this_name, sizeof(fallback_name));
            if (IORegistryEntryGetRegistryEntryID(child, &fallback_id) != KERN_SUCCESS) {
                fallback_id = 0;
            }
        }

        if (whole_name[0] != 0) break; /* Whole found, done */
    }

    const char *chosen = whole_name[0] ? whole_name
                       : fallback_name[0] ? fallback_name
                       : NULL;
    if (!chosen) return -1;
    if (media_id_out) *media_id_out = whole_name[0] ? whole_id : fallback_id;
    /* parse_bsd_unit normalizes any rdisk/ /dev/ prefix (some USB bridges
       expose only a raw entry), rejects partition/non-whole shapes, and
       returns the unit or -1. Identity is an integer from here; the
       canonical "diskN" is reconstructed only at output via
       mos_bsd_name_format(). */
    return mos_internal_parse_bsd_unit(chosen);
}

/* ---- Enumeration ---------------------------------------------------- */

#define MOS_ENUM_CAP 64

void mos_enumerate_devices(mos_enumerate_cb cb, void *ctx)
{
    if (!cb) return;

    /* DR-backed (the directory — mos_dr.c): the snapshot arrives in
       DR device-array order, which is the public ordering contract
       (the same array drutil enumerates; drutil parity by provenance,
       not by sort approximation). DR already dedups — one DRDevice
       per drive — so the pre-pivot class-walk dedup died with the
       walk. Identity strings ride the snapshot but mos_device_info_t
       deliberately doesn't surface them yet: identity is handle data
       (open the device), not enumeration data — unchanged contract. */
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

/* Enumeration yields bsd_unit + registry_id only (identity is handle
   data — see mos_enumerate_devices). Returns -1 for an empty/open-tray
   drive. */
int64_t mos_device_info_bsd_unit(const mos_device_info_t *i) { return i ? i->bsd_unit : -1; }
uint64_t mos_device_info_registry_id(const mos_device_info_t *i) { return i ? i->registry_id : 0; }

/* Handle-side accessor. Surfaces the bsd_unit a handle was opened
   against, for partial-failure paths where mos_open_by_*() succeeded but
   a later call (mos_query_state, mos_raw_cdb) returned an error. */
int64_t mos_handle_bsd_unit(const mos_handle_t *h)
{
    /* -1: an empty/open-tray drive carries no unit. */
    return h ? h->bsd_unit : -1;
}

uint64_t mos_internal_device_info_registry_id(const mos_device_info_t *i) {
    return i ? i->registry_id : 0;
}

io_service_t mos_internal_handle_get_service(mos_handle_t *h) {
    /* No retain taken — caller must IOObjectRetain before mos_close(h).
       Lifecycle contract in mos_internal.h. */
    return h ? h->svc : IO_OBJECT_NULL;
}

/* ---- Open / close --------------------------------------------------- */

static mos_handle_t *mos_internal_open_service(io_service_t svc, mos_error *err)
{
    mos_handle_t *h = calloc(1, sizeof(*h));
    if (!h) { IOObjectRelease(svc); if (err) *err = MOS_ERR_OOM; return NULL; }
    h->svc = svc;
    /* Attachment identity, captured once at open. Best-effort like
       bsd_unit below: a failed call leaves the calloc'd 0, which the
       result/JSON contract documents as "unavailable" — never a
       fabricated ID. */
    if (IORegistryEntryGetRegistryEntryID(svc, &h->drive_registry_id)
            != KERN_SUCCESS) {
        h->drive_registry_id = 0;
    }
    /* Best-effort, not a gate (same posture as the enumeration snapshot):
       a nameless empty drive
       opens fine since the MMC plug-in and queries run off `svc`. Must
       assign unconditionally — the handle is calloc'd, so an unset bsd_unit
       would read 0 ("disk0"), a valid unit, not "no media". */
    h->bsd_unit = mos_internal_bsd_unit(svc, &h->media_id);  /* -1 if no media */

    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        svc,
        kIOMMCDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID,
        &h->plugin, &score);
    if (kr != KERN_SUCCESS || !h->plugin) {
        /* Apple's kext declined to attach SCSITaskUserClient. See KNOWN
           UNKNOWNS — the match behavior has shifted across macOS releases. */
        if (err) *err = MOS_ERR_DRIVER_REJECTED;
        mos_close(h);
        return NULL;
    }

    HRESULT hr = (*h->plugin)->QueryInterface(
        h->plugin,
        CFUUIDGetUUIDBytes(kIOMMCDeviceInterfaceID),
        (LPVOID *)&h->mmc);
    if (hr != S_OK || !h->mmc) {
        /* COM contract says a failed QueryInterface leaves the out
           pointer untouched (it's calloc-NULL here); NULL it anyway so
           mos_close can never Release a garbage value if an Apple
           plug-in ever violates that contract. */
        h->mmc = NULL;
        if (err) *err = MOS_ERR_DRIVER_REJECTED;
        mos_close(h);
        return NULL;
    }

    /* Identity from the DR directory (device-static; the open-time
       INQUIRY retired with the DR pivot). Non-fatal on failure —
       empty strings, the same contract the INQUIRY path had. */
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

    /* parse_bsd_unit accepts disk4 / rdisk4 / /dev/ forms and returns -1 for
       anything invalid. An empty drive has no unit, so this never resolves to
       "whichever nameless drive came first" — empty drives are index/
       enumeration-only. (err_out is already MOS_ERR_INVALID_ARG.) This gate
       also preserves the CLI contract: malformed name = invalid_arg/64,
       well-formed-but-absent = no_device/66. */
    int64_t want_unit = mos_internal_parse_bsd_unit(want);
    if (want_unit < 0) return NULL;

    /* DR resolves the name (the directory). Reconstruct the canonical
       "diskN" form first — DRDeviceCopyDeviceForBSDName documents the
       plain /dev entry name, and normalization authority stays with
       parse_bsd_unit, not with whatever prefix the caller typed. The
       resulting registry ID reopens atomically below, same TOCTOU
       posture as open-by-index: a name that DR resolved but whose
       entry terminated in between returns NO_DEVICE, never a
       different drive. This replaced the pre-pivot class-walk-and-
       match enumeration (and dissolved ARCHITECTURE's never-built
       walk-up resolution). */
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

/* Collects registry IDs (not BSD names) for by-index reopen: BSD names can
   shift on hot-plug / IOMedia reattach between the enumerate and reopen
   passes (a TOCTOU race), whereas a registry entry ID is stable for the
   io_service_t's lifetime and reopens atomically via
   IORegistryEntryIDMatching. */
typedef struct {
    uint64_t ids[MOS_ENUM_CAP];
    size_t   count;
} mos_internal_id_collect;

static bool mos_internal_collect_cb(const mos_device_info_t *info, void *ctx)
{
    mos_internal_id_collect *c = (mos_internal_id_collect *)ctx;
    if (c->count >= MOS_ENUM_CAP) return false;
    uint64_t id = mos_internal_device_info_registry_id(info);
    if (id != 0) {
        c->ids[c->count++] = id;
    }
    return true;
}

/* Reopen a device by registry entry ID — the race-free counterpart to
   open-by-name (kernel resolves the match atomically). Used by
   mos_open_by_index; non-static for the watch adapter's per-probe reopen.
   Contract in mos_internal.h. */
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

    /* IOServiceGetMatchingService consumes the matching dictionary
       reference (Apple IOKit ownership rule), regardless of whether
       a match is found. No CFRelease needed here. */
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (svc == IO_OBJECT_NULL) {
        /* The drive enumerated earlier in this session is no longer
           in the registry — hot-unplugged between enumeration and
           open. Return NO_DEVICE so callers can distinguish this
           from a genuinely-bad index. */
        if (err_out) *err_out = MOS_ERR_NO_DEVICE;
        return NULL;
    }

    return mos_internal_open_service(svc, err_out);
}

mos_handle_t *mos_open_by_index(int one_based, mos_error *err_out)
{
    if (err_out) *err_out = MOS_ERR_INVALID_ARG;
    if (one_based < 1) return NULL;

    /* Two-stage but race-free: enumerate to collect registry IDs in
       DR device-array order (the public index contract), then reopen the
       captured ID via IORegistryEntryIDMatching. The kernel resolves that
       second match atomically, so a hot-plug between passes either succeeds
       or returns NO_DEVICE — never silently opens a different drive that
       inherited the original BSD name. */

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

    /* The info's registry ID reopens atomically (IORegistryEntryIDMatching
       — see mos_internal_open_by_registry_id), so opening from inside the
       enumeration callback carries no TOCTOU window and no re-enumeration:
       this is the one-snapshot path the CLI list/status use. */
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
 * Do NOT require exclusive access (that's the point of using MMC vs raw
 * SCSITaskDeviceInterface). Can still return kIOReturnExclusiveAccess
 * when another client holds the drive — that's a caller-sees-BUSY.
 *
 * Signatures verified against the macOS 26.5 SDK SCSITaskLib.h.
 */

/* Pin the IOKit symbolic constants to the literals the pure mapping
   expects, so a hypothetical SDK renumbering breaks the build loudly
   instead of silently miscategorizing IOReturn codes. */
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

/* mos_raw_cdb passes mos_xfer_dir straight to SetScatterGatherEntries with
   a (UInt8) cast, so the public values MUST equal the SDK's
   kSCSIDataTransfer_* enumerators. Pin them — an SDK renumbering fails the
   build instead of silently sending CDBs in the wrong direction. */
_Static_assert((int)MOS_XFER_NONE        == (int)kSCSIDataTransfer_NoDataTransfer,
               "MOS_XFER_NONE no longer matches kSCSIDataTransfer_NoDataTransfer "
               "— mos_raw_cdb passes mos_xfer_dir directly to "
               "SetScatterGatherEntries; the values MUST be identical.");
_Static_assert((int)MOS_XFER_TO_TARGET   == (int)kSCSIDataTransfer_FromInitiatorToTarget,
               "MOS_XFER_TO_TARGET no longer matches "
               "kSCSIDataTransfer_FromInitiatorToTarget — see MOS_XFER_NONE "
               "assertion above for the rationale.");
_Static_assert((int)MOS_XFER_FROM_TARGET == (int)kSCSIDataTransfer_FromTargetToInitiator,
               "MOS_XFER_FROM_TARGET no longer matches "
               "kSCSIDataTransfer_FromTargetToInitiator — see MOS_XFER_NONE "
               "assertion above for the rationale.");

/* Thin shim over the pure mapping in mos_pure.c (int32_t in, so it can be
   fixture-tested without IOKit — tests/test_ioreturn.c covers every code).
   CHECK CONDITION rides taskStatus/sense, not IOReturn, so this maps only
   transport-layer failures. */
static mos_error mos_internal_ioreturn_to_mos_error(IOReturn rc)
{
    return mos_internal_ioreturn_to_error((int32_t)rc);
}

mos_error mos_internal_mmc_get_tray_state(mos_handle_t *h, bool *tray_open)
{
    if (!h || !tray_open) return MOS_ERR_INVALID_ARG;

    /* Raw GET EVENT STATUS NOTIFICATION (opcode 0x4A), Media class, Polled.
       We deliberately do NOT call the MMCDeviceInterface GetTrayState
       convenience method: on a GESN failure it hard-codes (closed, success)
       (ARCHITECTURE.md §4.2, §9.7), masking the failure as a confident "closed."
       Issuing the CDB ourselves keeps a failure a failure, so the state core
       can fork on the TUR sense instead of trusting a fabricated verdict.

       Reached only on the not-ready path (TUR already proved the drive
       reachable and not mounted), so taking exclusive access here is free of
       the usual mount conflict. mos_raw_cdb acquires and RELEASES the lock
       per call — a single-shot poll, never held for the handle's lifetime.

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

    mos_error e = mos_raw_cdb(h, cdb, sizeof cdb,
                              resp, sizeof resp,
                              MOS_XFER_FROM_TARGET,
                              2000,                 /* ms, ARCHITECTURE.md §4.2 */
                              &task_status, sense, NULL);
    if (e != MOS_OK) return e;                      /* transport/lock: honest fail */
    if (task_status != kSCSITaskStatus_GOOD)        /* CHECK CONDITION etc.        */
        return MOS_ERR_IO;

    /* Validity gates (NEA, Media class, device-reported full span) live in the
       pure decoder so they are fuzz/ASan-checked headless. We pass the buffer
       span as the bound — the decoder trusts the reply's own Event Data Length
       field, not the transport's realized count (some USB bridges under-report
       it). A false return means "no authoritative bit" — fork on the TUR sense. */
    if (!mos_internal_gesn_media_door_open(resp, sizeof resp, tray_open))
        return MOS_ERR_IO;

    return MOS_OK;
}

mos_error mos_internal_mmc_test_unit_ready(mos_handle_t *h,
                                           uint32_t *status,
                                           uint8_t sense[18])
{
    if (!h || !h->mmc || !status || !sense) return MOS_ERR_INVALID_ARG;

    /* TestUnitReady reports a non-IOKit failure via outTaskStatus == CHECK
       CONDITION with valid sense; IOReturn only covers transport errors. */
    SCSITaskStatus  task_status  = 0;
    SCSI_Sense_Data sense_struct = {0};

    IOReturn rc = (*h->mmc)->TestUnitReady(h->mmc, &task_status, &sense_struct);
    if (rc != kIOReturnSuccess) {
        /* On transport failure, zero both outputs and return the mapped
           error. Callers see MOS_OK vs !=MOS_OK; *status is only
           meaningful on MOS_OK. */
        *status = 0;
        memset(sense, 0, 18);
        return mos_internal_ioreturn_to_mos_error(rc);
    }

    *status = (uint32_t)task_status;
    memcpy(sense, &sense_struct, 18);
    return MOS_OK;
}

/* RETURN-CONVENTION NOTE: this function returns MOS_ERR_IO for
   command-reached-drive-but-unusable replies (non-GOOD status,
   truncated GOOD) rather than clearing the out-param in-band. That is
   load-bearing, not pedantry: the profile's in-band "absent" value,
   0x0000, is a REAL drive answer ("no current profile"), so a
   malformed reply must be distinguishable out-of-band or it
   masquerades as legitimate no-media — the exact silent-0x0000 bug
   the third review fixed. (Identity strings, by contrast, have no
   such collision — an empty vendor is never a meaningful drive answer
   — which is why the retired INQUIRY path, and the DR identity seam
   that replaced it, clear in-band and stay non-fatal.) The caller
   treats both shapes as non-fatal enrichment skips. */
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
        /* Distinguish transport failure (map the IOReturn) from a non-GOOD
           status (command reached the drive but returned no usable data) —
           report I/O failure, not a misleading "no profile". Enrichment is
           optional, so the state core ignores a non-OK here. */
        return (rc != kIOReturnSuccess) ? mos_internal_ioreturn_to_mos_error(rc)
                                        : MOS_ERR_IO;
    }

    /* Gate the profile extraction on the reply's Feature Header Data Length
       (pure decoder, fuzz-checked): a drive that returns GOOD with a short
       transfer must surface as "no profile" rather than a silent 0x0000 that
       reads like real "no media". The buffer is the bound; the reply's own
       Data Length is the validity check (the convenience GetConfiguration
       reports no realized count). */
    uint16_t parsed = 0x0000;
    if (!mos_internal_config_current_profile(buf, sizeof(buf), &parsed)) {
        *profile = 0x0000;
        return MOS_ERR_IO;          /* truncated/short reply — enrichment skips it */
    }
    *profile = parsed;
    return MOS_OK;
}

mos_error mos_query_disc_info(mos_handle_t *h, const mos_disc_info **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* 34 bytes = the MMC standard response's fixed numeric region plus
       the lead-in/lead-out address fields — the exact shape of the
       committed fixtures (tests/fixtures/readdiscinfo_*.bin), which the
       pure decoder is built to. The convenience method reports no
       realized count, so sizeof buf is the trusted length (dual-length
       rule O-4); the reply's own Disc Information Length can only
       shrink the decode, never extend it. Convenience = non-exclusive:
       no lock interaction, safe at any state. */
    uint8_t         buf[34] = {0};
    SCSITaskStatus  st      = 0;
    SCSI_Sense_Data sd      = {0};

    IOReturn rc = (*h->mmc)->ReadDiscInformation(
        h->mmc, buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        /* Same convention as get_current_profile above: transport
           failures map their IOReturn; a command that reached the
           drive but returned no usable data (no medium, units that
           reject 0x51) is MOS_ERR_IO — out-of-band, so it can never
           masquerade as a real all-zero disc-info answer. */
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_disc_info_parse(buf, sizeof(buf), &h->disc_info)) {
        return MOS_ERR_IO;   /* truncated/short reply — refused whole */
    }
    *out = &h->disc_info;
    return MOS_OK;
}

/*
 * mos_internal_mmc_get_features — STUB (v0.4, hardware-gated).
 *
 * The IOKit half of the GET CONFIGURATION feature surface. Its pure,
 * bounds-safe walker (mos_internal_config_next_feature in mos_config.c) is
 * already written and fuzz/ASan-tested; this is the production caller it
 * waits for. Wired as a real caller over an empty header — so the walker is
 * a live seam, not an orphan with only test callers — and returns
 * UNSUPPORTED until the RT=0 issuance (header + all features) is written and
 * HW-validated. The walker owns the bounds; this half must not re-derive
 * them, only issue the command and consume features.
 */
mos_error mos_internal_mmc_get_features(mos_handle_t *h)
{
    if (!h || !h->mmc) return MOS_ERR_INVALID_ARG;

    uint8_t            buf[8] = {0};   /* TODO(v0.4): GetConfiguration(RT=0) fills a real,
                                          larger buffer — parse bound via
                                          mos_internal_trusted_len (contract O-4):
                                          min(allocated, realizedByteCount), header
                                          Data Length only ever shrinks it. */
    size_t             cursor = 8;     /* past the 8-byte feature header */
    mos_config_feature feat;

    while (mos_internal_config_next_feature(buf, sizeof buf, &cursor, &feat)) {
        /* TODO(v0.4): record feat.feature_code / feat.current / feat.data
           into the (not-yet-designed) features output. */
    }

    return MOS_ERR_UNSUPPORTED;        /* not implemented until the issuance lands */
}

/* The open-time INQUIRY (and its fixed-width SPC-4 string copier with
   per-call-site _Static_assert width pins) retired with the DR pivot:
   identity is directory data from DRDeviceCopyInfo — the same INQUIRY
   bytes, pre-parsed by the framework — copied through the bounded
   truncating seam in mos_dr.c. The output layer's escaping
   (mos_cli_json_str / mos_cli_safe_ascii) is unchanged: it guards the
   terminal and the JSON encoding against hostile bytes regardless of
   which substrate produced the string. */

/* ---- Raw CDB (diagnostic only) ------------------------------------- */

mos_error mos_raw_cdb(mos_handle_t *h,
                      const uint8_t *cdb, size_t cdb_len,
                      void *data_buf, size_t data_len,
                      mos_xfer_dir direction,
                      uint32_t timeout_ms,
                      uint32_t *scsi_task_status,
                      uint8_t   sense[18],
                      uint64_t *bytes_transferred)
{
    /* SetCommandDescriptorBlock only accepts 6, 10, 12, or 16 (see
       kSCSICDBSize_* in SCSITask.h). Reject other lengths at the API
       boundary so callers get MOS_ERR_INVALID_ARG instead of an opaque
       execute-time failure. */
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
    /* Reject timeout 0 — SetTimeoutDuration reads it as "Wait Forever",
       which would hang on a non-responsive command. */
    if (timeout_ms == 0)
        return MOS_ERR_INVALID_ARG;

    /* Zero outputs after arg validation: any error path below returns non-OK
       but leaves consumers who inspect buffers without checking the return a
       deterministic zero state. Validation failures above leave outputs
       untouched — "never started", caller's buffers presumed uninitialized. */
    *scsi_task_status = 0;
    memset(sense, 0, 18);
    if (bytes_transferred) *bytes_transferred = 0;

    /* Lazily acquire the SCSITaskDeviceInterface. */
    if (!h->std) {
        h->std = (*h->mmc)->GetSCSITaskDeviceInterface(h->mmc);
        if (!h->std) return MOS_ERR_DRIVER_REJECTED;
    }

    /* Raw CDB requires exclusive access. Route the IOReturn through the
       shared mapper, like every other site in this file. */
    if (!h->have_exclusive) {
        IOReturn rx = (*h->std)->ObtainExclusiveAccess(h->std);
        if (rx != kIOReturnSuccess)
            return mos_internal_ioreturn_to_mos_error(rx);
        h->have_exclusive = true;
    }

    SCSITaskInterface **t = (*h->std)->CreateSCSITask(h->std);
    if (!t) {
        /* Release the lock — by the function's invariant it was
           acquired above (every exit path below clears have_exclusive,
           so it is always false on entry; the conditional acquire
           exists for that documented invariant, not for a held-lock
           entry case). Holding it serves no purpose without a task. */
        (*h->std)->ReleaseExclusiveAccess(h->std);
        h->have_exclusive = false;
        return MOS_ERR_IO;
    }

    /* Check each Set* IOReturn — ignoring them ships a malformed task and
       turns into a baffling execute-time error. Cleanup is identical, so
       all three converge on one label. */
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

    /* sense was already zeroed in the zero-outputs block after arg
       validation; nothing between there and here writes it. */
    SCSI_Sense_Data sense_struct = {0};
    SCSITaskStatus   st           = 0;
    UInt64           xferred      = 0;

    IOReturn er = (*t)->ExecuteTaskSync(t, &sense_struct, &st, &xferred);
    (*t)->Release(t);

    /* Release before returning: a diagnostic command must not hold the drive
       locked for the handle's lifetime (would block Finder / MakeMKV / DA
       mounts). Released on both success and IO-failure paths; only the
       InvalidArg exits skip it, having never acquired. */
    (*h->std)->ReleaseExclusiveAccess(h->std);
    h->have_exclusive = false;

    /* On transport failure, outputs stay at the zeros set above (defined,
       not stack garbage); st/sense/xferred are not copied. Whether the API
       populates anything useful before a non-success IOReturn is undocumented
       — revisit under the v0.4 hardware-gate fixtures. */
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
