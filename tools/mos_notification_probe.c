/*
 * mos_notification_probe.c — diagnostic that subscribes to every plausible
 * push-notification source for one optical drive and logs each event as
 * NDJSON (mos.notification_probe.v0) with monotonic + RFC 3339 timestamps.
 *
 * Purpose: measure empirically which IOKit/DA notifications actually fire
 * for the MMC storage stack, and at what latency — the data the mos watch
 * architecture's "free push notifications" assumption rests on, which
 * Apple's docs don't pin down.
 *
 * Usage: mos_notification_probe <bsd_name>.  Subscribes to:
 *   - kIOGeneralInterest on the io_service_t (IsTerminated, PropertyChange,
 *     BusyStateChange, WasClosed, ...)
 *   - kIOBusyInterest on the io_service_t (BusyStateChange)
 *   - DARegisterDiskDescriptionChangedCallback on the BSD name
 *   - DRNotificationCenter: kDRDeviceAppeared / kDRDeviceDisappeared /
 *     kDRDeviceStatusChanged (device-global — the DR pivot's doorbell
 *     candidates; events for OTHER drives are emitted too, tagged by
 *     whatever identity the info dictionary carries)
 * Runs until SIGINT.
 *
 * Usage: mos_notification_probe --dr-dump
 *   One-shot instead: DRCopyDeviceArray order plus each device's
 *   DRDeviceCopyInfo / DRDeviceCopyStatus dictionary as an XML plist,
 *   then exit. This is the Phase 0 fixture-capture mode of the DR
 *   pivot (doc/research/2026-06-10-dr-pivot-implementation-plan.md);
 *   it answers the registry-path-shape and identity-byte-shape
 *   questions the plan lists. (Absorbed from the briefly-separate
 *   mos_dr_probe — one observation tool, not a zoo.)
 *
 * Deliberately does NOT link mos_core — only IOKit/CoreFoundation/DA/
 * DiscRecording directly — so it still builds if the library is broken
 * and its observations pass through none of mos's abstractions.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <DiscRecording/DRCoreDevice.h>
#include <DiscRecording/DRCoreNotifications.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>

#include <inttypes.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>

/* mos_pure.h / mos.h give us mos_internal_normalize_bsd_name and the
   canonical mos_json_escape. These live in the pure layer (no IOKit, no
   state interpretation), so including them does NOT link mos_core and does
   not compromise the observe-unfiltered design — and it avoids a second,
   subtly-different escape rule in the repo. */
#include "../src/mos_pure.h"
#include "mos.h"
#include "../cli/io.h"

/* ---- Signal handling --------------------------------------------- */

static volatile sig_atomic_t g_interrupted = 0;
static void on_sigint(int sig) { (void)sig; g_interrupted = 1; }

/* ---- Time helpers ------------------------------------------------- */

static uint64_t g_start_mono_ns;
static mach_timebase_info_data_t g_timebase;

static void init_timebase(void) {
    /* On failure denom would stay 0 and mono_ms_since_start would divide
       by zero; fall back to identity (1/1), which is also the Apple-Silicon
       common case where ticks already are nanoseconds. */
    kern_return_t kr = mach_timebase_info(&g_timebase);
    if (kr != KERN_SUCCESS || g_timebase.denom == 0) {
        g_timebase.numer = 1;
        g_timebase.denom = 1;
    }
    g_start_mono_ns = mach_absolute_time();
}

static uint64_t mono_ms_since_start(void) {
    uint64_t abs_now = mach_absolute_time();
    uint64_t abs_delta = abs_now - g_start_mono_ns;
    /* Convert mach_absolute_time units to nanoseconds, then to ms.
       Divide-first to keep the multiply in range: a plain
       delta*numer/denom overflows uint64 on Intel timebases
       (numer>1) at large uptimes; quotient/remainder split loses
       nothing (remainder*numer < denom*numer fits trivially). */
    uint64_t q  = abs_delta / g_timebase.denom;
    uint64_t r  = abs_delta % g_timebase.denom;
    uint64_t ns = q * g_timebase.numer + r * g_timebase.numer / g_timebase.denom;
    return ns / 1000000ULL;
}

