/*
 * mos_dr.c — DiscRecording-side adapter: the directory.
 *
 * Supplies discovery, identity, and addressing from the DiscRecording C API;
 * it never decides drive STATE — the TUR⊕GESN core in mos_state_core.c is the
 * sole authority (the §5.5 nub gate runs on TUR sense bytes DR does not
 * expose). DR is not a SCSI command author (AGENTS.md scope doctrine): it is
 * a substrate above the same kext the MMC path uses, and DR authors no raw
 * CDB itself — every raw verb (GESN, the tray opcodes, INQUIRY) lives in the
 * MMC path via mos_internal_raw_cdb (AGENTS.md tracks the running count).
 *
 * Identity resolution: DR exposes a device's IORegistry *path*
 * (kDRDeviceIORegistryEntryPathKey), not its entry ID. mos's identity
 * currency — registry_id, the reopen authority, the media-swap fingerprint —
 * is the uint64 entry ID, so each path resolves path → entry → ID here. An
 * unresolvable node is skipped, preserving the index ↔ open-by-index
 * correspondence the public API documents.
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

#include <stdio.h>
#include <string.h>

/* Strict CFString → C-buffer copy: COMPLETE-OR-EMPTY, the mos_vpd80 identity
   rule applied to DR's dictionary strings. The full UTF-8 form is written only
   if it fits in cap-1 bytes AND carries no interior NUL; otherwise dst is left
   empty. No truncated write: a value over-wide for its field (the real domain
   is ≤16 bytes — an absurdly long value is hostile per scope-doctrine layer 4)
   and an interior NUL (which would sever the field at a downstream C-string
   sink, the mos_vpd80 bug class) both collapse to "", which every reader treats
   as absent. CFStringGetBytes (not CFStringGetCString) reports both the
   converted-character count and the realized byte count, so the device's
   claimed length and its delivery are both checked. Non-string values yield "".
   dst is always NUL-terminated. Returns whether a complete value was written. */
static bool mos_internal_dr_cfstring_strict(CFTypeRef value,
                                            char *dst, size_t cap)
{
    if (!dst || cap == 0) return false;
    dst[0] = 0;
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return false;

    CFStringRef s = (CFStringRef)value;
    CFIndex nchars = CFStringGetLength(s);
    CFIndex used   = 0;
    CFIndex got = CFStringGetBytes(s, CFRangeMake(0, nchars),
                                   kCFStringEncodingUTF8, 0 /*no loss byte*/,
                                   false /*no BOM*/, (UInt8 *)dst,
                                   (CFIndex)(cap - 1), &used);
    /* Not every character fit in cap-1 bytes ⇒ over-width ⇒ empty. */
    if (got != nchars || used < 0 || (size_t)used >= cap) {
        dst[0] = 0;
        return false;
    }
    /* A complete value of `used` bytes must carry no interior NUL. */
    if (memchr(dst, 0, (size_t)used) != NULL) {
        dst[0] = 0;
        return false;
    }
    dst[used] = 0;
    return true;
}

/* Bounded CFString → C-buffer copy, complete-or-empty (see the strict helper).
   Shared with the DA volume lookup (mos_da.c), which reads volume-controlled
   strings under the same trust terms. */
void mos_internal_dr_copy_string(CFTypeRef value, char *dst, size_t cap)
{
    (void)mos_internal_dr_cfstring_strict(value, dst, cap);
}

/* path → IORegistry entry → uint64 entry ID; 0 on any failure (the
   documented "unavailable" sentinel, never a fabricated ID). Also used by
   the watch adapter's DR-doorbell per-device filter. */
uint64_t mos_internal_dr_id_for_path_value(CFTypeRef path)
{
    io_string_t p;
    /* Reject a non-string, over-width, or interior-NUL path outright. An
       interior NUL would sever the path at IORegistryEntryFromPath's C-string
       boundary, so a prefix could resolve a DIFFERENT entry and fabricate a
       non-zero WRONG registry_id — defeating the 0 = "unavailable" sentinel
       that disarms swap detection. Complete-or-empty keeps a malformed path
       at the 0 sentinel. */
    if (!mos_internal_dr_cfstring_strict(path, p, sizeof p)) return 0;
    if (p[0] == 0) return 0;

    io_registry_entry_t e MOS_IO_AUTO =
        IORegistryEntryFromPath(kIOMainPortDefault, p);
    if (e == IO_OBJECT_NULL) return 0;

    uint64_t id = 0;
    if (IORegistryEntryGetRegistryEntryID(e, &id) != KERN_SUCCESS) id = 0;
    return id;
}

