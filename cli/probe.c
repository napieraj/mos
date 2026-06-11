/* cli/probe.c — the probe command: diagnostic substrate observer.
 * Relocated from tools/mos_notification_probe.c (2026-06-11) when the
 * standalone probes were consolidated into the CLI; compiled in only
 * under MOS_CLI_PROBE (default ON — see CMakeLists.txt; retiring the
 * command is one default flip).
 *
 * Two modes:
 *
 *   mos probe <drive>   Subscribe to the empirical push-notification
 *                       sources for one optical drive and log each
 *                       event as NDJSON (mos.probe.v0) with monotonic
 *                       + RFC 3339 timestamps, until SIGINT:
 *                         - kIOGeneralInterest on the io_service_t
 *                           (IsTerminated, PropertyChange,
 *                           BusyStateChange, WasClosed, ...)
 *                         - kIOBusyInterest on the io_service_t
 *                         - DRNotificationCenter: kDRDeviceAppeared /
 *                           kDRDeviceDisappeared / kDRDeviceStatusChanged
 *                           (device-global — the DR doorbell; events for
 *                           OTHER drives are emitted too, tagged by
 *                           whatever identity the info dictionary carries)
 *
 *   mos probe --dump    One-shot instead: DRCopyDeviceArray order plus
 *                       each device's DRDeviceCopyInfo / DRDeviceCopyStatus
 *                       dictionary as an XML plist, then exit. This is the
 *                       DR-pivot fixture-capture mode; it answers the
 *                       registry-path-shape and identity-byte-shape
 *                       questions in INTEGRATION_HARNESS.md.
 *
 * The DiskArbitration legs the standalone tool carried were retired with
 * the consolidation (2026-06-11, ROADMAP append): DA is filesystem-level,
 * the DR doorbell is the design's wake source, and no decision depends on
 * doorbell completeness — doorbells are latency-only over the poll floor
 * and the kernel itself polls media at 1000 ms.
 *
 * The standalone tool deliberately did not link mos_core, so it still
 * built when the library was broken; that build independence was traded
 * away in the consolidation. The OBSERVATION path is still raw — events
 * come straight from IOKit/DiscRecording callbacks and pass through none
 * of mos's state interpretation.
 */
#include "common.h"

#include <CoreFoundation/CoreFoundation.h>
#include <DiscRecording/DRCoreDevice.h>
#include <DiscRecording/DRCoreNotifications.h>
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

/* mos_pure.h gives us mos_internal_parse_bsd_unit. It lives in the pure
   layer (no IOKit, no state interpretation), so the observe-unfiltered
   design is intact — and it avoids a second, subtly-different BSD-name
   parse rule in the repo. The only cli/ TU that reaches a private
   header (CONTRIBUTING.md records the exception). */
#include "../src/mos_pure.h"

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
   buffer is left empty. Pre-escape step for CFString-sourced values
   (IORegistry property strings, DR dictionary values). */

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
    fputs("\"schema\":\"mos.probe.v0\"", stdout);
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
    fputs("\"schema\":\"mos.probe.v0\"", stdout);
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

static void emit_shutdown(const char *bsd_name, const char *reason) {
    char ts[64];
    format_rfc3339_utc(ts, sizeof(ts));
    fputs("{", stdout);
    fputs("\"schema\":\"mos.probe.v0\"", stdout);
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
    char bsd_name[64];   /* canonical "diskN": NDJSON output + IOKit emit */
};

/* ---- DiscRecording source ----------------------------------------- */

/* DR notification, NDJSON line. The info dictionary for StatusChanged
   is the device's status dict; surface the doorbell-relevant fields
   (tray-open, media state, media BSD name) when present, validated
   and escaped like every other external string. Full dictionaries
   belong to --dump, not the event stream. */