static void format_rfc3339_utc(char *out, size_t cap) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_utc;
    gmtime_r(&ts.tv_sec, &tm_utc);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm_utc.tm_year + 1900,
             tm_utc.tm_mon + 1,
             tm_utc.tm_mday,
             tm_utc.tm_hour,
             tm_utc.tm_min,
             tm_utc.tm_sec,
             ts.tv_nsec / 1000000L);
}

/* ---- IOKit message-type names ------------------------------------ *
 *
 * Looked up by numeric value. Returns a stable string identifier;
 * unknown messages fall through to "kIOMessageUnknown" with the
 * numeric value preserved in a separate field. */
static const char *message_type_name(uint32_t mt) {
    switch (mt) {
        case kIOMessageServiceIsTerminated:       return "kIOMessageServiceIsTerminated";
        case kIOMessageServiceIsSuspended:        return "kIOMessageServiceIsSuspended";
        case kIOMessageServiceIsResumed:          return "kIOMessageServiceIsResumed";
        case kIOMessageServiceIsRequestingClose:  return "kIOMessageServiceIsRequestingClose";
        case kIOMessageServiceIsAttemptingOpen:   return "kIOMessageServiceIsAttemptingOpen";
        case kIOMessageServiceWasClosed:          return "kIOMessageServiceWasClosed";
        case kIOMessageServiceBusyStateChange:    return "kIOMessageServiceBusyStateChange";
        case kIOMessageServicePropertyChange:     return "kIOMessageServicePropertyChange";
        case kIOMessageDeviceWillPowerOff:        return "kIOMessageDeviceWillPowerOff";
        case kIOMessageDeviceHasPoweredOn:        return "kIOMessageDeviceHasPoweredOn";
        case kIOMessageCanSystemSleep:            return "kIOMessageCanSystemSleep";
        case kIOMessageSystemWillNotSleep:        return "kIOMessageSystemWillNotSleep";
        default:                                  return "kIOMessageUnknown";
    }
}

/* Read a CFTypeRef expected to be a CFString into a C buffer. Returns true
   only if it was a CFString and CFStringGetCString succeeded; on false the
   buffer is left empty. Pre-escape step for CFString-sourced values (DA
   description keys, IORegistry property strings). */

