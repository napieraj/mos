/* cli/watch.c — the watch command: NDJSON end to end. */
#include "common.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

/* ---- Signal handling (watch) ------------------------------------------- *
 *
 * SIGINT terminates the loop cleanly: stop pumping, close the handle, exit
 * 0. A volatile sig_atomic_t flag is the portable handler-to-loop channel. */
#include <unistd.h>  /* isatty(), STDOUT_FILENO */
static volatile sig_atomic_t watch_interrupted = 0;
static void watch_sigint_handler(int signum)
{
    (void)signum;
    watch_interrupted = 1;
}

/* Watch-emit status — local vocabulary over the shared classifier
   (mos_cli_stdout_finalize in cli/io; fflush + sticky ferror + EPIPE),
   the same one the one-shot paths use:

     WATCH_EMIT_OK            wrote, keep watching
     WATCH_EMIT_PIPE_CLOSED   EPIPE → tail-f clean exit, EX_OK
     WATCH_EMIT_WRITE_ERROR   other (ENOSPC, EIO, ...) → EX_IOERR */
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
        /* A set-but-invalid value is a typo: warn on stderr (stdout stays
           NDJSON-clean) and fall back — cadence tuning must never break a
           watch. Env is the same trust class as argv, so render through
           the safe-ASCII choke point, never verbatim to a tty. */
        fprintf(stderr, "%s: ignoring %s=\"", progname, name);
        mos_cli_safe_ascii(stderr, v);
        fputs("\" (want integer ms, 0..3600000); using default\n", stderr);
        return default_value;
    }
    return (uint32_t)n;
}

/* Command descriptor (see mos_cli_command in common.h). The NDJSON flag is
   what makes watch NDJSON end to end: main.c forces --json on for it and the
   shared error emitter (cli/common.c) reads the same flag off mos_cli_selected
   to pick compact single-line framing — no per-verb global. */
const mos_cli_command mos_cli_command_watch = {
    .name = "watch", .synopsis = "[drive]", .run = mos_cli_run_watch,
    .summary = "Stream state events as NDJSON until SIGINT",
    .help =
        "Stream state-change events as NDJSON (mos.event.v1), one line per\n"
        "event, until SIGINT. With no selector it watches the whole bus (zero\n"
        "drives is a valid empty stream awaiting hot-plug); a selector narrows\n"
        "to one drive. Output is always NDJSON, so --json is a no-op here;\n"
        "--json-seq frames each line with an RS (0x1E) per RFC 7464. Poll\n"
        "cadence is tunable via MOS_WATCH_STABLE_MS / MOS_WATCH_TRANSITION_MS.\n"
        "\n"
        "Examples:\n"
        "  mos watch            every drive, NDJSON to stdout\n"
        "  mos watch 2          one drive until SIGINT\n"
        "  mos watch disk4 | jq .   pipe the event stream",
    .flags = MOS_CLI_CMD_NDJSON,
};

int mos_cli_run_watch(void)
{
    /* Install the SIGINT handler before opening, so a Ctrl-C during the
       open call still cleans up. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = watch_sigint_handler;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: next_event should return promptly on SIGINT so the
       loop can re-check watch_interrupted. */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uint32_t stable_ms     = getenv_uint("MOS_WATCH_STABLE_MS",     0);
    uint32_t transition_ms = getenv_uint("MOS_WATCH_TRANSITION_MS", 0);

    mos_error err = MOS_OK;
    mos_watch_t *w = NULL;
    bool single_target = (opt_bsd || opt_index || opt_registry);
    if (opt_bsd) {
        /* Same as mos_cli_run_state: the library parse normalizes; don't duplicate. */
        w = mos_watch_open_by_bsd_name(opt_bsd, stable_ms, transition_ms, &err);
    } else if (opt_index) {
        w = mos_watch_open_by_index(opt_index, stable_ms, transition_ms, &err);
    } else if (opt_registry) {
        w = mos_watch_open_by_registry_id(opt_registry, stable_ms,
                                          transition_ms, &err);
    } else {
        /* No selector: the whole bus. Watch is a stream tool, so the
           default is total coverage (journalctl -f) and a selector
           NARROWS. Zero drives is a valid empty stream awaiting hot-plug;
           doorbell-unavailable fails honestly (open_all contract, mos.h). */
        w = mos_watch_open_all(stable_ms, transition_ms, &err);
    }

    if (!w) return mos_cli_emit_unknown_and_fail("could not open drive for watch",
                                         err, NULL);

    /* Loop: pump events until SIGINT or device_removed. */
    int rc = EX_OK;
    while (!watch_interrupted) {
        const mos_watch_event *ev = NULL;
        /* This timeout bounds SIGNAL latency, not poll cadence: SIGINT only
           sets a flag, and CFRunLoopRunInMode is not reliably broken by
           signal delivery, so Ctrl-C is noticed when the slice expires and
           the loop re-checks. Capped independently of the poll rates —
           coupling it to transition_ms would mean an up-to-one-hour Ctrl-C
           stall at MOS_WATCH_TRANSITION_MS=3600000. Shorter slices don't
           probe more often (the pump's deadlines are internal; a TIMEOUT
           just re-enters); they only bound shutdown latency. */
        int timeout_ms = (int)(transition_ms && transition_ms < 500
                                   ? transition_ms : 500);
        mos_error ne = mos_watch_next_event(w, &ev, timeout_ms);
        if (ne == MOS_ERR_TIMEOUT) {
            continue;  /* no event this slice; re-check the signal */
        }
        if (ne != MOS_OK) {
            /* Pump-level failure (distinct from a kind-ERROR event). Format
               the BSD unit before mos_watch_close so the envelope can carry
               it; a no-media unit gives "" / false. */
            char bsd_buf[24];
            if (!mos_bsd_dev_node(mos_watch_bsd_unit(w), bsd_buf, sizeof bsd_buf)) {
                bsd_buf[0] = 0;
            }
            mos_watch_close(w);
            return mos_cli_emit_unknown_and_fail("watch pump failed", ne,
                                         bsd_buf[0] ? bsd_buf : NULL);
        }

        watch_emit_status est = mos_cli_emit_watch_ndjson(ev);

        /* Downstream pipe handling, `tail -f` semantics. SIGPIPE is
           ignored at main() entry so writes return EPIPE; on a closed
           stdout, stop probing, release the watch, and return EX_OK — the
           producer succeeded, the consumer ended the conversation. */
        if (est == WATCH_EMIT_PIPE_CLOSED) {
            mos_watch_close(w);
            return EX_OK;
        }
        if (est == WATCH_EMIT_WRITE_ERROR) {
            /* Non-EPIPE write failure (ENOSPC, EIO, ...). Distinct exit
               code so scripts can tell "consumer closed cleanly" from
               "writing the stream failed for real". */
            mos_watch_close(w);
            return EX_IOERR;
        }

        if (single_target &&
            mos_watch_event_kind(ev) == MOS_EVENT_DEVICE_REMOVED) {
            rc = EX_OK;  /* single-target removal ends the watch */
            break;
        }
        /* On the bus, device_removed is per-drive and the stream continues
           (open_all contract); only SIGINT/pipe ends it. */
    }

    mos_watch_close(w);
    return rc;
}

