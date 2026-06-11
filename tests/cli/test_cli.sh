#!/bin/sh
# tests/cli/test_cli.sh — JSON and exit-code contract tests for the mos CLI.
#
# Pins the user-facing surface of `mos` against accidental drift. The
# pure-data tests in tests/test_*.c cover the library's decision logic;
# this script covers what consumers actually parse: stdout shape, exit
# codes, and the JSON envelope (mos.state.v1, mos.error.v1, mos.event.v1,
# mos.list.v1).
#
# Tests are limited to scenarios reachable without a real optical drive
# attached (CI has none). The success-path JSON contract for state=ready
# etc. is exercised by the integration harness on real hardware, not here.
#
# Usage: test_cli.sh <path-to-mos-binary>
#   or:  MOS_BIN=<path> test_cli.sh
#
# Returns 0 on all-pass, 1 on any failure. Output is human-readable.

set -u

MOS_BIN="${1:-${MOS_BIN:-./build/bin/mos}}"

if [ ! -x "$MOS_BIN" ]; then
    printf 'fail: mos binary not found or not executable at %s\n' "$MOS_BIN" >&2
    exit 1
fi

pass=0
fail=0

run_mos() {
    OUT=$("$MOS_BIN" "$@" 2>/tmp/mos_cli_stderr)
    EC=$?
    ERR=$(cat /tmp/mos_cli_stderr)
}

assert_ec() {
    label="$1"; expected="$2"; actual="$3"
    if [ "$actual" = "$expected" ]; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$label"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s\n          expected exit %s, got %s\n' \
               "$label" "$expected" "$actual" >&2
        [ -n "${OUT:-}" ] && printf '          stdout: %s\n' "$OUT" >&2
        [ -n "${ERR:-}" ] && printf '          stderr: %s\n' "$ERR" >&2
    fi
}

# NDJSON framing: the captured stdout (trailing newline already stripped
# by $(...) ) must contain no embedded newline — exactly one object, one
# line. Watch-mode JSON errors are held to this since the third review.
assert_single_line() {
    label="$1"; haystack="$2"
    nl_count=$(printf '%s' "$haystack" | wc -l | tr -d ' ')
    if [ "$nl_count" = "0" ] && [ -n "$haystack" ]; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$label"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s\n          expected single-line stdout, got %s embedded newline(s)\n          stdout: %s\n' \
               "$label" "$nl_count" "$haystack" >&2
    fi
}

assert_contains() {
    label="$1"; haystack="$2"; needle="$3"
    case "$haystack" in
        *"$needle"*)
            pass=$((pass + 1))
            printf '  ok    %s\n' "$label"
            ;;
        *)
            fail=$((fail + 1))
            printf '  FAIL  %s\n          expected to contain: %s\n          got: %s\n' \
                   "$label" "$needle" "$haystack" >&2
            ;;
    esac
}

assert_not_contains() {
    label="$1"; haystack="$2"; needle="$3"
    case "$haystack" in
        *"$needle"*)
            fail=$((fail + 1))
            printf '  FAIL  %s\n          must NOT contain: %s\n          got: %s\n' \
                   "$label" "$needle" "$haystack" >&2
            ;;
        *)
            pass=$((pass + 1))
            printf '  ok    %s\n' "$label"
            ;;
    esac
}

assert_equals() {
    label="$1"; expected="$2"; actual="$3"
    if [ "$actual" = "$expected" ]; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$label"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s\n          expected: %s\n          got:      %s\n' \
               "$label" "$expected" "$actual" >&2
    fi
}

# ---------------------------------------------------------------------------

printf 'CLI contract tests against %s\n' "$MOS_BIN"

# Test 1: --version exits 0 and prints a recognizable version string.
run_mos --version
assert_ec       "version exit code"      "0" "$EC"
assert_contains "version output shape"   "$OUT" "0."

# Test 2: --help exits 0 and prints the Options block, including --watch
# (new in v0.3).
run_mos --help
assert_ec       "help exit code"          "0" "$EC"
assert_contains "help mentions Options"   "$OUT" "Options:"
assert_contains "help advertises --watch" "$OUT" "watch"

# Test 3: unknown flag exits 64 (EX_USAGE per sysexits.h). Pins that
# getopt-level errors are distinguishable from query failures (which
# now use other sysexits classes like 66 EX_NOINPUT).
run_mos --bogus-flag-that-does-not-exist
assert_ec "invalid flag exits 64 (EX_USAGE)" "64" "$EC"