static bool cf_string_to_cstr_safe(CFTypeRef cf, char *buf, size_t cap) {
    if (cap == 0) return false;
    buf[0] = '\0';
    if (!cf) return false;
    if (CFGetTypeID(cf) != CFStringGetTypeID()) return false;
    /* On false, Apple's spec leaves the buffer contents undefined (possibly
       partial, unterminated), so treat it as a hard failure and clear. */
    if (!CFStringGetCString((CFStringRef)cf, buf,
                            (CFIndex)cap, kCFStringEncodingUTF8)) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

/* ---- NDJSON emission --------------------------------------------- */

static void emit_startup(const char *bsd_name, const char *vendor,
                         const char *product) {
    char ts[64];
    format_rfc3339_utc(ts, sizeof(ts));
    /* vendor/product are INQUIRY-derived (drive controls the bytes), so all
       strings go through mos_cli_json_str, which quotes and escapes. */
    fputs("{", stdout);
    fputs("\"schema\":\"mos.notification_probe.v0\"", stdout);
    fputs(",\"event\":\"startup\"", stdout);
    fputs(",\"ts\":", stdout); mos_cli_json_str(stdout, ts);
    printf(",\"mono_ms\":%" PRIu64, mono_ms_since_start());
    fputs(",\"bsd_name\":", stdout); mos_cli_json_str(stdout, bsd_name);
    if (vendor) {
        fputs(",\"vendor\":", stdout); mos_cli_json_str(stdout, vendor);
    }
    if (product) {
        fputs(",\"product\":", stdout); mos_cli_json_str(stdout, product);
    }
    fputs("}\n", stdout);
    fflush(stdout);
}

static void emit_iokit_event(const char *source,
                             uint32_t message_type,
                             void *message_argument,
                             const char *bsd_name) {
    char ts[64];
    format_rfc3339_utc(ts, sizeof(ts));
    fputs("{", stdout);
    fputs("\"schema\":\"mos.notification_probe.v0\"", stdout);
    fputs(",\"event\":\"iokit_message\"", stdout);
    fputs(",\"ts\":", stdout); mos_cli_json_str(stdout, ts);
    printf(",\"mono_ms\":%" PRIu64, mono_ms_since_start());
    /* source is an internal const string; escaped anyway for uniformity. */
    fputs(",\"source\":", stdout); mos_cli_json_str(stdout, source);
    fputs(",\"message_type\":", stdout);
    mos_cli_json_str(stdout, message_type_name(message_type));
    printf(",\"message_type_raw\":\"0x%x\"", message_type);
    /* message_argument is a kernel pointer; its numeric value has no
       diagnostic use to the operator (changes every boot). Emit
       whether it was NULL as a boolean instead. Avoids the
       implementation-defined "%p" rendering of NULL ("(nil)", "0x0",
       "nil" depending on libc version). */
    fputs(",\"message_arg_nonnull\":", stdout);
    fputs(message_argument ? "true" : "false", stdout);
    fputs(",\"bsd_name\":", stdout); mos_cli_json_str(stdout, bsd_name);
    fputs("}\n", stdout);
    fflush(stdout);
}

static void emit_da_event(const char *event_name,
                          const char *bsd_name,
                          CFArrayRef keys) {
    char ts[64];
    format_rfc3339_utc(ts, sizeof(ts));
    fputs("{", stdout);
    fputs("\"schema\":\"mos.notification_probe.v0\"", stdout);
    fputs(",\"event\":", stdout); mos_cli_json_str(stdout, event_name);
    fputs(",\"ts\":", stdout); mos_cli_json_str(stdout, ts);
    printf(",\"mono_ms\":%" PRIu64, mono_ms_since_start());
    fputs(",\"source\":\"da\"", stdout);
    fputs(",\"bsd_name\":", stdout); mos_cli_json_str(stdout, bsd_name);
    /* For description-changed events, surface which keys changed.
       Each entry must be validated (CFTypeRef → CFString → C string)
       and JSON-escaped. Entries that fail validation are skipped
       entirely, NOT emitted as empty strings, so the comma logic
       tracks whether any entry has actually been written. */
    if (keys) {
        fputs(",\"keys_changed\":[", stdout);
        CFIndex n = CFArrayGetCount(keys);
        bool any_emitted = false;
        for (CFIndex i = 0; i < n; i++) {
            CFTypeRef k = CFArrayGetValueAtIndex(keys, i);
            char buf[256];
            if (!cf_string_to_cstr_safe(k, buf, sizeof(buf))) {
                /* Not a CFString, or didn't fit in buf. Skip this
                   entry rather than emit potentially-malformed JSON. */
                continue;
            }
            if (any_emitted) fputc(',', stdout);
            mos_cli_json_str(stdout, buf);
            any_emitted = true;
        }
        fputc(']', stdout);
    }
    fputs("}\n", stdout);
    fflush(stdout);
}

static void emit_shutdown(const char *bsd_name, const char *reason) {
    char ts[64];
    format_rfc3339_utc(ts, sizeof(ts));
    fputs("{", stdout);
    fputs("\"schema\":\"mos.notification_probe.v0\"", stdout);
    fputs(",\"event\":\"shutdown\"", stdout);
    fputs(",\"ts\":", stdout); mos_cli_json_str(stdout, ts);
    printf(",\"mono_ms\":%" PRIu64, mono_ms_since_start());
    fputs(",\"bsd_name\":", stdout); mos_cli_json_str(stdout, bsd_name);
    fputs(",\"reason\":", stdout); mos_cli_json_str(stdout, reason);
    fputs("}\n", stdout);
    fflush(stdout);
}

/* ---- IOKit callback wrappers ------------------------------------- *
 *
 * One callback per interest type so the source name is distinguishable
 * at log time (we can't recover the interest type from the message
 * alone — both interests can deliver overlapping message sets). */
struct probe_ctx {
    char    bsd_name[64];   /* canonical "diskN": NDJSON output + IOKit emit */
    int64_t bsd_unit;       /* parsed unit: the DA self/partition filter */
};

/* ---- DiscRecording source ----------------------------------------- */

/* DR notification, NDJSON line. The info dictionary for StatusChanged
   is the device's status dict; surface the doorbell-relevant fields
   (tray-open, media state, media BSD name) when present, validated
   and escaped like every other external string. Full dictionaries
   belong to --dr-dump, not the event stream. */
static void emit_dr_event(CFStringRef name, CFDictionaryRef info)
{
    char ts[64], buf[256];
    format_rfc3339_utc(ts, sizeof(ts));
    fputs("{", stdout);
    fputs("\"schema\":\"mos.notification_probe.v0\"", stdout);
    fputs(",\"event\":\"dr_notification\"", stdout);
    fputs(",\"ts\":", stdout); mos_cli_json_str(stdout, ts);
    printf(",\"mono_ms\":%" PRIu64, mono_ms_since_start());
    fputs(",\"source\":\"dr\"", stdout);
    fputs(",\"name\":", stdout);
    if (cf_string_to_cstr_safe(name, buf, sizeof(buf))) {
        mos_cli_json_str(stdout, buf);
    } else {
        fputs("\"(unrenderable)\"", stdout);
    }
    if (info) {
        CFTypeRef tray = CFDictionaryGetValue(info, kDRDeviceIsTrayOpenKey);
        if (tray && CFGetTypeID(tray) == CFBooleanGetTypeID()) {
            fputs(",\"tray_open\":", stdout);
            fputs(CFBooleanGetValue((CFBooleanRef)tray) ? "true" : "false",
                  stdout);
        }
        if (cf_string_to_cstr_safe(
                CFDictionaryGetValue(info, kDRDeviceMediaStateKey),
                buf, sizeof(buf))) {
            fputs(",\"media_state\":", stdout);
            mos_cli_json_str(stdout, buf);
        }
        CFTypeRef mi = CFDictionaryGetValue(info, kDRDeviceMediaInfoKey);
        if (mi && CFGetTypeID(mi) == CFDictionaryGetTypeID() &&
            cf_string_to_cstr_safe(
                CFDictionaryGetValue((CFDictionaryRef)mi,
                                     kDRDeviceMediaBSDNameKey),
                buf, sizeof(buf))) {
            fputs(",\"media_bsd\":", stdout);
            mos_cli_json_str(stdout, buf);
        }
    }
    fputs("}\n", stdout);
    fflush(stdout);
}

static void dr_notification_cb(DRNotificationCenterRef center,
                               void *observer, CFStringRef name,
                               DRTypeRef object, CFDictionaryRef info)
{
    (void)center; (void)observer; (void)object;
    emit_dr_event(name, info);
}

/* ---- --dr-dump: one-shot DR dictionary capture --------------------- */

/* XML-plist dump of any CF property-list object; the fixture format.
   Not NDJSON on purpose — captures are whole-run redirections and XML
   plists diff cleanly. */
static void dr_dump_plist(CFTypeRef obj)
{
    if (!obj) {
        puts("  (null)");
        return;
    }
    CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault, obj,
                                              kCFPropertyListXMLFormat_v1_0,
                                              0, NULL);
    if (data) {
        size_t len = (size_t)CFDataGetLength(data);
        if (fwrite(CFDataGetBytePtr(data), 1, len, stdout) != len) {
            /* A truncated fixture is worse than no fixture: say so on
               stderr (stdout may be the broken stream itself). */
            fputs("warning: short write dumping plist — fixture "
                  "output is incomplete\n", stderr);
        }
        CFRelease(data);
        return;
    }
    puts("  (not serializable as a plist)");
}

