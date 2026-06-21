/*
 * mos_fake_apple.c — link-seam fake of the Apple framework layer.
 *
 * Supplies the IOKit + DiscRecording C symbols the one-shot adapter imports
 * (mos_scsi.c / mos_state.c / mos_dr.c), so the real adapter TUs run headless
 * on a macOS build host with the real SDK headers but WITHOUT linking IOKit /
 * DiscRecording. Real CoreFoundation stays linked — CF objects here are
 * genuine. Mechanism: doc/research/2026-06-11-headless-adapter-emulation.md.
 *
 * Phase 1 (open / query / enumerate); the watch lifecycle's notification and
 * time symbols are phase 2 in mos_fake_watch.c (linked only into
 * mos_adapter_watch_tests).
 *
 * Model: ONE optical drive. IOKit handles are small integers (io_object_t is
 * a mach_port_t) resolved through a fixed table; the DR "device" is an
 * immortal CFSTR sentinel (a real CF object, so the adapter's CFRelease is
 * safe). MMC replies are scripted from committed fixtures via the control
 * surface in mos_fake_apple.h.
 */

#include "mos_fake_apple.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOBSD.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICmds_REQUEST_SENSE_Defs.h>
#include <IOKit/storage/IOMedia.h>   /* kIOMediaSizeKey / kIOMediaPreferredBlockSizeKey */
#include <DiscRecording/DRCoreDevice.h>
#include <DiskArbitration/DiskArbitration.h>

#include <string.h>
#include <stdio.h>

/* kIOMainPortDefault is an extern const on macOS 12+ (older SDKs spell it
   kIOMasterPortDefault). IOKit isn't linked, so we define the symbol the
   adapter references; guard against the macro form. */
#ifndef kIOMainPortDefault
const mach_port_t kIOMainPortDefault = 0;
#endif

/* ---- DiscRecording key constants. The adapter reads these symbols; we
   define them since DiscRecording is not linked. Values are arbitrary but
   must be the SAME objects the dict-builders below use — they are, since
   both sides reference these symbols. ---- */
const CFStringRef kDRDeviceVendorNameKey        = CFSTR("mos.fake.VendorName");
const CFStringRef kDRDeviceProductNameKey       = CFSTR("mos.fake.ProductName");
const CFStringRef kDRDeviceFirmwareRevisionKey  = CFSTR("mos.fake.FirmwareRevision");
const CFStringRef kDRDeviceIORegistryEntryPathKey = CFSTR("mos.fake.IORegistryEntryPath");
const CFStringRef kDRDeviceMediaInfoKey         = CFSTR("mos.fake.MediaInfo");
const CFStringRef kDRDeviceMediaBSDNameKey      = CFSTR("mos.fake.MediaBSDName");
const CFStringRef kDRDeviceStatusChangedNotification = CFSTR("mos.fake.StatusChanged");
const CFStringRef kDRDeviceAppearedNotification = CFSTR("mos.fake.Appeared");
const CFStringRef kDRDeviceDisappearedNotification = CFSTR("mos.fake.Disappeared");
const CFStringRef kDRDeviceIsTrayOpenKey        = CFSTR("mos.fake.IsTrayOpen");
const CFStringRef kDRDeviceMediaStateKey        = CFSTR("mos.fake.MediaState");

/* ---- Object handles (io_object_t == mach_port_t == unsigned int) --- */
#define FAKE_SVC    ((io_service_t)1)        /* the drive service          */
#define FAKE_MEDIA  ((io_object_t)2)         /* whole-disk IOMedia child   */
#define FAKE_DA_MEDIA ((io_object_t)3)       /* IOMedia behind a DADiskRef  */
#define FAKE_ITER   ((io_iterator_t)10)      /* the (single) child iterator */
#define FAKE_DEV    ((DRDeviceRef)CFSTR("mos.fake.device"))
#define FAKE_ID_KEY  CFSTR("mos.fake.matchID")

/* ---- Scenario state ----------------------------------------------- */
static struct {
    bool     present;
    uint64_t drive_id;
    uint64_t media_id;
    uint64_t media_bytes;       /* kIOMediaSizeKey; 0 == property absent */
    uint32_t media_block_bytes; /* kIOMediaPreferredBlockSizeKey; 0 == absent */
    int64_t  bsd_unit;          /* -1 == no whole-disk IOMedia child    */
    char     vendor[16];
    char     product[24];
    char     revision[8];
    char     path[128];
    uint32_t tur_status;        uint8_t tur_sense[18];
    uint32_t cfg_status;        uint8_t cfg[64];  size_t cfg_len;
    uint32_t rdi_status;        uint8_t rdi[64];  size_t rdi_len;
    uint32_t toc_status;        uint8_t toc[804]; size_t toc_len;
    uint32_t ds_status;         uint8_t ds[4096]; size_t ds_len;
    uint32_t rti_status;        uint8_t rti[64];  size_t rti_len;
    uint32_t perf_status;       uint8_t perf[64]; size_t perf_len;
    bool     da_present;        /* DADiskCopyDescription returns a dict   */
    char     da_name[256];      /* VolumeName; "" = key absent            */
    char     da_path[1024];     /* VolumePath; "" = key absent (unmounted)*/
    uint64_t da_media_id;       /* registry id of the IOMedia behind the  */
    bool     da_media_id_set;   /* DADiskRef; unset == tracks media_id (no */
                                /* reuse). Set forces the endpoint-guard   */
                                /* mismatch (a diskN reuse mid-lookup).    */

    /* Raw-CDB script (the GESN tray probe path). */
    uint32_t method_rc[6];      /* per-method IOReturn injection;
                                   indexed by mos_fake_method, 0 = success */
    bool     plugin_fail;
    bool     exclusive_denied;
    bool     mounted_busy;      /* ObtainExclusiveAccess returns BUSY (a mount)
                                   until a successful DADiskUnmount clears it */
    bool     unmount_refused;   /* a GRACEFUL DADiskUnmount dissents (busy FS,
                                   open handles): mount stays, mos surfaces BUSY */
    bool     unmount_never_completes; /* DADiskUnmount delivers NO callback —
                                   models the F1 silent-failure / wedged-daemon
                                   case; mos_internal_da_unmount must return
                                   bounded-false, not hang */
    bool     release_fail;      /* ReleaseExclusiveAccess returns non-success
                                   AND leaves the lock held (no decrement) */
    uint32_t raw_status;        uint8_t raw[64];  size_t raw_len;
    uint64_t raw_realized;      uint8_t raw_sense[18];