/* Strip trailing spaces (SPC wire padding DR may or may not have trimmed).
   Leading/interior spaces are data and stay. */
static void mos_internal_dr_strip_trailing_spaces(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}

/* The single extraction of the three identity strings; every reader funnels
   through here. Buffers keep the SPC-4 field widths. */
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
    if (vendor && vcap) mos_internal_dr_strip_trailing_spaces(vendor);
    if (product && pcap) mos_internal_dr_strip_trailing_spaces(product);
    if (revision && rcap) mos_internal_dr_strip_trailing_spaces(revision);
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

/* The media BSD name lives in the Status dict's media-info sub-dictionary
   (kDRDeviceMediaInfoKey → kDRDeviceMediaBSDNameKey), media-scoped: absent
   with no media loaded, hence unit -1. */
static int64_t mos_internal_dr_bsd_unit_from_status(CFDictionaryRef status)
{
    CFTypeRef mi = CFDictionaryGetValue(status, kDRDeviceMediaInfoKey);
    if (!mi || CFGetTypeID(mi) != CFDictionaryGetTypeID()) return -1;

    char name[MOS_BSD_NAME_CAP];
    mos_internal_dr_copy_string(
        CFDictionaryGetValue((CFDictionaryRef)mi, kDRDeviceMediaBSDNameKey),
        name, sizeof name);
    if (name[0] == 0) return -1;
    /* Normalizes rdisk//dev/ forms and rejects partition shapes — same
       authority as everywhere else. */
    return mos_internal_parse_bsd_unit(name);
}

