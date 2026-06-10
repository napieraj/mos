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

/* ---- Class list. Order matters: broadest → narrowest. --------------- */

static const char *const mos_internal_classes[] = {
    "IOBDBlockStorageDevice",
    "IODVDBlockStorageDevice",
    "IOCDBlockStorageDevice",
    NULL
};

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

typedef struct {
    uint64_t seen[MOS_ENUM_CAP];
    size_t   seen_count;
} mos_internal_dedup;

static void mos_internal_visit_collect(io_service_t s,
                                       mos_internal_dedup *dedup,
                                       struct mos_device_info *slots,
                                       size_t slot_cap, size_t *slot_n)
{
    /* Registry ID is the public index contract: mos_open_by_index()
       reopens via IORegistryEntryIDMatching against the stored ID.
       If we can't get a valid ID for this entry, skip it — surfacing
       an entry that can't be reopened by index would violate the
       enumeration/index correspondence the public API documents.
       Also guards dedup: id==0 from a failure would collide against
       every other failed lookup, treating unrelated devices as
       duplicates. */
    uint64_t id = 0;
    if (IORegistryEntryGetRegistryEntryID(s, &id) != KERN_SUCCESS || id == 0) {
        return;
    }

    /* Dedup: the same drive can match the IOBD and IODVD classes both.
       If we can't store this ID (seen[] full) we must drop the drive —
       accepting it without storing would let the same drive appear
       twice. The cap itself is documented at the public enumerate API. */
    for (size_t i = 0; i < dedup->seen_count; ++i) {
        if (dedup->seen[i] == id) return;
    }
    if (dedup->seen_count >= MOS_ENUM_CAP) return;
    dedup->seen[dedup->seen_count++] = id;

    if (*slot_n >= slot_cap) return; /* silently drop beyond cap */

    /* Resolve the BSD unit but do NOT gate on it: an empty/open-tray drive
       has no IOMedia child and returns -1, yet we still surface it. Identity
       is registry_id, which mos_open_by_index reopens via
       IORegistryEntryIDMatching (not BSD name), so a nameless drive stays
       reachable by index/enumeration — just not by mos_open_by_bsd_name,
       which is correct. */
    struct mos_device_info *info = &slots[(*slot_n)++];
    memset(info, 0, sizeof(*info));
    info->registry_id  = id;
    info->bsd_unit     = mos_internal_bsd_unit(s, NULL);   /* -1 if no media */

    /* TODO: INQUIRY at enumeration time is expensive and requires
       opening the drive. We skip it here; vendor/product/revision come
       back NULL from the enumeration callback. Callers who need them
       should mos_open_by_bsd_name() and inspect mos_state_result.vendor
       et al. */
}

static int mos_internal_info_cmp_by_id(const void *a, const void *b)
{
    uint64_t ia = ((const struct mos_device_info *)a)->registry_id;
    uint64_t ib = ((const struct mos_device_info *)b)->registry_id;
    if (ia < ib) return -1;
    if (ia > ib) return  1;
    return 0;
}

void mos_enumerate_devices(mos_enumerate_cb cb, void *ctx)
{
    if (!cb) return;

    /* Two-phase enumeration: collect into a local array, sort by
       registry ID (to satisfy the public ordering contract — see the
       mos_enumerate_devices() docstring in mos.h), then invoke the
       callback in sorted order. */
    struct mos_device_info slots[MOS_ENUM_CAP];
    size_t slot_n = 0;

    mos_internal_dedup dedup = { {0}, 0 };

    for (const char *const *cls = mos_internal_classes; *cls; ++cls) {
        CFMutableDictionaryRef m = IOServiceMatching(*cls);
        if (!m) continue;

        /* IOServiceGetMatchingServices consumes the matching dictionary
           reference regardless of success — same contract as in
           mos_open_by_bsd_name (below in this file). */
        io_iterator_t it MOS_IO_AUTO = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it)
                != KERN_SUCCESS) continue;

        for (;;) {
            io_service_t s MOS_IO_AUTO = IOIteratorNext(it);
            if (s == IO_OBJECT_NULL) break;
            mos_internal_visit_collect(s, &dedup, slots, MOS_ENUM_CAP, &slot_n);
        }
    }

    if (slot_n > 1) {
        qsort(slots, slot_n, sizeof(slots[0]), mos_internal_info_cmp_by_id);
    }

    for (size_t i = 0; i < slot_n; ++i) {
        if (!cb(&slots[i], ctx)) break;
    }
}

