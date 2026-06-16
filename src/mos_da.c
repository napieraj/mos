/*
 * mos_da.c — one-shot DiskArbitration volume lookup.
 *
 * One modality only: a synchronous DADiskCopyDescription read of what the
 * mounted-volume layer already knows — no session scheduling, no run loop,
 * no callbacks, no commands to the drive (AGENTS.md scope doctrine). Callers
 * gate on the media nub (bsd_unit present); with no IOMedia node nothing is
 * mounted and DA is never consulted.
 *
 * Trust terms: the description dictionary is system-supplied but its values
 * are volume-controlled (a hostile disc names its volume), so extraction
 * goes through the same bounded, type-checked, fail-to-empty seam as the DR
 * identity copies. The CLI's output escaping guards terminal/JSON regardless.
 */

#include "mos_internal.h"

/* DiskArbitration is an OPTIONAL link dependency. Build with
   -DMOS_USE_DISKARBITRATION=0 (and drop -framework DiskArbitration) and
   mos_query_volume always reports unmounted (volume name/path null) — which
   the CLI and JSON schemas already permit, so no shape changes. Default on. */
#ifndef MOS_USE_DISKARBITRATION
#define MOS_USE_DISKARBITRATION 1
#endif

#if MOS_USE_DISKARBITRATION
#include <DiskArbitration/DiskArbitration.h>

/* Mounted-volume name and mount path for a whole-disk "diskN". True only
   when DA has a description AND the volume is mounted (VolumePath present);
   name may still be "" if the key is absent or hostile — the caller maps ""
   to null. Both buffers are always NUL-terminated. */
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
        /* VolumePath is the mount proof: DA also describes unmounted media,
           so an absent/non-URL path means "not mounted", not an error.
           CFURLGetFileSystemRepresentation returns false when the path
           exceeds the buffer, yielding not-mounted rather than a truncated
           path a consumer might chdir into. */
        CFTypeRef path = CFDictionaryGetValue(
            desc, kDADiskDescriptionVolumePathKey);
        bool is_url = path && CFGetTypeID(path) == CFURLGetTypeID();
        if (is_url && path_buf && path_cap) {
            if (CFURLGetFileSystemRepresentation((CFURLRef)path, true,
                                                 (UInt8 *)path_buf,
                                                 (CFIndex)path_cap)) {
                mounted = true;
            } else {
                path_buf[0] = 0;
            }
        } else if (is_url) {
            /* No path buffer: VolumePath presence alone is the mount proof,
               so a name-only caller (e.g. `mos status`) still sees mounted. */
            mounted = true;
        }
        if (mounted)
            mos_internal_dr_copy_string(
                CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey),
                name_buf, name_cap);
        CFRelease(desc);
    }

    if (disk)    CFRelease(disk);
    CFRelease(session);
    return mounted;
}

#else  /* !MOS_USE_DISKARBITRATION */

/* No DiskArbitration linked: the mount layer is never consulted, so every
   disc reports unmounted — same contract as a disk DA cannot describe.
   Buffers cleared, false returned. */
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

/* Public wrapper (contract in mos.h): the nub gate lives here, so no caller
   consults DA for a drive the kernel says holds no media. */
mos_error mos_query_volume(mos_handle_t *h, bool *mounted,
                           char *name_buf, size_t name_cap,
                           char *path_buf, size_t path_cap)
{
    if (mounted) *mounted = false;
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    if (!h) return MOS_ERR_INVALID_ARG;

    /* Held-handle freshness: re-resolve so a handle held across an insert
       sees the current disc's whole-disk node
       (mos_internal_refresh_media_identity). */
    mos_internal_refresh_media_identity(h);

    if (h->bsd_unit < 0) return MOS_OK;     /* no IOMedia node: unmounted */

    char bsd[24];
    if (!mos_bsd_name_format(h->bsd_unit, bsd, sizeof bsd)) return MOS_OK;

    bool m = mos_internal_da_volume(bsd, name_buf, name_cap,
                                    path_buf, path_cap);
    if (mounted) *mounted = m;
    return MOS_OK;
}