bool mos_internal_dr_device_snapshot(CFTypeRef device_ref,
                                     mos_internal_dr_snapshot *s)
{
    if (!device_ref || !s) return false;
    DRDeviceRef dev = (DRDeviceRef)device_ref;

    memset(s, 0, sizeof *s);
    s->bsd_unit = -1;

    /* A DRDeviceRef can go stale while held: DRCopyDeviceArray is a point-in-
       time snapshot, and a device can be unplugged between that snapshot and
       these reads. DRDeviceIsValid exists precisely for this — it checks
       whether a held reference is "still usable" (DRCoreDevice.h). Validate
       before reading, again before the status read, and once more before
       committing; otherwise a vanished device's stale identity strings could
       be paired with a registry id that a reused topology path now resolves
       to. Build into a local temp and commit only if valid throughout. */
    if (!DRDeviceIsValid(dev)) return false;

    mos_internal_dr_snapshot tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.bsd_unit = -1;

    CFDictionaryRef info = DRDeviceCopyInfo(dev);
    if (!info) return false;
    mos_internal_dr_fill_from_info(info, &tmp);
    CFRelease(info);

    /* No reopenable identity ⇒ not usable (header). */
    if (tmp.registry_id == 0) return false;
    if (!DRDeviceIsValid(dev)) return false;

    CFDictionaryRef status = DRDeviceCopyStatus(dev);
    if (status) {
        tmp.bsd_unit = mos_internal_dr_bsd_unit_from_status(status);
        CFRelease(status);
    }

    if (!DRDeviceIsValid(dev)) return false;   /* vanished mid-read: refuse */
    *s = tmp;
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
        /* Skip-not-fail: an unresolvable entry must not hide its siblings.
           The index is the position among reopenable devices — DR array
           order when every device resolves, the expected case. */
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
    /* Callers pass the canonical "diskN" form (mos_bsd_name_format). */
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

/* kDRDevicePhysicalInterconnectKey CFString value -> the small int code
   mos_internal_interconnect_token names. 0 (absent / unrecognized) -> the
   field is omitted. CFEqual against the SDK constants, not string parsing. */
static int mos_internal_dr_interconnect_code(CFTypeRef v)
{
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return 0;
    CFStringRef s = (CFStringRef)v;
    if (CFEqual(s, kDRDevicePhysicalInterconnectATAPI))        return 1;
    if (CFEqual(s, kDRDevicePhysicalInterconnectFibreChannel)) return 2;
    if (CFEqual(s, kDRDevicePhysicalInterconnectFireWire))     return 3;
    if (CFEqual(s, kDRDevicePhysicalInterconnectUSB))          return 4;
    if (CFEqual(s, kDRDevicePhysicalInterconnectSCSI))         return 5;
    return 0;
}

/* kDRDevicePhysicalInterconnectLocationKey CFString value -> int code. */
static int mos_internal_dr_location_code(CFTypeRef v)
{
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return 0;
    CFStringRef s = (CFStringRef)v;
    if (CFEqual(s, kDRDevicePhysicalInterconnectLocationInternal)) return 1;
    if (CFEqual(s, kDRDevicePhysicalInterconnectLocationExternal)) return 2;
    if (CFEqual(s, kDRDevicePhysicalInterconnectLocationUnknown))  return 3;
    return 0;
}

/* Physical interconnect (bus) + location tokens from a DR info dict, into
   caller buffers; empty string when the key is absent / unrecognized. */
static void mos_internal_dr_read_interconnect(CFDictionaryRef info,
                                              char *ic, size_t iccap,
                                              char *loc, size_t loccap)
{
    if (ic && iccap) ic[0] = 0;
    if (loc && loccap) loc[0] = 0;
    if (!info) return;
    const char *t = mos_internal_interconnect_token(
        mos_internal_dr_interconnect_code(
            CFDictionaryGetValue(info, kDRDevicePhysicalInterconnectKey)));
    if (t && ic && iccap) snprintf(ic, iccap, "%s", t);
    const char *lt = mos_internal_interconnect_location_token(
        mos_internal_dr_location_code(
            CFDictionaryGetValue(info, kDRDevicePhysicalInterconnectLocationKey)));
    if (lt && loc && loccap) snprintf(loc, loccap, "%s", lt);
}

bool mos_internal_dr_copy_identity_for_service(io_service_t svc,
                                               char *vendor, size_t vcap,
                                               char *product, size_t pcap,
                                               char *revision, size_t rcap,
                                               char *interconnect, size_t iccap,
                                               char *location, size_t loccap)
{
    if (vendor && vcap) vendor[0] = 0;
    if (product && pcap) product[0] = 0;
    if (interconnect && iccap) interconnect[0] = 0;
    if (location && loccap) location[0] = 0;
    if (revision && rcap) revision[0] = 0;
    if (svc == IO_OBJECT_NULL) return false;

    /* The identity must describe THIS service. Capture svc's own registry id
       up front; the DR device we resolve by path must report the SAME id, or a
       reused topology path could pair another device's vendor/product/revision
       with this handle. A zero id (unresolvable) fails closed. */
    uint64_t expected_id = 0;
    if (IORegistryEntryGetRegistryEntryID(svc, &expected_id) != KERN_SUCCESS ||
        expected_id == 0) {
        return false;
    }

    io_string_t path;
    if (IORegistryEntryGetPath(svc, kIOServicePlane, path) != KERN_SUCCESS) {
        return false;
    }
    CFStringRef cfpath = CFStringCreateWithCString(kCFAllocatorDefault, path,
                                                   kCFStringEncodingUTF8);
    if (!cfpath) return false;

    DRDeviceRef dev = DRDeviceCopyDeviceForIORegistryEntryPath(cfpath);
    CFRelease(cfpath);
    if (!dev) return false; /* identity stays empty, non-fatal */

    bool ok = false;
    /* A DRDeviceRef can go stale while held (DRDeviceIsValid, DRCoreDevice.h);
       validate before and after reading its info. */
    if (DRDeviceIsValid(dev)) {
        CFDictionaryRef info = DRDeviceCopyInfo(dev);
        if (info) {
            /* Bind: the DR device's own IORegistry path must resolve to the
               SAME registry id as the service, and the device must still be
               valid, before its strings are trusted. */
            uint64_t got_id = mos_internal_dr_id_for_path_value(
                CFDictionaryGetValue(info, kDRDeviceIORegistryEntryPathKey));
            if (got_id == expected_id && DRDeviceIsValid(dev)) {
                mos_internal_dr_copy_identity_from_info(info, vendor, vcap,
                                                        product, pcap,
                                                        revision, rcap);
                mos_internal_dr_read_interconnect(info, interconnect, iccap,
                                                  location, loccap);
                ok = true;
            }
            CFRelease(info);
        }
    }
    CFRelease(dev);
    return ok;
}
