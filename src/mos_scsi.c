/*
 * mos_scsi.c — IOKit lifecycle, enumeration, MMC convenience wrappers.
 * The only IOKit-linked file in the library.
 *
 * All internal functions must be `static` or `mos_internal_`-prefixed;
 * this library is statically linked into downstream projects (HandBrake
 * etc.) and cannot collide on generic names.
 */

#include "mos_internal.h"

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
            /* Deliberately NO id capture on this branch. media_id's
               contract is "whole-disk IOMedia registry entry ID"
               (mos_pure.h) — re-minted on a physical swap. A
               non-IOMedia bridge node's ID identifies the BRIDGE, not
               the medium, and plausibly survives a swap; a stale
               non-zero fingerprint disarms BOTH swap detectors at
               once (id_changed needs both ids non-zero-and-different;
               the profile-class fallback needs both zero —
               mos_watch_core.c). Zero is the documented "unavailable,
               don't infer a swap" sentinel and keeps the no-id
               fallback armed. Whether real bridges take this branch
               at all is a rig question (hardware checklist). */
        }

        if (whole_name[0] != 0) break; /* Whole found, done */
    }

    const char *chosen = whole_name[0] ? whole_name
                       : fallback_name[0] ? fallback_name
                       : NULL;
    if (!chosen) return -1;
    if (media_id_out) *media_id_out = whole_name[0] ? whole_id : 0;
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

mos_handle_t *mos_open_by_registry_id(uint64_t registry_id,
                                      mos_error *err_out)
{
    if (registry_id == 0) {
        if (err_out) *err_out = MOS_ERR_INVALID_ARG;
        return NULL;
    }
    return mos_internal_open_by_registry_id(registry_id, err_out);
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
    uint64_t id = mos_device_info_registry_id(info);
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

mos_error mos_query_toc(mos_handle_t *h, const mos_toc **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* Format 0000b worst case: 4-byte header + 100 descriptors
       (99 tracks + lead-out) x 8. The convenience method reports no
       realized count, so sizeof buf is the trusted length (O-4); the
       reply's own TOC Data Length only ever shrinks the parse. MSF=0
       (LBA), starting track 0 (= from first). Non-exclusive: no lock. */
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
        return MOS_ERR_IO;   /* incoherent/hostile TOC — refused whole */
    }
    *out = &h->toc;
    return MOS_OK;
}

mos_error mos_query_drive_caps(mos_handle_t *h, const mos_drive_caps **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* RT=0: header + every feature the drive implements. 1024 bytes
       holds real-world feature lists several times over (a loaded
       BD-RE's full list runs ~400 bytes); a longer hostile claim is
       simply clamped — the walker trusts sizeof buf, and the reply's
       own lengths only ever shrink the walk (O-4). Decode is the pure,
       fuzz-checked mos_internal_aacs_caps_from_config: feature absent
       (every non-BD drive) reads as aacs=false, which is data, not an
       error. Non-exclusive convenience call: no lock interaction. */
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

    mos_internal_aacs_caps_from_config(buf, sizeof(buf), &h->caps);
    *out = &h->caps;
    return MOS_OK;
}

mos_error mos_enumerate_features(mos_handle_t *h,
                                 bool (*cb)(const mos_feature_info_t *f,
                                            void *ctx),
                                 void *ctx)
{
    if (!h || !h->mmc || !cb) return MOS_ERR_INVALID_ARG;

    /* Same issuance and trust terms as mos_query_drive_caps above:
       RT=0 into a zero-init 1024-byte buffer, sizeof buf is the
       trusted length, reply lengths only shrink the walk (O-4). */
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
        mos_feature_info info = {
            .code       = feat.feature_code,
            .current    = feat.current,
            .persistent = feat.persistent,
            .version    = feat.version,
        };
        if (!cb(&info, ctx)) break;     /* caller stop, not an error */
    }
    return MOS_OK;
}

