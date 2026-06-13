/*
 * mos.h — mac-optical-state public C API (version: MOS_VERSION_STRING below)
 *
 * Pure-C public surface for querying macOS optical drive state via
 * MMCDeviceInterface. Raw SCSITaskDeviceInterface access is exposed
 * through mos_raw_cdb(); the default state path issues one raw CDB
 * (GESN) on its not-ready branch, briefly taking the exclusive lock
 * — safe by the nub invariant. See ARCHITECTURE.md §3/§5.5.
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
} mos_state_enum;
MOS_ABI_PIN_I32(mos_state_enum);

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
    MOS_ERR_BUSY              = -5, /* contention from another client (e.g.
                                       raw-CDB callers competing for exclusive
                                       access) — NOT "media is mounted", which
                                       the default path handles correctly. */
    MOS_ERR_TIMEOUT           = -6,
    MOS_ERR_IO                = -7, /* unexpected IOKit / kernel error */
    MOS_ERR_UNSUPPORTED       = -8, /* command not supported by drive */
    MOS_ERR_OOM               = -9,
} mos_error;
MOS_ABI_PIN_I32(mos_error);

/* Transfer direction constants for mos_raw_cdb(). */
typedef enum {
    MOS_XFER_NONE         = 0,
    MOS_XFER_FROM_TARGET  = 2, /* drive → host (read-like) */
    MOS_XFER_TO_TARGET    = 1, /* host → drive (write-like) */
} mos_xfer_dir;
MOS_ABI_PIN_I32(mos_xfer_dir);

/* READ DISC INFORMATION disc status (MMC-6 0x51 byte 2, bits 1:0) —
   the disc-completion signal a blank-vs-finalized decision needs. */
typedef enum {
    MOS_DISC_BLANK      = 0,  /* empty recordable                  */
    MOS_DISC_APPENDABLE = 1,  /* incomplete session, writable      */
    MOS_DISC_COMPLETE   = 2,  /* finalized                         */
    MOS_DISC_OTHER      = 3,  /* reserved/other (random-recordable) */
} mos_disc_status;
MOS_ABI_PIN_I32(mos_disc_status);

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
mos_state_enum mos_state_result_state(const mos_state_result *r);

/* Whole-disk BSD unit (the N in "diskN"), or -1 when there is no media
   (empty/open tray) and hence no resolvable name. Render to "diskN" with
   mos_bsd_name_format().

   OPEN-TIME SEMANTICS (v0.3, still current): for a held handle this is
   captured once at mos_open* and NOT refreshed per query — a handle
   opened on an empty drive keeps reporting -1 even after a query
   returns READY for newly inserted media. Re-open the handle for fresh
   naming, or use the watch API, whose events carry event-time units.
   (The PLANNED v0.4 held-handle refresh — not yet implemented; the
   DiscRecording substrate it builds on landed 2026-06-10 — will make
   the field query-time via kDRDeviceMediaBSDNameKey; see ROADMAP
   "Standing context".) */
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

/* ---- Disc information (v0.4 typed API) ------------------------------- */

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

/* "blank" / "appendable" / "complete" / "other". Stable lowercase
   tokens, same contract as mos_state_description(). */
const char     *mos_disc_status_description(mos_disc_status s);

/* ---- Table of contents (v0.4 typed API) ------------------------------ */

/* Result of a TOC query. Opaque, handle-owned, read through the
   accessors; valid until the next mos_query_toc() call or mos_close(). */
typedef struct mos_toc mos_toc;

/*
 * Query READ TOC/PMA/ATIP format 0000b (LBA) — the normalized table of
 * contents, the disc-identity primitive. Issued on demand through the
 * non-exclusive ReadTableOfContents convenience method; never part of
 * the state path. FAIL-CLOSED end to end: a hostile or incoherent TOC
 * (out-of-range, duplicate, or non-ascending tracks; a header span the
 * descriptor list doesn't cover) returns MOS_ERR_IO with *out NULL —
 * identity from a half-parsed TOC would be a falsely-stable
 * fingerprint. A TOC without a lead-out parses; identity consumers
 * must require mos_toc_have_leadout(). Same media preconditions as
 * mos_query_disc_info(). `out` REQUIRED (NULL → MOS_ERR_INVALID_ARG).
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

/* ---- Mounted volume (v0.4, DiskArbitration-sourced) ------------------ */

