/* cli/watch.c — the watch command: NDJSON end to end. */
#include "common.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

/* ---- Signal handling (--watch) ----------------------------------------- *
 *
 * SIGINT during a watch loop should terminate cleanly: stop pumping,
 * close the watch handle, exit 0. A volatile sig_atomic_t flag is the
 * portable way to communicate from a signal handler. */
#include <signal.h>
#include <unistd.h>  /* isatty(), STDOUT_FILENO */
static volatile sig_atomic_t watch_interrupted = 0;
static void watch_sigint_handler(int signum)
{
    (void)signum;
    watch_interrupted = 1;
}

#include <sysexits.h>

static const char *event_kind_string(mos_event_kind k)
{
    switch (k) {
        case MOS_EVENT_SNAPSHOT:       return "snapshot";
        case MOS_EVENT_STATE_CHANGED:  return "state_changed";
        case MOS_EVENT_MEDIA_CHANGED:  return "media_changed";
        case MOS_EVENT_ERROR:          return "error";
        case MOS_EVENT_DEVICE_REMOVED: return "device_removed";
    }
    return "unknown";
}

/* Watch-emit status — bridges "the downstream pipe just closed" from
   emit_watch_* up to the watch loop without either function needing to
   know how to call mos_watch_close.

   The classification rule (fflush + sticky ferror + best-effort EPIPE)
   lives once in cli/io (mos_cli_stdout_finalize), shared with the
   one-shot status/list paths. These aliases let the watch code below read
   in its own vocabulary while routing through that single definition.

     WATCH_EMIT_OK            successful write, keep watching
     WATCH_EMIT_PIPE_CLOSED   write failed with EPIPE → tail-f
                              semantics: clean exit, EX_OK
     WATCH_EMIT_WRITE_ERROR   write failed with something else
                              (ENOSPC, EIO, etc.) → exit EX_IOERR */
typedef mos_cli_stdout_status watch_emit_status;
#define WATCH_EMIT_OK          MOS_CLI_STDOUT_OK
#define WATCH_EMIT_PIPE_CLOSED MOS_CLI_STDOUT_PIPE_CLOSED
#define WATCH_EMIT_WRITE_ERROR MOS_CLI_STDOUT_WRITE_ERROR

static watch_emit_status watch_emit_check_stdout(void)
{
    return mos_cli_stdout_finalize();
}

