/* cli/main.c — argument parsing, usage, dispatch. One file per
 * command over cli/common; adding a verb = a new cli/<verb>.c + a
 * dispatch line below. */
#include "common.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

void print_usage(FILE *f)
{
    fprintf(f,
        "usage: %s [subcommand] [drive] [options]\n"
        "\n"
        "Report the state of a macOS optical drive.\n"
        "\n"
        "Subcommands:\n"
        "  status [drive]    Report drive state (default if no subcommand).\n"
        "  list              List all drives with their states.\n"
        "  watch  [drive]    Stream state events (NDJSON) until SIGINT.\n"
        "Future subcommands (v0.4+ typed APIs, not yet implemented):\n"
        "  capacity, identity, tray, speed, features.\n"
        "\n"
        "Drive (positional): an Index from 'mos list' (all digits), or a\n"
        "BSD form (disk4, rdisk4, /dev/disk4). With one drive attached it\n"
        "may be omitted; with several it is required.\n"
        "\n"
        "Options:\n"
        "  -i, --index N     1-based drive index (the Index column in\n"
        "                    'mos list'); explicit form of the positional\n"
        "      --bsd NAME    BSD form; explicit form of the positional\n"
        "  -l, --list        List drives and exit\n"
        "  -w, --watch       Stream state events (NDJSON) until SIGINT\n"
        "  -j, --json        Emit JSON (mos.state.v1 / mos.error.v1 /\n"
        "                    mos.list.v1). watch is always NDJSON\n"
        "                    (mos.event.v1); --json is a no-op there.\n"
        "  -h, --help        Show this help\n"
        "      --version     Show version\n"
        "\n"
        "Environment (--watch only; integer ms, 0..3600000, 0 = library\n"
        "default; out-of-range or non-numeric warn on stderr, use default):\n"
        "  MOS_WATCH_STABLE_MS      poll period in stable states (default 2000)\n"
        "  MOS_WATCH_TRANSITION_MS  poll period in transitional states (default 200)\n"
        "\n"
        "States: open, empty, loading, ready, busy, formatting,\n"
        "        media_unreadable, device_fault, empty_or_open, unknown\n"
        "Exit:   sysexits.h codes — 0 on observed state, 64 usage, 66 no\n"
        "        device, 69 unavailable, 71 OS err, 74 I/O, 75 temp-fail.\n",
        progname);
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
};

static const struct option long_options[] = {
    { "index",   required_argument, 0, 'i' },
    { "bsd",     required_argument, 0, OPT_BSD },
    { "list",    no_argument,       0, 'l' },
    { "watch",   no_argument,       0, 'w' },
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

/* --json takes no argument in v0.3 (schema names carry their own version).
   Reject --json=value so legacy --json=v2 callers get a clear usage error
   instead of silent acceptance; bare --json (v == NULL) is fine. */
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
       EX_OK exit. Installed for all subcommands, not just --watch: even
       `mos --json | head -c0` can race SIGPIPE between printf and flush.
       Invariant: the emit paths must keep honoring fflush return values —
       discarding them reintroduces the kill-on-pipe-close bug. signal()
       here cannot fail (SIG_IGN is always installable). */
    signal(SIGPIPE, SIG_IGN);

    /* Subcommands are additive aliases for the flag forms, recognized only
       when argv[1] is a bare word (flag-first invocations reach getopt
       unchanged). The five v0.4 names are reserved so a premature use gets
       a clearer diagnostic than "unknown subcommand". */
    bool had_status_subcommand = false;
    if (argc >= 2 && argv[1][0] != '-' && argv[1][0] != '\0') {
        const char *cmd = argv[1];

        if (strcmp(cmd, "status") == 0) {
            /* implicit-status default; no flag to set, but track the
               explicit subcommand so we can reject `mos status --list`
               and `mos status --watch` as contradictory. */
            had_status_subcommand = true;
        } else if (strcmp(cmd, "list") == 0) {
            flag_list = true;
        } else if (strcmp(cmd, "watch") == 0) {
            flag_watch = true;
        } else if (strcmp(cmd, "capacity")  == 0 ||
                   strcmp(cmd, "identity")  == 0 ||
                   strcmp(cmd, "tray")      == 0 ||
                   strcmp(cmd, "speed")     == 0 ||
                   strcmp(cmd, "features")  == 0) {
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
            fputs("\nRecognized: status, list, watch.\n"
                  "Reserved (v0.4): capacity, identity, tray, speed, "
                  "features.\n", stderr);
            return EX_USAGE;
        }
        /* Shift past the subcommand word so getopt parses the
           remaining args from position 1. argv[0] now points at the
           subcommand word, which getopt skips like a progname. */
        argc--;
        argv++;
    }

    int c;
    while ((c = getopt_long(argc, argv, "i:ljwh", long_options, NULL)) != -1) {
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
            case 'l': flag_list  = true;  break;
            case 'w': flag_watch = true;  break;
            case 'j':
                if (!reject_legacy_json_version(optarg)) return EX_USAGE;
                flag_json = true;
                break;
            case 'h': print_usage(stdout); return EX_OK;
            case OPT_VERSION: print_version(); return EX_OK;
            case '?':
            default:
                print_usage(stderr);
                return EX_USAGE;
        }
    }

    /* Positional drive subject (CLI design 2026-06-10): one bare
       argument; SYNTACTIC disambiguation — all digits = index, anything
       else = a bsd form (the library parse accepts disk4 / rdisk4 /
       /dev/diskN). --registry-id (future) stays flag-only: its large
       decimals would collide with the index grammar. */
    if (optind < argc) {
        const char *subject = argv[optind];
        if (optind + 1 < argc) {
            fprintf(stderr, "%s: more than one drive argument: ", progname);
            mos_cli_safe_ascii(stderr, argv[optind + 1]);
            fputc('\n', stderr);
            print_usage(stderr);
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
            int v = parse_index(subject);
            if (v < 0) {
                fprintf(stderr, "%s: invalid drive index: ", progname);
                mos_cli_safe_ascii(stderr, subject);
                fputc('\n', stderr);
                return EX_USAGE;
            }
            opt_index = v;
        } else {
            opt_bsd = subject;
        }
        optind++;
    }

    /* list enumerates everything — a positional subject alongside it is
       the same contradiction as the flag forms below. */
    if (flag_list && (opt_index > 0 || opt_bsd != NULL)) {
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

    if (flag_list && flag_watch) {
        fprintf(stderr, "%s: --list and --watch are mutually exclusive\n",
                progname);
        return EX_USAGE;
    }

    /* (--list + selector is rejected above, where the positional
       subject also lands — one guard, one message.) */

    if (had_status_subcommand && (flag_list || flag_watch)) {
        fprintf(stderr,
                "%s: 'status' subcommand cannot be combined with %s\n",
                progname, flag_list ? "--list" : "--watch");
        return EX_USAGE;
    }

    if (flag_list)  return run_list();
    if (flag_watch) return run_watch();
    return run_query();
}
