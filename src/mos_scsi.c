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

#include "mos_internal.h"
#include "mos_scsi_status.h"

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

/* Capture the handle's whole-disk media identity in ONE registry walk:
   bsd_unit, the whole-IOMedia entry ID (media_id, the swap fingerprint),
   kernel-cached size/block, the optical "Type" token, and the Writable flag —
   all read off the SAME IOMedia node so the six fields describe one media
   generation. (Earlier this was three independent recursive walks — bsd_unit,
   read_media_type, read_writable — whose results could straddle a media swap;
   R3's mos_state.c audit, 2026-06-20, demonstrated the resulting incoherent
   results. One node, one generation.) All fields fail closed to their absent
   sentinels (unit -1, id/size/block 0, type NULL, writable -1) when no media
   node is present. Zero SCSI commands, no exclusive access. */
/* One capture attempt. *invalidated is set true iff the registry iterator
   went stale mid-walk: IOIteratorNext returns IO_OBJECT_NULL for BOTH
   exhaustion AND invalidation (a topology change — concurrent insert/eject —
   invalidates a recursive iterator), so IOIteratorIsValid is the only way to
   tell them apart (IOKitLib). On invalidation the partial walk is untrustworthy
   — returns false WITHOUT committing, so the caller retries rather than
   reporting a churn as genuine media absence. Returns true (with s populated or
   left at its absent sentinels) on a clean walk. */
static bool mos_internal_capture_media_snapshot_once(io_service_t svc,
                                                     mos_media_snapshot *s,
                                                     bool *invalidated)
{
    *invalidated = false;
    s->bsd_unit    = -1;
    s->media_id    = 0;
    s->media_bytes = 0;
    s->block_bytes = 0;
    s->media_type  = NULL;
    s->writable    = -1;

    io_iterator_t it MOS_IO_AUTO = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(svc, kIOServicePlane,
            kIORegistryIterateRecursively, &it) != KERN_SUCCESS) {
        return true;   /* no iterator: absent, not an invalidation */
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
    const char *whole_type = NULL;
    signed char whole_writable = -1;

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

            /* "Type" token and Writable flag off the SAME node — the
               coherence the redesign buys. Gated on the optical media class
               exactly as the prior separate walks were (string-name
               conformance avoids the class headers); a non-optical whole node
               leaves both absent. */
            if (IOObjectConformsTo(child, "IOCDMedia")  ||
                IOObjectConformsTo(child, "IODVDMedia") ||
                IOObjectConformsTo(child, "IOBDMedia")) {
                CFTypeRef tv MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                        child, CFSTR("Type"), kCFAllocatorDefault, 0);
                char type[16] = {0};
                if (tv && CFGetTypeID(tv) == CFStringGetTypeID() &&
                    CFStringGetCString((CFStringRef)tv, type, sizeof type,
                                       kCFStringEncodingUTF8) && type[0]) {
                    whole_type = mos_internal_media_type_token(type);
                }
                CFTypeRef wv MOS_CF_AUTO = IORegistryEntryCreateCFProperty(
                        child, CFSTR(kIOMediaWritableKey), kCFAllocatorDefault, 0);
                if (wv && CFGetTypeID(wv) == CFBooleanGetTypeID()) {
                    whole_writable = CFBooleanGetValue((CFBooleanRef)wv) ? 1 : 0;
                }
            }
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

    /* Distinguish a genuine end-of-walk from an iterator the kernel
       invalidated under topology churn. On invalidation, refuse to commit
       (even a found Whole node may be racing an eject) and signal a retry. */
    if (!IOIteratorIsValid(it)) {
        *invalidated = true;
        return false;
    }

    const char *chosen = whole_name[0] ? whole_name
                       : fallback_name[0] ? fallback_name
                       : NULL;
    if (!chosen) return true;   /* no media node: every field keeps its sentinel */
    /* parse_bsd_unit normalizes any rdisk/ /dev/ prefix, rejects
       partition/non-whole shapes, and returns the unit or -1. Identity is
       an integer from here; "diskN" is reconstructed only at output. */
    s->bsd_unit = mos_internal_parse_bsd_unit(chosen);
    if (whole_name[0]) {
        /* Size, id, type, and writability ride the Whole node only — the
           fallback bridge node's are not the medium's (see the no-id
           reasoning above), so they stay at their sentinels for it. */
        s->media_id    = whole_id;
        s->media_bytes = whole_bytes;
        s->block_bytes = whole_block;
        s->media_type  = whole_type;
        s->writable    = whole_writable;
    }
    return true;
}

