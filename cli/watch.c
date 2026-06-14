/* cli/watch.c — the watch command: NDJSON end to end. */
#include "common.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

/* ---- Signal handling (watch) ------------------------------------------- *
 *
 * SIGINT during a watch loop should terminate cleanly: stop pumping,
 * close the watch handle, exit 0. A volatile sig_atomic_t flag is the
 * portable way to communicate from a signal handler. */
#include <unistd.h>  /* isatty(), STDOUT_FILENO */
static volatile sig_atomic_t watch_interrupted = 0;
static void watch_sigint_handler(int signum)
{
    (void)signum;
    watch_interrupted = 1;
}

/* Watch-emit status — bridges "the downstream pipe just closed" from
   mos_cli_emit_watch_ndjson up to the watch loop without either side
   needing to know how to call mos_watch_close.

   The classification rule (fflush + sticky ferror + best-effort EPIPE)
   lives once in cli/io (mos_cli_stdout_finalize), shared with the
   one-shot status/list paths. These aliases let the watch code below read
   in its own vocabulary while routing through that single definition.

     WATCH_EMIT_OK            successful write, keep watching
     WATCH_EMIT_PIPE_CLOSED   write failed with EPIPE → tail-f
                              semantics: clean exit, EX_OK
     WATCH_EMIT_WRITE_ERROR   write failed with something else
                              (ENOSPC, EIO, etc.) → exit EX_IOERR

   The NDJSON line emitter itself (mos_cli_emit_watch_ndjson) lives in
   cli/common.c, not here: it renders an event with no Apple-side
   dependency, and keeping it out of this adapter-bound TU lets the
   headless emit harness (tests/emit) validate its real output against
   mos.event.v1 without linking the watch pump and its IOKit/DR seam. */
typedef mos_cli_stdout_status watch_emit_status;
#define WATCH_EMIT_OK          MOS_CLI_STDOUT_OK
#define WATCH_EMIT_PIPE_CLOSED MOS_CLI_STDOUT_PIPE_CLOSED
#define WATCH_EMIT_WRITE_ERROR MOS_CLI_STDOUT_WRITE_ERROR

static uint32_t getenv_uint(const char *name, uint32_t default_value)
{
    const char *v = getenv(name);
    if (!v || !*v) return default_value;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (!end || *end != 0 || n > 3600000U) {
        /* A set-but-invalid value is a user typo, not an unset default:
           say so on stderr (the diagnostics channel in both output
           modes; stdout stays envelope/NDJSON-clean) and fall back
           rather than failing — cadence tuning should never be able to
           break a watch. */
        /* Env values are the same trust class as argv: render through
           the safe-ASCII choke point, never verbatim to a tty. */
        fprintf(stderr, "%s: ignoring %s=\"", progname, name);
        mos_cli_safe_ascii(stderr, v);
        fputs("\" (want integer ms, 0..3600000); using default\n", stderr);
        return default_value;
    }
    return (uint32_t)n;
}

