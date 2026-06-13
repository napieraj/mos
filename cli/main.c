/* cli/main.c — argument parsing, usage, dispatch. One file per
 * command over cli/common; adding a verb = a new cli/<verb>.c + a
 * dispatch line below. */
#include "common.h"
#include "mos_pure.h"   /* mos_internal_value_is_registry_id (selector floor) */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

/* Reserved v0.4 subcommand names — single source of truth for the
   dispatch rejection, the usage text, and the unknown-subcommand
   diagnostic, so promoting a name in v0.4 is a one-site edit.
   tests/cli/test_cli.sh (Test 18) loops over the same set. */
static const char *const reserved_subcommands[] = {
    /* "identity" retired 2026-06-12: its surface shipped as the
       metadata + drive verbs (design doc taxonomy); "features" shipped
       the same day as the feature-list verb. */
    "capacity", "tray", "speed", NULL
};

static bool is_reserved_subcommand(const char *cmd)
{
    for (const char *const *r = reserved_subcommands; *r; ++r)
        if (strcmp(cmd, *r) == 0) return true;
    return false;
}

static void print_reserved_subcommands(FILE *f)
{
    for (const char *const *r = reserved_subcommands; *r; ++r)
        fprintf(f, "%s%s", r == reserved_subcommands ? "" : ", ", *r);
}

