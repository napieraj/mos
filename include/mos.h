/*
 * mos.h — mac-optical-state public C API (version: MOS_VERSION_STRING below)
 *
 * Pure-C public surface for querying macOS optical drive state via
 * MMCDeviceInterface. The default state path issues one raw CDB (GESN)
 * on its not-ready branch, briefly taking the exclusive lock — safe by
 * the nub invariant. See ARCHITECTURE.md §3/§5.5. (Raw CDB issuance is
 * a library-internal mechanism, not public surface.)
 *
 * Design goals:
 *   - Trivially embeddable in non-macOS-specific C/C++ projects.
 *     Conditional-compile the whole thing out under #ifdef __APPLE__.
 *   - No Apple-specific headers leak into this file.
 *   - Stable ABI: the opaque handle hides all IOKit state, and the
 *     returned result/event objects are opaque too — read through
 *     accessors, so fields can be appended without breaking ABI. Borrowed
 *     pointers (including strings) are owned by the handle/watch; callers
 *     must copy anything they need to retain past the next query or close.
 *
 * Threading: NOT thread-safe. Callers must serialize access per handle.
 * Opening multiple handles to different drives from different threads is
 * fine; sharing one handle across threads is not.
 */

#ifndef MOS_H
#define MOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Opaque handle types --------------------------------------------- */

typedef struct mos_handle      mos_handle_t;
typedef struct mos_device_info mos_device_info_t;

/* ---- Enumerations ---------------------------------------------------- */

/* ABI-width pin, applied to every public enum just after its
   definition. Public enums must be exactly int32_t-wide: consumer FFI
   bindings (Swift importer, bindgen, cgo) assume it, and a bare enum
   lets the compiler pick any representing type — under -fshort-enums
   (positive-only value sets are especially free to narrow) a consumer
   assuming int32_t reads garbage upper bits, and an enum embedded in a
   struct corrupts the layout silently. mos_error's negative values
   force a signed type; the others rely on the pin entirely. If a pin
   fires: use `enum ... : int32_t` (C23) or disable -fshort-enums. See
   doc/research/2026-04-27-v2-contract-design.md item 5. */