static watch_emit_status emit_watch_ndjson(const mos_watch_event *e)
{
    mos_event_kind kind_e = mos_watch_event_kind(e);
    uint16_t    profile  = mos_watch_event_current_profile(e);
    const char *kind  = event_kind_string(kind_e);
    const char *state = mos_state_description(mos_watch_event_state(e));
    const char *prev  = mos_state_description(mos_watch_event_prev_state(e));
    const char *pname = mos_profile_name(profile);
    const char *vendor   = mos_watch_event_vendor(e);
    const char *product  = mos_watch_event_product(e);
    const char *revision = mos_watch_event_revision(e);
    mos_error   err      = mos_watch_event_error(e);
    uint32_t    latency  = mos_watch_event_latency_ms(e);
    uint8_t     sk, asc, ascq;
    mos_watch_event_sense(e, &sk, &asc, &ascq);

    fputs("{", stdout);
    fputs("\"schema\":\"mos.event.v1\"", stdout);
    fputs(",\"event\":\"", stdout); fputs(kind, stdout); fputc('"', stdout);
    /* Session identity as two plain JSON numbers — no composite token;
       consumers wanting one key concatenate. Both fit IEEE doubles for
       any realistic uptime (registry IDs start at 2^32+256 and epoch ms
       is ~2^41), so no string-quoting workaround is needed. */
    fprintf(stdout, ",\"registry_id\":%llu",
            (unsigned long long)mos_watch_event_registry_id(e));
    fprintf(stdout, ",\"stream_open_ms\":%llu",
            (unsigned long long)mos_watch_event_stream_open_ms(e));
    fprintf(stdout, ",\"seq\":%llu", (unsigned long long)mos_watch_event_seq(e));
    fputs(",\"ts\":", stdout); mos_cli_json_str(stdout, mos_watch_event_ts(e));
    /* bsd is required by mos.event.v1 and nullable: emit it
       unconditionally. mos_cli_bsd_dev_node renders unit < 0 as `null`, so an
       empty-drive event keeps the required field present as null rather
       than dropping it (which would fail schema validation), and a real
       unit as "diskN" — the JSON wire shape is unchanged. */
    fputs(",\"bsd\":", stdout); mos_cli_bsd_dev_node(stdout, mos_watch_event_bsd_unit(e));

    /* device_removed carries only prev_state; every other kind also carries
       the current state. prev_state is written unconditionally either way. */
    if (kind_e != MOS_EVENT_DEVICE_REMOVED) {
        fputs(",\"state\":\"", stdout); fputs(state, stdout); fputc('"', stdout);
    }
    fputs(",\"prev_state\":\"", stdout); fputs(prev, stdout); fputc('"', stdout);

    if (kind_e == MOS_EVENT_SNAPSHOT || kind_e == MOS_EVENT_STATE_CHANGED ||
        kind_e == MOS_EVENT_MEDIA_CHANGED) {
        fprintf(stdout, ",\"current_profile\":\"0x%04x\"", profile);
        /* Match emit_json's suppression: skip current_profile_name when
           current_profile is the SCSI sentinel 0x0000. See emit_json
           comment for rationale. */
        if (mos_cli_profile_present(profile) && pname) {
            fputs(",\"current_profile_name\":", stdout);
            mos_cli_json_str(stdout, pname);
        }
        /* Same derivation + suppression as emit_json's media_class. */
        {
            const char *mclass = mos_profile_class(profile);
            if (mos_cli_profile_present(profile) && mclass) {
                fputs(",\"media_class\":", stdout);
                mos_cli_json_str(stdout, mclass);
            }
        }
        if (vendor && *vendor) {
            fputs(",\"vendor\":", stdout);  mos_cli_json_str(stdout, vendor);
        }
        if (product && *product) {
            fputs(",\"product\":", stdout); mos_cli_json_str(stdout, product);
        }
        if (revision && *revision) {
            fputs(",\"revision\":", stdout); mos_cli_json_str(stdout, revision);
        }
        if (sk != 0 || asc != 0 || ascq != 0) {
            fprintf(stdout,
                ",\"sense\":{\"key\":\"0x%02x\","
                "\"asc\":\"0x%02x\",\"ascq\":\"0x%02x\"}",
                sk, asc, ascq);
        }
    }

    if (kind_e == MOS_EVENT_ERROR) {
        fputs(",\"error\":{\"code\":", stdout);
        mos_cli_json_str(stdout, mos_error_to_code(err));
        fputs(",\"message\":", stdout);
        mos_cli_json_str(stdout, mos_error_description(err));
        fputs(",\"recoverable\":", stdout);
        fputs(mos_error_is_recoverable(err) ? "true" : "false", stdout);
        fputc('}', stdout);
    }

    if (latency > 0) {
        fprintf(stdout, ",\"latency_ms\":%u", latency);
    }

    fputs("}\n", stdout);
    return watch_emit_check_stdout();
}


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
        fprintf(stderr,
                "mos: ignoring %s=\"%s\" (want integer ms, 0..3600000); "
                "using default\n", name, v);
        return default_value;
    }
    return (uint32_t)n;
}

int run_watch(void)
{
    /* Watch is NDJSON end to end (CLI design 2026-06-10): the event
       stream AND any error envelope. Forcing the flag here keeps
       emit_unknown_and_fail on its compact single-line framing without
       a second mode check. */
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
    if (opt_bsd) {
        /* Same as run_query: the library parse normalizes; don't duplicate. */
        w = mos_watch_open_by_bsd_name(opt_bsd, stable_ms, transition_ms, &err);
    } else {
        w = mos_watch_open_by_index(opt_index ? opt_index : 1,
                                    stable_ms, transition_ms, &err);
    }

    if (!w) return emit_unknown_and_fail("could not open drive for watch",
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
            return emit_unknown_and_fail("watch pump failed", ne,
                                         bsd_buf[0] ? bsd_buf : NULL);
        }

        /* CLI design 2026-06-10: watch is NDJSON unconditionally — a
           stream consumed by orchestrators has one format. --json is a
           no-op here; the plain token-per-line mode is removed. */
        watch_emit_status est = emit_watch_ndjson(ev);

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

        if (mos_watch_event_kind(ev) == MOS_EVENT_DEVICE_REMOVED) {
            rc = EX_OK;  /* Clean exit on device removal — watch ended. */
            break;
        }
    }

    mos_watch_close(w);
    return rc;
}

