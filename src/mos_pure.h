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

#include "mos.h"  /* for mos_state_enum */

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
    /* Session identity, as two plain values (no composite token — JSON
       carries them as separate fields and consumers needing a single
       correlation key derive one; the data layer stays normalized).
       registry_id is the watch target's attachment identity (on the
       Apple adapter, the IORegistry entry ID — xnu guarantees real IDs
       >= 2^32+256); stream_open_wall_ms is the per-process-
       monotonicized wall epoch captured at watch open. The pair is
       unique per session; both are constant for the stream's life. */
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

/* True if the BSD name looks like a whole-disk entry (diskN) rather
   than a partition (diskNsM). */
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
   partition children. The unit is parsed from reported's leading digits
   and compared numerically — so the disk4-vs-disk40 prefix collision is
   simply 4 != 40 — and the suffix is validated as `(s<digits>)*`. Returns
   false for NULL reported, whole_unit < 0, a non-"disk" prefix, or a
   malformed suffix. Pure, no IOKit. Pinned by tests/test_bsd_name.c.
   Consumers: src/mos_watch.c and tools/mos_notification_probe.c DA
   filtering. */
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
 * contents. THE disc-identity primitive for batch-rip dedup: the track
 * layout + leadout is the canonical known-good fingerprint of a CD
 * (MusicBrainz/CDDB ids are pure functions of exactly these fields),
 * and reading it is unprivileged — no LibreDrive, no exclusive access,
 * no raw CDB (ReadTableOfContents is an MMCDeviceInterface convenience
 * method). Layout per MMC-6 §6.27.2.3 (response header: 2-byte TOC Data
 * Length counting bytes AFTER itself, first/last track; then 8-byte
 * descriptors: [1]=ADR<<4|CONTROL, [2]=track (0xAA=lead-out),
 * [4..7]=start LBA big-endian with MSF=0). Cross-checked against Linux
 * sr.c / cdrom.h TOC ioctls and libcdio.
 *
 * `len` is the TRUSTED length (O-4 at the seam); the header's own Data
 * Length is a device claim that only SHRINKS the walk (computed in
 * 64-bit, clamped via mos_internal_trusted_len). FAIL-CLOSED like the
 * config walk: a track number outside 1..99/0xAA, a duplicate, or a
 * non-ascending track sequence rejects the whole TOC — identity derived
 * from a half-parsed hostile TOC would be a falsely-stable fingerprint.
 * A TOC without a lead-out parses (have_leadout=false); identity
 * consumers must require it. */
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

/* Extract the Current Profile from a GET CONFIGURATION feature header,
   gated on the reply's own Data Length (bytes 0-3) so a truncated response
   is reported as "no profile" (false) rather than silently read as profile
   0x0000. `len` bounds the buffer; validity comes from the Data Length.
   See mos_config.c. Pure, fuzz/ASan-checked headless. */
bool mos_internal_config_current_profile(const uint8_t *buf, size_t len,
                                         uint16_t *profile);

/* ---- READ DISC INFORMATION decode (mos_discinfo.c) --------------- *
 *
 * Disc status from the MMC READ DISC INFORMATION (0x51) standard response —
 * the disc-completion signal: a Complete disc has finalized, readable content; a
 * Blank one has nothing to rip. byte 2 carries the status, last-session, and
 * erasable bits; the session/track counts are split LSB/MSB across the fixed
 * header (bytes 4..11). */
typedef enum {
    MOS_DISC_BLANK      = 0,  /* byte 2 bits 1:0 = 00b — empty recordable    */
    MOS_DISC_APPENDABLE = 1,  /*                  01b — incomplete, writable */
    MOS_DISC_COMPLETE   = 2,  /*                  10b — finalized            */
    MOS_DISC_OTHER      = 3,  /*                  11b — reserved/other       */
} mos_disc_status;

typedef struct {
    mos_disc_status status;              /* byte 2, bits 1:0 */
    uint8_t  last_session_state;         /* byte 2, bits 3:2: 0 empty,
                                            1 incomplete, 2 damaged, 3 complete */
    bool     erasable;                   /* byte 2, bit 4 */
    uint8_t  first_track_on_disc;        /* byte 3 */
    uint16_t number_of_sessions;         /* byte 9 (MSB) : byte 4 (LSB) */
    uint16_t first_track_last_session;   /* byte 10 : byte 5 */
    uint16_t last_track_last_session;    /* byte 11 : byte 6 */
} mos_disc_info;

/* Decode a READ DISC INFORMATION (0x51, data type 000b) response. `buf`/`len`
 * are the response and the byte count you trust. Fills *out and returns true
 * when the fixed numeric region (through byte 11) is present per BOTH `len`
 * and the response's own Disc Information Length; returns false on a NULL
 * argument or a buffer/declared length too short to hold those fields. The
 * device-reported length can only shrink the trusted region — no read ever
 * leaves [buf, buf+len). Address fields (lead-in/lead-out, bytes 16..23) and
 * the validity-gated identifiers are deliberately not decoded here: status and
 * the counts are the disc-completion/capacity signal; the rest is v0.4+. Pure, so
 * ASan/fuzz-checked headless. */
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
 * Media-class polled reply. Returns true and fills *door_open only when the
 * reply is a valid, NEA-clear, full-span Media descriptor; false (no
 * authoritative bit → caller forks on sense) otherwise. The state core's
 * get_tray_state op is built on this so the bit offsets are fuzz/ASan-checked
 * headless rather than living only in the Apple shell. See ARCHITECTURE.md
 * §4.2 and research/2026-05-29-gesn-single-poll.md. */