#if defined(__cplusplus)
#define MOS_ABI_PIN_I32(T) \
    static_assert(sizeof(T) == sizeof(int32_t), \
                  #T " must be int32_t-wide for FFI ABI stability")
#else
#define MOS_ABI_PIN_I32(T) \
    _Static_assert(sizeof(T) == sizeof(int32_t), \
                   #T " must be int32_t-wide for FFI ABI stability")
#endif

/*
 * Reportable drive states. "ready" means the unit reported ready via
 * TEST UNIT READY — NOT merely that media is present. See ARCHITECTURE.md
 * §5 for the full decision tree.
 */
typedef enum {
    MOS_STATE_UNKNOWN = 0,
    MOS_STATE_OPEN,      /* tray extended (GESN door bit set) */
    MOS_STATE_EMPTY,     /* tray closed / slot-load, no media */
    MOS_STATE_LOADING,   /* unit spinning up / becoming ready */
    MOS_STATE_READY,     /* unit reports ready; media present and addressable */
    MOS_STATE_BUSY,      /* unit busy / contended (SAM-5 BUSY-class status) */
    /* Later-appended states. The enum is int32-wide-pinned and
       accessor-only across the ABI, so appending here is binary-
       compatible. Order is load-bearing only in that new values stay
       at the end. */
    MOS_STATE_FORMATTING,      /* media present, format in progress (sense 04/04) */
    MOS_STATE_MEDIA_UNREADABLE,/* media present but unreadable (MEDIUM ERROR / 57/00 TOC) */
    MOS_STATE_DEVICE_FAULT,    /* drive hardware fault (sense key HARDWARE ERROR) */
    MOS_STATE_EMPTY_OR_OPEN,   /* no media, but the tray bit was unobservable
                                  (GESN unavailable and sense couldn't place it).
                                  A degraded *observation*, not a physical state:
                                  the drive is empty or open, we just couldn't
                                  tell which this probe. Watch treats it as
                                  transitional so a re-poll resolves it once GESN
                                  is reachable. */
} mos_state;
MOS_ABI_PIN_I32(mos_state);

/*
 * Error codes. mos_query_state() returns MOS_OK even when the reported
 * state is UNKNOWN — that is a valid query result. Negative codes mean
 * the query itself could not be executed.
 */
typedef enum {
    MOS_OK                    =  0,
    MOS_ERR_INVALID_ARG       = -1,
    MOS_ERR_NO_DEVICE         = -2, /* no drive at that index/bsd */
    MOS_ERR_DRIVER_REJECTED   = -3, /* IOKit would not attach SCSITaskUserClient */
    MOS_ERR_EXCLUSIVE_ACCESS  = -4, /* another client holds the drive */
    MOS_ERR_BUSY              = -5, /* contention from another client competing
                                       for exclusive access — NOT "media is
                                       mounted", which the default path handles
                                       correctly. */
    MOS_ERR_TIMEOUT           = -6,
    MOS_ERR_IO                = -7, /* unexpected IOKit / kernel error */
    MOS_ERR_UNSUPPORTED       = -8, /* command not supported by drive */
    MOS_ERR_OOM               = -9,
} mos_error;
MOS_ABI_PIN_I32(mos_error);

/* READ DISC INFORMATION disc status (MMC-6 0x51 byte 2, bits 1:0) —
   the disc-completion signal a blank-vs-finalized decision needs. */
typedef enum {
    MOS_DISC_BLANK      = 0,  /* empty recordable                  */
    MOS_DISC_APPENDABLE = 1,  /* incomplete session, writable      */
    MOS_DISC_COMPLETE   = 2,  /* finalized                         */
    MOS_DISC_OTHER      = 3,  /* reserved/other (random-recordable) */
} mos_disc_status;
MOS_ABI_PIN_I32(mos_disc_status);

/* Outcome of a tray-control verb (mos_tray_*). A command the drive
   ANSWERED always returns MOS_OK with one of these — the refusal is a
   reported fact, not a query failure. A transport/lock failure (the drive
   is mounted or held by another client, gone, or an IOKit error) returns a
   negative mos_error instead and leaves the outcome unmodified. */
typedef enum {
    MOS_TRAY_DONE           = 0, /* command completed GOOD                  */
    MOS_TRAY_REFUSED_LOCKED = 1, /* CHECK CONDITION 53/02 MEDIA REMOVAL
                                    PREVENTED — an eject/close hit a lock    */
    MOS_TRAY_REFUSED_OTHER  = 2, /* CHECK CONDITION, other sense (e.g. a
                                    drive without Persistent Prevent support
                                    rejecting the persistent state with
                                    5/24/00)                                 */
    MOS_TRAY_ALREADY_LOCKED = 3, /* tray lock on a MOUNTED disc: the lock CDB
                                    can't issue (media still mounted), but a
                                    mounted disc is already removal-locked by
                                    macOS — the requested state already holds,
                                    so this is a success, not an error       */
} mos_tray_outcome;
MOS_ABI_PIN_I32(mos_tray_outcome);

/* ---- Query result struct -------------------------------------------- */

/*
 * Result of a state query. Opaque: the layout is internal and may grow
 * (fields appended) without breaking ABI — read it only through the
 * accessor functions below. The object is owned by the handle and remains
 * valid until the next mos_query_state() call or mos_close(); copy out any
 * fields (strings included) you need to retain past that.
 *
 * vendor / product / revision may be NULL when the device directory has
 * no identity for the drive.
 */
typedef struct mos_state_result mos_state_result;

/* OPEN / EMPTY / LOADING / READY / BUSY / FORMATTING / MEDIA_UNREADABLE /
   DEVICE_FAULT / EMPTY_OR_OPEN / UNKNOWN. */
mos_state mos_state_result_state(const mos_state_result *r);

/* Whole-disk BSD unit (the N in "diskN"), or -1 when there is no media
   (empty/open tray) and hence no resolvable name. Render to "diskN" with
   mos_bsd_name_format().

   QUERY-TIME SEMANTICS: for a held handle this is re-resolved on every
   mos_query_state from the drive's stable service, so a handle opened on
   an empty drive reports the inserted disc's unit once a query returns
   READY (and reverts to -1 after an eject). The re-resolve is a local
   IORegistry walk off the drive node — no command, no exclusive access.
   mos_query_capacity and mos_query_volume refresh the same way. */
int64_t        mos_state_result_bsd_unit(const mos_state_result *r);

/* The drive service's IORegistry entry ID — the attachment identity,
   identical to the registry_id the watch stream (mos.event.v1) emits,
   so one-shot state documents and event streams about the same drive
   share a join key. 0 when unavailable. */
uint64_t       mos_state_result_registry_id(const mos_state_result *r);

/* Render a whole-disk BSD unit as its canonical DEVICE NODE
   ("/dev/" + BSD name, diskutil's term — Apple vocabulary: kIOBSDUnitKey
   is the unit, kIOBSDNameKey is "diskN", the node is their /dev form)
   into out. The in-domain maximum is "/dev/disk4294967295" — 20 bytes
   with NUL; recommend cap 24 (what the CLI uses). Returns false
   (out = "") for unit < 0 (an empty/open drive has no node by
   definition), for unit > UINT32_MAX (refused, never rendered as a
   valid-looking node no disk can have — same domain as
   mos_bsd_name_format), and for a cap too small for the rendering. */
bool           mos_bsd_dev_node(int64_t unit, char *out, size_t out_cap);

/* Drive identity strings (the INQUIRY vendor/product/revision fields,
   sourced from the system's device directory at open); NULL when
   unavailable. Borrowed, with the same lifetime as the result object. */
const char    *mos_state_result_vendor(const mos_state_result *r);
const char    *mos_state_result_product(const mos_state_result *r);
const char    *mos_state_result_revision(const mos_state_result *r);

/* Kernel optical-media type token ("cd_rom"/"dvd_minus_r"/"bd_re"/…, the
   IORegistry kIO{CD,DVD,BD}MediaTypeKey mapped to a stable token), or NULL when
   no optical media node carries a Type. Read zero-MMC off the media node, so —
   unlike current_profile/media_class — it is present even when state is not
   READY: a loading/busy/unreadable disc can still be named. Finer than the
   profile's class (ROM vs recordable). */
const char    *mos_state_result_media_type(const mos_state_result *r);

/* Kernel IOMedia Writable flag (kIOMediaWritableKey), read zero-MMC off the same
   media node as media_type. Tri-state: -1 = no media node / flag absent, 0 =
   read-only (ROM or write-protected), 1 = writable. Present even when state is
   not READY, like media_type. This is the kernel's MECHANISM bit, not a
   blank/appendable claim: the precise blank/appendable/complete tri-state is a
   READ DISC INFORMATION fact (mos_query_disc_info), off the poll path. */
int            mos_state_result_writable(const mos_state_result *r);

/* GET CONFIGURATION current profile, populated only when state is READY
   (the profile byte is unreliable on empty/open trays — many firmwares
   cache the last-inserted-disc profile). 0x0000 means none/not queried. */
uint16_t       mos_state_result_current_profile(const mos_state_result *r);

/* Last CHECK CONDITION seen, all zero if none. Any of the out-params may
   be NULL. */
void           mos_state_result_sense(const mos_state_result *r,
                                                 uint8_t *sense_key,
                                                 uint8_t *asc, uint8_t *ascq);

/* ---- Enumeration API ------------------------------------------------- */

/*
 * Callback invoked once per discovered optical drive.
 * Strings are valid only for the duration of the callback. Copy if needed.
 * Return true to continue enumeration, false to stop early.
 */
typedef bool (*mos_enumerate_cb)(const mos_device_info_t *info, void *ctx);

/* Walk all optical drives, invoking cb once per drive (return false to
 * stop early). Order: position in the system's device array — the same
 * array drutil enumerates, so the 1-based index agrees with drutil's
 * by provenance. The order is stable only within an enumerate→open
 * window; cross-run stability is not promised. The mos_device_info_t
 * is opaque — read it through the accessors below; it is valid only
 * for the callback's duration (open it there via mos_open_device(),
 * or copy what you need).
 *
 * Up to 64 drives per call; any beyond that are silently dropped. If you
 * exceed this, please get help from someone qualified. */
void mos_enumerate_devices(mos_enumerate_cb cb, void *ctx);

/* Accessors for the opaque device-info handle passed to the callback.
   bsd_unit is -1 for an empty/open-tray drive (no media, hence no
   IOMedia child to name); such drives still enumerate and stay openable
   by snapshot position (mos_open_by_index) or in-callback
   (mos_open_device). Render to a string with mos_bsd_name_format(). */
int64_t mos_device_info_bsd_unit(const mos_device_info_t *);

/* The drive service's IORegistry entry ID for an enumeration entry —
   the attachment identity mos_open_by_registry_id selects by and the
   one mos.state.v1/mos.event.v1 carry. 0 if unavailable. */
uint64_t mos_device_info_registry_id(const mos_device_info_t *);

/* Render a whole-disk unit to its canonical BSD name ("diskN") in buf.
   Returns true and writes "diskN" for a valid unit [0, UINT32_MAX];
   returns false and writes "" (when cap > 0) for unit < 0 (no media), a
   unit outside that domain (refused rather than rendered as a wrong-but-
   valid-looking name), or a buffer too small. buf is always NUL-terminated
   when cap > 0; 16 bytes always suffices ("disk4294967295" is 15 chars). */
bool mos_bsd_name_format(int64_t unit, char *buf, size_t cap);

/* ---- Open / query / close ------------------------------------------- */

/*
 * Open by 1-based index (matches drutil convention) or by BSD name.
 * Return value is a borrowed error code via *err_out when non-NULL.
 * On success returns a handle; on failure returns NULL and sets *err_out.
 *
 * SELECTOR STABILITY — mos_open_by_index is POSITIONAL, not durable. The
 * index is a slot in a freshly enumerated snapshot, so it races device
 * hotplug: a drive appearing or disappearing between the enumeration the
 * caller read and this open shifts every higher slot, which can then open
 * a DIFFERENT drive or return MOS_ERR_NO_DEVICE. Use it only for one-shot,
 * human-driven, single-drive invocations. For programmatic selection that
 * must survive hotplug, prefer mos_open_by_registry_id (atomic, never
 * reused) or mos_open_by_bsd_name (more stable, though "diskN" can be
 * recycled after an eject). Same caveat applies to mos_device_info_bsd_unit.
 */
mos_handle_t *mos_open_by_index(int one_based, mos_error *err_out);
mos_handle_t *mos_open_by_bsd_name(const char *bsd_name, mos_error *err_out);

/* Open by IORegistry entry ID — the attachment identity every result
   and event carries as registry_id (xnu mints real IDs >= 2^32+256,
   never reused). Selects "this exact attachment": after a replug or
   firmware flash the old ID yields MOS_ERR_NO_DEVICE — itself the
   confirmation that attachment is gone. */
mos_handle_t *mos_open_by_registry_id(uint64_t registry_id,
                                      mos_error *err_out);

/*
 * Open the drive an enumeration callback is currently visiting. Call
 * ONLY from inside the callback (the info object dies when the
 * callback returns); the returned handle is independent of the info
 * and outlives it. This is the one-snapshot pattern: enumerate once
 * and open each drive of interest without re-enumerating per open —
 * the open resolves atomically against the kernel registry, so a
 * drive that vanished since the snapshot yields MOS_ERR_NO_DEVICE,
 * never a different drive.
 */
mos_handle_t *mos_open_device(const mos_device_info_t *info,
                              mos_error *err_out);

/*
 * Return the whole-disk BSD unit a handle was opened against (the N in
 * "diskN"). Useful in partial-failure paths where mos_open_by_*()
 * succeeded but mos_query_state() later fails — callers can surface
 * which device the failure refers to. Returns -1 if h is NULL, or if the
 * drive has no media (empty/open tray) and therefore no unit. Render to
 * a string with mos_bsd_name_format().
 */
int64_t mos_handle_bsd_unit(const mos_handle_t *h);

/*
 * Query drive state. Uses MMC convenience methods, plus one raw GESN
 * on the not-ready branch (briefly takes the exclusive lock; see
 * ARCHITECTURE.md §5.5 for why that cannot collide with a mount). `out` is REQUIRED — NULL returns MOS_ERR_INVALID_ARG
 * (unlike the optional err_out parameters elsewhere). On success returns
 * MOS_OK and points *out at a handle-owned result; on failure returns a
 * negative code with *out set to NULL. Read it through the
 * mos_state_result_* accessors.
 *
 * The result is handle-owned: do not free it, and do not retain it across
 * calls — it is valid only until the next mos_query_state() on this handle
 * or mos_close(h). Copy out anything you need to keep.
 */
mos_error mos_query_state(mos_handle_t *h, const mos_state_result **out);

/* ---- Disc information ------------------------------- */

/* Result of a disc-information query. Opaque on the same terms as
   mos_state_result: handle-owned, read through the accessors, valid
   until the next mos_query_disc_info() call or mos_close(). */
typedef struct mos_disc_info mos_disc_info;

/*
 * Query READ DISC INFORMATION (MMC 0x51) — the disc-completion signal:
 * blank vs appendable vs finalized, plus session/track counts. Issued
 * on demand through the non-exclusive convenience method; never part
 * of the default state path (no state decision needs it), so calling
 * this costs exactly one MMC command.
 *
 * Meaningful only with media present and the unit ready: with no disc
 * (or a non-recordable unit that rejects 0x51) the drive fails the
 * command and this returns MOS_ERR_IO — query state first if you need
 * to distinguish "no disc" from "drive unreachable". `out` is REQUIRED
 * (NULL returns MOS_ERR_INVALID_ARG). On success returns MOS_OK and
 * points *out at a handle-owned result; on failure returns a negative
 * code with *out set to NULL.
 */
mos_error mos_query_disc_info(mos_handle_t *h, const mos_disc_info **out);

/* Accessors. NULL-tolerant like every result accessor: a NULL object
   reads as MOS_DISC_OTHER / false / 0. */
mos_disc_status mos_disc_info_status(const mos_disc_info *d);
bool            mos_disc_info_erasable(const mos_disc_info *d);
uint8_t         mos_disc_info_first_track(const mos_disc_info *d);
uint16_t        mos_disc_info_session_count(const mos_disc_info *d);
uint16_t        mos_disc_info_first_track_last_session(const mos_disc_info *d);
uint16_t        mos_disc_info_last_track_last_session(const mos_disc_info *d);
/* State of Last Session, raw (byte 2 bits 3:2): 0 empty, 1 incomplete,
   2 damaged, 3 complete. Raw on purpose — four values, no semantics
   mos adds; an enum here would be ceremony. */
uint8_t         mos_disc_info_last_session_state(const mos_disc_info *d);
/* BG Format Status, raw (byte 7 bits 1:0): 0 none, 1 inactive (started,
   not running), 2 active (in progress), 3 complete — the background-
   format state of DVD+RW / BD-RE / Mount Rainier media. Map to a token
   with mos_bg_format_status_name(). */
uint8_t         mos_disc_info_bg_format_status(const mos_disc_info *d);

/* "blank" / "appendable" / "complete" / "other". Stable lowercase
   tokens, same contract as mos_state_description(). */
const char     *mos_disc_status_description(mos_disc_status s);

/* "none" / "inactive" / "active" / "complete" for BG Format Status
   0..3; NULL for out-of-range. Tokens track Linux CDM_MRW_* (cdrom.h). */
const char     *mos_bg_format_status_name(uint8_t status);

/* ---- Table of contents ------------------------------ */

/* Result of a TOC query. Opaque, handle-owned, read through the
   accessors; valid until the next mos_query_toc() call or mos_close(). */
typedef struct mos_toc mos_toc;

/*
 * Query the normalized table of contents — the disc-identity primitive;
 * never part of the state path. For CDs the source is PRIMARILY the macOS
 * kernel-cached full-TOC (kIOCDMediaTOCKey) — a superset of READ TOC, read
 * with zero SCSI commands and no exclusive access, fresh off the current
 * IOCDMedia node; the non-exclusive ReadTableOfContents convenience method
 * (READ TOC/PMA/ATIP format 0000b, LBA) is the FALLBACK when no IOCDMedia
 * node is up yet (just-inserted / unrecognized CD) and the only path for
 * DVD/BD, where no cached TOC exists. FAIL-CLOSED end to end: a hostile or
 * incoherent TOC (out-of-range, duplicate, or non-ascending tracks; a header
 * span the descriptor list doesn't cover) returns MOS_ERR_IO with *out NULL —
 * identity from a half-parsed TOC would be a falsely-stable fingerprint, and
 * a fail-closed cached decode degrades to the issued read rather than to a
 * bad TOC. A TOC without a lead-out parses; identity consumers must require
 * mos_toc_have_leadout(). Same media preconditions as mos_query_disc_info().
 * `out` REQUIRED (NULL → MOS_ERR_INVALID_ARG).
 */
mos_error mos_query_toc(mos_handle_t *h, const mos_toc **out);

/* Accessors. NULL-tolerant: a NULL object reads as 0 / false. Track
   entries are indexed 0..mos_toc_track_count()-1; out-of-range reads
   as 0. control bit 2 set = data track (MMC Q-channel control). */
uint8_t  mos_toc_first_track(const mos_toc *t);
uint8_t  mos_toc_last_track(const mos_toc *t);
size_t   mos_toc_track_count(const mos_toc *t);
bool     mos_toc_have_leadout(const mos_toc *t);
uint32_t mos_toc_leadout_lba(const mos_toc *t);
uint8_t  mos_toc_track_number(const mos_toc *t, size_t i);
uint8_t  mos_toc_track_adr(const mos_toc *t, size_t i);
uint8_t  mos_toc_track_control(const mos_toc *t, size_t i);
uint32_t mos_toc_track_start_lba(const mos_toc *t, size_t i);

/* ---- Mounted volume (DiskArbitration-sourced) ------------------------ */

/*
 * Volume name and mount path for the drive's current media — the
 * filesystem view, complementing the SCSI facts above. One synchronous
 * DiskArbitration read: no callbacks, no drive commands, no elevation.
 * Gated on the whole-disk IOMedia node (the handle's bsd_unit, re-resolved
 * per call — same query-time semantics as mos_state_result_bsd_unit), so a
 * handle opened empty reports the inserted disc's volume and reverts to
 * unmounted after eject.
 *
 * UNMOUNTED IS NOT AN ERROR (also the common case for UDF video discs):
 * the result is MOS_OK with *mounted=false and empty buffers; only a NULL
 * handle returns MOS_ERR_INVALID_ARG. Buffers are optional (NULL/0 skips
 * the field), always NUL-terminated, and name may be "" even when mounted.
 * Recommended caps: name 256, path 1024; a path over its cap reports
 * unmounted rather than truncated. Volume names are disc-controlled —
 * escape before terminals or structured output.
 *
 * OPTIONAL DEPENDENCY: a build with MOS_USE_DISKARBITRATION=0 keeps this
 * function and its contract, always reporting unmounted — CLI and JSON
 * shapes are identical with the volume fields null.
 */
mos_error mos_query_volume(mos_handle_t *h, bool *mounted,
                           char *name_buf, size_t name_cap,
                           char *path_buf, size_t path_cap);

/* ---- Drive capabilities ----------------------------- */

/* Result of a drive-capabilities query. Opaque, handle-owned; valid
   until the next mos_query_drive_caps() call or mos_close(). */
typedef struct mos_drive_caps mos_drive_caps;

/*
 * Query the drive's content-protection and profile facts: one full GET
 * CONFIGURATION (RT=0) through the non-exclusive convenience method, decoded
 * by the bounds-checked feature walk. The protection fields are the
 * spec-grounded bits a MakeMKV drive dump shows ("Highest AACS version",
 * bus-encryption flags) WITHOUT the LibreDrive status synthesis, which is a
 * MakeMKV database property, not a drive property.
 *
 * SEMANTICS: a protection scheme reported here is a drive CAPABILITY — the
 * drive can authenticate that scheme. It does NOT mean protected media is
 * loaded (the per-feature Current bit, media-dependent, is ignored) nor that
 * protection is enforced (region/key state lives behind REPORT KEY, which mos
 * does not issue — scope doctrine: no raw verb without GESN-grade
 * justification). bus_encryption / write_bus_encryption are the DRIVE-REPORTED
 * AACS BEC/WBE support bits (firmware assertions); the cryptographically
 * signed BEC bit lives in the AACS drive certificate, out of scope.
 *
 * Drives without a given feature (every non-BD unit for AACS) report it
 * false — that is data, not an error.
 */
mos_error mos_query_drive_caps(mos_handle_t *h, const mos_drive_caps **out);

/* Content-protection accessors. NULL-tolerant (NULL reads as 0/false). A
   *_version is meaningful only when its scheme bool is true; SecurDisc and
   VCPS are presence-only (no version). bus_encryption / write_bus_encryption
   are meaningful only when aacs is true. */
bool    mos_drive_caps_css(const mos_drive_caps *c);
uint8_t mos_drive_caps_css_version(const mos_drive_caps *c);
bool    mos_drive_caps_cprm(const mos_drive_caps *c);
uint8_t mos_drive_caps_cprm_version(const mos_drive_caps *c);
bool    mos_drive_caps_aacs(const mos_drive_caps *c);
uint8_t mos_drive_caps_aacs_version(const mos_drive_caps *c);
bool    mos_drive_caps_bus_encryption(const mos_drive_caps *c);
bool    mos_drive_caps_write_bus_encryption(const mos_drive_caps *c);
bool    mos_drive_caps_securdisc(const mos_drive_caps *c);
bool    mos_drive_caps_vcps(const mos_drive_caps *c);

/* Supported-profile set from the Profile List feature (0x0000) — the
   drive-static disc types this drive handles (the modern, BD-aware "what can
   this drive read/write", superseding the legacy page-0x2A media bits). The
   per-descriptor CurrentP bit is media-dependent and deliberately omitted.
   _count is the number of codes; _code(c, i) returns the i-th MMC profile
   number (0 for out-of-range i / NULL c). Map a code to a name with
   mos_profile_name() / mos_profile_class(); an unknown code has a NULL name
   and is surfaced as its hex value. */
uint8_t  mos_drive_caps_profile_count(const mos_drive_caps *c);
uint16_t mos_drive_caps_profile_code(const mos_drive_caps *c, uint8_t i);

/* Firmware creation timestamp from the Firmware Information feature (010Ch),
   as an ISO-8601 GMT string "YYYY-MM-DDTHH:MM:SSZ", or NULL when the drive
   does not implement the feature. Borrowed from the handle (valid until the
   next mos_query_drive_caps() / mos_close()). Zero commands beyond the
   GET CONFIGURATION walk the caps query already performs. */
const char *mos_drive_caps_firmware_date(const mos_drive_caps *c);

/* Drive serial from the Logical Unit Serial Number feature (0108h), as an ASCII
   string, or NULL when the drive does not implement the feature / programs no
   serial (it is OPTIONAL in MMC and population is firmware-dependent — some
   firmware leaves it blank). The drive-serial source: it rides the non-exclusive
   GET CONFIGURATION walk the caps query already performs (no raw command, no
   exclusive access — readable even while a disc is mounted). Borrowed from the
   handle (valid until the next mos_query_drive_caps() / mos_close()). */
const char *mos_drive_caps_serial(const mos_drive_caps *c);

/* Current Profile (loaded medium) from the same RT=0 GET CONFIGURATION reply,
   or 0 when the tray is empty / the field was absent. MEDIA-DEPENDENT (unlike
   the rest of mos_drive_caps): surfaced so a caller can name the loaded disc's
   class via mos_profile_class() — e.g. to scale a media-dependent read/write
   speed to a 1x multiple. Zero commands beyond the caps walk. */
uint16_t mos_drive_caps_current_profile(const mos_drive_caps *c);

/* Drive identity for a held handle — the open-time directory data
   (DiscRecording pre-parsed INQUIRY strings) and the drive service's
   registry entry ID. Zero commands. Strings are borrowed from the
   handle (valid until mos_close), NULL when the directory had no
   identity; registry id 0 = unavailable. */
const char *mos_handle_vendor(const mos_handle_t *h);
const char *mos_handle_product(const mos_handle_t *h);
const char *mos_handle_revision(const mos_handle_t *h);
uint64_t    mos_handle_registry_id(const mos_handle_t *h);

/* The drive serial is read from the Logical Unit Serial Number feature (0108h)
   via mos_drive_caps_serial() — the non-exclusive GET CONFIGURATION path. There
   is no raw VPD-0x80 serial entry point: VPD page 0x80 is the SPC/block-storage
   carrier and is the wrong abstraction for an MMC optical drive (architecturally
   and empirically — it read empty on the drives surveyed). See the AGENTS.md
   serial-source ADR. */

/* ---- Drive standards (standard INQUIRY) ------------- */

/* Result of a drive-standards query. Opaque, handle-owned; valid until the
   next mos_query_drive_inquiry() call or mos_close(). */
typedef struct mos_drive_inquiry mos_drive_inquiry;

/*
 * Query the standards the drive claims: the VERSION byte (SPC compliance
 * level) and the version-descriptor list, from a raw STANDARD INQUIRY
 * (EVPD=0, allocation length >= 74) on the internal exclusive-access raw-CDB path.
 * The version descriptors live at INQUIRY bytes 58-73, which macOS's
 * convenience Inquiry (a 36-byte standard-header read) cannot reach — the
 * same layer-1 raw-verb showing as the serial, the same INQUIRY opcode in a
 * different mode (AGENTS.md scope-doctrine ADR; design:
 * doc/research/2026-06-16-drive-identity-enrichment-survey.md).
 *
 * Like the serial it takes ObtainExclusiveAccess (MOS_ERR_BUSY on MOUNTED
 * media — benign: a static drive fact, read with the tray empty). `out`
 * REQUIRED (NULL => MOS_ERR_INVALID_ARG); on success *out is valid until the
 * next query or mos_close().
 */
mos_error mos_query_drive_inquiry(mos_handle_t *h,
                                    const mos_drive_inquiry **out);

/* Accessors. NULL-tolerant. vendor/product/revision are the drive's
   self-reported identity FRESH from this INQUIRY (trailing-trimmed; NULL when
   the reply was too short) — `mos drive` prefers these over the DiscRecording
   cache (mos_handle_vendor/...), which it falls back to. Same display rules as
   the other identity strings (escape drive-controlled bytes before output).
   spc_version is the raw INQUIRY byte 2 (map with mos_spc_version_name; 0 =
   none/unknown). _descriptor_count is the number of non-empty version-
   descriptor codes; _descriptor_code(s, i) returns the i-th (0 for
   out-of-range). Map a descriptor code with mos_version_descriptor_name; an
   unknown code has a NULL name and is surfaced as hex. */
const char *mos_drive_inquiry_vendor(const mos_drive_inquiry *s);
const char *mos_drive_inquiry_product(const mos_drive_inquiry *s);
const char *mos_drive_inquiry_revision(const mos_drive_inquiry *s);
uint8_t  mos_drive_inquiry_spc_version(const mos_drive_inquiry *s);
uint8_t  mos_drive_inquiry_descriptor_count(const mos_drive_inquiry *s);
uint16_t mos_drive_inquiry_descriptor_code(const mos_drive_inquiry *s,
                                             uint8_t i);

/* ---- Disc identity from disc structure -------------- */

/* Result of a disc-structure identity query. Opaque, handle-owned;
   valid until the next mos_query_disc_id() call or mos_close(). */
typedef struct mos_disc_id mos_disc_id;

/*
 * Query the disc's REGISTERED identity from a Blu-ray Disc Information
 * (DI) structure: READ DISC STRUCTURE (BD media type, format 0x00)
 * through the non-exclusive ReadDiscStructure convenience method. This
 * is the manufacturer/media-code data a drive reads to pick its write
 * strategy — including, for Millenniata M-DISC BD, the registered
 * manufacturer "MILLEN" / media type "MR1". BD-ONLY: the DI structure
 * is Blu-ray; on CD/DVD media this returns MOS_ERR_IO (no DI), so
 * callers gate on a BD profile. mos surfaces the registered ID bytes
 * faithfully and does NOT classify them (MILLEN => M-DISC is the
 * consumer's call, same division as MusicBrainz ids). `out` REQUIRED
 * (NULL => MOS_ERR_INVALID_ARG); on success *out is valid until the
 * next query or mos_close().
 */
mos_error mos_query_disc_id(mos_handle_t *h, const mos_disc_id **out);

/* Accessors. NULL/absent reads as NULL. All four are disc-controlled
   ASCII (fixed-width, trailing spaces stripped); escape before display.
   disc_type is "BDR" (BD-R) / "BDW" (BD-RE) / "BDO" (BD-ROM) read from
   the disc's own structure, independent of the MMC profile. */
const char *mos_disc_id_disc_type(const mos_disc_id *d);
const char *mos_disc_id_manufacturer(const mos_disc_id *d);
const char *mos_disc_id_media_type(const mos_disc_id *d);
const char *mos_disc_id_revision(const mos_disc_id *d);

/* ---- CD-TEXT album identity ------------------------- */

/* Result of a CD-TEXT query. Opaque, handle-owned; valid until the next
   mos_query_cdtext() call or mos_close(). */
typedef struct mos_cdtext mos_cdtext;

/*
 * Query the disc-level (album) Title and Performer from CD-TEXT: READ
 * TOC/PMA/ATIP format 0101b through the non-exclusive
 * ReadTableOfContents convenience method (the wrapper mos_query_toc falls
 * back to). The "which album is in the drive" disambiguator,
 * parallel to mos_query_volume for data discs. CD-ONLY: CD-TEXT lives in
 * the lead-in of CD media, so callers gate on a cd profile class; on
 * other media (or a CD without CD-TEXT) this returns MOS_ERR_IO.
 *
 * SCOPE — this surfaces the FIRST language block's album Title and
 * Performer plus the per-track TITLES and PERFORMERS, all single-byte
 * charset; a double-byte (DBCC) field reads as NULL rather than
 * mis-decoded. The other CD-TEXT field types (songwriter/composer/…) and
 * additional language blocks are not decoded. CD-TEXT is BEST-EFFORT
 * DISPLAY TEXT, not a
 * fingerprint: audio-CD dedup keys ride on mos_query_toc, the fail-closed
 * identity primitive. Title/Performer are disc-controlled bytes — escape
 * them before terminals or structured output. `out` REQUIRED (NULL =>
 * MOS_ERR_INVALID_ARG); on success *out is valid until the next query or
 * mos_close().
 */
mos_error mos_query_cdtext(mos_handle_t *h, const mos_cdtext **out);

/* Accessors. NULL/absent reads as NULL (an empty field reads NULL so the
   emitters suppress it uniformly). All are disc-controlled ASCII/Latin-1;
   escape before display. */
const char *mos_cdtext_title(const mos_cdtext *c);
const char *mos_cdtext_performer(const mos_cdtext *c);

/* Per-track titles (song names) and performers (various-artists discs).
   mos_cdtext_track_count is the highest track number that carried a
   non-empty title OR performer (0 = none); both are independently sparse,
   so a track in 1..count may read NULL for either or both fields.
   mos_cdtext_track_title / _track_performer return track n's string (n is
   1-based, 1..MMC track max 99), or NULL when absent/empty or out of
   range. */
uint8_t     mos_cdtext_track_count(const mos_cdtext *c);
const char *mos_cdtext_track_title(const mos_cdtext *c, uint8_t track);
const char *mos_cdtext_track_performer(const mos_cdtext *c, uint8_t track);

/* ---- Physical structure: DVD/HD-DVD ----------------- */

/* Result of a physical-structure query. Opaque, handle-owned; valid
   until the next mos_query_physical_structure() call or mos_close(). */
typedef struct mos_physical_structure mos_physical_structure;

/*
 * Query the disc's Physical Format Information and Copyright Management
 * Information: READ DISC STRUCTURE (DVD/HD-DVD media type) format 0x00
 * and 0x01 through the non-exclusive ReadDiscStructure convenience
 * method. The DVD/HD-DVD analog of mos_query_disc_id (BD DI) — the
 * geometry the disc reports (book type, layer layout, data-area sector
 * boundaries, the layer break) plus the protection-system type and
 * region mask. Named "physical structure", not "dvd", because the same
 * media-type-0 reply carries HD-DVD book types too. DVD/HD-DVD-ONLY: on
 * CD/BD media these formats are absent, so callers gate on a dvd or
 * hd_dvd profile class. Surfaces the spec values faithfully and does NOT
 * classify them (book_type => media name, cpst => "CSS-protected" are
 * the consumer's call). The two formats are read with one query; a drive
 * that answers one but not the other leaves the missing half's
 * mos_physical_structure_have_* accessor false. `out` REQUIRED (NULL =>
 * MOS_ERR_INVALID_ARG); MOS_ERR_IO when neither format parses.
 */
mos_error mos_query_physical_structure(mos_handle_t *h,
                                       const mos_physical_structure **out);

/* Accessors. NULL-tolerant (NULL reads as 0/false). The physical fields
   are meaningful only when have_physical is true; the copyright fields
   only when have_copyright is true. Numeric fields are the raw spec
   codes; map them to names with the helpers below. */
bool     mos_physical_structure_have_physical(const mos_physical_structure *d);
uint8_t  mos_physical_structure_book_type(const mos_physical_structure *d);
uint8_t  mos_physical_structure_part_version(const mos_physical_structure *d);
uint8_t  mos_physical_structure_disc_size(const mos_physical_structure *d);
uint8_t  mos_physical_structure_max_rate(const mos_physical_structure *d);
uint8_t  mos_physical_structure_layer_type(const mos_physical_structure *d);
uint8_t  mos_physical_structure_track_path(const mos_physical_structure *d);
uint8_t  mos_physical_structure_num_layers(const mos_physical_structure *d);
uint8_t  mos_physical_structure_linear_density(const mos_physical_structure *d);
uint8_t  mos_physical_structure_track_density(const mos_physical_structure *d);
bool     mos_physical_structure_bca(const mos_physical_structure *d);
uint32_t mos_physical_structure_start_sector(const mos_physical_structure *d);
uint32_t mos_physical_structure_end_sector(const mos_physical_structure *d);
uint32_t mos_physical_structure_end_sector_l0(const mos_physical_structure *d);
bool     mos_physical_structure_have_copyright(const mos_physical_structure *d);
uint8_t  mos_physical_structure_protection(const mos_physical_structure *d);
uint8_t  mos_physical_structure_region(const mos_physical_structure *d);

/* Stable snake_case token for a DVD/HD-DVD book-type code (0 -> "dvd_rom",
   2 -> "dvd_r", 0x0a -> "dvd_plus_r", 0x4 -> "hd_dvd_rom", ...) or NULL
   if unrecognized (consumers fall back to the numeric code, same
   contract as mos_profile_name). */
const char *mos_book_type_name(uint8_t book_type);
/* "ptp" (parallel, single/sequential) or "otp" (opposite track path). */
const char *mos_track_path_name(uint8_t track_path);
/* Copyright-protection-system token: "none" / "css_cppm" / "cprm" /
   "aacs", or NULL for reserved/unknown codes. */
const char *mos_protection_name(uint8_t protection);

/* ---- Track information / capacity -------------------- */

/* Result of a track-information query. Opaque, handle-owned; valid until
   the next mos_query_track_info() call or mos_close(). */
typedef struct mos_track_info mos_track_info;

/*
 * Query READ TRACK INFORMATION (0x52) for the first track (logical track
 * number 1) through the non-exclusive ReadTrackInformation
 * convenience method: the capacity / append-state surface — track start,
 * next writable address, free blocks, track size, last recorded address,
 * plus the track/data mode and blank/damage bits. For a single-track
 * pressed DVD/BD the track size is effectively the disc capacity; for a
 * blank/appendable recordable, next_writable (when valid) is the append
 * point. Meaningful with media present and the unit ready; a drive that
 * rejects 0x52 returns MOS_ERR_IO. `out` REQUIRED (NULL =>
 * MOS_ERR_INVALID_ARG); on success *out is valid until the next query or
 * mos_close().
 */
mos_error mos_query_track_info(mos_handle_t *h, const mos_track_info **out);

/* Accessors. NULL-tolerant (NULL reads as 0/false). next_writable is
   meaningful only when nwa_valid is true; last_recorded only when
   lra_valid is true — the consumer MUST check the validity accessor. */
uint16_t mos_track_info_track_number(const mos_track_info *t);
uint16_t mos_track_info_session_number(const mos_track_info *t);
uint8_t  mos_track_info_track_mode(const mos_track_info *t);
uint8_t  mos_track_info_data_mode(const mos_track_info *t);
bool     mos_track_info_blank(const mos_track_info *t);
bool     mos_track_info_damage(const mos_track_info *t);
bool     mos_track_info_nwa_valid(const mos_track_info *t);
bool     mos_track_info_lra_valid(const mos_track_info *t);
uint32_t mos_track_info_track_start(const mos_track_info *t);
uint32_t mos_track_info_next_writable(const mos_track_info *t);
uint32_t mos_track_info_free_blocks(const mos_track_info *t);
uint32_t mos_track_info_track_size(const mos_track_info *t);
uint32_t mos_track_info_last_recorded(const mos_track_info *t);

/* ---- Session layout (CD-only, kernel-cached full-TOC) -- */

/* Result of a session-layout query. Opaque, handle-owned; valid until the
   next mos_query_session_layout() call or mos_close(). */
typedef struct mos_session_layout mos_session_layout;

/*
 * Read the macOS kernel-cached full-TOC (kIOCDMediaTOCKey, the Apple CDTOC
 * blob on the IOCDMedia node) and decode it into per-session boundaries —
 * for each session, the first/last track number and the lead-out LBA. This
 * is the richer structure the issued READ TOC format-0000b (mos_query_toc)
 * omits and that READ DISC INFORMATION carries only for the LAST session: it
 * resolves a multi-session disc (e.g. CD-Extra — audio in session 1, a data
 * track in session 2). CD-ONLY: the property exists only on IOCDMedia, so
 * this is meaningful only for the cd media class. ZERO SCSI COMMANDS and no
 * exclusive access — a pure IORegistry read, like the cached capacity, so it
 * works while the disc is mounted. `out` REQUIRED (NULL => MOS_ERR_INVALID_ARG);
 * MOS_ERR_IO when no cached TOC is present (not a CD, no media, or the
 * property is absent) or the blob carried no session boundaries.
 */
mos_error mos_query_session_layout(mos_handle_t *h,
                                   const mos_session_layout **out);

/* Accessors. NULL-tolerant (NULL reads as 0/false). i is a 0-based index in
   [0, count). first_track / last_track read 0 when the session carried no
   POINT 0xA0 / 0xA1 (0 is not a valid track number, so it doubles as the
   "absent" sentinel); leadout_lba is meaningful only when have_leadout. */
uint8_t  mos_session_layout_count(const mos_session_layout *s);
uint8_t  mos_session_layout_session(const mos_session_layout *s, uint8_t i);
uint8_t  mos_session_layout_first_track(const mos_session_layout *s, uint8_t i);
uint8_t  mos_session_layout_last_track(const mos_session_layout *s, uint8_t i);
bool     mos_session_layout_have_leadout(const mos_session_layout *s, uint8_t i);
uint32_t mos_session_layout_leadout_lba(const mos_session_layout *s, uint8_t i);

/* ---- Disc capacity ---------------------------------- */

/* Result of a capacity query. Opaque, handle-owned; valid until the next
   mos_query_capacity() call or mos_close(). */
typedef struct mos_capacity mos_capacity;

/*
 * Assemble the disc's capacity WITHOUT authoring a raw capacity CDB, from
 * three sources already reachable by an open handle:
 *   - the whole-disk IOMedia node's kernel-cached byte and block size
 *     (kIOMediaSizeKey / kIOMediaPreferredBlockSizeKey) — the kernel's own
 *     attach-time READ CAPACITY, read as a registry property with no SCSI
 *     command and no exclusive access, so it works on MOUNTED media (a raw
 *     READ CAPACITY would return BUSY). Re-resolved per call (query-time
 *     semantics; see mos_state_result_bsd_unit).
 *   - the recordable / append view (free blocks, next writable address,
 *     first-track size) from READ TRACK INFORMATION, the same non-exclusive
 *     read mos_query_track_info uses — best-effort.
 *   - the formattable view from READ FORMAT CAPACITIES (0x23), issued via the
 *     non-exclusive ReadFormatCapacities convenience method (MMCDeviceInterface)
 *     — no raw CDB, no exclusive access, so it too works on MOUNTED media. Only
 *     read for formattable profiles (see the formattable accessor below).
 * Each view is independently nullable: the media size is absent on
 * blank/absent media, the recordable view on a pressed disc with no
 * readable track, the formattable view on a non-formattable profile. No raw
 * capacity CDB is authored — the kernel-cached size is a registry read and the
 * other two views are non-exclusive convenience reads. `out` REQUIRED (NULL =>
 * MOS_ERR_INVALID_ARG); on success *out is valid until the next query or
 * mos_close().
 */
mos_error mos_query_capacity(mos_handle_t *h, const mos_capacity **out);

/* Accessors. NULL-tolerant (NULL reads as 0/false). media_bytes /
   block_bytes are 0 when the whole-disk IOMedia node carries no size
   (blank/absent media); media_blocks is then 0. The recordable view
   (free_blocks / next_writable / track_size) is meaningful only when
   have_recordable is true, and next_writable only when nwa_valid. */
bool     mos_capacity_have_media_size(const mos_capacity *c);
uint64_t mos_capacity_media_bytes(const mos_capacity *c);
uint32_t mos_capacity_block_bytes(const mos_capacity *c);
/* Whole-disk size in natural blocks (media_bytes / block_bytes), or 0
   when either is absent — derived, never a separately-reported field. */
uint64_t mos_capacity_media_blocks(const mos_capacity *c);
bool     mos_capacity_have_recordable(const mos_capacity *c);
bool     mos_capacity_nwa_valid(const mos_capacity *c);
uint32_t mos_capacity_free_blocks(const mos_capacity *c);
uint32_t mos_capacity_next_writable(const mos_capacity *c);
uint32_t mos_capacity_track_size(const mos_capacity *c);

/* Formattable view from READ FORMAT CAPACITIES (0x23) — the capacities a
   rewritable medium reports (DVD±RW, DVD-RAM, BD-RE), which the media-size and
   recordable halves above cannot give on a freshly BLANK rewritable (no
   whole-disk node yet, no track to read). Meaningful only when
   have_formattable is true. The read is GATED on the current profile: it runs
   only for formattable media — the rewritable profiles plus BD-R — so pressed /
   write-once CD-R,DVD±R / empty drives report have_formattable=false with no
   read attempted. For those formattable profiles it is issued via the
   non-exclusive ReadFormatCapacities convenience method (MMCDeviceInterface),
   so it takes no exclusive access and the view is reported even on MOUNTED
   formattable media. mos reports these capacities; it never issues FORMAT UNIT.

   The Current/Maximum Capacity Descriptor: format_type is 1 unformatted /
   2 formatted / 3 no-media (map with mos_format_capacity_type_name);
   formattable_blocks x formattable_block_bytes is its capacity. Then the
   Formattable Capacity Descriptor list (the drive's format options):
   _descriptor_count entries (0..MOS max), each read by index — blocks, the
   raw format-type code, and the type-dependent parameter (the block length for
   the common types). Out-of-range i / NULL c read as 0. */
bool     mos_capacity_have_formattable(const mos_capacity *c);
uint8_t  mos_capacity_format_type(const mos_capacity *c);
uint32_t mos_capacity_formattable_blocks(const mos_capacity *c);
uint32_t mos_capacity_formattable_block_bytes(const mos_capacity *c);
uint8_t  mos_capacity_formattable_descriptor_count(const mos_capacity *c);
uint32_t mos_capacity_formattable_descriptor_blocks(const mos_capacity *c,
                                                    uint8_t i);
uint8_t  mos_capacity_formattable_descriptor_type(const mos_capacity *c,
                                                  uint8_t i);
uint32_t mos_capacity_formattable_descriptor_param(const mos_capacity *c,
                                                   uint8_t i);

/* Stable snake_case token for a Current/Maximum Capacity Descriptor type:
   1 -> "unformatted", 2 -> "formatted", 3 -> "no_media"; NULL for 0/reserved
   (consumers fall back to the numeric code). */
const char *mos_format_capacity_type_name(uint8_t type);

/* ---- Drive speeds ----------------------------------- */

/* Result of a drive-speed query. Opaque, handle-owned; valid until the
   next mos_query_drive_perf() call or mos_close(). */
typedef struct mos_drive_perf mos_drive_perf;

/*
 * Query GET PERFORMANCE (0xAC, Type 00h Performance Data) through the
 * non-exclusive GetPerformance convenience method: the drive's supported
 * read/write speeds, summarized as the max read and max write speed
 * (kB/s) and the descriptor count. The MMC-sanctioned modern speed
 * source. MEDIA-DEPENDENT: the
 * write-speed list reflects the loaded medium, so an empty or read-only
 * drive may report zero descriptors — mos_drive_perf_have() is then
 * false, which is data, not an error. `out` REQUIRED (NULL =>
 * MOS_ERR_INVALID_ARG); MOS_ERR_IO when the READ command itself fails.
 *
 * Read vs write direction. The READ direction is the gate: its command-level
 * failure (CHECK CONDITION / malformed) returns MOS_ERR_IO. The WRITE direction
 * is best-effort enrichment — a drive that refuses write GET PERFORMANCE on
 * read-only media, or answers an empty list, leaves max_write_kbps 0 (absent,
 * not an error). A TRANSPORT failure on EITHER direction (device lost / exclusive
 * access lost) is fatal to the whole query and is returned, never flattened to a
 * zero speed.
 */
mos_error mos_query_drive_perf(mos_handle_t *h, const mos_drive_perf **out);

/* Accessors. NULL-tolerant (NULL reads as 0/false). The speeds are
   meaningful only when have is true (>= 1 descriptor). */
bool     mos_drive_perf_have(const mos_drive_perf *p);
uint16_t mos_drive_perf_speed_count(const mos_drive_perf *p);
uint32_t mos_drive_perf_max_read_kbps(const mos_drive_perf *p);
uint32_t mos_drive_perf_max_write_kbps(const mos_drive_perf *p);

/* ---- Mechanical + error-recovery (MODE SENSE) ------------------------ */

/* Results of the two read-only MODE SENSE(10) page reads. Opaque,
   handle-owned; each valid until the next
   call of its query or mos_close(). */
typedef struct mos_mode_caps      mos_mode_caps;
typedef struct mos_error_recovery mos_error_recovery;

/*
 * Query MODE SENSE(10) page 0x2A (MM Capabilities & Mechanical Status)
 * through the non-exclusive ModeSense10 convenience method: the
 * mechanical facts GET CONFIGURATION cannot carry — loading-mechanism
 * type, eject/lock support, the live media-locked bit, and the drive
 * buffer size. Read-only (no MODE SELECT — mos reports, never tunes).
 * `out` REQUIRED; MOS_ERR_IO when the command fails or page 0x2A is
 * absent. Page 0x2A is the MMC-5/6 Legacy annex; mos reads only the
 * non-deprecated residue (speeds come from mos_query_drive_perf).
 */
mos_error mos_query_mode_caps(mos_handle_t *h, const mos_mode_caps **out);

/* Accessors. NULL-tolerant (0/false). loading_mechanism is the raw code;
   map it with mos_loading_mechanism_name(). */
uint8_t  mos_mode_caps_loading_mechanism(const mos_mode_caps *m);
bool     mos_mode_caps_can_eject(const mos_mode_caps *m);
bool     mos_mode_caps_lock_supported(const mos_mode_caps *m);
bool     mos_mode_caps_locked(const mos_mode_caps *m);
uint16_t mos_mode_caps_buffer_kb(const mos_mode_caps *m);

/* Loading-mechanism token: "caddy" / "tray" / "popup" / "changer_disc" /
   "changer_cartridge", or NULL for reserved/unknown codes. */
const char *mos_loading_mechanism_name(uint8_t code);

/*
 * Query MODE SENSE(10) page 0x01 (Read/Write Error Recovery) through the
 * ModeSense10 convenience method: the drive's read error-recovery
 * configuration — AWRE, ARRE, PER, DCR, and the read-retry count.
 * Read-only (the MODE SELECT tuning recovery tools perform is out of
 * scope). `out` REQUIRED; MOS_ERR_IO when the command fails or page 0x01
 * is absent.
 */
mos_error mos_query_error_recovery(mos_handle_t *h,
                                   const mos_error_recovery **out);

/* Accessors. NULL-tolerant (0/false). */
bool    mos_error_recovery_awre(const mos_error_recovery *e);
bool    mos_error_recovery_arre(const mos_error_recovery *e);
bool    mos_error_recovery_per(const mos_error_recovery *e);
bool    mos_error_recovery_dcr(const mos_error_recovery *e);
uint8_t mos_error_recovery_read_retry_count(const mos_error_recovery *e);

/* ---- Feature enumeration ----------------------------- */

/* One MMC feature descriptor's header facts. Opaque; valid only inside
   the callback (stack-backed per iteration, like mos_device_info_t). */
typedef struct mos_feature_info mos_feature_info_t;

uint16_t mos_feature_info_code(const mos_feature_info_t *f);
/* True when the feature is active for the CURRENTLY mounted medium —
   the writability answer: e.g. BD-R write support is feature 0x0041's
   current bit with a writable BD-R inserted, false for the same drive
   with a pressed disc in it. */
bool     mos_feature_info_current(const mos_feature_info_t *f);
bool     mos_feature_info_persistent(const mos_feature_info_t *f);
uint8_t  mos_feature_info_version(const mos_feature_info_t *f);

/*
 * Enumerate every feature the drive reports: one GET CONFIGURATION
 * (RT=0) through the non-exclusive convenience method, walked by the
 * same bounds-checked iterator the typed queries use. The callback
 * runs once per descriptor in reply order; return false to stop early
 * (not an error). Feature PAYLOADS are not exposed — payload facts go
 * public as typed queries (mos_query_drive_caps is the template).
 * Codes are MMC feature numbers (0x0000 Profile List ... 0x010D AACS);
 * consumers map them against MMC-6 §5.3. Same preconditions as
 * mos_query_drive_caps; the feature list itself needs no media, but
 * which features are CURRENT depends on what is mounted.
 */
mos_error mos_enumerate_features(mos_handle_t *h,
                                 bool (*cb)(const mos_feature_info_t *f,
                                            void *ctx),
                                 void *ctx);

/* ---- Tray control ----------------------------------------------------- *
 *
 * The commands mos issues that CHANGE drive state rather than report it
 * (ARCHITECTURE.md §1's reporter-only stance is narrowed, not reversed —
 * the query path never issues these). Each is one raw 6-byte CDB on the
 * internal exclusive-access raw-CDB path, so each
 * acquires and RELEASES exclusive access within the call:
 *
 *   - MOS_ERR_BUSY / MOS_ERR_EXCLUSIVE_ACCESS when the drive is mounted as a
 *     volume or held by another client. close/unlock need the consumer to
 *     quiesce first. Two exceptions: mos_tray_eject GRACEFULLY unmounts a
 *     mounted disc before ejecting (both default and --force — matching `drutil
 *     tray eject`), but NEVER forces, so a BUSY filesystem (open file handles)
 *     surfaces MOS_ERR_BUSY rather than being destroyed; and mos_tray_lock on a
 *     mounted disc returns MOS_OK / ALREADY_LOCKED (a mounted disc is already
 *     removal-locked by macOS — the requested state already holds). Neither path
 *     preempts another userland client (MOS_ERR_EXCLUSIVE_ACCESS).
 *   - On a command the drive ANSWERED, MOS_OK and *out carries the outcome
 *     (DONE / REFUSED_LOCKED / REFUSED_OTHER / ALREADY_LOCKED) — mechanism facts
 *     only.
 *
 * Lock lifetime (T10 04-349r1 §6.18): the PREVENT state is per-I_T-nexus and
 * survives a handle close / process exit — it clears only on bus/LU/hard
 * reset, power on, or an explicit ALLOW on the same initiator. A Mac presents
 * a single initiator, so a stale lock is always clearable by a later
 * mos_tray_unlock on the same host. The lock is therefore FIRE-AND-FORGET:
 * mos holds nothing for the lock window and there is no held-session variant.
 *
 * `out` is REQUIRED (the outcome is the whole point of the call) and is written
 * ONLY on MOS_OK; on a negative return *out is unspecified (a caller must read it
 * only when the call returned MOS_OK). `sense` is
 * OPTIONAL (NULL to ignore): on MOS_OK it receives the {key, asc, ascq} the
 * drive returned — meaningful on a refusal (REFUSED_LOCKED is always asc/ascq
 * 53/02, the sense key 05 with media present or 02 on an empty drive;
 * REFUSED_OTHER is whatever the drive reported, e.g. 5/24/00 for an
 * unsupported Persistent Prevent), all-zero on DONE / ALREADY_LOCKED. Zeroed on
 * a transport failure (negative return). Same shape as the internal raw-CDB
 * sense out-param.
 */

/* Eject the tray / unload the medium (START STOP UNIT 0x1B, LoEj=1 START=0).
   GRACEFUL by design — mos NEVER forces the filesystem:
     - A mounted disc is GRACEFULLY unmounted first (DADiskUnmount, Whole, NO
       Force), then ejected. If a volume is BUSY (open file handles) the unmount
       fails and mos returns MOS_ERR_BUSY — exactly like `diskutil unmountDisk
       diskN`; nothing is destroyed. This happens on the default AND with --force.
     - `force` adds ONE thing: it clears a Prevent LOCK in the way. On
       REFUSED_LOCKED (a basic Prevent) --force clears both Prevent states (so
       nothing is left locked) and re-ejects. WITHOUT force a basic-locked drive
       answers REFUSED_LOCKED, untouched. `--force` means "open past the LOCK",
       never past the filesystem.
   Neither path can preempt another userland client holding exclusive access
   (no SCSI preempt) -> MOS_ERR_EXCLUSIVE_ACCESS, tray closed. The graceful
   unmount needs DiskArbitration; in an opt-out build (MOS_USE_DISKARBITRATION=0)
   a mounted disc reports MOS_ERR_BUSY (the consumer unmounts with `diskutil`
   first). (`sense` reflects the final eject, not the prevent-clears.) */
mos_error mos_tray_eject (mos_handle_t *h, bool force,
                          mos_tray_outcome *out, uint8_t sense[3]);

/* Close / load the tray (START STOP UNIT 0x1B, LoEj=1 START=1). */
mos_error mos_tray_close (mos_handle_t *h,
                          mos_tray_outcome *out, uint8_t sense[3]);

/* Prevent medium removal (PREVENT ALLOW MEDIUM REMOVAL 0x1E, byte4 0x01 — the
   basic Prevent state). Basic Prevent is the HARD removal block: it refuses a
   front-panel eject at the drive. mos does NOT use the Persistent Prevent
   (0x03): on macOS the optical stack honors a level-2/3 lock's cooperative
   soft-eject (a button press raises a GESN EjectRequest the OS acts on), so the
   persistent lock does not block the button. Basic Prevent survives a handle
   close / process exit (it clears only on a real nexus loss — bus reset, power),
   so the fire-and-forget lock outlives the process; release with mos_tray_unlock.
   On a MOUNTED disc the lock CDB cannot issue (media still mounted), but a
   mounted disc is already removal-locked by macOS, so this returns MOS_OK /
   ALREADY_LOCKED rather than MOS_ERR_BUSY. */
mos_error mos_tray_lock  (mos_handle_t *h,
                          mos_tray_outcome *out, uint8_t sense[3]);

/* Allow medium removal (PREVENT ALLOW MEDIUM REMOVAL 0x1E). Clears BOTH Prevent
   states — basic ALLOW 0x00 then persistent ALLOW 0x02 — so the tray ends
   UNLOCKED whichever state was set (the two are independent, 04-349r1 §6.18.2).
   A drive without Persistent Prevent answers the 0x02 with 5/24/00; that is
   tolerated (the basic ALLOW already cleared what such a drive can hold), so the
   outcome is DONE. Cannot run on a MOUNTED disc (media still mounted) —
   MOS_ERR_BUSY; eject to release a mounted disc's lock. */
mos_error mos_tray_unlock(mos_handle_t *h,
                          mos_tray_outcome *out, uint8_t sense[3]);

/* Stable lower_snake_case token for an outcome: "done" / "refused_locked" /
   "refused_other" / "already_locked". Same contract as mos_state_description
   (never NULL). */
const char *mos_tray_outcome_description(mos_tray_outcome o);

/* Safe to call on NULL. Do not call twice on the same handle. */
void mos_close(mos_handle_t *h);

/* ---- Watch API ------------------------------------------------------- */

/*
 * Watch the state of a single drive over time. Emits an immediate
 * initial snapshot, then state-change deltas, then a terminal
 * device_removed event if the drive disappears. Uses IOKit
 * kIOGeneralInterest notifications on macOS to react to termination
 * promptly, plus bounded polling during transition windows (loading,
 * busy, unknown).
 *
 * The watch handle is independent of mos_handle_t — internally it
 * opens its own short-lived mos_handle_t per probe. This keeps the
 * watch loop tolerant of mid-watch driver detach/reattach without
 * inheriting the open handle's reservation state.
 *
 * Threading: not thread-safe. One watch handle per thread, and
 * mos_watch_open_*, mos_watch_next_event, and mos_watch_close must all run
 * on the thread that called open — the watch captures that thread's run
 * loop (CFRunLoopGetCurrent) at open and schedules its IOKit and
 * DiscRecording sources in a private run-loop mode, not
 * kCFRunLoopDefaultMode. The pump runs that same private mode, so wake
 * callbacks dispatch only while mos_watch_next_event is waiting: host-app
 * default-mode work runs alongside undisturbed, and the watch's
 * CFRunLoopStop cannot stop a run-loop invocation the host owns.
 */
typedef struct mos_watch mos_watch_t;

typedef enum {
    MOS_EVENT_SNAPSHOT      = 1, /* initial state on watch open */
    MOS_EVENT_STATE_CHANGED = 2, /* state transitioned */
    MOS_EVENT_ERROR         = 3, /* transient probe error, recovery TBD */
    MOS_EVENT_DEVICE_REMOVED= 4, /* terminal: drive went away */
    MOS_EVENT_MEDIA_CHANGED = 5, /* same-state media swap: drive stayed READY
                                    but the disc was replaced (whole-disk
                                    IOMedia identity changed) */
    MOS_EVENT_DEVICE_APPEARED = 6, /* watch-all only: a drive joined the
                                      stream mid-flight (hot-plug). Carries
                                      the same full payload as a snapshot;
                                      a drive present at open emits snapshot
                                      instead. Single-target watches never
                                      emit this. */
} mos_event_kind;
MOS_ABI_PIN_I32(mos_event_kind);

/* A watch event. Opaque and ABI-stable on the same terms as
   mos_state_result: read through accessors; borrowed, owned by the watch,
   valid until the next mos_watch_next_event() call or mos_watch_close(). */
typedef struct mos_watch_event mos_watch_event;

mos_event_kind mos_watch_event_kind(const mos_watch_event *e);

/* Monotonic counter starting at 1 for the first event in a stream. */
uint64_t       mos_watch_event_seq(const mos_watch_event *e);

/* RFC 3339 UTC timestamp, NUL-terminated (YYYY-MM-DDTHH:MM:SSZ). */
const char    *mos_watch_event_ts(const mos_watch_event *e);

/* Session identity, as two plain values (constant for the stream's life;
   the pair is unique per watch session). registry_id is the watched
   drive's IORegistry entry ID — its attachment identity, which exists for
   the whole plug session including media-less periods (an empty/open-tray
   drive has no BSD name but always has a registry ID; xnu mints real IDs
   from a never-reused monotone counter >= 2^32+256, and a replug is a new
   ID by construction). stream_open_ms is the wall-clock (Unix epoch ms)
   watch-open time, monotonicized per process. Consumers wanting a single
   correlation key can concatenate; mos emits the normalized facts. */
uint64_t       mos_watch_event_registry_id(const mos_watch_event *e);
uint64_t       mos_watch_event_stream_open_ms(const mos_watch_event *e);

/* Drive identity. bsd_unit reflects the media present at the time of THIS
   event (event-time, not watch-open time): it is -1 for an empty/open-tray
   drive and can change across eject/reinsert, while registry_id and
   stream_open_ms stay fixed for the watch session. The identity strings may be NULL. Render the unit
   with mos_bsd_name_format(). */
int64_t        mos_watch_event_bsd_unit(const mos_watch_event *e);
const char    *mos_watch_event_vendor(const mos_watch_event *e);
const char    *mos_watch_event_product(const mos_watch_event *e);
const char    *mos_watch_event_revision(const mos_watch_event *e);

/* Drive serial (Logical Unit Serial Number feature 0108h), the durable inventory
   key that survives replug where registry_id does not. Read from the non-exclusive
   GET CONFIGURATION walk (no raw command, no exclusive access — readable even while
   a disc is mounted), grabbed ONCE per session on a probe handle and cached: NULL
   in early event lines until the probe reaches a terminal outcome (a read, or an
   answered absence), then stable for the session. NULL when the drive does not
   implement the feature / programs no serial (OPTIONAL in MMC, firmware-dependent).
   NULL-tolerant. (mos state never carries serial; it is a watch/drive datum.) */
const char    *mos_watch_event_serial(const mos_watch_event *e);

/* Kernel optical-media type token (see mos_state_result_media_type), or NULL.
   Read zero-MMC off the media node, so it is present even on a not-ready event
   (loading / media_changed) where current_profile is 0 — naming the disc class
   the profile cannot yet. */
const char    *mos_watch_event_media_type(const mos_watch_event *e);

/* Kernel IOMedia Writable flag (see mos_state_result_writable). Tri-state:
   -1 = absent, 0 = read-only, 1 = writable. Present even on a not-ready event,
   like media_type; the kernel's mechanism bit, not a blank/appendable claim. */
int            mos_watch_event_writable(const mos_watch_event *e);

/* GET PERFORMANCE max read / max write speed (kB/s) and the read-direction
   descriptor count for the loaded disc (see mos_query_drive_perf). MEDIA-
   DEPENDENT and grabbed once per media identity, then cached for the session:
   speed_count is 0 (and the rates 0) in early lines until the first ready poll
   for a disc lands the read, then stable until the next media change. Absent on
   error / device_removed events. speed_count == 0 ⇒ no speeds known. */
uint32_t       mos_watch_event_max_read_kbps(const mos_watch_event *e);
uint32_t       mos_watch_event_max_write_kbps(const mos_watch_event *e);
uint16_t       mos_watch_event_speed_count(const mos_watch_event *e);

/* Current and previous state (prev is MOS_STATE_UNKNOWN on snapshot), and
   the current profile — meaningful for snapshot and state_changed events. */
mos_state mos_watch_event_state(const mos_watch_event *e);
mos_state mos_watch_event_prev_state(const mos_watch_event *e);
uint16_t       mos_watch_event_current_profile(const mos_watch_event *e);

/* Sense data when relevant, all zero when not. Any out-param may be NULL. */
void           mos_watch_event_sense(const mos_watch_event *e,
                                               uint8_t *sense_key,
                                               uint8_t *asc, uint8_t *ascq);

/* MOS_OK on non-error events. */
mos_error      mos_watch_event_error(const mos_watch_event *e);

/* Probe duration in ms for this event's underlying query; 0 for events
   that ran no probe (notification-driven removal). */
uint32_t       mos_watch_event_latency_ms(const mos_watch_event *e);

/* Open a watch on a drive. Returns NULL on failure with *err_out set.
   Backoff parameters control the polling rate during stable vs.
   transition states. Default values (0 for either) select sensible
   defaults: stable_ms=2000, transition_ms=200.

   Precondition: transition_poll_ms <= stable_poll_ms. Transitional states
   poll at transition_poll_ms (the faster, smaller interval), and after a
   probe error the retry interval starts at transition_poll_ms and doubles,
   converging UP to stable_poll_ms. An inverted pair (transition > stable) is
   not rejected but degrades: transitional states then poll SLOWER than stable
   ones, and error backoff clamps straight to stable_poll_ms on the first
   error (no progressive backoff). Pass transition_poll_ms <= stable_poll_ms
   to get the intended cadence. */
mos_watch_t *mos_watch_open_by_bsd_name(const char *bsd_name,
                                        uint32_t stable_poll_ms,
                                        uint32_t transition_poll_ms,
                                        mos_error *err_out);

mos_watch_t *mos_watch_open_by_index(int one_based,
                                     uint32_t stable_poll_ms,
                                     uint32_t transition_poll_ms,
                                     mos_error *err_out);

mos_watch_t *mos_watch_open_by_registry_id(uint64_t registry_id,
                                           uint32_t stable_poll_ms,
                                           uint32_t transition_poll_ms,
                                           mos_error *err_out);

/* Open a watch on EVERY optical drive — sit on the bus. The stream
   multiplexes per-drive events, demuxed by registry_id (and bsd when
   media is present); seq is stream-global. Contract differences from
   the single-target handles, documented here because the handle type
   is shared:
     - Each drive present at open emits a snapshot. A drive arriving
       later emits MOS_EVENT_DEVICE_APPEARED (same payload shape as a
       snapshot).
     - MOS_EVENT_DEVICE_REMOVED is PER-DRIVE and non-terminal: the
       stream continues, and the same physical drive replugged joins
       again (new registry_id) with device_appeared. The stream ends
       only at mos_watch_close. Removal detection rides the system's
       device-disappeared notification with the poll as the floor, so
       worst-case removal latency is stable_poll_ms (single-target
       watches additionally hold a kernel interest notification and
       typically see removal faster).
     - Zero drives at open is a valid empty stream that waits for
       arrivals; mos_watch_next_event returns MOS_ERR_TIMEOUT slices
       until something appears.
     - Arrival discovery REQUIRES the system notification source, so
       this open FAILS (NULL, MOS_ERR_IO) if that setup fails —
       unlike the single-target opens, where notifications are
       latency-only over the poll floor, an all-watch without them
       could never see a drive plugged in after open and would wait
       forever on an initially-empty stream.
     - stream_open_ms is the ALL-WATCH's open time, shared by every
       drive's events including later joiners (the event's ts carries
       join time); (registry_id, stream_open_ms) stays per-session
       unique because a replug re-mints the registry_id.
     - Up to 64 concurrently watched drives; arrivals beyond that are
       dropped for that plug session (no rescan when a slot frees — a
       replug re-announces the drive).
   mos_watch_bsd_unit() returns -1 on an all-watch (no single unit).
   Threading/run-loop contract is identical to the single-target opens. */
mos_watch_t *mos_watch_open_all(uint32_t stable_poll_ms,
                                uint32_t transition_poll_ms,
                                mos_error *err_out);

/* Return the next event, waiting up to timeout_ms while the watch is
   idle. `out` is REQUIRED (NULL returns MOS_ERR_INVALID_ARG). On event,
   returns MOS_OK and points *out at a watch-owned event; otherwise a
   non-OK code (MOS_ERR_TIMEOUT if none in time) with *out set to NULL. A
   negative timeout_ms blocks indefinitely.

   TIMEOUT SCOPE: timeout_ms bounds the IDLE wait between probes — the
   run-loop / sleep slices, not an OS/device operation already in flight.
   A probe issues IOKit / DiscRecording / SCSITaskLib convenience calls
   (TEST UNIT READY, GET CONFIGURATION, the raw GESN, the opportunistic
   serial INQUIRY) whose APIs take no caller timeout; a wedged bus or
   kernel transaction can therefore overrun timeout_ms, and a probe that
   has already begun runs to completion. timeout_ms == 0 is thus a
   non-idle-wait bound (drain a ready event, do not sleep), not a hard
   syscall/device deadline — a hard deadline would need worker/process
   isolation the current design does not have.

   MOS_EVENT_DEVICE_REMOVED is terminal only for SINGLE-TARGET watches:
   close the watch; subsequent calls return MOS_ERR_NO_DEVICE. For
   mos_watch_open_all() it is per-drive and non-terminal — keep reading
   the stream (contract block above).

   The event is watch-owned: do not free it or retain it (or any string
   reachable through it) across calls; valid only until the next
   mos_watch_next_event() or mos_watch_close(). Read it through the
   mos_watch_event_* accessors. */
mos_error mos_watch_next_event(mos_watch_t *w, const mos_watch_event **out,
                               int timeout_ms);

/* Safe to call on NULL. */
void mos_watch_close(mos_watch_t *w);

/* Return the whole-disk BSD unit this watch was opened against (the N
   in "diskN"), or -1 if w is NULL or the drive has no media. Render with
   mos_bsd_name_format(). Useful when surfacing pump-level failures in an
   attribution-bearing envelope. */
int64_t mos_watch_bsd_unit(const mos_watch_t *w);

/* ---- String helpers -------------------------------------------------- */

/* Static string table; do not free the returned pointer. */
const char *mos_state_description(mos_state s);
const char *mos_error_description(mos_error e);

/* Stable snake_case profile name for the MMC current-profile code (e.g.
   0x0008 → "cd_rom", 0x0040 → "bd_rom"), or NULL if unrecognized. Tracks
   MMC-6 §5.4 Feature Header Profile Codes; unknown values return NULL so
   consumers can fall back to the hex form. */
const char *mos_profile_name(uint16_t profile_code);

/* Coarse media class for a profile code: "cd", "dvd", "bd", "hd_dvd",
   or NULL when no class applies (0x0000 no-profile, MO, legacy
   removable, unknown codes). The class is derived from the MMC profile
   number ranges, so it costs nothing beyond the GET CONFIGURATION the
   state query already performs — this is what lets `mos list`-style
   output distinguish "the BD in drive A" from "the DVD in drive B" on
   identical hardware. Finer disambiguation by volume name comes from
   mos_query_volume. */
const char *mos_profile_class(uint16_t profile_code);

/* Stable snake_case token for the standard INQUIRY VERSION byte (byte 2):
   0x03 "spc" … 0x07 "spc_5". NULL for 0x00/none, legacy SCSI-1/2, or unknown
   (consumer falls back to the numeric value). */
const char *mos_spc_version_name(uint8_t version);

/* Stable snake_case token for an INQUIRY version-descriptor code (bytes
   58-73), e.g. 0x04E0 → "mmc_6", 0x0460 → "spc_4". Maps the "no version
   claimed" family codes drives emit; a specific-revision or non-listed code
   returns NULL so consumers fall back to the hex code. */
const char *mos_version_descriptor_name(uint16_t code);


/* sysexits.h class for an error, suitable for use as a process exit
   code. EX_OK (0) for MOS_OK; the failure mapping is:
     MOS_ERR_INVALID_ARG       → 64 (EX_USAGE)
     MOS_ERR_NO_DEVICE         → 66 (EX_NOINPUT)
     MOS_ERR_DRIVER_REJECTED   → 69 (EX_UNAVAILABLE)
     MOS_ERR_EXCLUSIVE_ACCESS  → 75 (EX_TEMPFAIL)
     MOS_ERR_BUSY              → 75 (EX_TEMPFAIL)
     MOS_ERR_TIMEOUT           → 75 (EX_TEMPFAIL)
     MOS_ERR_IO                → 74 (EX_IOERR)
     MOS_ERR_UNSUPPORTED       → 69 (EX_UNAVAILABLE)
     MOS_ERR_OOM               → 71 (EX_OSERR)
   Implementation must keep this docstring synchronized with the table
   in src/mos_strings.c. */
int mos_error_sysexit(mos_error e);

/* Retry hint for consuming adapters: true for errors that
   typically clear on retry within seconds (BUSY, TIMEOUT,
   EXCLUSIVE_ACCESS); false for errors that won't clear without
   external action (INVALID_ARG, NO_DEVICE, DRIVER_REJECTED, IO,
   UNSUPPORTED, OOM). MOS_OK returns false (no retry needed). */
bool mos_error_is_recoverable(mos_error e);

/* ---- Pure string escape helpers -------------------------------------
 *
 * Useful to C consumers that emit their own JSON output or render
 * drive-controlled strings (INQUIRY vendor/product) to a terminal.
 * Both write to a caller-provided buffer and return the number of
 * bytes that *would* be written excluding the terminating NUL — like
 * snprintf's return — so callers can detect truncation by checking
 * `return >= out_cap`. Output is always NUL-terminated when out_cap
 * >= 1. Passing NULL `in` is safe (treated as empty).
 *
 * Buffer contract: `out` may be NULL only when `out_cap == 0` — the
 * measure-only mode for sizing a two-pass allocation. Passing NULL
 * `out` with non-zero `out_cap` is a programming error and will
 * dereference NULL. The functions return size_t, not mos_error, so
 * there is no signalling channel for rejecting invalid buffers.
 *
 * Neither function adds surrounding quotes; the caller controls
 * wrapping syntax. */

/* RFC 8259 JSON string-escape. Escapes the seven mandatory forms
 * (", \, \b, \f, \n, \r, \t) plus all bytes outside the printable
 * ASCII range (< 0x20 or >= 0x7f) as \u00XX. 0x7F (DEL) is escaped
 * defensively against terminal-aware downstream consumers. Bytes
 * >= 0x80 are escaped because INQUIRY fields aren't guaranteed
 * UTF-8 — escaping high bytes keeps the output valid JSON regardless
 * of the consumer's encoding. */
size_t mos_json_escape(const char *in, char *out, size_t out_cap);

/* Plain-text safe rendering. Bytes outside the printable ASCII range
 * [0x20, 0x7e] — including control bytes, 0x7F, and high bytes —
 * render as \xNN. Prevents terminal-control-sequence injection
 * (ANSI escape, OSC 52 clipboard, cursor-position-report, title-bar
 * manipulation) when drive-controlled INQUIRY bytes are written to
 * a tty. */
size_t mos_safe_ascii(const char *in, char *out, size_t out_cap);

/* ---- Library version ------------------------------------------------- */

/* Numeric components, for compile-time feature gating by a consumer that
   statically links or vendors mos:  #if MOS_VERSION_HEX >= 0x000500  (>= 0.5.0).
   MOS_VERSION_HEX packs MAJOR.MINOR.PATCH one byte each (0x00MMmmpp) so it is
   monotonic and >=/< comparable. Kept in lockstep with CMake project(VERSION)
   by a configure-time assert (CMakeLists.txt). */
#define MOS_VERSION_MAJOR 0
#define MOS_VERSION_MINOR 4
#define MOS_VERSION_PATCH 0
#define MOS_VERSION_HEX   ((MOS_VERSION_MAJOR << 16) | \
                           (MOS_VERSION_MINOR << 8)  | \
                           (MOS_VERSION_PATCH))

#define MOS_VERSION_STRING "0.4.0"

const char *mos_version_string(void);

#ifdef __cplusplus
}
#endif
#endif /* MOS_H */