/* Enumeration yields bsd_unit + registry_id only (no INQUIRY — see the
   skip in visit_collect). Returns -1 for an empty/open-tray drive. */
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
    /* Best-effort, not a gate (as in visit_collect): a nameless empty drive
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
        if (err) *err = MOS_ERR_DRIVER_REJECTED;
        mos_close(h);
        return NULL;
    }

    /* Populate vendor/product via INQUIRY. Non-fatal on failure. */
    (void)mos_internal_mmc_inquiry(h);

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
       enumeration-only. (err_out is already MOS_ERR_INVALID_ARG.) */
    int64_t want_unit = mos_internal_parse_bsd_unit(want);
    if (want_unit < 0) return NULL;

    for (const char *const *cls = mos_internal_classes; *cls; ++cls) {
        CFMutableDictionaryRef m = IOServiceMatching(*cls);
        if (!m) continue;

        /* IOServiceGetMatchingServices consumes the matching dictionary
           reference regardless of success (Apple docs), so we do NOT
           release m on either path. */
        io_iterator_t it MOS_IO_AUTO = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it)
                != KERN_SUCCESS) continue;

        for (;;) {
            io_service_t s MOS_IO_AUTO = IOIteratorNext(it);
            if (s == IO_OBJECT_NULL) break;

            if (mos_internal_bsd_unit(s, NULL) == want_unit) {
                /* Transfer ownership of `s` to mos_internal_open_service:
                   null the local to disable MOS_IO_AUTO so it isn't
                   double-released (the callee owns it on both success and
                   failure). `s`'s +1 from IOIteratorNext is independent of
                   `it`, so it stays valid past the iterator's release. */
                io_service_t consumed = s;
                s = IO_OBJECT_NULL;
                return mos_internal_open_service(consumed, err_out);
            }
        }
    }
    if (err_out) *err_out = MOS_ERR_NO_DEVICE;
    return NULL;
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
       canonical sort order, then reopen the captured ID via
       IORegistryEntryIDMatching. The kernel resolves that second match
       atomically, so a hot-plug between passes either succeeds or returns
       NO_DEVICE — never silently opens a different drive that inherited the
       original BSD name. */

    mos_internal_id_collect c = { {0}, 0 };
    mos_enumerate_devices(mos_internal_collect_cb, &c);
    if ((size_t)one_based > c.count) {
        if (err_out) *err_out = MOS_ERR_NO_DEVICE;
        return NULL;
    }
    return mos_internal_open_by_registry_id(c.ids[one_based - 1], err_out);
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

/* RETURN-CONVENTION NOTE (deliberate asymmetry with mmc_inquiry): this
   function returns MOS_ERR_IO for command-reached-drive-but-unusable
   replies (non-GOOD status, truncated GOOD), while mmc_inquiry returns
   MOS_OK with cleared strings for the same shape. The asymmetry is
   load-bearing, not drift: the profile's in-band "absent" value, 0x0000,
   is a REAL drive answer ("no current profile"), so a malformed reply
   must be distinguishable out-of-band or it masquerades as legitimate
   no-media — the exact silent-0x0000 bug the third review fixed. Identity
   strings have no such collision: an empty vendor is never a meaningful
   drive answer, so in-band clearing suffices there. Both callers treat
   both shapes as non-fatal enrichment skips. */
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