mos_error mos_query_disc_id(mos_handle_t *h, const mos_disc_id **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* One-shot read of the full BD Disc Information into a fixed,
       zero-init buffer (BD DI maxes ~3588 bytes; 4096 covers it). We
       deliberately do NOT do dvd+rw-mediainfo's two-phase
       read-the-length-then-reallocate dance: a single fixed buffer
       means no device-reported length ever drives an allocation or a
       second transfer. sizeof buf is the trusted length handed to the
       pure decoder (O-4); the reply's own Disc Structure Data Length
       can only SHRINK the parse, never extend it, and an under-filled
       reply leaves zeros that fail the 'DI' gate. MEDIA_TYPE=1 (BD),
       FORMAT=0x00 (Disc Information), ADDRESS/LAYER 0. Non-exclusive
       convenience call: no lock. */
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

    /* Two convenience reads of READ DISC STRUCTURE for the DVD/HD-DVD
       media type (MEDIA_TYPE=0): FORMAT 0x00 (Physical Format
       Information) and FORMAT 0x01 (Copyright Management Information).
       Each into a fixed zero-init buffer — sizeof buf is the trusted
       length (O-4); the reply's own Disc Structure Data Length can only
       SHRINK the parse, and an under-filled reply fails the per-format
       min-length gate. Both halves merge into one handle-owned struct;
       the reads are independent (partial-readability ladder), so a drive
       that answers one format but not the other still yields the half it
       gave. No lock (convenience). */
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
        return MOS_ERR_IO;   /* neither format answered (non-DVD, or refused) */
    }
    *out = d;
    return MOS_OK;
}

mos_error mos_query_track_info(mos_handle_t *h, const mos_track_info **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* READ TRACK INFORMATION (0x52) for the first track via the
       convenience method. ADDRESS_TYPE = 01b (logical track number),
       ADDRESS = 1 (the first track) — well-defined on any media with a
       track. 64 bytes covers the Track Information Block (the core block
       is 36 bytes; MMC-6 extends it slightly). sizeof buf is the trusted
       length (O-4); the reply's Track Information Length only shrinks the
       parse. No lock (convenience). Signature confirmed against
       SCSITaskLib.h (ADDRESS_NUMBER_TYPE, LBA/track/session, buffer,
       bufferSize, taskStatus, senseData). */
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
        return MOS_ERR_IO;   /* truncated/short reply — refused whole */
    }
    *out = &h->track_info;
    return MOS_OK;
}

/* One GET PERFORMANCE (0xAC) Performance Data read in the given
   direction (WRITE=0 read, WRITE=1 write). TOLERANCE=10b nominal,
   EXCEPT=0 (nominal performance), STARTING_LBA=0. Returns MOS_OK and the
   decoded max performance (kB/s) + descriptor count, or a non-OK code on
   command failure. Convenience method: no lock. */
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

    /* Two Performance Data reads — read direction (WRITE=0) and write
       direction (WRITE=1) — assembled into one result. The read read is
       the gate (its success defines `have`); the write read is
       best-effort (a read-only drive or non-writable medium simply
       leaves max_write_kbps 0). */
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
    p->have             = (rd_cnt > 0 || wr_cnt > 0);
    *out = p;
    return MOS_OK;
}

/* Shared MODE SENSE(10) issuance for the two read-only optical pages
   (AGENTS scope addendum 2026-06-13). Signature confirmed against
   SCSITaskLib.h (LLBAA, DBD, PC, PAGE_CODE, buffer, bufferSize,
   taskStatus, senseData). PC = 00b (current values); DBD=1 (no block
   descriptor) keeps the reply compact, though the pure walker tolerates
   a descriptor either way. Non-exclusive convenience: no lock. */
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

/* Open-time directory identity, exposed for the drive verb: zero
   commands, same borrowed-string terms as the state result's copies
   (which point into these same buffers). */
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