/*
 * Mounted-volume name and mount path for the drive's current media —
 * what the FILESYSTEM layer says, complementing the SCSI-derived facts
 * above. One synchronous DiskArbitration description read; no
 * callbacks, no commands to the drive, no elevation. Gated on the
 * whole-disk IOMedia node existing: media absent or no nub means DA is
 * never consulted and *mounted is false. That nub is the handle's
 * OPEN-TIME bsd_unit (same open-time-capture semantics as
 * mos_state_result_bsd_unit above), NOT refreshed per query — so a
 * handle opened on an empty drive reports unmounted for its whole life
 * even after media is inserted and a later query returns READY. Re-open
 * the handle (or use the watch API) for a freshly-inserted disc's
 * volume. UNMOUNTED IS NOT AN ERROR — it is also the common case for
 * UDF video discs — so the result is MOS_OK with *mounted=false and
 * empty buffers; only a NULL handle returns
 * MOS_ERR_INVALID_ARG. Buffers are optional (NULL/0 skips that field)
 * and always NUL-terminated; name may be "" even when mounted (a
 * volume can lack a label). Recommended caps: name 256, path 1024 —
 * a mount path that exceeds the cap reports unmounted rather than a
 * truncated path. Volume names are disc-controlled bytes: escape them
 * before terminals or structured output.
 *
 * OPTIONAL DEPENDENCY: DiskArbitration is the only thing this call
 * needs. A build with MOS_USE_DISKARBITRATION=0 (no -framework
 * DiskArbitration) keeps this function and its contract — it simply
 * always reports unmounted (MOS_OK, *mounted=false, empty buffers), so
 * the CLI and JSON shapes are identical with the volume fields null.
 */
mos_error mos_query_volume(mos_handle_t *h, bool *mounted,
                           char *name_buf, size_t name_cap,
                           char *path_buf, size_t path_cap);

/* ---- Drive capabilities (v0.4 typed API) ----------------------------- */

/* Result of a drive-capabilities query. Opaque, handle-owned; valid
   until the next mos_query_drive_caps() call or mos_close(). */
typedef struct mos_drive_caps mos_drive_caps;

/*
 * Query the drive's AACS capability facts: one full GET CONFIGURATION
 * (RT=0) through the non-exclusive convenience method, decoded by the
 * bounds-checked feature walk. These are the spec-grounded fields a
 * MakeMKV drive dump shows ("Highest AACS version", bus-encryption
 * support) WITHOUT the LibreDrive status synthesis, which is a MakeMKV
 * database property, not a drive property (design doc 2026-06-10).
 * bus_encryption is the DRIVE-REPORTED support bit from the AACS
 * feature payload (0x010D byte 0 bit 1) — a firmware assertion; the
 * cryptographically signed BEC bit lives in the AACS drive
 * certificate behind a raw REPORT KEY, which mos does not issue
 * (scope doctrine: no raw verb without GESN-grade justification).
 * Drives without the AACS feature (every non-BD unit) report
 * aacs=false — that is data, not an error.
 */
mos_error mos_query_drive_caps(mos_handle_t *h, const mos_drive_caps **out);

/* Accessors. NULL-tolerant (NULL reads as 0/false). aacs_version and
   bus_encryption are meaningful only when aacs is true. */
bool    mos_drive_caps_aacs(const mos_drive_caps *c);
uint8_t mos_drive_caps_aacs_version(const mos_drive_caps *c);
bool    mos_drive_caps_bus_encryption(const mos_drive_caps *c);

/* Drive identity for a held handle — the open-time directory data
   (DiscRecording pre-parsed INQUIRY strings) and the drive service's
   registry entry ID. Zero commands. Strings are borrowed from the
   handle (valid until mos_close), NULL when the directory had no
   identity; registry id 0 = unavailable. */