    /* Media-coherence knobs (mos_query_state S1/S2 retry). The capture walk's
       effective identity (eff_unit/eff_id) tracks (bsd_unit, media_id) by
       default; a swap arms a different identity from the 2nd capture on (one
       media change, then stable → the retry succeeds); churn mints a fresh id
       every walk (continuous change → the retry gives up → MOS_ERR_BUSY). */
    bool     swap_armed;
    bool     swap_first_done;
    int64_t  swap_unit;
    uint64_t swap_id;
    bool     churn;
    unsigned capture_walks;     /* total IORegistry capture walks since reset */
    int64_t  eff_unit;          /* this walk's effective bsd_unit / media_id  */
    uint64_t eff_id;
} g;

static int      g_lock_balance;
static int      g_lock_acquires;
static int      g_release_calls;   /* ReleaseExclusiveAccess invocations    */
static int      g_task_creates;    /* CreateSCSITask invocations            */
static int      g_execute_calls;   /* ExecuteTaskSync invocations           */
static unsigned g_iter_remaining;

/* Per-task capture: the CDB and scatter-gather the adapter set, so
   ExecuteTaskSync can deliver into the caller's buffer and a test can
   pin the authored CDB bytes. One task at a time (phase-1 adapter
   creates, executes, releases — never concurrent). */
static struct {
    uint8_t  cdb[16];  size_t cdb_len;
    void    *buf;      size_t buf_len;
    uint32_t timeout_ms;
} g_task;

/* ---- Control surface ---------------------------------------------- */

void mos_fake_reset(void)
{
    memset(&g, 0, sizeof g);
    g.present  = true;
    g.drive_id = 0x100000123ull;
    g.media_id = 0x100000456ull;
    g.bsd_unit = 4;
    g.eff_unit = g.bsd_unit;
    g.eff_id   = g.media_id;
    strcpy(g.vendor,   "HL-DT-ST");
    strcpy(g.product,  "DVDROM");
    strcpy(g.revision, "A100");
    strcpy(g.path,     "IOService:/fake/MMCDevice");
    g.tur_status = 0; /* kSCSITaskStatus_GOOD */
    g_lock_balance   = 0;
    g_lock_acquires  = 0;
    g_release_calls  = 0;
    g_task_creates   = 0;
    g_execute_calls  = 0;
    g_iter_remaining = 0;
    memset(&g_task, 0, sizeof g_task);
}

void mos_fake_set_no_drive(void) { g.present = false; }

void mos_fake_set_drive_present(bool present) { g.present = present; }

void mos_fake_set_bsd_unit(int64_t unit) { g.bsd_unit = unit; }

void mos_fake_set_drive_id(uint64_t id) { g.drive_id = id; }
void mos_fake_set_media_id(uint64_t id) { g.media_id = id; }

void mos_fake_set_media_size(uint64_t bytes, uint32_t block_bytes)
{
    g.media_bytes = bytes;
    g.media_block_bytes = block_bytes;
}

/* Arm a one-time media swap: the FIRST capture walk after this call still sees
   the current (bsd_unit, media_id); every later walk sees (unit, id). Models a
   media change between a state query's S1 capture and its S2 confirmation —
   the query's coherence retry should re-observe and publish the post-swap
   generation. Resets the walk counter so a test can assert the retry count. */
void mos_fake_set_media_swap_after_first_capture(int64_t unit, uint64_t id)
{
    g.swap_armed = true;
    g.swap_first_done = false;
    g.swap_unit = unit;
    g.swap_id = id;
    g.capture_walks = 0;
}

/* Continuous churn: every capture walk mints a fresh media_id, so no two
   captures agree — the coherence retry exhausts and the query returns BUSY
   rather than publish a mixed observation. Resets the walk counter. */
void mos_fake_set_media_churn(bool on)
{
    g.churn = on;
    g.capture_walks = 0;
}

/* Total IORegistry capture walks since the last reset/knob-set — lets a test
   confirm a query retried (4 walks: S1,S2,S1',S2') vs ran clean (2). */
unsigned mos_fake_capture_walks(void) { return g.capture_walks; }

void mos_fake_set_identity(const char *vendor, const char *product,
                           const char *revision)
{
    if (vendor)   { strncpy(g.vendor,   vendor,   sizeof g.vendor   - 1);   g.vendor[sizeof g.vendor - 1] = 0; }
    if (product)  { strncpy(g.product,  product,  sizeof g.product  - 1);   g.product[sizeof g.product - 1] = 0; }
    if (revision) { strncpy(g.revision, revision, sizeof g.revision - 1);   g.revision[sizeof g.revision - 1] = 0; }
}

void mos_fake_set_tur(uint32_t task_status, const uint8_t sense[18])
{
    g.tur_status = task_status;
    if (sense) memcpy(g.tur_sense, sense, 18);
    else       memset(g.tur_sense, 0, 18);
}

void mos_fake_set_getconfig_reply(uint32_t task_status,
                                  const uint8_t *bytes, size_t len)
{
    g.cfg_status = task_status;
    g.cfg_len = (len > sizeof g.cfg) ? sizeof g.cfg : len;
    if (bytes && g.cfg_len) memcpy(g.cfg, bytes, g.cfg_len);
}

void mos_fake_set_readdiscinfo_reply(uint32_t task_status,
                                     const uint8_t *bytes, size_t len)
{
    g.rdi_status = task_status;
    g.rdi_len = (len > sizeof g.rdi) ? sizeof g.rdi : len;
    if (bytes && g.rdi_len) memcpy(g.rdi, bytes, g.rdi_len);
}

void mos_fake_set_toc_reply(uint32_t task_status,
                            const uint8_t *bytes, size_t len)
{
    g.toc_status = task_status;
    g.toc_len = (len > sizeof g.toc) ? sizeof g.toc : len;
    if (bytes && g.toc_len) memcpy(g.toc, bytes, g.toc_len);
}

void mos_fake_set_disc_structure_reply(uint32_t task_status,
                                       const uint8_t *bytes, size_t len)
{
    g.ds_status = task_status;
    g.ds_len = (len > sizeof g.ds) ? sizeof g.ds : len;
    if (bytes && g.ds_len) memcpy(g.ds, bytes, g.ds_len);
}

void mos_fake_set_perf_reply(uint32_t task_status,
                             const uint8_t *bytes, size_t len)
{
    g.perf_status = task_status;
    g.perf_len = (len > sizeof g.perf) ? sizeof g.perf : len;
    if (bytes && g.perf_len) memcpy(g.perf, bytes, g.perf_len);
}

