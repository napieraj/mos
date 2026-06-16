/*
 * mos_internal.h — internal library declarations; not public ABI
 * (consumers include only <mos.h>). The IOKit-free pure-data prototypes
 * live in mos_pure.h so tests can include them without IOKit.
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
    uint64_t                  drive_registry_id; /* IORegistryEntryGetRegistryEntryID(svc),
                                                    0 on failure; the attachment identity
                                                    (same value the watch emits). */
    IOCFPlugInInterface     **plugin;
    MMCDeviceInterface      **mmc;
    SCSITaskDeviceInterface **std;   /* lazy; only allocated on first raw CDB */

    bool                      have_exclusive;

    /* Whole-disk identity (the BSD unit, kIOBSDUnitKey); -1 = no whole-disk IOMedia node (media absent). The string
       buffers below back the variable-length INQUIRY fields. */
    int64_t                   bsd_unit;
    uint64_t                  media_id;        /* whole-disk IOMedia entry ID,
                                                  0 == no media; re-resolved with
                                                  bsd_unit per media-scoped query
                                                  (swap fingerprint) */
    uint64_t                  media_bytes;     /* kIOMediaSizeKey off the same
                                                  whole-disk node; 0 == absent
                                                  (query-time, like bsd_unit) */
    uint32_t                  media_block_bytes; /* kIOMediaPreferredBlockSizeKey;
                                                    0 == absent */
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

    /* Handle-owned TOC result (mos_query_toc). Same terms. */
    struct mos_toc            toc;

    /* Handle-owned drive-caps result (mos_query_drive_caps). Same terms. */
    struct mos_drive_caps     caps;

    /* Handle-owned disc-id result (mos_query_disc_id). Same terms. */
    struct mos_disc_id        disc_id;

    /* Handle-owned CD-TEXT result (mos_query_cdtext). Same terms;
       plain values into fixed buffers, no borrowed pointers. */
    struct mos_cdtext         cdtext;

    /* Handle-owned physical structure result (mos_query_physical_structure).
       Same terms; plain values, no borrowed pointers. */
    struct mos_physical_structure physical_structure;

    /* Handle-owned track-info result (mos_query_track_info). Same terms. */
    struct mos_track_info     track_info;

    /* Handle-owned capacity result (mos_query_capacity). Assembled from
       the open-time IOMedia size above + a fresh track_info read. */
    struct mos_capacity       capacity;

    /* Handle-owned drive-performance result (mos_query_drive_perf). */
    struct mos_drive_perf     drive_perf;

    /* Handle-owned MODE SENSE results (mos_query_mode_caps /
       mos_query_error_recovery). Same terms. */
    struct mos_mode_caps      mode_caps;
    struct mos_error_recovery error_recovery;

    /* Handle-owned INQUIRY VPD-0x80 serial (mos_query_serial). Filled by the
       raw-INQUIRY shell, returned by borrowed pointer; 64 holds any real
       drive serial (SPC max 255 truncates, never overflows). */
    char                      serial_str[64];

    /* Handle-owned standard-INQUIRY result (mos_query_drive_standards). */
    struct mos_drive_standards drive_standards;
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
 * The directory half: discovery, identity, addressing. Never state —
 * that stays with the MMC seam below. */

/* One enumerated device, extracted from DR's dictionaries into plain C at
   the adapter seam (no CF types cross this line). Identity buffers carry
   the SPC-4 INQUIRY field widths. */
typedef struct {
    uint64_t registry_id;     /* path → entry → ID; never 0 in a snapshot */
    int64_t  bsd_unit;        /* -1 = no whole-disk IOMedia node (media absent) */
    char     vendor[9];       /* 8 chars + NUL */
    char     product[17];     /* 16 chars + NUL */
    char     revision[5];     /* 4 chars + NUL */
} mos_internal_dr_snapshot;

/* Fill up to `cap` slots in DR device-array order (the array drutil
   enumerates — the index-provenance contract); returns the count. Devices
   whose registry path won't resolve to an entry ID are skipped (an
   un-reopenable index entry would break enumeration/index correspondence). */
size_t mos_internal_dr_copy_snapshot(mos_internal_dr_snapshot *slots,
                                     size_t cap);

/* Resolve a canonical "diskN" name to the drive's registry entry ID
   via DRDeviceCopyDeviceForBSDName; 0 when DR knows no such device. */
uint64_t mos_internal_dr_registry_id_for_bsd_name(const char *disk_name);

/* Resolve a kDRDeviceIORegistryEntryPathKey value (CFString expected;
   anything else yields 0) to the entry's uint64 registry ID. Shared by
   the snapshot builder and the watch doorbell's per-device filter. */
uint64_t mos_internal_dr_id_for_path_value(CFTypeRef path);

/* Bounded CFTypeRef-string -> C-buffer copy (mos_dr.c); "" on
   non-string, oversize, or conversion failure. Always terminates. */