bool mos_internal_gesn_media_door_open(const uint8_t *resp, size_t len,
                                       bool *door_open);

/* ---- IOReturn → mos_error mapping (mos_pure.c) -------------------- *
 *
 * Translate an IOReturn (or any int32_t mach-style error code from
 * <IOKit/IOReturn.h>) to a mos_error. The pure interface takes int32_t
 * so this function can live in the pure layer and be unit-tested
 * without IOKit headers; the Apple adapter passes the IOReturn through
 * with a cast.
 *
 * The IOKit symbolic constants are stable ABI; the Apple adapter in
 * src/mos_scsi.c static_asserts the symbolic names against the numeric
 * values this function maps, so an SDK change fails the build loudly
 * rather than silently miscategorizing errors.
 *
 * Putting the mapping in the pure layer is the load-bearing decision:
 * an unplugged drive surfaces as kIOReturnNoDevice from the IOKit
 * convenience methods, and the watch core depends on that being
 * mapped to MOS_ERR_NO_DEVICE so its terminal-removal path can fire.
 * Manual mapping at individual callsites is the bug shape this
 * function exists to prevent — every IOReturn-handling site in the
 * adapter should route through here. */
mos_error mos_internal_ioreturn_to_error(int32_t rc);

/* ---- Decision-tree core (mos_state_core.c) ------------------------- *
 *
 * The mos_query_state() decision tree operates against a small vtable
 * (mos_mmc_ops_t) whose three callbacks issue the MMC convenience
 * commands the tree consumes. The Apple-side mos_query_state() in
 * mos_state.c fills env from a real mos_handle_t and forwards here.
 * Tests in tests/test_state_core.c provide a fake mos_mmc_ops_t whose
 * callbacks return scripted responses, exercising every branch of the
 * tree without IOKit.
 *
 * Each callback:
 *   - Receives a caller-supplied void *ctx (handle on the real path,
 *     fixture pointer on the test path).
 *   - Returns MOS_OK on success and fills its out-parameter.
 *   - Returns a negative mos_error otherwise. The decision tree's
 *     interpretation of those errors is documented in mos_state_core.c.
 *
 * Division of labour (see mos_state_core.c for the full tree):
 *   - test_unit_ready: the CONVENIENCE TUR, non-exclusive. Trusted for
 *     PRESENCE ("do I have a disc, am I ready"). Runs first, always; a GOOD
 *     status short-circuits to READY without ever taking a lock. Its sense
 *     bytes also enrich the closed branch below.
 *   - get_tray_state: the tray bit, and ONLY the tray bit. The Apple impl
 *     issues a RAW GET EVENT STATUS NOTIFICATION under exclusive access and
 *     reads the door-open bit — never the GetTrayState convenience method,
 *     which hard-codes (closed, success) on a GESN failure and so would
 *     mask a failure as a confident "closed." Reached only when TUR is not
 *     ready (⇒ not mounted ⇒ the lock is free). MOS_OK means *tray_open is
 *     authoritative; ANY error (no lock, or GESN silent) means "no bit" and
 *     the tree forks on the TUR sense instead. A good GESN verdict is never
 *     overturned by the sense.
 *   - get_current_profile: READY-only metadata enrichment.
 *
 * sense buffer is fixed-format 18 bytes per SPC-4 §4.5.3, matching the
 * existing mos_internal_mmc_test_unit_ready() contract. */
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
       time: scheduling code calls mono_ms, ts-emission calls
       wall_ms. The two values are not interchangeable — passing one
       where the other is expected previously produced a first-poll
       deadline decades in the future. */
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
       lifetime constraint — the former composite stream_id string was
       retired in favor of the two value fields. */
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
    /* Error backoff (P2): consecutive identical probe errors escalate the
       retry interval from transition_poll_ms toward stable_poll_ms by
       doubling, so a persistently failing probe (another client holding
       the drive for an hour) emits a bounded error stream instead of one
       event per transition poll (~18k lines/hour at the 200 ms default).
       Every error still emits — the stream stays live — only the spacing
       grows. A success, a different error code, or a notify_wake (which
       pulls the deadline to 0 regardless) resets the escalation. */
    int32_t        last_probe_err;     /* mos_error of the previous probe, MOS_OK if it succeeded */
    uint32_t       consec_probe_errs;  /* consecutive probes returning last_probe_err */
    /* Remembered media identity for same-state swap detection (F1). The
       primary fingerprint is last_media_id (the whole-disk IOMedia registry
       entry ID); last_profile doubles as the fallback gate on bridges that
       never expose an id (cross-class swaps only). Both refresh on every
       event built from a fresh probe; on a no-change probe each refreshes
       only when non-zero — 0 means "unavailable", never an observation, so
       a known identity is kept across an unavailability gap. */
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
       (registry_id, stream_open_wall_ms) pair; the former composite
       stream_id string is retired.
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
    /* Slot joined after the stream opened: its first event (the core's
       snapshot) is relabeled MOS_EVENT_DEVICE_APPEARED, then cleared. */
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