void mos_cli_print_usage(FILE *f)
{
    fprintf(f,
        "usage: %s [subcommand] [drive] [options]\n"
        "\n"
        "Report the state of a macOS optical drive.\n"
        "\n"
        "Subcommands:\n"
        "  status [drive]    Report drive state (default when only flags are given).\n"
        "  list              List all drives with their states.\n"
        "  watch  [drive]    Stream state events (NDJSON) until SIGINT;\n"
        "                    all drives unless a drive narrows it (hot-plug\n"
        "                    joins; removal is per-drive, stream continues).\n"
        "  metadata [drive]  Disc identity record (profile, TOC, disc\n"
        "                    info, mounted volume) — mos.metadata.v1.\n"
        "  drive [drive]     Drive facts (identity, AACS capabilities)\n"
        "                    — mos.drive.v1.\n"
        "  features [drive]  MMC feature list with current bits (the\n"
        "                    medium-writability surface) — mos.features.v1.\n"
#ifdef MOS_CLI_PROBE
        "  probe  <drive>    Diagnostic: stream raw IOKit/DiscRecording\n"
        "                    notification events (NDJSON, mos.probe.v0)\n"
        "                    until SIGINT; with --dump, a one-shot\n"
        "                    DiscRecording Info/Status plist capture.\n"
#endif
        "Future subcommands (v0.4+ typed APIs, not yet implemented):\n"
        "  ",
        progname);
    print_reserved_subcommands(f);
    fputs(".\n"
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
#ifdef MOS_CLI_PROBE
        "      --dump        With probe: one-shot DR dictionary capture\n"
        "                    (text + XML plists; takes no drive argument)\n"
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

/* ---- JSON output (hand-coded, no library) ----------------------------- */

/* JSON-string and safe-ASCII writing is shared via cli/io
   (mos_cli_json_str / mos_cli_safe_ascii) — see the #include above. */

/* ---- Argument parsing -------------------------------------------------- */

enum {
    OPT_BSD = 1000,
    OPT_VERSION,
#ifdef MOS_CLI_PROBE
    OPT_DUMP,
#endif
};

static const struct option long_options[] = {
    { "index",   required_argument, 0, 'i' },
    { "bsd",     required_argument, 0, OPT_BSD },
#ifdef MOS_CLI_PROBE
    /* Compiled out with the probe so an OFF build rejects --dump as an
       unknown option (usage + 64) instead of half-recognizing it. */
    { "dump",    no_argument,       0, OPT_DUMP },
#endif
    /* optional_argument, not no_argument: lets --json=v2 reach the
       legacy-rejection diagnostic (no_argument would silently discard the
       =value). Bare --json works — optarg defaults to NULL. */
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

/* --json takes no argument — schema names carry their own version
   (AGENTS.md, JSON schema ADR). Reject --json=value so a caller
   expecting per-invocation pinning gets a clear usage error instead of
   silent acceptance; bare --json (v == NULL) is fine. */
static bool reject_legacy_json_version(const char *v)
{
    if (!v) return true;  /* bare --json is fine */
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

    /* CLI-only (never in libmos — a library must not change global signal
       disposition). Default SIGPIPE kills the process on a closed
       downstream pipe; ignoring it makes writes return EPIPE instead, which
       the watch emitters detect (fflush + ferror) and turn into a clean
       EX_OK exit. Installed for all subcommands, not just watch: even
       `mos --json | head -c0` can race SIGPIPE between printf and flush.
       Invariant: the emit paths must keep honoring fflush return values —
       discarding them reintroduces the kill-on-pipe-close bug. signal()
       here cannot fail (SIG_IGN is always installable). */
    signal(SIGPIPE, SIG_IGN);

    /* Bare `mos` is an entry point, not an implicit status (the
       single-drive default was a carryover from the one-word-stdout
       era; retired 2026-06-12) — and not a probe either: the drive
       table's state column rides the not-ready GESN branch, which
       takes the exclusive lock, and an intent-free invocation must
       not touch hardware (same-day revision of the table-at-entry
       shape). Usage + hint, EX_USAGE; the table is one deliberate
       `mos list` away. */
    if (argc == 1) {
        /* %1$s: POSIX numbered conversions — one progname argument,
           reused; mixing numbered and unnumbered in one format is UB,
           so every conversion here must stay numbered. */
        fprintf(stderr,
                "%1$s: no subcommand (state is `%1$s status`; drives, `%1$s list`).\n\n",
                progname);
        mos_cli_print_usage(stderr);
        return EX_USAGE;
    }

    /* Subcommands are additive aliases for the flag forms, recognized only
       when argv[1] is a bare word (flag-first invocations reach getopt
       unchanged). The remaining v0.4 names are reserved so a premature use gets
       a clearer diagnostic than "unknown subcommand". */
    if (argc >= 2 && argv[1][0] != '-' && argv[1][0] != '\0') {
        const char *cmd = argv[1];

        if (strcmp(cmd, "status") == 0) {
            /* implicit-status default; nothing to set. */
        } else if (strcmp(cmd, "list") == 0) {
            flag_list = true;
        } else if (strcmp(cmd, "watch") == 0) {
            flag_watch = true;
        } else if (strcmp(cmd, "metadata") == 0) {
            flag_metadata = true;
        } else if (strcmp(cmd, "drive") == 0) {
            flag_drive = true;
        } else if (strcmp(cmd, "features") == 0) {
            flag_features = true;
        } else if (strcmp(cmd, "probe") == 0) {
#ifdef MOS_CLI_PROBE
            flag_probe = true;
#else
            /* Known verb, compiled out — a specific diagnostic, not the
               unknown-subcommand or reserved-name message (both would
               mislead: the verb exists and is not a future typed API). */
            fprintf(stderr, "%s: 'probe' is not built into this binary "
                    "(diagnostic subcommand; rebuild with "
                    "-DMOS_CLI_PROBE=ON)\n", progname);
            return EX_USAGE;
#endif
        } else if (is_reserved_subcommand(cmd)) {
            /* cmd is provably one of the five literals here, so escaping is
               unnecessary — done anyway so a future edit that broadens the
               cmd source can't silently regress the escape-all-user-bytes
               style. */
            fprintf(stderr, "%s: subcommand '", progname);
            mos_cli_safe_ascii(stderr, cmd);
            fputs("' is reserved for the v0.4 typed APIs "
                  "and is not yet implemented.\n"
                  "See ROADMAP.md for status; the underlying MMC operations "
                  "are available today via mos_raw_cdb() in the C library.\n",
                  stderr);
            return EX_USAGE;
        } else {
            fprintf(stderr, "%s: unknown subcommand: ", progname);
            mos_cli_safe_ascii(stderr, cmd);
            fputs("\nRecognized: status, list, watch, metadata, drive, features"
#ifdef MOS_CLI_PROBE
                  ", probe"
#endif
                  ".\nReserved (v0.4): ", stderr);
            print_reserved_subcommands(stderr);
            fputs(".\n", stderr);
            return EX_USAGE;
        }
        /* Shift past the subcommand word so getopt parses the
           remaining args from position 1. Copy the real program name
           into the slot getopt will see as argv[0] first: getopt's
           own diagnostics keep the real progname on every libc
           (glibc prints argv[0]; Apple's warnx/getprogname never
           reads it). The subcommand word was already consumed by the
           dispatch above — nothing references it past this point. */
        argv[1] = argv[0];
        argc--;
        argv++;
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
#ifdef MOS_CLI_PROBE
            case OPT_DUMP: flag_dump = true; break;
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

    /* Positional drive subject: one bare argument, SYNTACTIC dispatch —
       non-digit = a bsd form (disk4 / rdisk4 / /dev/diskN); all digits
       split on the xnu registry-ID floor (mos_pure.h): at/above
       2^32+256 = registry id, below = drutil-style index. Disjoint by
       kernel construction, so no fallback chain. */
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

    /* list enumerates everything — a positional subject alongside it is
       the same contradiction as the flag forms below. */
    if (flag_list && (opt_index > 0 || opt_bsd != NULL || opt_registry)) {
        fprintf(stderr,
                "%s: list takes no drive argument (it enumerates all)\n",
                progname);
        return EX_USAGE;
    }

    if (opt_index && opt_bsd) {
        fprintf(stderr, "%s: --index and --bsd are mutually exclusive\n",
                progname);
        return EX_USAGE;
    }

    /* (list + selector is rejected above, where the positional
       subject also lands — one guard, one message.) */

#ifdef MOS_CLI_PROBE
    /* (Verb-vs-verb contradictions are unrepresentable since verbs come
       only from the one-word dispatch — flags-as-commands retired
       2026-06-12.) */
    if (flag_dump && !flag_probe) {
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
    if (flag_probe && !flag_dump && opt_registry) {
        /* probe resolves its io_service_t by BSD name (cli/probe.c walks
           the IOMedia up to the SCSI peripheral); a registry-id selector
           has no resolution path there. The other selector-taking
           subcommands accept registry ids via mos_open_by_registry_id —
           probe is index/BSD only by design. Reject it with an accurate
           message rather than falling through to the "requires a drive"
           guard below, which would misreport a drive that *was* given. */
        fprintf(stderr, "%s: probe does not accept registry-id selectors; "
                        "use an index (see 'mos list') or a BSD form\n",
                progname);
        return EX_USAGE;
    }
    if (flag_probe && !flag_dump && !opt_index && !opt_bsd) {
        /* No sole-drive default here: the probe is a diagnostic aimed
           at one explicit drive (or --dump for the whole directory). */
        fprintf(stderr, "%s: probe requires a drive (index or BSD form) "
                        "or --dump\n", progname);
        return EX_USAGE;
    }
    /* probe's event stream is NDJSON unconditionally; --json is a
       no-op there, same documented rule as watch. */
#endif

#ifdef MOS_CLI_PROBE
    if (flag_probe) return mos_cli_run_probe();
#endif
    if (flag_list)  return mos_cli_run_list();
    if (flag_metadata) return mos_cli_run_metadata();
    if (flag_drive) return mos_cli_run_drive();
    if (flag_features) return mos_cli_run_features();
    if (flag_watch) return mos_cli_run_watch();
    return mos_cli_run_query();
}
