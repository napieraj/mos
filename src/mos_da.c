/*
 * mos_da.c — DiskArbitration: a one-shot volume lookup and the force-unmount.
 *
 * Two modalities, both confined here:
 *   1. SYNCHRONOUS volume lookup (mos_internal_da_volume) — a DADiskCopyDescription
 *      read of what the mounted-volume layer already knows: no session
 *      scheduling, no run loop, no callbacks, no commands to the drive
 *      (AGENTS.md scope doctrine). Callers gate on the media nub (bsd_unit
 *      present); with no IOMedia node nothing is mounted and DA is never consulted.
 *   2. The ASYNC force-unmount (mos_internal_da_unmount) — the single DA ACTION
 *      mos performs, used ONLY by `tray eject --force`. DADiskUnmount delivers
 *      via a callback on a dispatch queue; we block on a semaphore until it
 *      fires (see that function). Data-loss capable, strictly opt-in.
 *
 * Trust terms: the description dictionary is system-supplied but its values
 * are volume-controlled (a hostile disc names its volume), so extraction
 * goes through the same bounded, type-checked, fail-to-empty seam as the DR
 * identity copies. The CLI's output escaping guards terminal/JSON regardless.
 */

/* strlcpy (the identity-checked buffer commit in mos_internal_da_volume) needs
   _DARWIN_C_SOURCE; set before any header. The amalgamation already sets it via
   mos_watch.c, so this keeps the standalone mos_da.c compile consistent. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

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
#include <dispatch/dispatch.h>   /* semaphore wait on the async unmount callback */

/* True when the IOMedia currently behind `disk` carries the expected whole-disk
   registry entry id. DADiskCopyIOMedia re-reads what the daemon resolves the
   ref's diskN name to RIGHT NOW (owned io_service_t, released here); registry
   entry ids are globally unique and never reused, so equality means no diskN
   reuse has repointed the ref since we built it. (Signature
   `io_service_t DADiskCopyIOMedia(DADiskRef)` is the real DA header's,
   compile-gated green on the macOS -Werror legs since PR #85.) */
static bool mos_internal_da_disk_is_media(DADiskRef disk,
                                          uint64_t expected_media_id)
{
    if (expected_media_id == 0) return false;       /* identity unknown */
    io_service_t media = DADiskCopyIOMedia(disk);
    if (media == IO_OBJECT_NULL) return false;      /* diskN backs no IOMedia */
    uint64_t got = 0;
    bool match = (IORegistryEntryGetRegistryEntryID(media, &got) == KERN_SUCCESS)
                 && got == expected_media_id;
    IOObjectRelease(media);
    return match;
}

/* Mounted-volume name and mount path for the whole-disk IOMedia identified by
   `media_id` (its globally-unique registry entry id — never reused, unlike
   "diskN"). We resolve that EXACT IOMedia by id, but the description still comes
   back through a NAME: DADiskCreateFromIOMedia reads the object's kIOBSDNameKey
   and delegates to DADiskCreateFromBSDName (first-hand in DADisk.c — the same
   fact the force-unmount path documents), so the DADiskRef is name-backed and
   DADiskCopyDescription resolves that diskN at the daemon. A diskN reuse in the
   create→describe window could therefore hand back a DIFFERENT disc's volume.
   So this is NOT identity-exact by construction: we read into LOCAL buffers,
   then re-confirm via mos_internal_da_disk_is_media that the ref still resolves
   to our exact media_id, and commit to the caller ONLY on a match — refusing any
   DETECTED reuse. This is a READ, so an observation-time identity check is the
   right tool: unlike the unmount ACTION (where the daemon re-resolves the name
   AFTER our check, so the bind cannot hold), nothing re-resolves after we read.
   A stale id whose media is gone resolves to nothing (→ unmounted); true only
   when DA has a description, the volume is mounted (VolumePath present), AND the
   identity holds. Name may still be "" if the key is absent/hostile — caller
   maps "" to null. A zero media_id fails closed. Both buffers NUL-terminated. */
bool mos_internal_da_volume(uint64_t media_id,
                            char *name_buf, size_t name_cap,
                            char *path_buf, size_t path_cap)
{
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    if (media_id == 0) return false;   /* identity unknown: fail closed */

    io_service_t media = IOServiceGetMatchingService(
        kIOMainPortDefault, IORegistryEntryIDMatching(media_id));
    if (media == IO_OBJECT_NULL) return false;   /* media no longer present */

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) { IOObjectRelease(media); return false; }

    bool      mounted          = false;
    char      local_name[256]  = {0};   /* the legit VolumeName domain (≤255) */
    char      local_path[1024] = {0};   /* a mount path the consumer may use  */
    DADiskRef disk             = DADiskCreateFromIOMedia(kCFAllocatorDefault,
                                                         session, media);
    if (disk) {
        CFDictionaryRef desc = DADiskCopyDescription(disk);
        if (desc) {
            /* VolumePath is the mount proof: DA also describes unmounted media,
               so an absent/non-URL path means "not mounted", not an error.
               CFURLGetFileSystemRepresentation returns false when the path
               exceeds the buffer, yielding not-mounted rather than a truncated
               path a consumer might chdir into. The name-only caller (e.g.
               `mos state`, path_buf == NULL) still uses VolumePath presence. */
            CFTypeRef path = CFDictionaryGetValue(
                desc, kDADiskDescriptionVolumePathKey);
            bool is_url = path && CFGetTypeID(path) == CFURLGetTypeID();
            if (is_url && path_buf && path_cap) {
                if (CFURLGetFileSystemRepresentation((CFURLRef)path, true,
                                                     (UInt8 *)local_path,
                                                     sizeof local_path)) {
                    mounted = true;
                }
            } else if (is_url) {
                mounted = true;
            }
            if (mounted)
                mos_internal_dr_copy_string(
                    CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey),
                    local_name, sizeof local_name);
            CFRelease(desc);
        }
        /* Endpoint identity guard: commit the locals to the caller ONLY if the
           ref still resolves to our exact media_id (catches a diskN reuse during
           the read); any detected reuse refuses. */
        if (mounted && mos_internal_da_disk_is_media(disk, media_id)) {
            if (name_buf && name_cap) strlcpy(name_buf, local_name, name_cap);
            if (path_buf && path_cap) strlcpy(path_buf, local_path, path_cap);
        } else {
            mounted = false;
        }
        CFRelease(disk);
    }

    CFRelease(session);
    IOObjectRelease(media);
    return mounted;
}