void mos_fake_set_readtrackinfo_reply(uint32_t task_status,
                                      const uint8_t *bytes, size_t len)
{
    g.rti_status = task_status;
    g.rti_len = (len > sizeof g.rti) ? sizeof g.rti : len;
    if (bytes && g.rti_len) memcpy(g.rti, bytes, g.rti_len);
}

void mos_fake_set_da_volume(const char *name, const char *path)
{
    g.da_present = true;
    g.da_name[0] = 0;
    g.da_path[0] = 0;
    if (name) strlcpy(g.da_name, name, sizeof g.da_name);
    if (path) strlcpy(g.da_path, path, sizeof g.da_path);
}

/* Force the endpoint identity guard's mismatch: make DADiskCopyIOMedia's IOMedia
   report registry id `id` instead of the current media_id, modelling a diskN
   reused by another disc between create and the post-read identity re-check. */
void mos_fake_set_da_media_id(uint64_t id)
{
    g.da_media_id = id;
    g.da_media_id_set = true;
}

void mos_fake_set_raw_reply(uint32_t task_status,
                            const uint8_t *bytes, size_t len,
                            uint64_t realized,
                            const uint8_t sense[18])
{
    g.raw_status = task_status;
    g.raw_len = (len > sizeof g.raw) ? sizeof g.raw : len;
    if (bytes && g.raw_len) memcpy(g.raw, bytes, g.raw_len);
    g.raw_realized = realized;
    if (sense) memcpy(g.raw_sense, sense, 18);
    else       memset(g.raw_sense, 0, 18);
}

void mos_fake_set_exclusive_denied(bool denied) { g.exclusive_denied = denied; }

/* Model a mounted volume: ObtainExclusiveAccess returns BUSY until a successful
   GRACEFUL DADiskUnmount clears it (so `tray eject` unmounts then re-ejects). */
void mos_fake_set_mounted_busy(bool busy) { g.mounted_busy = busy; }
/* Model a BUSY filesystem (open handles): the graceful DADiskUnmount DISSENTS,
   the mount stays, mos surfaces MOS_ERR_BUSY (never forces). */
void mos_fake_set_unmount_refused(bool refused) { g.unmount_refused = refused; }
/* Model the F1 case: the unmount callback never arrives (silent
   DASessionScheduleWithRunLoop failure / wedged daemon). mos_internal_da_unmount
   must fall through its bounded run-loop wait and return false, never hang. */
void mos_fake_set_unmount_never_completes(bool never)
{ g.unmount_never_completes = never; }
void mos_fake_set_release_fail(bool fail) { g.release_fail = fail; }

void mos_fake_set_plugin_fail(bool fail) { g.plugin_fail = fail; }

void mos_fake_set_method_ioreturn(mos_fake_method m, uint32_t io_return)
{
    if ((unsigned)m < 6u) g.method_rc[m] = io_return;
}

size_t mos_fake_last_cdb(uint8_t out[16])
{
    if (out) memcpy(out, g_task.cdb, 16);
    return g_task.cdb_len;
}

int mos_fake_lock_balance(void)  { return g_lock_balance;  }
int mos_fake_lock_acquires(void) { return g_lock_acquires; }
int mos_fake_release_calls(void) { return g_release_calls; }
int mos_fake_task_creates(void)  { return g_task_creates;  }
int mos_fake_execute_calls(void) { return g_execute_calls; }

/* ---- IOKit registry ------------------------------------------------ */

/* Compute this capture walk's effective media identity (see the swap/churn
   knobs). Called once per registry walk so the BSD-name read, the Whole/size
   reads, and the entry-ID read within the walk all see one consistent
   generation. */
static void fake_begin_capture_walk(void)
{
    g.capture_walks++;
    g.eff_unit = g.bsd_unit;
    g.eff_id   = g.media_id;
    if (g.churn) {
        g.eff_id = g.media_id + (uint64_t)g.capture_walks;   /* fresh every walk */
    } else if (g.swap_armed) {
        if (g.swap_first_done) {
            g.eff_unit = g.swap_unit;
            g.eff_id   = g.swap_id;
        } else {
            g.swap_first_done = true;   /* first capture sees the pre-swap state */
        }
    }
}

kern_return_t IORegistryEntryCreateIterator(io_registry_entry_t entry,
                                            const io_name_t plane,
                                            IOOptionBits options,
                                            io_iterator_t *iterator)
{
    (void)plane; (void)options;
    if (entry != FAKE_SVC || !iterator) return KERN_FAILURE;
    fake_begin_capture_walk();
    /* One whole-disk IOMedia child iff media present this walk. */
    g_iter_remaining = (g.eff_unit >= 0) ? 1u : 0u;
    *iterator = FAKE_ITER;
    return KERN_SUCCESS;
}

io_object_t IOIteratorNext(io_iterator_t iterator)
{
    if (iterator != FAKE_ITER || g_iter_remaining == 0) return IO_OBJECT_NULL;
    g_iter_remaining--;
    return FAKE_MEDIA;
}

/* The fake iterator never invalidates (no topology churn modelled here), so a
   NULL from IOIteratorNext is always genuine exhaustion. Returning true keeps
   the capture/cdtoc retry path on its non-invalidation branch. */
boolean_t IOIteratorIsValid(io_iterator_t iterator)
{
    return iterator == FAKE_ITER;
}

CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry,
                                          CFStringRef key,
                                          CFAllocatorRef allocator,
                                          IOOptionBits options)
{
    (void)allocator; (void)options;
    if (entry != FAKE_MEDIA || !key) return NULL;
    if (CFEqual(key, CFSTR(kIOBSDNameKey))) {
        char name[32];
        snprintf(name, sizeof name, "disk%lld", (long long)g.eff_unit);
        return CFStringCreateWithCString(kCFAllocatorDefault, name,
                                         kCFStringEncodingUTF8);
    }
    if (CFEqual(key, CFSTR("Whole"))) {
        return CFRetain(kCFBooleanTrue);
    }
    /* Kernel-cached capacity off the whole-disk node (mos_query_capacity);
       0 models the property absent (blank/unrecorded media). */
    if (CFEqual(key, CFSTR(kIOMediaSizeKey)) && g.media_bytes) {
        long long v = (long long)g.media_bytes;
        return CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &v);
    }
    if (CFEqual(key, CFSTR(kIOMediaPreferredBlockSizeKey)) && g.media_block_bytes) {
        long long v = (long long)g.media_block_bytes;
        return CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &v);
    }
    return NULL;
}