int mos_cli_run_watch(void)
{
    /* Watch is NDJSON end to end — the event stream AND any error
       envelope (doc/research/2026-06-10-cli-design.md): a stream
       consumed by orchestrators has one format. Forcing the flag here
       keeps mos_cli_emit_unknown_and_fail on its compact single-line framing
       without a second mode check. */
    flag_json = true;

    /* Register SIGINT handler before opening the watch — if the user
       hits Ctrl-C during the open call, we want to clean up. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = watch_sigint_handler;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: we want the next_event call to return promptly
       on SIGINT so the loop can check watch_interrupted. */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uint32_t stable_ms     = getenv_uint("MOS_WATCH_STABLE_MS",     0);
    uint32_t transition_ms = getenv_uint("MOS_WATCH_TRANSITION_MS", 0);

    mos_error err = MOS_OK;
    mos_watch_t *w = NULL;
    bool single_target = (opt_bsd || opt_index || opt_registry);
    if (opt_bsd) {
        /* Same as mos_cli_run_query: the library parse normalizes; don't duplicate. */
        w = mos_watch_open_by_bsd_name(opt_bsd, stable_ms, transition_ms, &err);
    } else if (opt_index) {
        w = mos_watch_open_by_index(opt_index, stable_ms, transition_ms, &err);
    } else if (opt_registry) {
        w = mos_watch_open_by_registry_id(opt_registry, stable_ms,
                                          transition_ms, &err);
    } else {
        /* No selector: the bus. Watch is a stream tool, so the default
           is total coverage (journalctl -f shape) — a selector NARROWS.
           Zero drives is a valid empty stream that waits for hot-plug;
           the doorbell-unavailable case fails honestly (open_all
           contract, mos.h). Retired here: the sole-drive default
           (terminated on eject — every monitoring script needed a
           restart loop) and the --all flag (2026-06-12). */
        w = mos_watch_open_all(stable_ms, transition_ms, &err);
    }

    if (!w) return mos_cli_emit_unknown_and_fail("could not open drive for watch",
                                         err, NULL);

    /* Loop: pump events until SIGINT or device_removed. */
    int rc = EX_OK;
    while (!watch_interrupted) {
        const mos_watch_event *ev = NULL;
        /* The next_event timeout is the SIGNAL-LATENCY bound, not the
           poll cadence: SIGINT only sets a flag, and CFRunLoopRunInMode
           is not reliably broken by signal delivery, so Ctrl-C is
           noticed when this slice expires and the loop re-checks the
           flag. Cap it independently of the poll rates — coupling it to
           transition_ms inverted its own purpose at large env values
           (MOS_WATCH_TRANSITION_MS=3600000 meant an up-to-one-hour
           Ctrl-C stall). Shorter slices do NOT probe more often (the
           pump's deadlines are internal; a TIMEOUT return just
           re-enters), they only bound shutdown latency. 500 ms idle
           wakeups are negligible. */
        int timeout_ms = (int)(transition_ms && transition_ms < 500
                                   ? transition_ms : 500);
        mos_error ne = mos_watch_next_event(w, &ev, timeout_ms);
        if (ne == MOS_ERR_TIMEOUT) {
            /* No event in this slice; loop and re-check signal. */
            continue;
        }
        if (ne != MOS_OK) {
            /* Pump-level failure (distinct from an event of kind ERROR).
               Format the watch's BSD unit to "diskN" before
               mos_watch_close so the failure envelope can carry it;
               mos_bsd_dev_node yields "" / false for a no-media unit. */
            char bsd_buf[24];
            if (!mos_bsd_dev_node(mos_watch_bsd_unit(w), bsd_buf, sizeof bsd_buf)) {
                bsd_buf[0] = 0;
            }
            mos_watch_close(w);
            return mos_cli_emit_unknown_and_fail("watch pump failed", ne,
                                         bsd_buf[0] ? bsd_buf : NULL);
        }

        /* NDJSON unconditionally (forced in mos_cli_run_watch); --json is a
           no-op here. */
        watch_emit_status est = mos_cli_emit_watch_ndjson(ev);

        /* Downstream pipe handling. SIGPIPE is ignored at main() entry
           so writes return EPIPE instead of killing the process; the
           emit functions surface that via their return value. The
           clean-exit semantics here match `tail -f` and other long-
           running producers: when stdout closes, stop probing the
           drive, release the watch, return EX_OK so shell pipelines
           see a successful producer (the consumer is the one that
           ended the conversation). */
        if (est == WATCH_EMIT_PIPE_CLOSED) {
            mos_watch_close(w);
            return EX_OK;
        }
        if (est == WATCH_EMIT_WRITE_ERROR) {
            /* Non-EPIPE write failure (ENOSPC on a redirected log,
               EIO on a flaky filesystem, etc.). Distinct exit code
               so scripts can tell apart "consumer closed cleanly"
               from "writing the output stream failed for real." */
            mos_watch_close(w);
            return EX_IOERR;
        }

        if (single_target &&
            mos_watch_event_kind(ev) == MOS_EVENT_DEVICE_REMOVED) {
            rc = EX_OK;  /* Clean exit on device removal — watch ended. */
            break;
        }
        /* On the bus (no selector) device_removed is per-drive and the
           stream continues (mos.h open_all contract); only SIGINT/pipe
           ends it. */
    }

    mos_watch_close(w);
    return rc;
}

