/*
 * mos_dr_probe.c — standalone DiscRecording observation probe.
 *
 * Phase 0 of the DR pivot (doc/research/2026-06-10-dr-pivot-
 * implementation-plan.md): dump exactly what the DiscRecording C API
 * reports, unfiltered, so hardware capture sessions can turn the
 * output into committed fixtures and falsification evidence.
 *
 * Deliberately links DiscRecording + CoreFoundation ONLY — not
 * mos_core, not mos_pure — so it still builds if the library is
 * broken and its observations pass through none of mos's
 * abstractions (same doctrine as mos_notification_probe.c).
 *
 * Modes:
 *   mos_dr_probe          one-shot: device array order + per-device
 *                         Info and Status dictionaries as XML plists
 *   mos_dr_probe -n       notification stream: timestamped
 *                         kDRDeviceAppeared / kDRDeviceDisappeared /
 *                         kDRDeviceStatusChanged events (info dict as
 *                         XML plist per event) until SIGINT
 *
 * Output is a human/diffable text format, not NDJSON: fixtures are
 * captured by redirecting whole runs, and XML plists diff cleanly.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <DiscRecording/DRCoreDevice.h>
#include <DiscRecording/DRCoreNotifications.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t probe_interrupted = 0;

static void on_sigint(int sig)
{
    (void)sig;
    probe_interrupted = 1;
}

/* RFC 3339 UTC wall-clock timestamp with milliseconds. Probe-local on
   purpose (this tool vendors nothing from the library). */
static void format_ts(char *buf, size_t cap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_utc;
    gmtime_r(&ts.tv_sec, &tm_utc);
    size_t n = strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tm_utc);
    if (n > 0 && cap - n > 5) {
        snprintf(buf + n, cap - n, ".%03ldZ", ts.tv_nsec / 1000000L);
    }
}

/* Serialize any CF property-list object as XML to stdout. Falls back
   to CFCopyDescription for non-plist objects (shouldn't happen for
   DR's Info/Status dictionaries, but the probe must never lie by
   omission). */
static void dump_plist(CFTypeRef obj)
{
    if (!obj) {
        puts("  (null)");
        return;
    }
    CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault, obj,
                                              kCFPropertyListXMLFormat_v1_0,
                                              0, NULL);
    if (data) {
        fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data),
               stdout);
        CFRelease(data);
        return;
    }
    CFStringRef desc = CFCopyDescription(obj);
    if (desc) {
        char buf[1024];
        if (CFStringGetCString(desc, buf, sizeof buf,
                               kCFStringEncodingUTF8)) {
            printf("  (not a plist) %s\n", buf);
        }
        CFRelease(desc);
    }
}

static void dump_device(DRDeviceRef dev, long index1)
{
    printf("==== device %ld ====\n", index1);

    CFDictionaryRef info = DRDeviceCopyInfo(dev);
    printf("---- DRDeviceCopyInfo (device %ld) ----\n", index1);
    dump_plist(info);
    if (info) CFRelease(info);

    CFDictionaryRef status = DRDeviceCopyStatus(dev);
    printf("---- DRDeviceCopyStatus (device %ld) ----\n", index1);
    dump_plist(status);
    if (status) CFRelease(status);
}

static int run_oneshot(void)
{
    char ts[40];
    format_ts(ts, sizeof ts);

    CFArrayRef arr = DRCopyDeviceArray();
    long n = arr ? (long)CFArrayGetCount(arr) : -1;
    printf("mos_dr_probe one-shot %s\n", ts);
    printf("DRCopyDeviceArray: %ld device(s)%s\n",
           n < 0 ? 0 : n, arr ? "" : " (NULL array)");
    if (!arr) return 1;

    for (long i = 0; i < n; ++i) {
        DRDeviceRef dev =
            (DRDeviceRef)CFArrayGetValueAtIndex(arr, (CFIndex)i);
        if (dev) dump_device(dev, i + 1);
    }
    CFRelease(arr);
    return 0;
}

/* ---- Notification mode ----------------------------------------------- */

static void notif_cb(DRNotificationCenterRef center, void *observer,
                     CFStringRef name, DRTypeRef object,
                     CFDictionaryRef info)
{
    (void)center; (void)observer; (void)object;

    char ts[40], namebuf[128] = "(?)";
    format_ts(ts, sizeof ts);
    if (name) {
        (void)CFStringGetCString(name, namebuf, sizeof namebuf,
                                 kCFStringEncodingUTF8);
    }
    printf("==== %s %s ====\n", ts, namebuf);
    dump_plist(info);
    fflush(stdout);
}

static int run_notifications(void)
{
    DRNotificationCenterRef center = DRNotificationCenterCreate();
    if (!center) {
        fputs("mos_dr_probe: DRNotificationCenterCreate failed\n", stderr);
        return 1;
    }
    CFRunLoopSourceRef src = DRNotificationCenterCreateRunLoopSource(center);
    if (!src) {
        fputs("mos_dr_probe: DRNotificationCenterCreateRunLoopSource "
              "failed\n", stderr);
        CFRelease(center);
        return 1;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

    /* object == NULL: observe the name for every device. Whether
       StatusChanged actually delivers with a NULL object (vs requiring
       per-device registration) is one of the questions this probe
       exists to answer empirically — if the stream stays silent while
       drutil sees changes, that's the finding, not a probe bug. */
    DRNotificationCenterAddObserver(center, NULL, notif_cb,
                                    kDRDeviceAppearedNotification, NULL);
    DRNotificationCenterAddObserver(center, NULL, notif_cb,
                                    kDRDeviceDisappearedNotification, NULL);
    DRNotificationCenterAddObserver(center, NULL, notif_cb,
                                    kDRDeviceStatusChangedNotification, NULL);

    char ts[40];
    format_ts(ts, sizeof ts);
    printf("mos_dr_probe notification stream %s — SIGINT to stop\n", ts);
    fflush(stdout);

    signal(SIGINT, on_sigint);
    while (!probe_interrupted) {
        /* Wake periodically so SIGINT is honored even if no source
           fires; 0.25s is observation granularity, not event latency
           (events interrupt the run loop immediately). */
        (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
    }

    DRNotificationCenterRemoveObserver(center, NULL,
                                       kDRDeviceAppearedNotification, NULL);
    DRNotificationCenterRemoveObserver(center, NULL,
                                       kDRDeviceDisappearedNotification, NULL);
    DRNotificationCenterRemoveObserver(center, NULL,
                                       kDRDeviceStatusChangedNotification,
                                       NULL);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    CFRelease(src);
    CFRelease(center);

    format_ts(ts, sizeof ts);
    printf("==== %s stream closed (SIGINT) ====\n", ts);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "-n") == 0) {
        return run_notifications();
    }
    if (argc != 1) {
        fputs("usage: mos_dr_probe [-n]\n"
              "  (no args)  one-shot device array + Info/Status plists\n"
              "  -n         timestamped notification stream until SIGINT\n",
              stderr);
        return 64;
    }
    return run_oneshot();
}
