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

#include "mos_internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <DiscRecording/DRCoreDevice.h>
#include <IOKit/IOKitLib.h>

#include <string.h>

/* Bounded CFString-ish → C-buffer copy. CFStringGetCString FAILS
   outright (no partial write contract we may rely on) when the buffer
   is too small, so conversion goes through a generous temp and
   truncates with strlcpy — dst is always NUL-terminated, oversize
   input truncates instead of vanishing. Non-string values (a hostile
   or surprising dictionary) yield "". */
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
   documented "unavailable" sentinel, never a fabricated ID). */
static uint64_t mos_internal_dr_id_for_path_cf(CFTypeRef path)
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

/* Fill identity + registry id from one device's Info dictionary.
   Identity buffers keep the SPC-4 field widths (the data is the same
   INQUIRY bytes, pre-parsed by DR); oversize values truncate per
   mos_internal_dr_copy_string. */
static void mos_internal_dr_fill_from_info(CFDictionaryRef info,
                                           mos_internal_dr_snapshot *s)
{
    s->registry_id = mos_internal_dr_id_for_path_cf(
        CFDictionaryGetValue(info, kDRDeviceIORegistryEntryPathKey));
    mos_internal_dr_copy_string(
        CFDictionaryGetValue(info, kDRDeviceVendorNameKey),
        s->vendor, sizeof s->vendor);
    mos_internal_dr_copy_string(
        CFDictionaryGetValue(info, kDRDeviceProductNameKey),
        s->product, sizeof s->product);
    mos_internal_dr_copy_string(
        CFDictionaryGetValue(info, kDRDeviceFirmwareRevisionKey),
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

size_t mos_internal_dr_copy_snapshot(mos_internal_dr_snapshot *slots,
                                     size_t cap)
{
    if (!slots || cap == 0) return 0;

    CFArrayRef arr = DRCopyDeviceArray();
    if (!arr) return 0;

    CFIndex n = CFArrayGetCount(arr);
    size_t out = 0;
    for (CFIndex i = 0; i < n && out < cap; ++i) {
        DRDeviceRef dev = (DRDeviceRef)CFArrayGetValueAtIndex(arr, i);
        if (!dev) continue;

        mos_internal_dr_snapshot *s = &slots[out];
        memset(s, 0, sizeof *s);
        s->bsd_unit = -1;

        CFDictionaryRef info = DRDeviceCopyInfo(dev);
        if (info) {
            mos_internal_dr_fill_from_info(info, s);
            CFRelease(info);
        }
        /* No reopenable identity ⇒ skip (see header comment). The
           index is the position among reopenable devices, which is
           the DR array order whenever every device resolves — the
           expected case. */
        if (s->registry_id == 0) continue;

        CFDictionaryRef status = DRDeviceCopyStatus(dev);
        if (status) {
            s->bsd_unit = mos_internal_dr_bsd_unit_from_status(status);
            CFRelease(status);
        }
        out++;
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
        id = mos_internal_dr_id_for_path_cf(
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
        mos_internal_dr_copy_string(
            CFDictionaryGetValue(info, kDRDeviceVendorNameKey),
            vendor, vcap);
        mos_internal_dr_copy_string(
            CFDictionaryGetValue(info, kDRDeviceProductNameKey),
            product, pcap);
        mos_internal_dr_copy_string(
            CFDictionaryGetValue(info, kDRDeviceFirmwareRevisionKey),
            revision, rcap);
        CFRelease(info);
        ok = true;
    }
    CFRelease(dev);
    return ok;
}