static int run_dr_dump(void)
{
    char ts[64];
    format_rfc3339_utc(ts, sizeof(ts));

    CFArrayRef arr = DRCopyDeviceArray();
    long n = arr ? (long)CFArrayGetCount(arr) : 0;
    printf("mos_notification_probe --dr-dump %s\n", ts);
    printf("DRCopyDeviceArray: %ld device(s)%s\n", n,
           arr ? "" : " (NULL array)");
    if (!arr) return EX_UNAVAILABLE;

    for (long i = 0; i < n; ++i) {
        DRDeviceRef dev =
            (DRDeviceRef)CFArrayGetValueAtIndex(arr, (CFIndex)i);
        if (!dev) continue;
        printf("==== device %ld ====\n", i + 1);
        CFDictionaryRef info = DRDeviceCopyInfo(dev);
        printf("---- DRDeviceCopyInfo (device %ld) ----\n", i + 1);
        dr_dump_plist(info);
        if (info) CFRelease(info);
        CFDictionaryRef status = DRDeviceCopyStatus(dev);
        printf("---- DRDeviceCopyStatus (device %ld) ----\n", i + 1);
        dr_dump_plist(status);
        if (status) CFRelease(status);
    }
    CFRelease(arr);
    return EX_OK;
}

static void general_interest_cb(void *refcon,
                                io_service_t service,
                                natural_t messageType,
                                void *messageArgument) {
    (void)service;
    struct probe_ctx *ctx = (struct probe_ctx *)refcon;
    if (!ctx) return;
    emit_iokit_event("iokit_general",
                     (uint32_t)messageType,
                     messageArgument,
                     ctx->bsd_name);
}