boolean_t IOObjectConformsTo(io_object_t object, const io_name_t className)
{
    return (object == FAKE_MEDIA && strcmp(className, "IOMedia") == 0);
}

kern_return_t IORegistryEntryGetRegistryEntryID(io_registry_entry_t entry,
                                               uint64_t *entryID)
{
    if (!entryID) return KERN_FAILURE;
    if (entry == FAKE_SVC)   { *entryID = g.drive_id; return KERN_SUCCESS; }
    if (entry == FAKE_MEDIA) { *entryID = g.eff_id; return KERN_SUCCESS; }
    if (entry == FAKE_DA_MEDIA) {
        *entryID = g.da_media_id_set ? g.da_media_id : g.media_id;
        return KERN_SUCCESS;
    }
    return KERN_FAILURE;
}

io_registry_entry_t IORegistryEntryFromPath(mach_port_t masterPort,
                                            const io_string_t path)
{
    (void)masterPort;
    if (g.present && path && strcmp(path, g.path) == 0) return FAKE_SVC;
    return IO_OBJECT_NULL;
}

kern_return_t IORegistryEntryGetPath(io_registry_entry_t entry,
                                     const io_name_t plane, io_string_t path)
{
    (void)plane;
    if (entry != FAKE_SVC || !path) return KERN_FAILURE;
    strlcpy(path, g.path, sizeof(io_string_t));
    return KERN_SUCCESS;
}

CFMutableDictionaryRef IORegistryEntryIDMatching(uint64_t entryID)
{
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!m) return NULL;
    int64_t v = (int64_t)entryID;
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &v);
    CFDictionarySetValue(m, FAKE_ID_KEY, n);
    CFRelease(n);
    return m;
}

io_service_t IOServiceGetMatchingService(mach_port_t masterPort,
                                         CFDictionaryRef matching)
{
    (void)masterPort;
    io_service_t result = IO_OBJECT_NULL;
    if (matching) {
        CFNumberRef n = (CFNumberRef)CFDictionaryGetValue(matching, FAKE_ID_KEY);
        int64_t id = 0;
        if (n) CFNumberGetValue(n, kCFNumberSInt64Type, &id);
        if (g.present && (uint64_t)id == g.drive_id) result = FAKE_SVC;
        /* The whole-disk IOMedia resolves by its own registry id (the volume
           lookup's identity-exact path). Present only when media is loaded. */
        else if (g.present && g.bsd_unit >= 0 && (uint64_t)id == g.media_id)
            result = FAKE_MEDIA;
        /* IOServiceGetMatchingService consumes the matching dict ref. */
        CFRelease(matching);
    }
    return result;
}

kern_return_t IOObjectRelease(io_object_t object) { (void)object; return KERN_SUCCESS; }
kern_return_t IOObjectRetain (io_object_t object) { (void)object; return KERN_SUCCESS; }

/* ---- COM plug-in: factory + vtables -------------------------------- */

static HRESULT plugin_QueryInterface(void *self, REFIID iid, LPVOID *ppv);
static ULONG   com_AddRef (void *self) { (void)self; return 1; }
static ULONG   com_Release(void *self) { (void)self; return 0; }

static IOReturn mmc_TestUnitReady(void *self, SCSITaskStatus *taskStatus,
                                  SCSI_Sense_Data *senseDataBuffer);
static IOReturn mmc_GetConfiguration(void *self, SCSICmdField1Byte RT,
                                     SCSICmdField2Byte feature, void *buffer,
                                     SCSICmdField2Byte bufferSize,
                                     SCSITaskStatus *taskStatus,
                                     SCSI_Sense_Data *senseDataBuffer);
static IOReturn mmc_ReadDiscInformation(void *self, void *buffer,
                                        SCSICmdField2Byte bufferSize,
                                        SCSITaskStatus *taskStatus,
                                        SCSI_Sense_Data *senseDataBuffer);
static IOReturn mmc_ReadTableOfContents(void *self, SCSICmdField1Bit MSF,
                                        SCSICmdField4Bit FORMAT,
                                        SCSICmdField1Byte TRACK_SESSION_NUMBER,
                                        void *buffer,
                                        SCSICmdField2Byte bufferSize,
                                        SCSITaskStatus *taskStatus,
                                        SCSI_Sense_Data *senseDataBuffer);
static IOReturn mmc_ReadDiscStructure(void *self, SCSICmdField4Bit MEDIA_TYPE,
                                      SCSICmdField4Byte ADDRESS,
                                      SCSICmdField1Byte LAYER_NUMBER,
                                      SCSICmdField1Byte FORMAT,
                                      void *buffer,
                                      SCSICmdField2Byte bufferSize,
                                      SCSITaskStatus *taskStatus,
                                      SCSI_Sense_Data *senseDataBuffer);
static SCSITaskDeviceInterface **mmc_GetSCSITaskDeviceInterface(void *self);
static IOReturn mmc_GetPerformance(void *self, SCSICmdField2Bit TOLERANCE,
                                   SCSICmdField1Bit WRITE, SCSICmdField2Bit EXCEPT,
                                   SCSICmdField4Byte STARTING_LBA,
                                   SCSICmdField2Byte MAXIMUM_NUMBER_OF_DESCRIPTORS,
                                   void *buffer, SCSICmdField2Byte bufferSize,
                                   SCSITaskStatus *taskStatus,
                                   SCSI_Sense_Data *senseDataBuffer);
static IOReturn mmc_ReadTrackInformation(void *self,
                                         SCSICmdField2Bit ADDRESS_NUMBER_TYPE,
                                         SCSICmdField4Byte LBA_TRACK_SESSION,
                                         void *buffer, SCSICmdField2Byte bufferSize,
                                         SCSITaskStatus *taskStatus,
                                         SCSI_Sense_Data *senseDataBuffer);
static IOReturn mmc_ModeSense10(void *self, SCSICmdField1Bit LLBAA,
                                SCSICmdField1Bit DBD, SCSICmdField2Bit PC,
                                SCSICmdField6Bit PAGE_CODE,
                                void *buffer, SCSICmdField2Byte bufferSize,
                                SCSITaskStatus *taskStatus,
                                SCSI_Sense_Data *senseDataBuffer);

static IOReturn std_ObtainExclusiveAccess(void *self);
static IOReturn std_ReleaseExclusiveAccess(void *self);
static SCSITaskInterface **std_CreateSCSITask(void *self);