# Test 4: --index pointing at a non-existent drive in plain mode emits
# NOTHING on stdout, a diagnostic on stderr, and exits 66 (EX_NOINPUT).
# Index 99 is well past any plausible drive count so the no-drive
# failure is guaranteed regardless of host drive layout.
run_mos --index 99
assert_ec     "no-drive plain exit 66 (EX_NOINPUT)" "66"  "$EC"
assert_equals "no-drive plain stdout empty"         ""    "$OUT"
ERR=$(cat /tmp/mos_cli_stderr 2>/dev/null || echo "")
case "$ERR" in
    *"could not open drive"*)
        pass=$((pass + 1))
        printf '  ok    no-drive plain stderr has diagnostic\n'
        ;;
    *)
        fail=$((fail + 1))
        printf '  FAIL  no-drive plain stderr missing diagnostic\n          got: %s\n' \
               "$ERR" >&2
        ;;
esac

# Test 5: --json --index 99 emits a mos.error.v1 envelope. The v0.3
# failure shape is:
#   {
#     "schema": "mos.error.v1",
#     "exit_code": 66,
#     "error": {
#       "code":        "no_device",
#       "message":     "...",
#       "context":     "...",
#       "recoverable": false
#     }
#   }
# The schema field discriminates against the success envelope
# (mos.state.v1); no shared required fields beyond schema.
run_mos --json --index 99
assert_ec       "no-drive JSON exit 66"              "66"   "$EC"
assert_contains "JSON has mos.error.v1 schema"       "$OUT" '"schema": "mos.error.v1"'
assert_contains "JSON has exit_code: 66"             "$OUT" '"exit_code": 66'
assert_contains "JSON has nested error.code"         "$OUT" '"code": "no_device"'
assert_contains "JSON has error.message"             "$OUT" '"message":'
assert_contains "JSON has error.context"             "$OUT" '"context":'
assert_contains "JSON has error.recoverable: false"  "$OUT" '"recoverable": false'

# Negative: the legacy flat error / schema_version shapes must not leak.
assert_not_contains "no legacy schema_version field" "$OUT" '"schema_version"'
assert_not_contains "no flat error_message field"    "$OUT" '"error_message"'
# Failure envelope must NOT include "state" — disjointness rule.
assert_not_contains "failure envelope has no state"  "$OUT" '"state"'
# Field name is bsd_name, not bare "bsd".
assert_not_contains "envelope: no bare bsd key"      "$OUT" '"bsd":'

# Test 6: --index and --bsd are mutually exclusive (EX_USAGE 64).
run_mos --index 1 --bsd disk0
assert_ec "index+bsd mutual exclusion exits 64" "64" "$EC"

# Test 7: --list and --watch are mutually exclusive (new in v0.3).
run_mos --list --watch
assert_ec "list+watch mutual exclusion exits 64" "64" "$EC"

# Test 8: --json=value is rejected with EX_USAGE 64. In v0.3 the --json
# flag takes no argument — schemas carry their own version. Legacy
# callers passing --json=v2 or --json=v3 must get a clear diagnostic.
run_mos --json=v2 --index 99
assert_ec "--json=v2 rejected with EX_USAGE" "64" "$EC"
run_mos --json=anything --index 99
assert_ec "--json=anything rejected with EX_USAGE" "64" "$EC"
ERR=$(cat /tmp/mos_cli_stderr 2>/dev/null || echo "")
assert_contains "--json=v2 diagnostic mentions mos.state.v1" "$ERR" "mos.state.v1"

# Test 9: --list with --json emits a mos.list.v1 envelope. The drives
# array may be empty on a CI runner without optical drives.
run_mos --list --json
assert_ec       "list --json exit 0"           "0"    "$EC"
assert_contains "list JSON has mos.list.v1"    "$OUT" '"schema": "mos.list.v1"'
assert_contains "list JSON has drives array"   "$OUT" '"drives":'

# Test 10: --bsd with a non-resolving name exits 66 with mos.error.v1
# envelope. Distinct from --index path because the open logic differs.
# The name must be WELL-FORMED (disk<N>): mos_open_by_bsd_name rejects
# malformed names with invalid_arg/64 before the no_device/66 lookup
# (mos_internal_parse_bsd_unit returns -1, see src/mos_scsi.c).
run_mos --bsd disk99 --json
assert_ec       "bad --bsd JSON exit 66"          "66"   "$EC"
assert_contains "bad --bsd JSON has mos.error.v1" "$OUT" '"schema": "mos.error.v1"'