static void busy_interest_cb(void *refcon,
                             io_service_t service,
                             natural_t messageType,
                             void *messageArgument) {
    (void)service;
    struct probe_ctx *ctx = (struct probe_ctx *)refcon;
    if (!ctx) return;
    emit_iokit_event("iokit_busy",
                     (uint32_t)messageType,
                     messageArgument,
                     ctx->bsd_name);
}

static void da_appeared_cb(DADiskRef disk, void *context) {
    struct probe_ctx *ctx = (struct probe_ctx *)context;
    if (!ctx) return;
    const char *bsd = DADiskGetBSDName(disk);
    if (!mos_internal_bsd_unit_matches(bsd, ctx->bsd_unit)) return;
    emit_da_event("da_disk_appeared", bsd, NULL);
}

static void da_disappeared_cb(DADiskRef disk, void *context) {
    struct probe_ctx *ctx = (struct probe_ctx *)context;
    if (!ctx) return;
    const char *bsd = DADiskGetBSDName(disk);
    if (!mos_internal_bsd_unit_matches(bsd, ctx->bsd_unit)) return;
    emit_da_event("da_disk_disappeared", bsd, NULL);
}

static void da_description_changed_cb(DADiskRef disk, CFArrayRef keys,
                                      void *context) {
    struct probe_ctx *ctx = (struct probe_ctx *)context;
    if (!ctx) return;
    const char *bsd = DADiskGetBSDName(disk);
    if (!mos_internal_bsd_unit_matches(bsd, ctx->bsd_unit)) return;
    emit_da_event("da_description_changed", bsd, keys);
}

/* ---- Service resolution by BSD name ------------------------------ */

static io_service_t resolve_io_service_by_bsd(const char *bsd_name) {
    /* IOMedia or IOMMCStorageServices both expose kIOBSDNameKey on
       their child IOMedia. We want the SCSI peripheral device (the
       drive itself), not just the IOMedia that disappears when there's
       no disc. Walk from the IOMedia up to its parents until we hit
       something in the SCSI peripheral class. */
    CFMutableDictionaryRef match = IOBSDNameMatching(kIOMainPortDefault,
                                                     0, bsd_name);
    if (!match) return IO_OBJECT_NULL;

    io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (media == IO_OBJECT_NULL) return IO_OBJECT_NULL;

    io_service_t cur = media;
    /* If IOObjectRetain fails (object invalidated between match and retain),
       skip the parent walk and return media at its original +1 — usable, just
       at the IOMedia layer. Without this, the first IOObjectRelease(cur) in
       the loop would free media and leave the return pointing at freed memory. */
    if (IOObjectRetain(cur) != KERN_SUCCESS) {
        return media;
    }
    while (cur != IO_OBJECT_NULL) {
        if (IOObjectConformsTo(cur, "IOSCSIPeripheralDeviceType05") ||
            IOObjectConformsTo(cur, "IODVDServices") ||
            IOObjectConformsTo(cur, "IOBDServices") ||
            IOObjectConformsTo(cur, "IOCDBlockStorageDevice") ||
            IOObjectConformsTo(cur, "IODVDBlockStorageDevice") ||
            IOObjectConformsTo(cur, "IOBDBlockStorageDevice")) {
            IOObjectRelease(media);
            return cur;
        }
        io_service_t parent = IO_OBJECT_NULL;
        kern_return_t kr = IORegistryEntryGetParentEntry(cur,
                                                         kIOServicePlane,
                                                         &parent);
        IOObjectRelease(cur);
        if (kr != KERN_SUCCESS) {
            cur = IO_OBJECT_NULL;
            break;
        }
        cur = parent;
    }

    /* Fall back to the IOMedia itself if we didn't find a SCSI
       peripheral parent. The notifications will still tell us
       something useful, just at a different layer of the stack. */
    return media;
}