static IOReturn task_SetCommandDescriptorBlock(void *task, UInt8 *inCDB,
                                               UInt8 inSize);
static IOReturn task_SetScatterGatherEntries(void *task,
                                             SCSITaskSGElement *list,
                                             UInt8 entries,
                                             UInt64 transferCount,
                                             UInt8 transferDirection);
static IOReturn task_SetTimeoutDuration(void *task, UInt32 ms);
static IOReturn task_ExecuteTaskSync(void *task,
                                     SCSI_Sense_Data *senseDataBuffer,
                                     SCSITaskStatus *outStatus,
                                     UInt64 *realizedTransferCount);

static IOCFPlugInInterface      g_plugin_vtbl;
static MMCDeviceInterface       g_mmc_vtbl;
static SCSITaskDeviceInterface  g_std_vtbl;
static SCSITaskInterface        g_task_vtbl;
static IOCFPlugInInterface      *g_plugin_ptr = &g_plugin_vtbl;
static MMCDeviceInterface       *g_mmc_ptr    = &g_mmc_vtbl;
static SCSITaskDeviceInterface  *g_std_ptr    = &g_std_vtbl;
static SCSITaskInterface        *g_task_ptr   = &g_task_vtbl;
static bool g_vtbls_ready;

static void ensure_vtbls(void)
{
    if (g_vtbls_ready) return;
    g_plugin_vtbl.QueryInterface = plugin_QueryInterface;
    g_plugin_vtbl.AddRef         = com_AddRef;
    g_plugin_vtbl.Release        = com_Release;

    g_mmc_vtbl.QueryInterface            = plugin_QueryInterface; /* unused on mmc */
    g_mmc_vtbl.AddRef                    = com_AddRef;
    g_mmc_vtbl.Release                   = com_Release;
    g_mmc_vtbl.TestUnitReady             = mmc_TestUnitReady;
    g_mmc_vtbl.GetConfiguration          = mmc_GetConfiguration;
    g_mmc_vtbl.ReadDiscInformation       = mmc_ReadDiscInformation;
    g_mmc_vtbl.ReadTableOfContents       = mmc_ReadTableOfContents;
    g_mmc_vtbl.ReadDiscStructure         = mmc_ReadDiscStructure;
    g_mmc_vtbl.ReadTrackInformation      = mmc_ReadTrackInformation;
    g_mmc_vtbl.GetPerformance            = mmc_GetPerformance;
    g_mmc_vtbl.ModeSense10               = mmc_ModeSense10;
    g_mmc_vtbl.GetSCSITaskDeviceInterface = mmc_GetSCSITaskDeviceInterface;

    g_std_vtbl.AddRef                 = com_AddRef;
    g_std_vtbl.Release                = com_Release;
    g_std_vtbl.ObtainExclusiveAccess  = std_ObtainExclusiveAccess;
    g_std_vtbl.ReleaseExclusiveAccess = std_ReleaseExclusiveAccess;
    g_std_vtbl.CreateSCSITask         = std_CreateSCSITask;

    g_task_vtbl.AddRef                     = com_AddRef;
    g_task_vtbl.Release                    = com_Release;
    g_task_vtbl.SetCommandDescriptorBlock  = task_SetCommandDescriptorBlock;
    g_task_vtbl.SetScatterGatherEntries    = task_SetScatterGatherEntries;
    g_task_vtbl.SetTimeoutDuration         = task_SetTimeoutDuration;
    g_task_vtbl.ExecuteTaskSync            = task_ExecuteTaskSync;
    g_vtbls_ready = true;
}

kern_return_t IOCreatePlugInInterfaceForService(io_service_t service,
                                               CFUUIDRef pluginType,
                                               CFUUIDRef interfaceType,
                                               IOCFPlugInInterface ***theInterface,
                                               SInt32 *theScore)
{
    (void)pluginType; (void)interfaceType;
    if (service != FAKE_SVC || !theInterface) return KERN_FAILURE;
    if (g.plugin_fail) return KERN_FAILURE;  /* kext declines to attach */
    ensure_vtbls();
    *theInterface = &g_plugin_ptr;
    if (theScore) *theScore = 0;
    return KERN_SUCCESS;
}

kern_return_t IODestroyPlugInInterface(IOCFPlugInInterface **interface)
{
    (void)interface;
    return KERN_SUCCESS;
}

static HRESULT plugin_QueryInterface(void *self, REFIID iid, LPVOID *ppv)
{
    (void)self; (void)iid;
    if (!ppv) return (HRESULT)0x80000000; /* E_POINTER-ish */
    ensure_vtbls();
    *ppv = &g_mmc_ptr;       /* hand back the MMCDeviceInterface */
    return S_OK;
}

static IOReturn mmc_TestUnitReady(void *self, SCSITaskStatus *taskStatus,
                                  SCSI_Sense_Data *senseDataBuffer)
{
    (void)self;
    /* A transport failure delivers nothing — outputs untouched. */
    if (g.method_rc[MOS_FAKE_METHOD_TUR]) {
        return (IOReturn)g.method_rc[MOS_FAKE_METHOD_TUR];
    }
    if (taskStatus)      *taskStatus = (SCSITaskStatus)g.tur_status;
    if (senseDataBuffer) memcpy(senseDataBuffer, g.tur_sense, 18);
    return kIOReturnSuccess;
}

static IOReturn mmc_GetConfiguration(void *self, SCSICmdField1Byte RT,
                                     SCSICmdField2Byte feature, void *buffer,
                                     SCSICmdField2Byte bufferSize,
                                     SCSITaskStatus *taskStatus,
                                     SCSI_Sense_Data *senseDataBuffer)
{
    (void)self; (void)RT; (void)feature; (void)senseDataBuffer;
    if (g.method_rc[MOS_FAKE_METHOD_GETCONFIG]) {
        return (IOReturn)g.method_rc[MOS_FAKE_METHOD_GETCONFIG];
    }
    if (buffer && bufferSize) {
        size_t n = (g.cfg_len < (size_t)bufferSize) ? g.cfg_len : (size_t)bufferSize;
        memset(buffer, 0, bufferSize);
        if (n) memcpy(buffer, g.cfg, n);
    }
    if (taskStatus) *taskStatus = (SCSITaskStatus)g.cfg_status;
    return kIOReturnSuccess;
}

