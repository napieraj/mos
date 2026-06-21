/* cli/main.c — argument parsing, usage, dispatch. One file per command,
 * each owning its mos_cli_command descriptor (name, synopsis, summary, run);
 * a new verb is a new cli/<verb>.c with its descriptor plus one entry in the
 * mos_cli_commands[] table below — which drives dispatch, --help's subcommand
 * list, and (parsed from source) the shell completions, all from one place. */
#include "common.h"
#include "mos_pure.h"   /* mos_internal_value_is_registry_id (selector floor) */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

/* The command table — the single source of truth for the verb surface.
   Each descriptor lives in its own cli/<verb>.c; this array fixes their
   order and membership and is what scripts/gen-cli-docs.py and
   schemas/check_readme.py parse for the verb list (the &mos_cli_command_*
   tokens). A new verb is a new descriptor + one line here. */
static const mos_cli_command *const mos_cli_commands[] = {
    &mos_cli_command_state,
    &mos_cli_command_list,
    &mos_cli_command_watch,
    &mos_cli_command_metadata,
    &mos_cli_command_drive,
    &mos_cli_command_features,
    &mos_cli_command_tray,
    &mos_cli_command_capacity,
#ifdef MOS_CLI_PROBE
    &mos_cli_command_probe,
#endif
};
static const size_t mos_cli_ncommands =
    sizeof mos_cli_commands / sizeof mos_cli_commands[0];

void mos_cli_print_usage(FILE *f)
{
    fprintf(f,
        "usage: %s [subcommand] [drive] [options]\n"
        "\n"
        "Report the state of a macOS optical drive.\n"
        "\n"
        "Subcommands:\n",
        progname);

    /* Subcommand list, generated from the command table so `--help` and the
       shell completions show the same text from one source (each command's
       own cli/<verb>.c descriptor). The description column sits at 20; a
       name+synopsis wider than 18 wraps its summary to the next line. */
    for (size_t k = 0; k < mos_cli_ncommands; k++) {
        const mos_cli_command *c = mos_cli_commands[k];
        char left[40];
        if (c->synopsis && *c->synopsis)
            snprintf(left, sizeof left, "%s %s", c->name, c->synopsis);
        else
            snprintf(left, sizeof left, "%s", c->name);
        if (strlen(left) <= 18)
            fprintf(f, "  %-18s%s\n", left, c->summary);
        else
            fprintf(f, "  %s\n%20s%s\n", left, "", c->summary);
    }

    fputs(
        "\n"
        "A bare drive selector runs `state` (e.g. `mos 2`, `mos disk4`).\n"
        "\n"
        "Drive (positional): an Index from 'mos list' (all digits), a\n"
        "registry_id from JSON output (all digits, above 2^32), or a BSD\n"
        "form (disk4, rdisk4, /dev/disk4). With one drive attached it may\n"
        "be omitted; with several it is required.\n"
#ifdef MOS_CLI_PROBE
        "(probe is the exception: index or BSD form only, no registry_id.)\n"
#endif
        "\n"
        "Options:\n"
        "  -i, --index N     1-based drive index (the Index column in\n"
        "                    'mos list'); explicit form of the positional\n"
        "      --bsd NAME    BSD form; explicit form of the positional\n"
        "      --force       tray eject: also clear a COLD tray Prevent LOCK in\n"
        "                    the way, then eject. NEVER forces the filesystem —\n"
        "                    a busy disc reports busy. (A plain eject already\n"
        "                    unmounts gracefully, like drutil.)\n"
#ifdef MOS_CLI_PROBE
        "      --dump        With probe: one-shot DR dictionary capture\n"
        "                    (text + XML plists; takes no drive argument)\n"
        "      --capture     With probe <drive>: issue the fixed menu of known\n"
        "                    MMC commands and emit each raw reply as NDJSON\n"
        "                    (mos.capture.v0) for fixture capture\n"
#endif
        "  -j, --json        Emit JSON (mos.state.v1 / mos.error.v1 /\n"
        "                    mos.list.v1). watch is always NDJSON\n"
        "                    (mos.event.v1); --json is a no-op there.\n"
        "  -h, --help        Show this help\n"
        "      --version     Show version\n"
        "\n"
        "Environment (watch only; integer ms, 0..3600000, 0 = library\n"
        "default; out-of-range or non-numeric warn on stderr, use default):\n"
        "  MOS_WATCH_STABLE_MS      poll period in stable states (default 2000)\n"
        "  MOS_WATCH_TRANSITION_MS  poll period in transitional states (default 200)\n"
        "\n"
        "States: open, empty, loading, ready, busy, formatting,\n"
        "        media_unreadable, device_fault, empty_or_open, unknown\n"
        "Exit:   sysexits.h codes — 0 on observed state, 64 usage, 66 no\n"
        "        device, 69 unavailable, 70 internal, 71 OS err, 74 I/O,\n"
        "        75 temp-fail.\n",
        f);
}