/* ---- Main --------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--dr-dump") == 0) {
        init_timebase();
        return run_dr_dump();
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s <bsd_name> | --dr-dump\n", argv[0]);
        fprintf(stderr, "  e.g. %s disk4\n", argv[0]);
        fprintf(stderr, "  --dr-dump: one-shot DiscRecording Info/Status"
                        " plist capture\n");
        return EX_USAGE;
    }

    init_timebase();
    signal(SIGINT, on_sigint);

    /* Parse to the whole-disk unit (-1 for partition/non-whole/malformed).
       The DA filter matches on the unit; the canonical "diskN" string
       reconstructed below is what IOBSDNameMatching and the NDJSON need. */
    int64_t bsd_unit = mos_internal_parse_bsd_unit(argv[1]);
    if (bsd_unit < 0) {
        /* argv can carry ANSI/OSC terminal-injection, so escape it even in
           an error path — same uniform escape rule as everywhere else. */
        fputs("error: ", stderr);
        mos_cli_safe_ascii(stderr, argv[1]);
        fputs(" does not look like a whole-disk BSD name\n"
              "       (expected forms: disk4, rdisk4, /dev/disk4, /dev/rdisk4)\n",
              stderr);
        return EX_USAGE;
    }

    /* Reconstruct canonical "diskN" for IOBSDNameMatching / NDJSON. Reject a
       unit that won't format rather than fail resolution with a vaguer error. */
    char bsd_name[16];
    if (!mos_bsd_name_format(bsd_unit, bsd_name, sizeof bsd_name)) {
        fprintf(stderr, "error: bsd unit %lld is not a valid whole-disk unit\n",
                (long long)bsd_unit);
        return EX_USAGE;
    }

    /* Resolve the drive's io_service_t. Fail fast if not found. */
    io_service_t svc = resolve_io_service_by_bsd(bsd_name);
    if (svc == IO_OBJECT_NULL) {
        fprintf(stderr, "error: no IOKit service for bsd_name=%s\n", bsd_name);
        return EX_NOINPUT;
    }

    /* Read vendor + product strings for the startup envelope so the
       operator can confirm they're watching the right drive. Both
       fields are optional in the envelope — if the property is absent,
       not a CFString, or doesn't fit in the buffer, we emit the
       envelope without that field rather than emitting garbage. */
    char vendor[64];
    char product[64];
    CFTypeRef cf_vendor = IORegistryEntrySearchCFProperty(
            svc, kIOServicePlane,
            CFSTR("Vendor Identification"), kCFAllocatorDefault,
            kIORegistryIterateRecursively | kIORegistryIterateParents);
    CFTypeRef cf_product = IORegistryEntrySearchCFProperty(
            svc, kIOServicePlane,
            CFSTR("Product Identification"), kCFAllocatorDefault,
            kIORegistryIterateRecursively | kIORegistryIterateParents);
    bool have_vendor  = cf_string_to_cstr_safe(cf_vendor,  vendor,  sizeof(vendor));
    bool have_product = cf_string_to_cstr_safe(cf_product, product, sizeof(product));
    if (cf_vendor)  CFRelease(cf_vendor);
    if (cf_product) CFRelease(cf_product);

    emit_startup(bsd_name,
                 have_vendor  ? vendor  : NULL,
                 have_product ? product : NULL);

    /* Set up context for callbacks. */
    struct probe_ctx ctx = {0};
    snprintf(ctx.bsd_name, sizeof(ctx.bsd_name), "%s", bsd_name);
    ctx.bsd_unit = bsd_unit;

    /* Notification port shared by both IOKit interests. */
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    if (!port) {
        fprintf(stderr, "error: IONotificationPortCreate failed\n");
        IOObjectRelease(svc);
        return EX_OSERR;
    }

    CFRunLoopSourceRef rls = IONotificationPortGetRunLoopSource(port);
    if (!rls) {
        fprintf(stderr, "error: IONotificationPortGetRunLoopSource failed\n");
        IONotificationPortDestroy(port);
        IOObjectRelease(svc);
        return EX_OSERR;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);

    /* Subscribe to kIOGeneralInterest. */
    io_object_t general_token = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceAddInterestNotification(
            port, svc, kIOGeneralInterest,
            general_interest_cb, &ctx, &general_token);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "warning: kIOGeneralInterest subscription failed (0x%x)\n", kr);
    }

    /* Subscribe to kIOBusyInterest. */
    io_object_t busy_token = IO_OBJECT_NULL;
    kr = IOServiceAddInterestNotification(
            port, svc, kIOBusyInterest,
            busy_interest_cb, &ctx, &busy_token);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "warning: kIOBusyInterest subscription failed (0x%x)\n", kr);
    }

    /* Set up DiskArbitration. */
    DASessionRef da = DASessionCreate(kCFAllocatorDefault);
    if (!da) {
        fprintf(stderr, "warning: DASessionCreate failed; DA events disabled\n");
    } else {
        DARegisterDiskAppearedCallback(da, NULL,
                                       da_appeared_cb, &ctx);
        DARegisterDiskDisappearedCallback(da, NULL,
                                          da_disappeared_cb, &ctx);
        DARegisterDiskDescriptionChangedCallback(da, NULL, NULL,
                                                 da_description_changed_cb,
                                                 &ctx);
        DASessionScheduleWithRunLoop(da, CFRunLoopGetCurrent(),
                                     kCFRunLoopDefaultMode);
    }

    /* Set up the DiscRecording notification source (device-global —
       DR's center has no per-device filter at registration; the
       events themselves carry identity). NULL-object registration is
       itself under test: if StatusChanged never fires here while
       drutil sees changes, that's a finding about the registration
       model, not a probe bug. */
    DRNotificationCenterRef dr = DRNotificationCenterCreate();
    CFRunLoopSourceRef dr_src = NULL;
    if (!dr) {
        fprintf(stderr, "warning: DRNotificationCenterCreate failed; "
                        "DR events disabled\n");
    } else {
        dr_src = DRNotificationCenterCreateRunLoopSource(dr);
        if (!dr_src) {
            fprintf(stderr, "warning: DR run-loop source creation failed; "
                            "DR events disabled\n");
            CFRelease(dr);
            dr = NULL;
        } else {
            CFRunLoopAddSource(CFRunLoopGetCurrent(), dr_src,
                               kCFRunLoopDefaultMode);
            DRNotificationCenterAddObserver(dr, &ctx, dr_notification_cb,
                                            kDRDeviceAppearedNotification,
                                            NULL);
            DRNotificationCenterAddObserver(dr, &ctx, dr_notification_cb,
                                            kDRDeviceDisappearedNotification,
                                            NULL);
            DRNotificationCenterAddObserver(dr, &ctx, dr_notification_cb,
                                            kDRDeviceStatusChangedNotification,
                                            NULL);
        }
    }

    /* Run the loop until SIGINT. CFRunLoopRunInMode with a short
       interval lets us notice the SIGINT flag promptly. */
    while (!g_interrupted) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
    }

    emit_shutdown(bsd_name, "sigint");

    /* Cleanup. */
    if (da) {
        /* Full teardown symmetry with mos_watch.c — unschedule,
           unregister every callback with its registration-time
           (fn, context) pair, then release — so a future
           copy-from-probe cannot regress the watch's sequence
           (fifth review, F12e: only the unregisters were missing). */
        DASessionUnscheduleFromRunLoop(da, CFRunLoopGetCurrent(),
                                       kCFRunLoopDefaultMode);
        DAUnregisterCallback(da, (void *)da_appeared_cb, &ctx);
        DAUnregisterCallback(da, (void *)da_disappeared_cb, &ctx);
        DAUnregisterCallback(da, (void *)da_description_changed_cb, &ctx);
        CFRelease(da);
    }
    if (dr) {
        /* Same teardown symmetry rule as the DA block above: remove
           every observer with its registration-time (observer, name)
           pair, drop the source, then release. */
        DRNotificationCenterRemoveObserver(dr, &ctx,
                                           kDRDeviceAppearedNotification,
                                           NULL);
        DRNotificationCenterRemoveObserver(dr, &ctx,
                                           kDRDeviceDisappearedNotification,
                                           NULL);
        DRNotificationCenterRemoveObserver(dr, &ctx,
                                           kDRDeviceStatusChangedNotification,
                                           NULL);
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), dr_src,
                              kCFRunLoopDefaultMode);
        CFRelease(dr_src);
        CFRelease(dr);
    }
    if (general_token != IO_OBJECT_NULL) IOObjectRelease(general_token);
    if (busy_token    != IO_OBJECT_NULL) IOObjectRelease(busy_token);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
    IONotificationPortDestroy(port);
    IOObjectRelease(svc);

    return EX_OK;
}