static IOReturn mmc_ReadDiscInformation(void *self, void *buffer,
                                        SCSICmdField2Byte bufferSize,
                                        SCSITaskStatus *taskStatus,
                                        SCSI_Sense_Data *senseDataBuffer)
{
    (void)self; (void)senseDataBuffer;
    if (g.method_rc[MOS_FAKE_METHOD_READDISCINFO]) {
        return (IOReturn)g.method_rc[MOS_FAKE_METHOD_READDISCINFO];
    }
    if (buffer && bufferSize) {
        size_t n = (g.rdi_len < (size_t)bufferSize) ? g.rdi_len : (size_t)bufferSize;
        memset(buffer, 0, bufferSize);
        if (n) memcpy(buffer, g.rdi, n);
    }
    if (taskStatus) *taskStatus = (SCSITaskStatus)g.rdi_status;
    return kIOReturnSuccess;
}

static IOReturn mmc_ReadTableOfContents(void *self, SCSICmdField1Bit MSF,
                                        SCSICmdField4Bit FORMAT,
                                        SCSICmdField1Byte TRACK_SESSION_NUMBER,
                                        void *buffer,
                                        SCSICmdField2Byte bufferSize,
                                        SCSITaskStatus *taskStatus,
                                        SCSI_Sense_Data *senseDataBuffer)
{
    (void)self; (void)MSF; (void)FORMAT; (void)TRACK_SESSION_NUMBER;
    (void)senseDataBuffer;
    if (g.method_rc[MOS_FAKE_METHOD_READTOC]) {
        return (IOReturn)g.method_rc[MOS_FAKE_METHOD_READTOC];
    }
    if (buffer && bufferSize) {
        size_t n = (g.toc_len < (size_t)bufferSize) ? g.toc_len : (size_t)bufferSize;
        memset(buffer, 0, bufferSize);
        if (n) memcpy(buffer, g.toc, n);
    }
    if (taskStatus) *taskStatus = (SCSITaskStatus)g.toc_status;
    return kIOReturnSuccess;
}

static IOReturn mmc_ReadDiscStructure(void *self, SCSICmdField4Bit MEDIA_TYPE,
                                      SCSICmdField4Byte ADDRESS,
                                      SCSICmdField1Byte LAYER_NUMBER,
                                      SCSICmdField1Byte FORMAT,
                                      void *buffer,
                                      SCSICmdField2Byte bufferSize,
                                      SCSITaskStatus *taskStatus,
                                      SCSI_Sense_Data *senseDataBuffer)
{
    (void)self; (void)MEDIA_TYPE; (void)ADDRESS; (void)LAYER_NUMBER;
    (void)FORMAT; (void)senseDataBuffer;
    if (g.method_rc[MOS_FAKE_METHOD_READDISCSTRUCT]) {
        return (IOReturn)g.method_rc[MOS_FAKE_METHOD_READDISCSTRUCT];
    }
    if (buffer && bufferSize) {
        size_t n = (g.ds_len < (size_t)bufferSize) ? g.ds_len : (size_t)bufferSize;
        memset(buffer, 0, bufferSize);
        if (n) memcpy(buffer, g.ds, n);
    }
    if (taskStatus) *taskStatus = (SCSITaskStatus)g.ds_status;
    return kIOReturnSuccess;
}

/* The v0.4 enrichment convenience methods. By default they return GOOD with
   a zeroed reply, so the pure decoders yield empty/null results
   (speeds/mechanical/error_recovery/track_info null) — enough to exercise
   the adapter call paths and emit valid documents. A scenario wanting
   populated values would script a canned reply (no current test needs it). */
static IOReturn mmc_ReadTrackInformation(void *self,
                                         SCSICmdField2Bit ADDRESS_NUMBER_TYPE,
                                         SCSICmdField4Byte LBA_TRACK_SESSION,
                                         void *buffer, SCSICmdField2Byte bufferSize,
                                         SCSITaskStatus *taskStatus,
                                         SCSI_Sense_Data *senseDataBuffer)
{
    (void)self; (void)ADDRESS_NUMBER_TYPE; (void)LBA_TRACK_SESSION;
    (void)senseDataBuffer;
    if (buffer && bufferSize) {
        size_t n = (g.rti_len < (size_t)bufferSize) ? g.rti_len : (size_t)bufferSize;
        memset(buffer, 0, bufferSize);
        if (n) memcpy(buffer, g.rti, n);
    }
    if (taskStatus)
        *taskStatus = (SCSITaskStatus)(g.rti_len ? g.rti_status
                                                 : kSCSITaskStatus_GOOD);
    return kIOReturnSuccess;
}

static IOReturn mmc_GetPerformance(void *self, SCSICmdField2Bit TOLERANCE,
                                   SCSICmdField1Bit WRITE, SCSICmdField2Bit EXCEPT,
                                   SCSICmdField4Byte STARTING_LBA,
                                   SCSICmdField2Byte MAXIMUM_NUMBER_OF_DESCRIPTORS,
                                   void *buffer, SCSICmdField2Byte bufferSize,
                                   SCSITaskStatus *taskStatus,
                                   SCSI_Sense_Data *senseDataBuffer)
{
    (void)self; (void)TOLERANCE; (void)WRITE; (void)EXCEPT;
    (void)STARTING_LBA; (void)MAXIMUM_NUMBER_OF_DESCRIPTORS;
    (void)senseDataBuffer;
    if (buffer && bufferSize) {
        size_t n = (g.perf_len < (size_t)bufferSize) ? g.perf_len : (size_t)bufferSize;
        memset(buffer, 0, bufferSize);
        if (n) memcpy(buffer, g.perf, n);
    }
    if (taskStatus) *taskStatus = (SCSITaskStatus)g.perf_status;
    return kIOReturnSuccess;
}

static IOReturn mmc_ModeSense10(void *self, SCSICmdField1Bit LLBAA,
                                SCSICmdField1Bit DBD, SCSICmdField2Bit PC,
                                SCSICmdField6Bit PAGE_CODE,
                                void *buffer, SCSICmdField2Byte bufferSize,
                                SCSITaskStatus *taskStatus,
                                SCSI_Sense_Data *senseDataBuffer)
{
    (void)self; (void)LLBAA; (void)DBD; (void)PC; (void)PAGE_CODE;
    (void)senseDataBuffer;
    if (buffer && bufferSize) memset(buffer, 0, bufferSize);
    if (taskStatus) *taskStatus = (SCSITaskStatus)kSCSITaskStatus_GOOD;
    return kIOReturnSuccess;
}