void mos_internal_capture_media_snapshot(io_service_t svc,
                                         mos_media_snapshot *s)
{
    if (!s) return;
    /* Retry once on iterator invalidation (topology churn). On a second
       invalidation, fall through leaving the absent sentinels the _once helper
       wrote — a caller with the S1/S2 guard turns persistent churn into
       MOS_ERR_BUSY rather than publishing a mix; a caller without it sees
       "absent", the safe direction. */
    for (int i = 0; i < 2; ++i) {
        bool invalidated = false;
        if (mos_internal_capture_media_snapshot_once(svc, s, &invalidated))
            return;
        if (!invalidated)
            return;
    }
}

/* Commit a captured snapshot to the handle's media-scoped fields. */
void mos_internal_apply_media_snapshot(mos_handle_t *h,
                                       const mos_media_snapshot *s)
{
    if (!h || !s) return;
    h->bsd_unit          = s->bsd_unit;
    h->media_id          = s->media_id;
    h->media_bytes       = s->media_bytes;
    h->media_block_bytes = s->block_bytes;
    h->media_type        = s->media_type;
    h->writable          = s->writable;
}

/* Two snapshots describe the same media generation iff the swap fingerprint
   (whole-IOMedia entry id) AND the whole-disk unit agree. media_id is the
   strong key — re-minted on a swap, so it catches an A→B change even when
   "diskN" is reused; bsd_unit carries the no-id fallback and the present↔absent
   transition (N ↔ -1). Used to detect a media change across a state query. */
bool mos_internal_media_snapshot_coherent(const mos_media_snapshot *a,
                                          const mos_media_snapshot *b)
{
    if (!a || !b) return false;
    return a->media_id == b->media_id && a->bsd_unit == b->bsd_unit;
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
    mos_media_snapshot s;
    mos_internal_capture_media_snapshot(h->svc, &s);
    mos_internal_apply_media_snapshot(h, &s);
}

/* Copy the kernel-cached full-TOC (kIOCDMediaTOCKey, a CDTOC CFData blob) off
   the drive's IOCDMedia node. CD-only: the property exists only on IOCDMedia
   (no DVD/BD equivalent — those expose only a media-type string). A pure
   IORegistry read, no SCSI command and no exclusive access, like the cached
   kIOMediaSize capacity — so it works on mounted media. Returns the bytes
   copied (clamped to cap), 0 when absent. */
/* One CD-TOC read attempt; *invalidated set true iff the iterator went stale
   mid-walk (see capture_media_snapshot_once — IOIteratorNext returns NULL for
   exhaustion AND invalidation). Returns bytes copied, or 0 when absent; on
   invalidation returns 0 with *invalidated true so the caller retries rather
   than reporting a churn as an absent TOC. */
static size_t mos_internal_read_cdtoc_once(io_service_t svc, uint8_t *buf,
                                           size_t cap, bool *invalidated)
{
    *invalidated = false;
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
        return copy;   /* found a valid TOC: no retry needed */
    }
    /* Exhausted with no TOC: distinguish genuine absence from a stale iterator
       so an insert/eject during the walk does not masquerade as no-TOC. */
    if (!IOIteratorIsValid(it)) *invalidated = true;
    return 0;
}

size_t mos_internal_read_cdtoc(io_service_t svc, uint8_t *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    /* Retry once on iterator invalidation (topology churn); see
       mos_internal_capture_media_snapshot. */
    for (int i = 0; i < 2; ++i) {
        bool invalidated = false;
        size_t n = mos_internal_read_cdtoc_once(svc, buf, cap, &invalidated);
        if (n || !invalidated) return n;
    }
    return 0;
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
        h->revision_str, sizeof h->revision_str,
        h->interconnect_str,          sizeof h->interconnect_str,
        h->interconnect_location_str, sizeof h->interconnect_location_str);

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

/* Release the exclusive lock, clearing have_exclusive ONLY on a kernel-
   confirmed release. SCSITaskLib defines a non-success ReleaseExclusiveAccess
   as "release not confirmed": the in-kernel logical-unit driver may still be
   quiesced. On that, the flag STAYS true and the handle is POISONED —
   mos_internal_raw_cdb fails closed on the next call (it can prove neither that
   it still holds nor that it has released the lock), and mos_close makes a
   final best-effort retry. Caller must hold the lock. */
static mos_error mos_internal_release_exclusive(mos_handle_t *h)
{
    assert(h && h->std && h->have_exclusive);
    IOReturn rc = (*h->std)->ReleaseExclusiveAccess(h->std);
    if (rc != kIOReturnSuccess)
        return mos_internal_ioreturn_to_mos_error(rc);
    h->have_exclusive = false;
    return MOS_OK;
}

