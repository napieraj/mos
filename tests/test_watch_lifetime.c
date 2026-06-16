/* tests/test_watch_lifetime.c — macOS integration test that pins the
 * watch-event string-lifetime contract under AddressSanitizer.
 *
 * WHAT IT GUARDS
 * --------------
 * mos.h documents that the string pointers in a mos_watch_event
 * (vendor / product / revision) remain valid
 * until the next mos_watch_next_event() call or mos_watch_close().
 * watch_probe() upholds that by re-homing the handle-borrowed strings
 * into watch-owned buffers before it closes the per-probe handle.
 *
 * The hazard is a partial re-home: if vendor and product are copied into
 * watch storage but `revision` rides the `*out = tmp` struct copy, it
 * dangles into the freed handle and the pure core forwards the dangling
 * pointer to the consumer.
 *
 * This test reads every drive-controlled string field AFTER the
 * probe's handle has been closed. Under ASan that read is the trap:
 * freed memory is poisoned, so a dangling field triggers
 * heap-use-after-free immediately — whether or not the freed bytes
 * happen to survive intact. A pure unit test cannot catch this; the
 * free happens in the IOKit adapter, below the pure layer's horizon
 * (see the ADAPTER POINTER-LIFETIME AUDIT RULE in src/mos_watch.c).
 *
 * SCOPE / LIMITATION
 * ------------------
 * It needs a real optical drive attached. With none present it SKIPS
 * (exit 0) — so in headless CI it is a no-op guard; run it on a Mac
 * with a drive for the real coverage. Insert/eject a disc while it
 * runs to drive extra STATE_CHANGED events through the same path.
 * The lifetime contract this pins survived the DR pivot's Phase 2a
 * mechanism change: identity is now captured ONCE at watch open into
 * watch-owned buffers (device-static directory data) and per-probe
 * results are repointed at them, so there is no per-probe re-home to
 * get wrong — but the observable contract (event strings valid until
 * the next call) is identical, and this test still pins it end to end
 * under ASan on the integrated IOKit path.
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

/* Force an actual read of each drive-controlled string. The snprintf
 * dereferences the pointer; if it dangles, ASan fires here. We do not
 * trust the bytes — only that touching them is legal. */
static void touch_event_strings(const mos_watch_event *ev)
{
    char sink[64];
    /* bsd_unit is a value, not a borrowed pointer, so it has
       no post-close lifetime concern; touch it as an int for completeness.
       The string fields below are the real subjects of this test. */
    const char *vendor    = mos_watch_event_vendor(ev);
    const char *product   = mos_watch_event_product(ev);
    const char *revision  = mos_watch_event_revision(ev);
    uint64_t registry_id = mos_watch_event_registry_id(ev);
    snprintf(sink, sizeof(sink), "%lld", (long long)mos_watch_event_bsd_unit(ev));
    snprintf(sink, sizeof(sink), "%s", vendor    ? vendor    : "(null)");
    snprintf(sink, sizeof(sink), "%s", product   ? product   : "(null)");
    snprintf(sink, sizeof(sink), "%s", revision  ? revision  : "(null)");
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

        /* The handle that produced this event has already been closed
         * inside watch_probe. Reading the strings now is the regression
         * trap: a dangling field is a poisoned-memory read under ASan. */
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