static SCSITaskDeviceInterface **mmc_GetSCSITaskDeviceInterface(void *self)
{
    (void)self;
    ensure_vtbls();
    return &g_std_ptr;
}

/* ---- SCSITaskDeviceInterface: the exclusive lock -------------------- */

static IOReturn std_ObtainExclusiveAccess(void *self)
{
    (void)self;
    /* A Finder/system mount: the kernel reports the media still mounted as
       BUSY (distinct from a peer client's EXCLUSIVE_ACCESS). Cleared by a
       successful DADiskUnmount, so a `tray eject --force` re-eject succeeds. */
    if (g.mounted_busy) return kIOReturnBusy;
    if (g.exclusive_denied) return kIOReturnExclusiveAccess;
    /* The kernel refuses a second exclusive open; modeled so a double-acquire
       in the adapter is a test failure, not a silent balance bump. */
    if (g_lock_balance > 0) return kIOReturnExclusiveAccess;
    g_lock_balance++;
    g_lock_acquires++;
    return kIOReturnSuccess;
}

static IOReturn std_ReleaseExclusiveAccess(void *self)
{
    (void)self;
    g_release_calls++;
    /* Model a kernel that does NOT confirm the release: return non-success and
       leave the lock HELD (balance unchanged). The adapter must then keep the
       handle poisoned (have_exclusive true) and retry at mos_close. */
    if (g.release_fail) return kIOReturnError;
    /* Over-release drives the balance negative; the test's balance==0 check
       catches it. */
    g_lock_balance--;
    return kIOReturnSuccess;
}

static SCSITaskInterface **std_CreateSCSITask(void *self)
{
    (void)self;
    g_task_creates++;
    ensure_vtbls();
    memset(&g_task, 0, sizeof g_task);
    return &g_task_ptr;
}

/* ---- SCSITaskInterface: capture, then deliver the scripted reply ---- */

static IOReturn task_SetCommandDescriptorBlock(void *task, UInt8 *inCDB,
                                               UInt8 inSize)
{
    (void)task;
    if (!inCDB || inSize > 16) return kIOReturnBadArgument;
    memcpy(g_task.cdb, inCDB, inSize);
    g_task.cdb_len = inSize;
    return kIOReturnSuccess;
}

static IOReturn task_SetScatterGatherEntries(void *task,
                                             SCSITaskSGElement *list,
                                             UInt8 entries,
                                             UInt64 transferCount,
                                             UInt8 transferDirection)
{
    (void)task; (void)transferDirection;
    g_task.buf = NULL;
    g_task.buf_len = 0;
    if (list && entries >= 1) {
        g_task.buf     = (void *)(uintptr_t)list[0].address;
        g_task.buf_len = (size_t)transferCount;
    }
    return kIOReturnSuccess;
}

static IOReturn task_SetTimeoutDuration(void *task, UInt32 ms)
{
    (void)task;
    g_task.timeout_ms = ms;
    return kIOReturnSuccess;
}

static IOReturn task_ExecuteTaskSync(void *task,
                                     SCSI_Sense_Data *senseDataBuffer,
                                     SCSITaskStatus *outStatus,
                                     UInt64 *realizedTransferCount)
{
    (void)task;
    g_execute_calls++;
    if (g.method_rc[MOS_FAKE_METHOD_EXECUTE]) {
        return (IOReturn)g.method_rc[MOS_FAKE_METHOD_EXECUTE];
    }
    if (g_task.buf && g_task.buf_len) {
        size_t n = (g.raw_len < g_task.buf_len) ? g.raw_len : g_task.buf_len;
        memset(g_task.buf, 0, g_task.buf_len);
        if (n) memcpy(g_task.buf, g.raw, n);
    }
    if (senseDataBuffer)        memcpy(senseDataBuffer, g.raw_sense, 18);
    if (outStatus)              *outStatus = (SCSITaskStatus)g.raw_status;
    if (realizedTransferCount)  *realizedTransferCount = g.raw_realized;
    return kIOReturnSuccess;
}

/* ---- DiscRecording directory --------------------------------------- */

static void dict_set_str(CFMutableDictionaryRef d, CFStringRef key,
                         const char *val)
{
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, val,
                                              kCFStringEncodingUTF8);
    if (s) { CFDictionarySetValue(d, key, s); CFRelease(s); }
}

/* LIMIT: the directory stays single-drive — a deliberate decoupling.
   Watch-all's adapter-layer additions (Appeared→snapshot→slot wiring,
   Disappeared→id-resolve→per-slot removal, the doorbell-or-fail open gate,
   stream_open_ms constancy across joins) are all exercised with one drive
   appearing, leaving, and rejoining under a re-minted ID
   (test_adapter_watch.c). The ascending-registry-id same-tick interleave
   across MULTIPLE drives lives in the pure multiplexer and is pinned by
   test_watch_core.c; modelling a second drive here would restructure every
   singleton table in this fake to re-test it. If a multi-drive adapter
   scenario ever earns its keep, this array is the starting point. */
CFArrayRef DRCopyDeviceArray(void)
{
    const void *vals[1];
    CFIndex n = 0;
    if (g.present) { vals[0] = FAKE_DEV; n = 1; }
    return CFArrayCreate(kCFAllocatorDefault, vals, n, &kCFTypeArrayCallBacks);
}

/* A held DRDeviceRef is "still usable" while the modelled drive is present.
   The fake does not model a device going stale mid-call, so this tracks
   presence (the snapshot/identity guards proceed when the device exists). */
Boolean DRDeviceIsValid(DRDeviceRef device)
{
    return device != NULL && g.present;
}

CFDictionaryRef DRDeviceCopyInfo(DRDeviceRef device)
{
    (void)device;
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!d) return NULL;
    dict_set_str(d, kDRDeviceVendorNameKey,         g.vendor);
    dict_set_str(d, kDRDeviceProductNameKey,        g.product);
    dict_set_str(d, kDRDeviceFirmwareRevisionKey,   g.revision);
    dict_set_str(d, kDRDeviceIORegistryEntryPathKey, g.path);
    return d;
}

CFDictionaryRef DRDeviceCopyStatus(DRDeviceRef device)
{
    (void)device;
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!d) return NULL;
    if (g.bsd_unit >= 0) {
        CFMutableDictionaryRef mi = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (mi) {
            char name[32];
            snprintf(name, sizeof name, "disk%lld", (long long)g.bsd_unit);
            dict_set_str(mi, kDRDeviceMediaBSDNameKey, name);
            CFDictionarySetValue(d, kDRDeviceMediaInfoKey, mi);
            CFRelease(mi);
        }
    }
    return d;
}