void mos_close(mos_handle_t *h)
{
    if (!h) return;
    if (h->std) {
        /* Final best-effort release of a still-held (or poisoned) lock before
           dropping the interface. A still-failing release leaves have_exclusive
           true, but the interface Release below tears down the user client,
           which the kernel cleans up on connection close. */
        if (h->have_exclusive) (void)mos_internal_release_exclusive(h);
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

const char *mos_handle_interconnect(const mos_handle_t *h)
{
    return (h && h->interconnect_str[0]) ? h->interconnect_str : NULL;
}

const char *mos_handle_interconnect_location(const mos_handle_t *h)
{
    return (h && h->interconnect_location_str[0])
               ? h->interconnect_location_str : NULL;
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

    /* Fail closed on a POISONED handle. The "lock never held across calls"
       guarantee holds only while every release is kernel-confirmed; a prior
       ReleaseExclusiveAccess that returned non-success left have_exclusive true
       (the kernel may still treat this client as the logical-unit driver). We
       can prove neither that we hold nor that we released the lock, so refuse
       rather than issue a CDB on an unproven lock — mos_close retries the
       release. (This replaces the old debug-only assert with a real release-
       build gate; see mos_internal_release_exclusive.) */
    if (h->have_exclusive)
        return MOS_ERR_IO;

    /* Raw CDB requires exclusive access. */
    IOReturn rc = (*h->std)->ObtainExclusiveAccess(h->std);
    if (rc != kIOReturnSuccess)
        return mos_internal_ioreturn_to_mos_error(rc);
    h->have_exclusive = true;

    /* Single post-acquire epilogue: every path below converges on `cleanup`,
       which releases the task (if any) and then attempts EXACTLY ONE release
       via the checked helper. A release failure takes PRECEDENCE over the
       command result — its consequences are system-wide (the drive stays
       locked out of Finder / DA) — and leaves the scalar outputs at their zero
       init. A diagnostic command must never hold the lock for the handle's
       lifetime. */
    SCSITaskInterface **t               = NULL;
    mos_error           operation_error = MOS_OK;
    mos_error           release_error;
    bool                reply_valid     = false;
    SCSI_Sense_Data     sense_struct    = {0};
    SCSITaskStatus      st              = 0;
    UInt64              xferred         = 0;
    IOVirtualRange      sg = { (IOVirtualAddress)data_buf, (IOByteCount)data_len };

    t = (*h->std)->CreateSCSITask(h->std);
    if (!t) {
        operation_error = MOS_ERR_IO;
        goto cleanup;
    }

    /* Check each Set* IOReturn — ignoring one ships a malformed task and a
       baffling execute-time error. */
    rc = (*t)->SetCommandDescriptorBlock(t, (UInt8 *)cdb, (UInt8)cdb_len);
    if (rc != kIOReturnSuccess) {
        operation_error = mos_internal_ioreturn_to_mos_error(rc);
        goto cleanup;
    }
    rc = (*t)->SetScatterGatherEntries(t,
        data_len ? &sg : NULL,
        (UInt8)(data_len ? 1 : 0),
        (UInt64)data_len,
        (UInt8)direction);
    if (rc != kIOReturnSuccess) {
        operation_error = mos_internal_ioreturn_to_mos_error(rc);
        goto cleanup;
    }
    rc = (*t)->SetTimeoutDuration(t, timeout_ms);
    if (rc != kIOReturnSuccess) {
        operation_error = mos_internal_ioreturn_to_mos_error(rc);
        goto cleanup;
    }

    /* sense_struct/st/xferred are zero-initialized above, so a transport
       failure (or a partial sense fill) copies defined data, never garbage. */
    rc = (*t)->ExecuteTaskSync(t, &sense_struct, &st, &xferred);
    if (rc != kIOReturnSuccess)
        operation_error = mos_internal_ioreturn_to_mos_error(rc);
    else
        reply_valid = true;

cleanup:
    if (t)
        (*t)->Release(t);

    /* Exactly one release attempt per acquire. */
    release_error = mos_internal_release_exclusive(h);
    if (release_error != MOS_OK)
        return release_error;   /* unlock failure: precedence; outputs stay zero */
    if (operation_error != MOS_OK)
        return operation_error;
    if (!reply_valid)
        return MOS_ERR_IO;      /* release-build backstop; not reachable today */

    *scsi_task_status = (uint32_t)st;
    memcpy(sense, &sense_struct, 18);
    if (bytes_transferred) *bytes_transferred = (uint64_t)xferred;
    return MOS_OK;
}
