/*
 * mos_scsi.c — IOKit lifecycle, enumeration, the MMC convenience
 * primitives the state core uses (TUR / current-profile / tray-state),
 * and the one raw-CDB path (mos_raw_cdb — the sole ObtainExclusiveAccess
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
            whole_block = (uint32_t)mos_internal_cf_number_u64(child,
                              CFSTR(kIOMediaPreferredBlockSizeKey));
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

/* mos_raw_cdb passes mos_xfer_dir straight to SetScatterGatherEntries, so
   the public values MUST equal the SDK's kSCSIDataTransfer_* enumerators —
   a renumbering would send CDBs in the wrong direction. Pin them. */
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

/* The pure tray classifier (mos_pure.c) compares mos_raw_cdb's task status
   against MOS_SCSI_STATUS_GOOD but can't include the SDK to check they
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
       mos_raw_cdb acquires and releases the lock per call — single-shot,
       never held.

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

mos_error mos_raw_cdb(mos_handle_t *h,
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

    /* Invariant pin (debug): every raw_cdb call releases exclusive access on
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
