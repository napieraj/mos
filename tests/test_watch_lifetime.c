/* tests/test_watch_lifetime.c — macOS integration test that pins the
 * watch-event string-lifetime contract under AddressSanitizer.
 *
 * Contract: a mos_watch_event's string pointers (vendor / product /
 * revision) stay valid until the next mos_watch_next_event() or
 * mos_watch_close(). The watch upholds it by homing drive-controlled
 * strings into watch-owned buffers before the per-probe handle closes.
 *
 * The hazard is a dangling field — e.g. one string riding a `*out = tmp`
 * struct copy into the freed handle while the others were copied out.
 * This test reads every string field AFTER the probe handle is closed;
 * under ASan a dangling field reads poisoned memory and traps as
 * heap-use-after-free, intact bytes or not. A pure unit test can't catch
 * this: the free is in the IOKit adapter, below the pure layer's horizon.
 *
 * Needs a real optical drive; with none present it SKIPs (exit 0), so in
 * headless CI it is a no-op guard. Insert/eject a disc while it runs to
 * push extra STATE_CHANGED events through the same path.
 *
 * BUILD (macOS, against the built static lib):
 *   cc -std=c11 -O1 -fsanitize=address,undefined \
 *      -fno-omit-frame-pointer -fno-sanitize-recover=all \
 *      -I include tests/test_watch_lifetime.c \
 *      build/libmos.a build/libmos_pure.a \
 *      -framework IOKit -framework CoreFoundation -framework DiscRecording \
 *      -o /tmp/test_watch_lifetime
 *   ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
 *      /tmp/test_watch_lifetime
 */

#include <stdio.h>
#include <string.h>
#include "mos.h"

#if !defined(__APPLE__)
int main(void)
{
    fprintf(stderr, "SKIP: test_watch_lifetime is macOS-only (needs IOKit).\n");
    return 0;
}
#else

/* Force an actual read of each drive-controlled string: snprintf
 * dereferences the pointer, so a dangling one fires ASan here. We test
 * that touching them is legal, not that the bytes are right. */
static void touch_event_strings(const mos_watch_event *ev)
{
    char sink[64];
    /* bsd_unit and registry_id are values, not borrowed pointers — no
       lifetime concern; the string fields are the real subjects. */
    const char *vendor    = mos_watch_event_vendor(ev);
    const char *product   = mos_watch_event_product(ev);
    const char *revision  = mos_watch_event_revision(ev);
    /* serial is a borrowed string too (re-homed to watch/slot storage); the
       lifetime test must touch it or it cannot trap a dangling serial. */
    const char *serial    = mos_watch_event_serial(ev);
    uint64_t registry_id = mos_watch_event_registry_id(ev);
    snprintf(sink, sizeof(sink), "%lld", (long long)mos_watch_event_bsd_unit(ev));
    snprintf(sink, sizeof(sink), "%s", vendor    ? vendor    : "(null)");
    snprintf(sink, sizeof(sink), "%s", product   ? product   : "(null)");
    snprintf(sink, sizeof(sink), "%s", revision  ? revision  : "(null)");
    snprintf(sink, sizeof(sink), "%s", serial    ? serial    : "(null)");
    snprintf(sink, sizeof(sink), "%llu", (unsigned long long)registry_id);
    (void)sink;
}

int main(void)
{
    mos_error err = MOS_OK;

    /* Bind to the first optical drive. No drive => clean skip. */
    mos_watch_t *w = mos_watch_open_by_index(1, 0, 0, &err);
    if (!w) {
        if (err == MOS_ERR_NO_DEVICE) {
            fprintf(stderr, "SKIP: no optical drive present (MOS_ERR_NO_DEVICE).\n");
            return 0;
        }
        fprintf(stderr, "FAIL: mos_watch_open_by_index returned err=%d\n", (int)err);
        return 1;
    }

    const int   MAX_EVENTS   = 16;   /* snapshot + a handful of changes   */
    const int   MAX_TIMEOUTS = 3;    /* idle drive: stop after a few idles */
    const int   TIMEOUT_MS   = 1500;

    int events = 0, timeouts = 0;
    fprintf(stderr, "watching drive; insert/eject a disc to exercise transitions...\n");

    while (events < MAX_EVENTS && timeouts < MAX_TIMEOUTS) {
        const mos_watch_event *ev = NULL;
        mos_error e = mos_watch_next_event(w, &ev, TIMEOUT_MS);

        if (e == MOS_ERR_TIMEOUT) {
            timeouts++;
            continue;
        }
        if (e != MOS_OK) {
            fprintf(stderr, "FAIL: next_event err=%d\n", (int)e);
            mos_watch_close(w);
            return 1;
        }

        /* The handle that produced this event is already closed inside
         * the probe; reading its strings now is the regression trap. */
        touch_event_strings(ev);
        events++;
        timeouts = 0;

        fprintf(stderr, "  event seq=%llu kind=%d state=%d revision=%s\n",
                (unsigned long long)mos_watch_event_seq(ev),
                (int)mos_watch_event_kind(ev), (int)mos_watch_event_state(ev),
                mos_watch_event_revision(ev) ? mos_watch_event_revision(ev) : "(none)");

        if (mos_watch_event_kind(ev) == MOS_EVENT_DEVICE_REMOVED) {
            fprintf(stderr, "  terminal device_removed; ending watch.\n");
            break;
        }
    }

    mos_watch_close(w);
    fprintf(stderr, "OK: watch lifetime clean (%d event(s) touched under ASan)\n",
            events);
    return 0;
}

#endif /* __APPLE__ */
