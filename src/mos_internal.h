/*
 * mos_internal.h — internal library declarations. Not part of the
 * public ABI. Consumers should include only <mos.h>.
 *
 * Pure-data prototypes (sense parser, BSD-name normalization,
 * status classifier, IOReturn mapper, watch-core state machine)
 * live in mos_pure.h so tests can include them without pulling in
 * IOKit.
 */

#ifndef MOS_INTERNAL_H
#define MOS_INTERNAL_H

#include "mos.h"
#include "mos_pure.h"

#include <stdbool.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICmds_REQUEST_SENSE_Defs.h>

/* ---- Handle layout (opaque to public callers) ----------------------- */

struct mos_handle {
    io_service_t              svc;
    uint64_t                  drive_registry_id; /* IORegistryEntryGetRegistryEntryID(svc);
                                                    0 if the call failed. The attachment
                                                    identity (same value the watch emits). */
    IOCFPlugInInterface     **plugin;
    MMCDeviceInterface      **mmc;
    SCSITaskDeviceInterface **std;   /* lazy; only allocated on first raw CDB */

    bool                      have_exclusive;

    /* Whole-disk identity (the BSD unit, kIOBSDUnitKey); -1 = no whole-disk IOMedia node (media absent). The string
       buffers below back the variable-length INQUIRY fields. */
    int64_t                   bsd_unit;
    uint64_t                  media_id;        /* whole-disk IOMedia registry
                                                  entry ID, 0 == no media;
                                                  captured at open alongside
                                                  bsd_unit (F1 swap fingerprint) */
    char                      vendor_str[9];   /* 8 chars + NUL */
    char                      product_str[17]; /* 16 chars + NUL */
    char                      revision_str[5]; /* 4 chars + NUL */

    /* Handle-owned result object returned (by borrowed pointer) from
       mos_query_state. Overwritten each query; its string fields point
       into the *_str buffers above. */
    mos_state_result          result;

    /* Handle-owned disc-information result (mos_query_disc_info).
       Overwritten each query; plain values, no borrowed pointers. */
    struct mos_disc_info      disc_info;
};

/* Device-info records returned by the enumeration callback. Allocated on
   the stack inside the enumerator and populated per iteration; caller
   must copy anything they want to keep. */
struct mos_device_info {
    int64_t  bsd_unit;    /* whole-disk BSD unit; -1 = no whole-disk IOMedia node (media absent) */
    uint64_t registry_id; /* for stable sort */
};

/* ---- DiscRecording-linked internal prototypes (mos_dr.c) ----------- *
 *
 * The directory half of the DR pivot: discovery, identity, addressing
 * (doc/research/2026-06-10-dr-pivot-implementation-plan.md). Never
 * state — that stays with the MMC seam below. */

/* One enumerated device, extracted from DR's dictionaries into plain C
   at the adapter seam (no CF types cross this line). Identity buffers
   carry the SPC-4 INQUIRY field widths — DR pre-parses the same bytes. */
typedef struct {
    uint64_t registry_id;     /* path → entry → ID; never 0 in a snapshot */
    int64_t  bsd_unit;        /* -1 = no whole-disk IOMedia node (media absent) */
    char     vendor[9];       /* 8 chars + NUL */
    char     product[17];     /* 16 chars + NUL */
    char     revision[5];     /* 4 chars + NUL */
} mos_internal_dr_snapshot;

/* Fill up to `cap` slots in DR device-array order (the same array
   drutil enumerates — the index provenance contract). Returns the
   count. Devices whose registry path cannot be resolved to an entry
   ID are skipped (an un-reopenable index entry would violate the
   enumeration/index correspondence). */
size_t mos_internal_dr_copy_snapshot(mos_internal_dr_snapshot *slots,
                                     size_t cap);

/* Resolve a canonical "diskN" name to the drive's registry entry ID
   via DRDeviceCopyDeviceForBSDName; 0 when DR knows no such device. */
uint64_t mos_internal_dr_registry_id_for_bsd_name(const char *disk_name);

/* Resolve a kDRDeviceIORegistryEntryPathKey value (CFString expected;
   anything else yields 0) to the entry's uint64 registry ID. Shared by
   the snapshot builder and the watch doorbell's per-device filter. */
uint64_t mos_internal_dr_id_for_path_value(CFTypeRef path);

/* Extract one device's snapshot (registry id, bsd unit, identity) from
   a DRDeviceRef passed as CFTypeRef (mos_internal.h stays free of
   DiscRecording types). False when the device's registry path doesn't
   resolve — the same skip gate the array snapshot applies. Used by the
   snapshot builder and the watch-all Appeared handler. */
bool mos_internal_dr_device_snapshot(CFTypeRef device_ref,
                                     mos_internal_dr_snapshot *s);