static void print_version(void)
{
    printf("%s %s\n", progname, mos_version_string());
}

enum {
    OPT_BSD = 1000,
    OPT_VERSION,
    OPT_FORCE,
#ifdef MOS_CLI_PROBE
    OPT_DUMP,
    OPT_CAPTURE,
#endif
};

static const struct option long_options[] = {
    { "index",   required_argument, 0, 'i' },
    { "bsd",     required_argument, 0, OPT_BSD },
    /* tray-only; the verb match is enforced below. */
    { "force",      no_argument,    0, OPT_FORCE },
#ifdef MOS_CLI_PROBE
    /* Compiled out so an OFF build rejects --dump/--capture as unknown rather
       than half-recognizing them. */
    { "dump",    no_argument,       0, OPT_DUMP },
    { "capture", no_argument,       0, OPT_CAPTURE },
#endif
    /* optional_argument so --json=value reaches the rejection diagnostic
       below rather than being silently discarded; bare --json gives a
       NULL optarg. */
    { "json",    optional_argument, 0, 'j' },
    { "help",    no_argument,       0, 'h' },
    { "version", no_argument,       0, OPT_VERSION },
    { 0, 0, 0, 0 }
};

static int parse_index(const char *arg)
{
    if (!arg || !*arg) return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(arg, &end, 10);
    if (errno != 0 || !end || *end != 0 || v < 1 || v > INT32_MAX) return -1;
    return (int)v;
}

/* True if the argument contains a decimal digit. The bare-selector gate
   (see main's dispatch block): every valid drive selector carries a digit
   — index and registry_id are all-digit, a whole-disk BSD form requires a
   unit digit (mos_internal_bsd_name_is_whole_shape, src/mos_pure.c) —
   while no subcommand name does. So a digit in argv[1] means "positional
   drive, not a verb", decided without scanning the verb table. */
static bool mos_cli_arg_has_digit(const char *s)
{
    for (; s && *s; ++s)
        if (*s >= '0' && *s <= '9') return true;
    return false;
}

/* --json takes no argument — each schema name already carries its version
   (AGENTS.md JSON schema ADR). Reject --json=value with a usage error
   rather than accepting it silently; bare --json (v == NULL) is fine. */
static bool reject_legacy_json_version(const char *v)
{
    if (!v) return true;  /* bare --json */
    fprintf(stderr,
            "%s: --json no longer takes a version argument "
            "(schemas carry their own version: mos.state.v1, "
            "mos.event.v1, etc.); got --json=",
            progname);
    mos_cli_safe_ascii(stderr, v);
    fputc('\n', stderr);
    return false;
}