typedef struct { dispatch_semaphore_t sem; bool ok; } mos_da_unmount_ctx;

static void mos_internal_da_unmount_cb(DADiskRef disk,
                                       DADissenterRef dissenter, void *context)
{
    (void)disk;
    mos_da_unmount_ctx *c = (mos_da_unmount_ctx *)context;
    c->ok = (dissenter == NULL);     /* NULL dissenter ⇒ unmount accepted */
    dispatch_semaphore_signal(c->sem);
}

/* Force-unmount EVERY volume on whole-disk "diskN"
   (kDADiskUnmountOptionForce | kDADiskUnmountOptionWhole). True on success.
   ONLY the `tray eject --force` path calls this: a forced unmount kills open
   file handles (data-loss capable) — the "open no matter what" contract,
   strictly opt-in behind --force and gated by selector in cli/tray.c.

   NAME SEMANTICS, by design. mos unmounts the disc currently named `bsd_name`,
   exactly as `diskutil unmountDisk` does — there is NO identity bind, because
   the public DA API cannot provide one: DADiskUnmount transmits the NAME and
   diskarbitrationd re-resolves it by name at request time. (DADiskCreateFromIOMedia
   is no escape — first-hand in DADisk.c it reads kIOBSDNameKey and delegates to
   DADiskCreateFromBSDName; the DADiskRef stores only the name.) A `diskN`
   reassigned in the request window is unmounted as-named, the same residual
   diskutil ships; the CLI selector gate is the consent mechanism (explicit
   bsd-node / sole-drive by default, identity selectors opt-in).

   DADiskUnmount is asynchronous (returns void, delivers via callback — verified
   against DADisk.h: takes the disk, not a session; options Force=0x00080000,
   Whole=0x1; success = NULL dissenter). We make it synchronous-from-our-side:
   deliver the callback on a background queue and block on a semaphore until it
   fires. The wait is UNBOUNDED on purpose — the callback fires exactly once when
   the unmount resolves, so the context cannot outlive a late callback (no
   use-after-free), and a genuinely wedged force-unmount blocks here just as
   `diskutil` would (the I/O path itself is stuck).

   KNOWN ISSUE (unbounded wait): DASessionSetDispatchQueue is void/fallible; a
   silent failure leaves no callback port and the DISPATCH_TIME_FOREVER wait can
   hang. A bounded fix needs a heap-owned context (a stack-local ctx makes a
   naive timeout a use-after-return) — a post-tag refinement. */
bool mos_internal_da_unmount(const char *bsd_name)
{
    if (!bsd_name || !bsd_name[0]) return false;

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return false;

    bool      ok   = false;
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault,
                                             session, bsd_name);
    if (disk) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        mos_da_unmount_ctx   ctx = { sem, false };
        DASessionSetDispatchQueue(session,
            dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
        DADiskUnmount(disk,
                      kDADiskUnmountOptionForce | kDADiskUnmountOptionWhole,
                      mos_internal_da_unmount_cb, &ctx);
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        DASessionSetDispatchQueue(session, NULL);   /* detach before release */
        ok = ctx.ok;
        dispatch_release(sem);
    }
    if (disk) CFRelease(disk);

    CFRelease(session);
    return ok;
}

#else  /* !MOS_USE_DISKARBITRATION */

/* No DiskArbitration linked: the mount layer is never consulted, so every
   disc reports unmounted — same contract as a disk DA cannot describe.
   Buffers cleared, false returned. */
bool mos_internal_da_volume(uint64_t media_id,
                            char *name_buf, size_t name_cap,
                            char *path_buf, size_t path_cap)
{
    (void)media_id;
    if (name_buf && name_cap) name_buf[0] = 0;
    if (path_buf && path_cap) path_buf[0] = 0;
    return false;
}

/* No DiskArbitration linked: there is no unmount path, so a forced eject cannot
   clear a Finder/system mount. Returns false (capability absent), which leaves
   `tray eject --force` reporting the mount as MOS_ERR_BUSY rather than opening
   it — the honest degradation for the opt-out build (the consumer unmounts
   with `diskutil unmountDisk` first, exactly as without --force). */
bool mos_internal_da_unmount(const char *bsd_name)
{
    (void)bsd_name;
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

    /* Identity unknown (bridge fallback / unresolved IOMedia id): fail closed
       as unmounted rather than consulting DA without a stable identity.
       Ambiguous identity reports unmounted, never an invented error. */
    if (h->media_id == 0) return MOS_OK;

    /* Resolve by registry id, not by name — no "diskN" round-trip. */
    bool m = mos_internal_da_volume(h->media_id, name_buf, name_cap,
                                    path_buf, path_cap);
    if (mounted) *mounted = m;
    return MOS_OK;
}