const char *mos_handle_vendor(const mos_handle_t *h);
const char *mos_handle_product(const mos_handle_t *h);
const char *mos_handle_revision(const mos_handle_t *h);
uint64_t    mos_handle_registry_id(const mos_handle_t *h);

/* ---- Disc identity from disc structure (v0.4 typed API) -------------- */

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

/* ---- Physical structure: DVD/HD-DVD (v0.4 typed API) ----------------- */

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

/* ---- Track information / capacity (v0.4 typed API) -------------------- */

/* Result of a track-information query. Opaque, handle-owned; valid until
   the next mos_query_track_info() call or mos_close(). */
typedef struct mos_track_info mos_track_info;

/*
 * Query READ TRACK INFORMATION (0x52) for the first track (the track
 * containing LBA 0) through the non-exclusive ReadTrackInformation
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

/* ---- Feature enumeration (v0.4 typed API) ----------------------------- */

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

/*
 * Diagnostic: issue a raw CDB against the drive. Requires exclusive
 * access; returns MOS_ERR_BUSY or MOS_ERR_EXCLUSIVE_ACCESS if the drive
 * is mounted or held by another process. For fixture capture and hardware
 * investigation — not production surface, and on the deprecation path for
 * v0.4 (prefer mos_query_state() and the typed APIs).
 *
 * cdb_len must be 6, 10, 12, or 16 (the lengths SCSITaskLib accepts);
 * other values return MOS_ERR_INVALID_ARG. timeout_ms must be > 0 — 0 is
 * SCSITaskLib's "Wait Forever", rejected at the boundary rather than
 * silently applied. scsi_task_status and sense are required (raw
 * diagnostics are uninformative without them); bytes_transferred may be
 * NULL.
 *
 * Exclusive access is acquired and released within the single call, so a
 * diagnostic does not leave the drive blocked against Finder /
 * DiskArbitration; a sequence of raw CDBs pays one obtain/release each.
 */
mos_error mos_raw_cdb(mos_handle_t *h,
                      const uint8_t *cdb, size_t cdb_len,
                      void *data_buf, size_t data_len,
                      mos_xfer_dir direction,
                      uint32_t timeout_ms,
                      /* out: */
                      uint32_t *scsi_task_status,
                      uint8_t   sense[18],
                      uint64_t *bytes_transferred);

/* Safe to call on NULL. Do not call twice on the same handle. */
void mos_close(mos_handle_t *h);

/* ---- Watch API (v0.3+) ---------------------------------------------- */

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

/* Current and previous state (prev is MOS_STATE_UNKNOWN on snapshot), and
   the current profile — meaningful for snapshot and state_changed events. */
mos_state_enum mos_watch_event_state(const mos_watch_event *e);
mos_state_enum mos_watch_event_prev_state(const mos_watch_event *e);
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
   defaults: stable_ms=2000, transition_ms=200. */
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
     - Up to 16 concurrently watched drives; arrivals beyond that are
       dropped for that plug session (no rescan when a slot frees — a
       replug re-announces the drive).
   mos_watch_bsd_unit() returns -1 on an all-watch (no single unit).
   Threading/run-loop contract is identical to the single-target opens. */
mos_watch_t *mos_watch_open_all(uint32_t stable_poll_ms,
                                uint32_t transition_poll_ms,
                                mos_error *err_out);

/* Block until the next event or until timeout_ms elapses with no
   transition. `out` is REQUIRED (NULL returns MOS_ERR_INVALID_ARG). On
   event, returns MOS_OK and points *out at a watch-owned event;
   otherwise a non-OK code (MOS_ERR_TIMEOUT if none in time) with *out
   set to NULL. A negative timeout_ms blocks indefinitely.

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
const char *mos_state_description(mos_state_enum s);
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
   identical hardware. Finer disambiguation (volume name) is the v0.4
   media-info work; see doc/research/2026-06-10-media-info-design.md. */
const char *mos_profile_class(uint16_t profile_code);


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

#define MOS_VERSION_STRING "0.4.0-dev"

const char *mos_version_string(void);

#ifdef __cplusplus
}
#endif
#endif /* MOS_H */