# Test 11: --watch against a non-resolving --bsd exits 66 with a
# mos.error.v1 envelope. Exercises the watch-open-failure path
# (mos_watch_open_by_bsd_name returns NULL, run_watch routes through
# emit_unknown_and_fail). This is the only --watch path testable
# without IOKit; the full event-stream behavior is exercised by the
# pure watch_core unit tests and the hardware integration matrix.
run_mos --watch --bsd disk99 --json
assert_ec          "bad --bsd --watch exit 66"           "66"   "$EC"
# Watch-mode JSON is NDJSON: the error envelope is COMPACT (no spaces
# after colons) and single-line, unlike the pretty one-shot envelope.
assert_contains    "bad --bsd --watch has mos.error.v1"  "$OUT" '"schema":"mos.error.v1"'
assert_contains    "bad --bsd --watch has no_device"     "$OUT" '"code":"no_device"'
assert_single_line "bad --bsd --watch envelope is one NDJSON line" "$OUT"

# Test 12: --watch against a non-existent index exits 66 with the same
# envelope (separate open path).
run_mos --watch --index 99 --json
assert_ec          "bad --index --watch exit 66"          "66"   "$EC"
assert_contains    "bad --index --watch has mos.error.v1" "$OUT" '"schema":"mos.error.v1"'
assert_single_line "bad --index --watch envelope is one NDJSON line" "$OUT"

# Test 13: subcommand 'status' is an alias for the implicit-status form.
# On a CI runner with no drive, both forms produce the same v0.3
# no-device error envelope.
run_mos status --json --index 99
assert_ec       "status subcommand exit 66"          "66"   "$EC"
assert_contains "status subcommand has mos.error.v1" "$OUT" '"schema": "mos.error.v1"'

# Test 14: subcommand 'list' is an alias for --list. The drives array
# may be empty on a CI runner.
run_mos list --json
assert_ec       "list subcommand exit 0"          "0"    "$EC"
assert_contains "list subcommand has mos.list.v1" "$OUT" '"schema": "mos.list.v1"'

# Test 15: subcommand 'watch' is an alias for --watch; same
# watch-open-failure surface as Test 11.
run_mos watch --bsd disk99 --json
assert_ec          "watch subcommand bad --bsd exit 66"           "66" "$EC"
assert_contains    "watch subcommand bad --bsd has mos.error.v1"  "$OUT" '"schema":"mos.error.v1"'
assert_single_line "watch subcommand envelope is one NDJSON line" "$OUT"

# Test 16: 'status' subcommand combined with --list or --watch is
# rejected as contradictory (EX_USAGE 64). Each form separately is
# valid; combining them is not.
run_mos status --list
assert_ec "status + --list rejected with EX_USAGE" "64" "$EC"
run_mos status --watch
assert_ec "status + --watch rejected with EX_USAGE" "64" "$EC"

# Test 17: unknown subcommand exits 64 with a diagnostic listing
# recognized and reserved names.
run_mos garbage
assert_ec "unknown subcommand exits 64" "64" "$EC"
ERR=$(cat /tmp/mos_cli_stderr 2>/dev/null || echo "")
assert_contains "unknown subcommand diagnostic names recognized set" "$ERR" "status, list, watch"

# Test 18: reserved-for-v0.4 subcommand names exit 64 with an
# informative diagnostic rather than the generic "unknown" message.
# This avoids the worst UX of users trying `mos capacity` once,
# getting a vague error, and never trying again when the typed APIs
# arrive.
for reserved in capacity identity tray speed features; do
    run_mos "$reserved" --bsd disk0
    assert_ec "reserved subcommand '$reserved' exits 64" "64" "$EC"
    ERR=$(cat /tmp/mos_cli_stderr 2>/dev/null || echo "")
    assert_contains "reserved '$reserved' diagnostic names v0.4" "$ERR" "v0.4"
done

# Test 19: --list is mutually exclusive with --index/--bsd. List is
# "enumerate ALL drives"; selectors target a single drive. Combining
# them is contradictory and must be rejected at the argument-parsing
# layer with EX_USAGE rather than silently picking one to honor.
#
# Four cases: --list and `list` subcommand, each crossed with --index
# and --bsd. All four must exit 64. (R3 v0.3.1-dev review flagged
# that the contract test was missing for these — the rejection logic
# was in the code, but only the --list+--watch combination was
# pinned.)
run_mos --list --index 1
assert_ec "list + --index rejected with EX_USAGE" "64" "$EC"
run_mos --list --bsd disk4
assert_ec "list + --bsd rejected with EX_USAGE"   "64" "$EC"
run_mos list --index 1
assert_ec "list subcommand + --index rejected with EX_USAGE" "64" "$EC"
run_mos list --bsd disk4
assert_ec "list subcommand + --bsd rejected with EX_USAGE"   "64" "$EC"

