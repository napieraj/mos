/*
 * mos_da.c — one-shot DiskArbitration volume lookup.
 *
 * DA's RE-ADMISSION IS NARROW (AGENTS.md append, 2026-06-12): the
 * 2026-06-11 retirement removed DA's CALLBACK roles (the watch's wake
 * legs, the probe's control arm) and those do not return. This file is
 * the other modality: a synchronous DADiskCopyDescription read of what
 * the mounted-volume layer already knows — no session scheduling, no
 * run loop, no callbacks, no commands to the drive. Callers gate on
 * the media nub existing (bsd_unit present); with no IOMedia node
 * there is nothing mounted and DA is never consulted.
 *
 * Trust terms: the description dictionary is system-supplied but the
 * values are volume-controlled (a hostile disc names its volume), so
 * string extraction goes through the same bounded, type-checked,
 * fail-to-empty seam as the DR identity copies. The CLI's output
 * escaping guards the terminal/JSON regardless.
 */

#include "mos_internal.h"

/* DiskArbitration is an OPTIONAL link dependency. Build with
   -DMOS_USE_DISKARBITRATION=0 (and drop -framework DiskArbitration) and
   mos_query_volume always reports unmounted — volume name/path null,
   which the CLI and JSON schemas already permit, so no shape changes.
   Default on (matches the linked build). */
#ifndef MOS_USE_DISKARBITRATION
#define MOS_USE_DISKARBITRATION 1
#endif

#if MOS_USE_DISKARBITRATION
#include <DiskArbitration/DiskArbitration.h>

/* Mounted-volume name and mount path for a whole-disk bsd_name
   ("diskN"). True only when DA has a description AND the volume is
   mounted (VolumePath present); name may still be empty ("") if the
   key is absent or hostile — the caller maps empty to null. Both
   buffers are always NUL-terminated. */
bool mos_internal_da_volume(const char *bsd_name,
                            char *name_buf, size_t name_cap,
                            char *path_buf, size_t path_cap)
{
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    if (!bsd_name || !bsd_name[0]) return false;

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return false;

    bool      mounted = false;
    DADiskRef disk    = DADiskCreateFromBSDName(kCFAllocatorDefault,
                                                session, bsd_name);
    CFDictionaryRef desc = disk ? DADiskCopyDescription(disk) : NULL;

    if (desc) {
        /* VolumePath is the mount proof: DA describes unmounted media
           too, and an absent/non-URL path means "not mounted", not an
           error. CFURLGetFileSystemRepresentation fails (false) when
           the path exceeds the buffer; that yields not-mounted rather
           than a truncated path a consumer might chdir into. */
        CFTypeRef path = CFDictionaryGetValue(
            desc, kDADiskDescriptionVolumePathKey);
        if (path && CFGetTypeID(path) == CFURLGetTypeID() &&
            path_buf && path_cap &&
            CFURLGetFileSystemRepresentation((CFURLRef)path, true,
                                             (UInt8 *)path_buf,
                                             (CFIndex)path_cap)) {
            mounted = true;
            mos_internal_dr_copy_string(
                CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey),
                name_buf, name_cap);
        } else if (path_buf && path_cap) {
            path_buf[0] = 0;
        }
        CFRelease(desc);
    }

    if (disk)    CFRelease(disk);
    CFRelease(session);
    return mounted;
}

#else  /* !MOS_USE_DISKARBITRATION */

/* No DiskArbitration linked: the mount layer is not consulted, so every
   disc reports unmounted. Buffers cleared, false returned — the same
   contract as a disk DA cannot describe; mos_query_volume surfaces null
   volume fields and the emitters suppress them as usual. */
bool mos_internal_da_volume(const char *bsd_name,
                            char *name_buf, size_t name_cap,
                            char *path_buf, size_t path_cap)
{
    (void)bsd_name;
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    return false;
}

#endif /* MOS_USE_DISKARBITRATION */

/* Public wrapper (contract in mos.h): the nub gate lives HERE, so no
   caller can consult DA for a drive the kernel says holds no media. */
mos_error mos_query_volume(mos_handle_t *h, bool *mounted,
                           char *name_buf, size_t name_cap,
                           char *path_buf, size_t path_cap)
{
    if (mounted) *mounted = false;
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    if (!h) return MOS_ERR_INVALID_ARG;

    if (h->bsd_unit < 0) return MOS_OK;     /* no IOMedia node: unmounted */

    char bsd[24];
    if (!mos_bsd_name_format(h->bsd_unit, bsd, sizeof bsd)) return MOS_OK;

    bool m = mos_internal_da_volume(bsd, name_buf, name_cap,
                                    path_buf, path_cap);
    if (mounted) *mounted = m;
    return MOS_OK;
}
