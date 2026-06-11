/*
 * mos_fake_apple.c — link-seam fake of the Apple framework layer.
 *
 * Provides the IOKit + DiscRecording C symbols the one-shot adapter
 * path imports (mos_scsi.c / mos_state.c / mos_dr.c), so the real
 * adapter TUs run headless on a macOS build host with the real SDK
 * headers but WITHOUT linking IOKit / DiscRecording. Real
 * CoreFoundation stays linked — CF objects here are genuine.
 *
 * Mechanism and rationale:
 * doc/research/2026-06-11-headless-adapter-emulation.md. This is
 * phase 1 (open / query / enumerate); the watch lifecycle (phase 2)
 * adds mos_watch.c and the notification/run-loop symbols.
 *
 * Model: ONE optical drive. IOKit object handles are small integers
 * (io_object_t is a mach_port_t), resolved through a fixed table; the
 * DR "device" is an immortal CFSTR sentinel (so the adapter's
 * CFRelease on it is safe — it is a real CF object). MMC replies are
 * scripted from committed fixture bytes via the control surface in
 * mos_fake_apple.h.
 */

#include "mos_fake_apple.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOBSD.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICmds_REQUEST_SENSE_Defs.h>
#include <DiscRecording/DRCoreDevice.h>

#include <string.h>
#include <stdio.h>

/* kIOMainPortDefault is an extern const on macOS 12+; older SDKs spell
   it kIOMasterPortDefault. We don't link IOKit, so we must define the
   symbol the adapter references. Guard against the macro form. */
#ifndef kIOMainPortDefault
const mach_port_t kIOMainPortDefault = 0;
#endif

/* ---- DiscRecording key constants (the adapter reads these symbols;
   we define them since DiscRecording is not linked). Values are
   arbitrary but must be the SAME objects the dict-builders below use —
   they are, because both sides reference these symbols. ---- */
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
#define FAKE_ITER   ((io_iterator_t)10)      /* the (single) child iterator */
#define FAKE_DEV    ((DRDeviceRef)CFSTR("mos.fake.device"))
#define FAKE_ID_KEY  CFSTR("mos.fake.matchID")

/* ---- Scenario state ----------------------------------------------- */
static struct {
    bool     present;
    uint64_t drive_id;
    uint64_t media_id;
    int64_t  bsd_unit;          /* -1 == no whole-disk IOMedia child    */
    char     vendor[16];
    char     product[24];
    char     revision[8];
    char     path[128];
    uint32_t tur_status;        uint8_t tur_sense[18];
    uint32_t cfg_status;        uint8_t cfg[64];  size_t cfg_len;
    uint32_t rdi_status;        uint8_t rdi[64];  size_t rdi_len;
} g;

static int      g_lock_balance;
static unsigned g_iter_remaining;

/* ---- Control surface ---------------------------------------------- */

void mos_fake_reset(void)
{
    memset(&g, 0, sizeof g);
    g.present  = true;
    g.drive_id = 0x100000123ull;
    g.media_id = 0x100000456ull;
    g.bsd_unit = 4;
    strcpy(g.vendor,   "HL-DT-ST");
    strcpy(g.product,  "DVDROM");
    strcpy(g.revision, "A100");
    strcpy(g.path,     "IOService:/fake/MMCDevice");
    g.tur_status = 0; /* kSCSITaskStatus_GOOD */
    g_lock_balance   = 0;
    g_iter_remaining = 0;
}

void mos_fake_set_no_drive(void) { g.present = false; }

void mos_fake_set_bsd_unit(int64_t unit) { g.bsd_unit = unit; }

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

int mos_fake_lock_balance(void) { return g_lock_balance; }

/* ---- IOKit registry ------------------------------------------------ */

kern_return_t IORegistryEntryCreateIterator(io_registry_entry_t entry,
                                            const io_name_t plane,
                                            IOOptionBits options,
                                            io_iterator_t *iterator)
{
    (void)plane; (void)options;
    if (entry != FAKE_SVC || !iterator) return KERN_FAILURE;
    /* One whole-disk IOMedia child iff media is present. */
    g_iter_remaining = (g.bsd_unit >= 0) ? 1u : 0u;
    *iterator = FAKE_ITER;
    return KERN_SUCCESS;
}

io_object_t IOIteratorNext(io_iterator_t iterator)
{
    if (iterator != FAKE_ITER || g_iter_remaining == 0) return IO_OBJECT_NULL;
    g_iter_remaining--;
    return FAKE_MEDIA;
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
        snprintf(name, sizeof name, "disk%lld", (long long)g.bsd_unit);
        return CFStringCreateWithCString(kCFAllocatorDefault, name,
                                         kCFStringEncodingUTF8);
    }
    if (CFEqual(key, CFSTR("Whole"))) {
        return CFRetain(kCFBooleanTrue);
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
    if (entry == FAKE_MEDIA) { *entryID = g.media_id; return KERN_SUCCESS; }
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
static SCSITaskDeviceInterface **mmc_GetSCSITaskDeviceInterface(void *self);

static IOCFPlugInInterface g_plugin_vtbl;
static MMCDeviceInterface  g_mmc_vtbl;
static IOCFPlugInInterface *g_plugin_ptr = &g_plugin_vtbl;
static MMCDeviceInterface  *g_mmc_ptr    = &g_mmc_vtbl;
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
    g_mmc_vtbl.GetSCSITaskDeviceInterface = mmc_GetSCSITaskDeviceInterface;
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
    if (buffer && bufferSize) {
        size_t n = (g.rdi_len < (size_t)bufferSize) ? g.rdi_len : (size_t)bufferSize;
        memset(buffer, 0, bufferSize);
        if (n) memcpy(buffer, g.rdi, n);
    }
    if (taskStatus) *taskStatus = (SCSITaskStatus)g.rdi_status;
    return kIOReturnSuccess;
}

static SCSITaskDeviceInterface **mmc_GetSCSITaskDeviceInterface(void *self)
{
    (void)self;
    /* Phase 1: the raw-CDB / GESN path is unused on the READY route
       (GESN is reached only on the not-ready path). Returning NULL makes
       mos_raw_cdb report DRIVER_REJECTED if ever reached — phase 2 adds
       a real SCSITaskDeviceInterface fake for the not-ready scenarios. */
    return NULL;
}

/* ---- DiscRecording directory --------------------------------------- */

static void dict_set_str(CFMutableDictionaryRef d, CFStringRef key,
                         const char *val)
{
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, val,
                                              kCFStringEncodingUTF8);
    if (s) { CFDictionarySetValue(d, key, s); CFRelease(s); }
}

CFArrayRef DRCopyDeviceArray(void)
{
    const void *vals[1];
    CFIndex n = 0;
    if (g.present) { vals[0] = FAKE_DEV; n = 1; }
    return CFArrayCreate(kCFAllocatorDefault, vals, n, &kCFTypeArrayCallBacks);
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

DRDeviceRef DRDeviceCopyDeviceForBSDName(CFStringRef name)
{
    (void)name;
    if (!g.present) return NULL;
    return (DRDeviceRef)CFRetain(FAKE_DEV); /* balances the adapter's CFRelease */
}

DRDeviceRef DRDeviceCopyDeviceForIORegistryEntryPath(CFStringRef path)
{
    (void)path;
    if (!g.present) return NULL;
    return (DRDeviceRef)CFRetain(FAKE_DEV);
}