# Test 20: positional drive subject (CLI design 2026-06-10). All-digits
# parses as an index: `mos status 99` matches the --index 99 contract.
run_mos status 99
assert_ec     "positional index: exit 66"     "66" "$EC"
assert_equals "positional index: stdout empty" ""  "$OUT"

# Test 21: positional + explicit selector is a contradiction (64).
run_mos status 2 --index 1
assert_ec "positional+--index conflict exits 64" "64" "$EC"

# Test 22: two positional drive arguments are rejected (64).
run_mos status 1 2
assert_ec "two positionals exit 64" "64" "$EC"

# Test 23: non-digit positional routes as a bsd form; a non-resolving
# name exits 66 through the open path (same as --bsd).
run_mos status disk99 --json
assert_ec       "positional bsd: exit 66"          "66"   "$EC"
assert_contains "positional bsd: mos.error.v1"     "$OUT" '"schema": "mos.error.v1"'

# Test 24: --all is watch-only and selector-exclusive (DR pivot 2b).
run_mos --all
assert_ec "--all without watch exits 64" "64" "$EC"
run_mos --watch --all --index 2
assert_ec "--all + selector exits 64" "64" "$EC"
run_mos status --all
assert_ec "status --all exits 64" "64" "$EC"
run_mos watch 2 --all
assert_ec "--all + positional selector exits 64" "64" "$EC"

# Test 25: watch is NDJSON end to end — the error envelope is emitted
# on stdout in compact single-line form even WITHOUT --json.
run_mos watch 99
assert_ec          "watch sans --json: exit 66"          "66"   "$EC"
assert_contains    "watch sans --json: envelope present" "$OUT" '"schema":"mos.error.v1"'
assert_single_line "watch sans --json: one NDJSON line"  "$OUT"

# Test 26: probe subcommand (MOS_CLI_PROBE consolidation, 2026-06-11).
# Bare `mos probe` exits 64 in BOTH build states: usage error ("probe
# requires a drive ... or --dump") when compiled in, the not-built
# diagnostic when compiled out. The rest of the block exercises the
# compiled-in surface only, so an OFF binary (its own contract is
# pinned by CI's build-noprobe leg) skips it by feature detection.
run_mos probe
assert_ec "probe with no selector exits 64" "64" "$EC"
case "$ERR" in
*"not built into this binary"*)
    printf '  ok    probe block skipped (MOS_CLI_PROBE=OFF binary)\n'
    ;;
*)
    # Test 27: --dump validation matrix — every contradiction is 64.
    run_mos probe --dump extra
    assert_ec "probe --dump + positional exits 64" "64" "$EC"
    run_mos probe --dump --bsd disk4
    assert_ec "probe --dump + --bsd exits 64" "64" "$EC"
    run_mos probe --dump --json
    assert_ec "probe --dump + --json exits 64" "64" "$EC"
    run_mos --dump
    assert_ec "--dump without probe exits 64" "64" "$EC"

    # Test 28: probe contradicts the other dispatches (64 each).
    run_mos probe --list
    assert_ec "probe + --list exits 64" "64" "$EC"
    run_mos probe --watch
    assert_ec "probe + --watch exits 64" "64" "$EC"
    run_mos probe --all
    assert_ec "probe + --all exits 64" "64" "$EC"

    # Test 29: stream-mode selector errors. A malformed BSD form is a
    # usage error (64, pure parse); a well-formed but absent drive
    # exits 66 through service resolution / index lookup.
    run_mos probe notadisk
    assert_ec "probe malformed bsd exits 64" "64" "$EC"
    run_mos probe disk99
    assert_ec "probe absent bsd exits 66" "66" "$EC"
    run_mos probe 99
    assert_ec "probe absent index exits 66" "66" "$EC"

    # Test 30: probe --dump is runnable without hardware: the runner
    # has no burner, so the DR device array is empty — banner, count
    # line, exit 0. (If a runner's DRCopyDeviceArray ever returns NULL
    # instead of an empty array, the observed contract is 69 with a
    # "(NULL array)" marker — re-pin on that evidence, not in advance.)
    run_mos probe --dump
    assert_ec       "probe --dump exits 0"        "0"    "$EC"
    assert_contains "probe --dump banner"         "$OUT" "mos probe --dump"
    assert_contains "probe --dump empty directory" "$OUT" "0 device(s)"
    ;;
esac

printf '\n%d passed, %d failed\n' "$pass" "$fail"
rm -f /tmp/mos_cli_stderr
[ "$fail" -eq 0 ] || exit 1
exit 0