/* Copy a fixed-width, space-padded SCSI string field of `len` bytes into
   `dst`, NUL-terminating at dst[len] and trimming trailing spaces/NULs per
   SPC-4. Interior bytes are preserved verbatim; the output layer
   (mos_json_escape / mos_safe_ascii) sanitizes hostile content.

   No runtime length clamp, by design: `len` is never drive-controlled — it
   is a compile-time INQUIRY field width (SPC-4: VENDOR=8, PRODUCT=16,
   REVISION=4) — and a _Static_assert at each call site checks
   sizeof(dst) > len, so a mis-sized buffer fails the build rather than
   truncating silently. The drive controls the contents, never the width. */
static void mos_internal_copy_scsi_string(char *dst,
                                          const char *src, size_t len)
{
    memcpy(dst, src, len);
    dst[len] = 0;
    for (size_t i = len; i > 0; --i) {
        unsigned char c = (unsigned char)dst[i - 1];
        if (c != ' ' && c != 0) break;
        dst[i - 1] = 0;
    }
}

/* Returns MOS_OK with CLEARED identity strings when the command reached
   the drive but the reply is unusable — see the return-convention note
   on mos_internal_mmc_get_current_profile for why this differs from the
   profile path on purpose. */
mos_error mos_internal_mmc_inquiry(mos_handle_t *h)
{
    if (!h || !h->mmc) return MOS_ERR_INVALID_ARG;

    /* Inquiry fills a typed SCSICmd_INQUIRY_StandardData struct (not a raw
       byte array); we read VENDOR_IDENTIFICATION and PRODUCT_IDENTIFICATION,
       space-padded per SPC-4 (trimmed on copy below). */
    SCSICmd_INQUIRY_StandardData inq  = {0};
    SCSITaskStatus               st   = 0;
    SCSI_Sense_Data              sd   = {0};

    IOReturn rc = (*h->mmc)->Inquiry(
        h->mmc,
        &inq, (UInt32)sizeof(inq),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        h->vendor_str[0]  = 0;
        h->product_str[0] = 0;
        h->revision_str[0] = 0;
        return (rc == kIOReturnSuccess) ? MOS_OK
                                        : mos_internal_ioreturn_to_mos_error(rc);
    }

    /* Each destination is sized field+1; pin that at build time so a
       buffer resize can't silently start truncating. sizeof(h->field)
       is a constant expression (non-VLA array member), so these are
       compile-time checks despite `h` being a runtime pointer. */
    _Static_assert(sizeof(h->vendor_str) > kINQUIRY_VENDOR_IDENTIFICATION_Length,
                   "vendor_str must hold the INQUIRY vendor field + NUL");
    mos_internal_copy_scsi_string(h->vendor_str,
                                  inq.VENDOR_IDENTIFICATION,
                                  kINQUIRY_VENDOR_IDENTIFICATION_Length);

    _Static_assert(sizeof(h->product_str) > kINQUIRY_PRODUCT_IDENTIFICATION_Length,
                   "product_str must hold the INQUIRY product field + NUL");
    mos_internal_copy_scsi_string(h->product_str,
                                  inq.PRODUCT_IDENTIFICATION,
                                  kINQUIRY_PRODUCT_IDENTIFICATION_Length);

    /* PRODUCT_REVISION_LEVEL is 4 ASCII bytes per SPC-4 §6.4.2,
       space-padded like the vendor/product fields. */
    _Static_assert(sizeof(h->revision_str) > kINQUIRY_PRODUCT_REVISION_LEVEL_Length,
                   "revision_str must hold the INQUIRY revision field + NUL");
    mos_internal_copy_scsi_string(h->revision_str,
                                  inq.PRODUCT_REVISION_LEVEL,
                                  kINQUIRY_PRODUCT_REVISION_LEVEL_Length);

    return MOS_OK;
}

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
    if (!h || !cdb || !scsi_task_status || !sense)
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
        /* Release the lock we just acquired — we can't make forward
           progress with this task and holding it serves no purpose. */
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