void mos_internal_dr_copy_string(CFTypeRef value, char *dst, size_t cap);

/* One-shot DiskArbitration mounted-volume lookup (mos_da.c). True only
   when mounted; gate calls on bsd_unit present. No callbacks, no run
   loop — see the re-admission terms at the top of mos_da.c. */
bool mos_internal_da_volume(const char *bsd_name,
                            char *name_buf, size_t name_cap,
                            char *path_buf, size_t path_cap);

/* Extract one device's snapshot (registry id, bsd unit, identity) from a
   DRDeviceRef passed as CFTypeRef (this header stays free of DiscRecording
   types). False when the registry path doesn't resolve — the same skip gate
   the array snapshot uses. Shared with the watch-all Appeared handler. */
bool mos_internal_dr_device_snapshot(CFTypeRef device_ref,
                                     mos_internal_dr_snapshot *s);

/* Device-static identity strings for an already-opened service, via
   DR's registry-path lookup. Best-effort: returns false (and empties
   the buffers) when DR cannot see the service (non-fatal). */
bool mos_internal_dr_copy_identity_for_service(io_service_t svc,
                                               char *vendor, size_t vcap,
                                               char *product, size_t pcap,
                                               char *revision, size_t rcap);

/* ---- IOKit-linked internal prototypes ------------------------------ *
 *
 * MMC convenience wrappers for the query path (mos_state.c). */
mos_error mos_internal_mmc_get_tray_state     (mos_handle_t *h, bool *tray_open);
mos_error mos_internal_mmc_test_unit_ready    (mos_handle_t *h,
                                               uint32_t *status,
                                               uint8_t sense[18]);
mos_error mos_internal_mmc_get_current_profile(mos_handle_t *h, uint16_t *profile);

/* Thin shim over the pure IOReturn→mos_error map (mos_scsi.c), so the typed
   query surface (mos_query.c) maps transport failures identically to the
   convenience wrappers and mos_raw_cdb. CHECK CONDITION rides task
   status/sense, not IOReturn, so this maps only transport failures. */
mos_error mos_internal_ioreturn_to_mos_error(IOReturn rc);

/* Re-resolve the handle's media-scoped identity (whole-disk bsd_unit,
   media_id swap fingerprint, kernel-cached size/block bytes) from its
   stable drive service — the freshness the media-scoped queries (state,
   capacity, volume) call first so a handle held across an insert/eject
   reports current media. Local IORegistry walk off h->svc; no SCSI
   command, no exclusive access (mos_scsi.c). */
void mos_internal_refresh_media_identity(mos_handle_t *h);

/* Issue one 6-byte tray CDB (START STOP UNIT 0x1B / PREVENT ALLOW MEDIUM
   REMOVAL 0x1E) via mos_raw_cdb and classify the result. Negative mos_error
   on transport/lock failure (BUSY, NO_DEVICE, IO); on an ANSWERED command,
   MOS_OK with *outcome (DONE / REFUSED_LOCKED / REFUSED_OTHER). sense_out,
   when non-NULL, gets {sk, asc, ascq} (zeroed on MOS_OK with no sense). Adds
   no ObtainExclusiveAccess — that stays mos_raw_cdb (§3). Shared by the four
   mos_tray_* verbs. */
mos_error mos_internal_tray_cmd(mos_handle_t *h, const uint8_t cdb[6],
                                mos_tray_outcome *outcome, uint8_t sense_out[3]);



/* Open a drive by its IORegistry entry ID — the identity-stable primitive:
   IORegistryEntryIDMatching resolves atomically, returning the SAME entry
   the ID came from or NO_DEVICE if it terminated; a recycled BSD name can't
   rebind it elsewhere. The watch's authority for which drive a session
   probes. Not public (registry IDs are IOKit-specific). *err_out: NO_DEVICE
   or IO. */
mos_handle_t *mos_internal_open_by_registry_id(uint64_t id,
                                               mos_error *err_out);

/* The validated io_service_t a handle was opened against — identity
   transfer with no second BSD lookup (which would be a TOCTOU window).
   IO_OBJECT_NULL on NULL input. The caller MUST IOObjectRetain before
   mos_close(h) drops the handle's reference, then owns and releases it. */
io_service_t mos_internal_handle_get_service(mos_handle_t *h);

/* ---- Auto-cleanup helpers for IOKit / CoreFoundation refcounts ----- *
 *
 * The gcc/clang cleanup attribute runs the callback at scope exit, so
 * refcount discipline holds across early exits. To hand a reference to a
 * longer-lived owner, null the variable to disable cleanup:
 *   io_service_t consumed = local; local = IO_OBJECT_NULL;
 * The callbacks check the sentinel before releasing and clear after, so
 * manual release-and-clear and auto-cleanup coexist safely. */

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