/* Matches the scenario's actual BSD name. The adapter passes the canonical
   "diskN" rendering, so a media-less drive (unit < 0) has no name and ANY
   lookup misses — making "well-formed but absent → NO_DEVICE" expressible
   headless. */
DRDeviceRef DRDeviceCopyDeviceForBSDName(CFStringRef name)
{
    if (!g.present || g.bsd_unit < 0 || !name) return NULL;
    char want[32], got[64];
    snprintf(want, sizeof want, "disk%lld", (long long)g.bsd_unit);
    if (!CFStringGetCString(name, got, sizeof got, kCFStringEncodingUTF8)) {
        return NULL;
    }
    if (strcmp(got, want) != 0) return NULL;
    return (DRDeviceRef)CFRetain(FAKE_DEV); /* balances the adapter's CFRelease */
}

DRDeviceRef DRDeviceCopyDeviceForIORegistryEntryPath(CFStringRef path)
{
    (void)path;
    if (!g.present) return NULL;
    return (DRDeviceRef)CFRetain(FAKE_DEV);
}

/* ---- DiskArbitration: the one-shot volume lookup (mos_da.c) --------- *
 *
 * Same seam rule as IOKit/DR: the fake supplies the DA symbols, real
 * CoreFoundation does the lifetimes — session and disk are real CF objects
 * (so the adapter's CFRelease discipline is exercised), and the description
 * is a real dictionary carrying a real CFURL, the type mos_da.c must check
 * for. Reset state (da_present false) models "DA knows no such disk":
 * DADiskCopyDescription returns NULL. */

const CFStringRef kDADiskDescriptionVolumeNameKey = CFSTR("DAVolumeName");
const CFStringRef kDADiskDescriptionVolumePathKey = CFSTR("DAVolumePath");

DASessionRef DASessionCreate(CFAllocatorRef allocator)
{
    return (DASessionRef)CFStringCreateWithCString(
        allocator, "fake-da-session", kCFStringEncodingUTF8);
}

DADiskRef DADiskCreateFromBSDName(CFAllocatorRef allocator,
                                  DASessionRef session, const char *name)
{
    (void)session;
    if (!name || !name[0]) return NULL;
    return (DADiskRef)CFStringCreateWithCString(
        allocator, name, kCFStringEncodingUTF8);
}

/* Name-backed ref (real DA reads kIOBSDNameKey and delegates to
   DADiskCreateFromBSDName): the resulting DADiskRef carries only the name, so
   what it later resolves to is re-read via DADiskCopyIOMedia, not pinned here. */
DADiskRef DADiskCreateFromIOMedia(CFAllocatorRef allocator,
                                  DASessionRef session, io_service_t media)
{
    (void)session;
    if (media == IO_OBJECT_NULL) return NULL;
    return (DADiskRef)CFStringCreateWithCString(
        allocator, "fake-da-disk-from-iomedia", kCFStringEncodingUTF8);
}

/* The IOMedia the DADiskRef currently resolves to (mos_internal_da_volume's
   endpoint identity guard). Returns FAKE_DA_MEDIA when the disk resolves to
   current whole-disk media (drive present, media inserted), else IO_OBJECT_NULL
   — modelling a "diskN" that no longer backs any IOMedia. Its registry id (via
   IORegistryEntryGetRegistryEntryID above) is the media's id by default, or the
   desync'd da_media_id when a test forces the wrong-target reuse case. The
   returned object is "owned"; mos_da.c releases it via IOObjectRelease (a fake
   no-op on this sentinel). */
io_service_t DADiskCopyIOMedia(DADiskRef disk)
{
    if (!disk || !g.present || g.bsd_unit < 0) return IO_OBJECT_NULL;
    return FAKE_DA_MEDIA;
}

CFDictionaryRef DADiskCopyDescription(DADiskRef disk)
{
    if (!disk || !g.da_present) return NULL;

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2,
        &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (g.da_name[0]) {
        CFStringRef v = CFStringCreateWithCString(
            kCFAllocatorDefault, g.da_name, kCFStringEncodingUTF8);
        if (v) { CFDictionarySetValue(d, kDADiskDescriptionVolumeNameKey, v);
                 CFRelease(v); }
    }
    if (g.da_path[0]) {
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault, (const UInt8 *)g.da_path,
            (CFIndex)strlen(g.da_path), true);
        if (u) { CFDictionarySetValue(d, kDADiskDescriptionVolumePathKey, u);
                 CFRelease(u); }
    }
    return d;
}

/* Unmount path (mos_internal_da_unmount). The real DADiskUnmount is async and
   delivers via the session's run-loop source; the fake models an immediate
   result by invoking the callback synchronously, which sets ctx.done before
   mos_internal_da_unmount enters its run-loop wait (so the loop is skipped).
   The schedule/unschedule calls are no-ops here — there is no real DA source —
   so when the callback never fires (unmount_never_completes) the run loop's
   private mode is empty and CFRunLoopRunInMode returns kCFRunLoopRunFinished at
   once: the F1 fast-false path, exercised without waiting the real timeout. All
   exist so the headless binary links — it does not link -framework
   DiskArbitration. */
void DASessionScheduleWithRunLoop(DASessionRef session, CFRunLoopRef rl,
                                  CFStringRef mode)
{
    (void)session; (void)rl; (void)mode;
}

void DASessionUnscheduleFromRunLoop(DASessionRef session, CFRunLoopRef rl,
                                    CFStringRef mode)
{
    (void)session; (void)rl; (void)mode;
}

void DADiskUnmount(DADiskRef disk, DADiskUnmountOptions options,
                   DADiskUnmountCallback callback, void *context)
{
    (void)options;   /* graceful (Whole, no Force): mos never sets Force */
    if (g.unmount_never_completes) return;  /* no callback: F1 bounded-wait case */
    if (g.unmount_refused) {
        /* Busy filesystem: the daemon DISSENTS, the mount stays. mos surfaces
           MOS_ERR_BUSY (it never forces). Non-NULL dissenter = failure. */
        if (callback) callback(disk, (DADissenterRef)&g, context);
        return;
    }
    /* Idle volume: the graceful unmount succeeds, clearing the Finder/system
       mount so the subsequent eject's ObtainExclusiveAccess no longer BUSYs. */
    g.mounted_busy = false;
    if (callback) callback(disk, NULL, context);   /* NULL dissenter = success */
}