int main(int argc, char **argv)
{
    if (argc > 0 && argv[0] && *argv[0]) {
        const char *s = strrchr(argv[0], '/');
        progname = s ? s + 1 : argv[0];
    }

    /* CLI-only — a library must not touch global signal disposition.
       Default SIGPIPE kills the process when a downstream pipe closes;
       ignoring it makes writes return EPIPE, which the emitters detect
       (fflush + ferror) and turn into a clean EX_OK. Installed for every
       subcommand, not just watch: even `mos --json | head -c0` can race
       SIGPIPE between printf and flush. The emit paths must keep checking
       fflush returns — dropping them reintroduces the kill-on-close bug. */
    signal(SIGPIPE, SIG_IGN);

    /* Bare `mos` is an entry point, not an implicit state: an intent-free
       invocation must not touch hardware (a state probe would ride the
       not-ready GESN branch, which takes the exclusive lock). Usage + hint,
       EX_USAGE. */
    if (argc == 1) {
        /* %1$s reuses the one progname argument; mixing numbered and
           unnumbered conversions in one format is UB, so keep all
           conversions here numbered. */
        fprintf(stderr,
                "%1$s: no subcommand (state is `%1$s state` or `%1$s <drive>`; "
                "drives, `%1$s list`).\n\n",
                progname);
        mos_cli_print_usage(stderr);
        return EX_USAGE;
    }

    /* A subcommand is recognized only when argv[1] is a bare, digit-free
       word; flag-first invocations fall through to getopt unchanged. A
       digit-bearing argv[1] is a positional drive selector for the default
       `state` verb (mos_cli_arg_has_digit above), so `mos 2`, `mos disk4`,
       `mos /dev/disk4` report state with no verb word; a digit-free
       non-verb still reaches the unknown-subcommand diagnostic. */
    const mos_cli_command *selected = &mos_cli_command_state; /* default verb */
    if (argc >= 2 && argv[1][0] != '-' && argv[1][0] != '\0' &&
        !mos_cli_arg_has_digit(argv[1])) {
        const char *cmd = argv[1];

        selected = NULL;
        for (size_t k = 0; k < mos_cli_ncommands; k++) {
            if (strcmp(cmd, mos_cli_commands[k]->name) == 0) {
                selected = mos_cli_commands[k];
                break;
            }
        }
        if (!selected) {
#ifndef MOS_CLI_PROBE
            /* probe is a real verb, just not compiled into this binary: keep
               the specific diagnostic rather than the unknown-subcommand
               message (the verb exists, this binary just lacks it). */
            if (strcmp(cmd, "probe") == 0) {
                fprintf(stderr, "%s: 'probe' is not built into this binary "
                        "(diagnostic subcommand; rebuild with "
                        "-DMOS_CLI_PROBE=ON)\n", progname);
                return EX_USAGE;
            }
#endif
            fprintf(stderr, "%s: unknown subcommand: ", progname);
            mos_cli_safe_ascii(stderr, cmd);
            /* Recognized list, generated from the same table so it can't drift
               from what actually dispatches. */
            fputs("\nRecognized:", stderr);
            for (size_t k = 0; k < mos_cli_ncommands; k++)
                fprintf(stderr, "%s %s",
                        k ? "," : "", mos_cli_commands[k]->name);
            fputs(".\n", stderr);
            return EX_USAGE;
        }

        /* tray takes an action word (eject/close/lock/unlock) ahead of any
           drive/flags. Capture it here; the second shift below hides it from
           getopt so the shared selector/flag parsing runs unchanged. A
           missing or flag-shaped action stays NULL and is diagnosed by
           mos_cli_run_tray. */
        if ((selected->flags & MOS_CLI_CMD_TRAY_ACTION) &&
            argc >= 3 && argv[2][0] != '-' && argv[2][0] != '\0')
            opt_tray_action = argv[2];

        /* Shift past the subcommand word so getopt starts at position 1.
           Copy the real program name into the new argv[0] first, so
           getopt's own diagnostics keep it (glibc prints argv[0]; Apple's
           warnx/getprogname never reads it). */
        argv[1] = argv[0];
        argc--;
        argv++;

        /* tray's action word now sits at argv[1] (opt_tray_action still
           points to it — only pointers moved). Shift again so getopt sees
           only the modifiers and the selector. */
        if ((selected->flags & MOS_CLI_CMD_TRAY_ACTION) && opt_tray_action) {
            argv[1] = argv[0];
            argc--;
            argv++;
        }
    }

    int c;
    while ((c = getopt_long(argc, argv, "i:jh", long_options, NULL)) != -1) {
        switch (c) {
            case 'i': {
                int v = parse_index(optarg);
                if (v < 0) {
                    fprintf(stderr, "%s: invalid --index: ", progname);
                    mos_cli_safe_ascii(stderr, optarg);
                    fputc('\n', stderr);
                    return EX_USAGE;
                }
                opt_index = v;
                break;
            }
            case OPT_BSD:
                opt_bsd = optarg;
                break;
            case OPT_FORCE:      flag_force = true; break;
#ifdef MOS_CLI_PROBE
            case OPT_DUMP:    flag_dump = true; break;
            case OPT_CAPTURE: flag_capture = true; break;
#endif
            case 'j':
                if (!reject_legacy_json_version(optarg)) return EX_USAGE;
                flag_json = true;
                break;
            case 'h': mos_cli_print_usage(stdout); return EX_OK;
            case OPT_VERSION: print_version(); return EX_OK;
            case '?':
            default:
                mos_cli_print_usage(stderr);
                return EX_USAGE;
        }
    }

    /* Positional drive subject, dispatched by syntax: a non-digit is a bsd
       form (disk4 / rdisk4 / /dev/diskN); all-digits split on the xnu
       registry-ID floor (mos_pure.h) — at/above 2^32+256 is a registry id,
       below is a drutil-style index. Disjoint by kernel construction, no
       fallback chain. */
    if (optind < argc) {
        const char *subject = argv[optind];
        if (optind + 1 < argc) {
            fprintf(stderr, "%s: more than one drive argument: ", progname);
            mos_cli_safe_ascii(stderr, argv[optind + 1]);
            fputc('\n', stderr);
            mos_cli_print_usage(stderr);
            return EX_USAGE;
        }
        if (opt_index || opt_bsd) {
            fprintf(stderr,
                    "%s: drive given both positionally and via %s\n",
                    progname, opt_index ? "--index" : "--bsd");
            return EX_USAGE;
        }
        bool all_digits = (*subject != 0);
        for (const char *p = subject; *p; p++)
            if (*p < '0' || *p > '9') { all_digits = false; break; }
        if (all_digits) {
            errno = 0;
            unsigned long long big = strtoull(subject, NULL, 10);
            if (errno == ERANGE) {
                fprintf(stderr, "%s: drive selector out of range: ",
                        progname);
                mos_cli_safe_ascii(stderr, subject);
                fputc('\n', stderr);
                return EX_USAGE;
            }
            if (mos_internal_value_is_registry_id(big)) {
                opt_registry = (uint64_t)big;
            } else {
                int v = parse_index(subject);
                if (v < 0) {
                    fprintf(stderr, "%s: invalid drive index: ", progname);
                    mos_cli_safe_ascii(stderr, subject);
                    fputc('\n', stderr);
                    return EX_USAGE;
                }
                opt_index = v;
            }
        } else {
            opt_bsd = subject;
        }
        optind++;
    }

    /* A no-drive verb (list) enumerates everything; a drive subject
       contradicts it. */
    if ((selected->flags & MOS_CLI_CMD_NO_DRIVE) &&
        (opt_index > 0 || opt_bsd != NULL || opt_registry)) {
        fprintf(stderr,
                "%s: %s takes no drive argument (it enumerates all)\n",
                progname, selected->name);
        return EX_USAGE;
    }

    if (opt_index && opt_bsd) {
        fprintf(stderr, "%s: --index and --bsd are mutually exclusive\n",
                progname);
        return EX_USAGE;
    }

    /* --force belongs to tray; reject it on any other verb the way --dump is
       rejected outside probe. The finer eject-only match happens in
       mos_cli_run_tray, where the action word is known. */
    if (flag_force && !(selected->flags & MOS_CLI_CMD_TRAY_ACTION)) {
        fprintf(stderr,
                "%s: --force applies only to the tray subcommand\n",
                progname);
        return EX_USAGE;
    }

#ifdef MOS_CLI_PROBE
    /* Verb-vs-verb contradictions can't arise — verbs come only from the
       one-word dispatch. */
    if (flag_dump && !(selected->flags & MOS_CLI_CMD_PROBE)) {
        fprintf(stderr, "%s: --dump requires the probe subcommand\n",
                progname);
        return EX_USAGE;
    }
    if (flag_dump && (opt_index || opt_bsd || opt_registry)) {
        fprintf(stderr, "%s: probe --dump captures every DiscRecording "
                        "device; a drive argument contradicts it\n",
                progname);
        return EX_USAGE;
    }
    if (flag_dump && flag_json) {
        fprintf(stderr, "%s: probe --dump output is plain text + XML "
                        "plists; --json does not apply\n", progname);
        return EX_USAGE;
    }
    if (flag_capture && !(selected->flags & MOS_CLI_CMD_PROBE)) {
        fprintf(stderr, "%s: --capture requires the probe subcommand\n",
                progname);
        return EX_USAGE;
    }
    if (flag_capture && flag_dump) {
        fprintf(stderr, "%s: probe --dump and --capture are different modes; "
                        "pick one\n", progname);
        return EX_USAGE;
    }
    if ((selected->flags & MOS_CLI_CMD_PROBE) && !flag_dump && opt_registry) {
        /* probe resolves by BSD name only (cli/probe.c walks IOMedia up to
           the SCSI peripheral); a registry-id selector has no path there.
           Reject it explicitly rather than fall through to the "requires a
           drive" guard, which would misreport a drive that WAS given. */
        fprintf(stderr, "%s: probe does not accept registry-id selectors; "
                        "use an index (see 'mos list') or a BSD form\n",
                progname);
        return EX_USAGE;
    }
    if ((selected->flags & MOS_CLI_CMD_PROBE) && !flag_dump &&
        !opt_index && !opt_bsd) {
        /* No sole-drive default: the probe targets one explicit drive (or
           --dump for the whole directory). */
        fprintf(stderr, "%s: probe requires a drive (index or BSD form) "
                        "or --dump\n", progname);
        return EX_USAGE;
    }
    /* probe streams NDJSON unconditionally; --json is a no-op, as in watch. */
#endif

    /* Force JSON for NDJSON-streaming verbs (watch), then publish the
       selection so the shared emitters can read its flags (e.g. compact
       error framing). Dispatch through the table — one return, no per-verb
       chain. */
    if (selected->flags & MOS_CLI_CMD_NDJSON) flag_json = true;
    mos_cli_selected = selected;
    return selected->run();
}