/* Device-static identity strings for an already-opened service, via
   DR's registry-path lookup. Best-effort: returns false (and empties
   the buffers) when DR cannot see the service — the same non-fatal
   contract the retired open-time INQUIRY had. */
bool mos_internal_dr_copy_identity_for_service(io_service_t svc,
                                               char *vendor, size_t vcap,
                                               char *product, size_t pcap,
                                               char *revision, size_t rcap);

/* ---- IOKit-linked internal prototypes ------------------------------ *
 *
 * MMC convenience wrappers for the query path (mos_state.c). The
 * open-time INQUIRY wrapper retired with the DR pivot — identity is
 * directory data now. */
mos_error mos_internal_mmc_get_tray_state     (mos_handle_t *h, bool *tray_open);
mos_error mos_internal_mmc_test_unit_ready    (mos_handle_t *h,
                                               uint32_t *status,
                                               uint8_t sense[18]);
mos_error mos_internal_mmc_get_current_profile(mos_handle_t *h, uint16_t *profile);

/* STUB (v0.4, hardware-gated): the IOKit half of GET CONFIGURATION's feature
 * surface. Returns MOS_ERR_UNSUPPORTED until the RT=0 issuance is written and
 * HW-validated. See mos_scsi.c for the walker seam. */
mos_error mos_internal_mmc_get_features       (mos_handle_t *h);

/* Internal-only accessor for the registry entry ID captured during
   enumeration. Used by mos_open_by_index to reopen by stable ID rather than
   racing on BSD-name re-resolution. */
uint64_t mos_internal_device_info_registry_id(const mos_device_info_t *i);

/* Open a drive by its IORegistry entry ID (captured during enumeration or
   from a validated handle). The identity-stable primitive: the kernel
   resolves IORegistryEntryIDMatching atomically, so the open either returns
   the SAME io_service_t the ID came from, or NO_DEVICE if that entry has
   been terminated. Unlike mos_open_by_bsd_name, a reassigned name (hot-unplug
   + sibling reattach recycling disk4) cannot land it on a different drive —
   so this is the watch's authority for which drive a session probes. Not in
   public mos.h: registry IDs are IOKit-specific and shouldn't enter the
   portable surface. *err_out: NO_DEVICE (entry gone) or IO (dict alloc). */
mos_handle_t *mos_internal_open_by_registry_id(uint64_t id,
                                               mos_error *err_out);

/* Exposes the validated io_service_t a handle was opened against, so a caller
   can transfer registry-level identity without a second BSD lookup (which
   would open a TOCTOU window — a hot unplug + reattach could rebind to a
   different drive). Used by mos_watch.c to register kIOGeneralInterest.

   Returns IO_OBJECT_NULL on NULL input. The caller MUST IOObjectRetain the
   result before mos_close(h) (which drops the handle's own reference); after
   that, the caller owns the extra retain and must IOObjectRelease it. */
io_service_t mos_internal_handle_get_service(mos_handle_t *h);

/* ---- Auto-cleanup helpers for IOKit / CoreFoundation refcounts ----- *
 *
 * The cleanup attribute is a gcc/clang extension that runs the named
 * callback when the variable goes out of scope. We use it to make
 * refcount discipline automatic in functions with multiple early-exit
 * paths (iterator loops, two-pass property lookups) where forgetting an
 * explicit release on one branch has bitten us before.
 *
 * Usage:
 *   io_object_t child MOS_IO_AUTO = IOIteratorNext(it);
 *   CFTypeRef prop  MOS_CF_AUTO = IORegistryEntryCreateCFProperty(...);
 *   // ... use, no explicit release ...
 *   // child and prop are released at scope exit
 *
 * Ownership transfer (when handing off to a longer-lived owner):
 *   io_service_t local MOS_IO_AUTO = IOIteratorNext(it);
 *   // ... validate ...
 *   io_service_t consumed = local;
 *   local = IO_OBJECT_NULL;   // disable cleanup; consumer now owns it
 *   return some_consumer(consumed);
 *
 * The cleanup callbacks check for the sentinel value before releasing
 * and clear the variable after, so manual release-and-clear and
 * auto-cleanup coexist safely. */

static inline void mos_internal_cleanup_cftype(CFTypeRef *p)
{
    if (*p) {
        CFRelease(*p);
        *p = NULL;
    }
}

static inline void mos_internal_cleanup_io_object(io_object_t *p)
{
    if (*p != IO_OBJECT_NULL) {
        IOObjectRelease(*p);
        *p = IO_OBJECT_NULL;
    }
}

#define MOS_CF_AUTO __attribute__((cleanup(mos_internal_cleanup_cftype)))
#define MOS_IO_AUTO __attribute__((cleanup(mos_internal_cleanup_io_object)))

#endif /* MOS_INTERNAL_H */