static void emit_dr_event(CFStringRef name, CFDictionaryRef info)
{
    char ts[64], buf[256];
    format_rfc3339_utc(ts, sizeof(ts));
    fputs("{", stdout);
    fputs("\"schema\":\"mos.probe.v0\"", stdout);
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

/* ---- --dump: one-shot DR dictionary capture ------------------------ */

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
    printf("mos probe --dump %s\n", ts);
    printf("DRCopyDeviceArray: %ld device(s)%s\n", n,
           arr ? "" : " (NULL array)");
    if (!arr) return finalize_failure_stdout(EX_UNAVAILABLE);

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
    return finalize_oneshot_stdout(EX_OK);
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

/* ---- Entry point --------------------------------------------------- */

int run_probe(void)
{
    if (flag_dump) {
        init_timebase();
        return run_dr_dump();
    }

    init_timebase();

    /* SIGINT/SIGTERM end the stream cleanly. Same shape as run_watch:
       sigaction without SA_RESTART so the run-loop slice below notices
       the flag promptly. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Selector → whole-disk unit. main.c guarantees exactly one of
       opt_bsd / opt_index is set for the stream mode. */
    int64_t bsd_unit = -1;
    if (opt_bsd) {
        /* Parse to the whole-disk unit (-1 for partition/non-whole/
           malformed). The canonical "diskN" string reconstructed below
           is what IOBSDNameMatching and the NDJSON need. */
        bsd_unit = mos_internal_parse_bsd_unit(opt_bsd);
        if (bsd_unit < 0) {
            /* opt_bsd is user argv and can carry ANSI/OSC terminal-
               injection, so escape it even in an error path — same
               uniform escape rule as everywhere else. */
            fprintf(stderr, "%s: ", progname);
            mos_cli_safe_ascii(stderr, opt_bsd);
            fputs(" does not look like a whole-disk BSD name\n"
                  "       (expected forms: disk4, rdisk4, /dev/disk4,"
                  " /dev/rdisk4)\n",
                  stderr);
            return EX_USAGE;
        }
    } else {
        if (!mos_cli_unit_for_index(opt_index, &bsd_unit)) {
            fprintf(stderr, "%s: no drive at index %d (see 'mos list')\n",
                    progname, opt_index);
            return EX_NOINPUT;
        }
        if (bsd_unit < 0) {
            /* The drive exists but has no whole-disk IOMedia node (media
               absent) — and the probe resolves its IOKit service through
               that node, so there is nothing to subscribe to yet. */
            fprintf(stderr,
                    "%s: drive %d has no BSD node (media absent); the probe"
                    " resolves its service by BSD name —\n"
                    "     load media first, or give a BSD form directly\n",
                    progname, opt_index);
            return EX_UNAVAILABLE;
        }
    }

    /* Reconstruct canonical "diskN" for IOBSDNameMatching / NDJSON. Reject a
       unit that won't format rather than fail resolution with a vaguer error. */
    char bsd_name[16];
    if (!mos_bsd_name_format(bsd_unit, bsd_name, sizeof bsd_name)) {
        fprintf(stderr, "%s: bsd unit %lld is not a valid whole-disk unit\n",
                progname, (long long)bsd_unit);
        return EX_USAGE;
    }

    /* Resolve the drive's io_service_t. Fail fast if not found. */
    io_service_t svc = resolve_io_service_by_bsd(bsd_name);
    if (svc == IO_OBJECT_NULL) {
        fprintf(stderr, "%s: no IOKit service for bsd_name=%s\n",
                progname, bsd_name);
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

    /* Notification port shared by both IOKit interests. */
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    if (!port) {
        fprintf(stderr, "%s: IONotificationPortCreate failed\n", progname);
        IOObjectRelease(svc);
        return EX_OSERR;
    }

    CFRunLoopSourceRef rls = IONotificationPortGetRunLoopSource(port);
    if (!rls) {
        fprintf(stderr, "%s: IONotificationPortGetRunLoopSource failed\n",
                progname);
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

    /* Run the loop until SIGINT — or until stdout dies. The standalone
       tool relied on default SIGPIPE delivery to exit when its consumer
       closed the pipe; inside mos, SIGPIPE is ignored process-wide
       (main.c), writes return EPIPE, and the emitters' fflush latches
       sticky ferror — which this condition checks. Without it,
       `mos probe diskN | head` would spin forever against a dead
       stream. CFRunLoopRunInMode with a short interval lets us notice
       both flags promptly. */
    while (!g_interrupted && !ferror(stdout)) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
    }

    int rc;
    if (g_interrupted) {
        emit_shutdown(bsd_name, "sigint");
        /* Clean write → EX_OK; consumer closed the pipe → still EX_OK
           (tail -f semantics, same fold as the watch); real write
           error → EX_IOERR. */
        rc = finalize_oneshot_stdout(EX_OK);
    } else {
        /* stdout failed mid-stream — the stream is dead, so no
           shutdown envelope; classify the latched failure. */
        rc = (mos_cli_stdout_finalize() == MOS_CLI_STDOUT_PIPE_CLOSED)
                 ? EX_OK : EX_IOERR;
    }

    /* Cleanup. Teardown symmetry rule (fifth review, F12e): remove
       every observer with its registration-time (observer, name)
       pair, drop the source, then release. */
    if (dr) {
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

    return rc;
}
