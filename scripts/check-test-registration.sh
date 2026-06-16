#!/bin/sh
# check-test-registration.sh — every TEST() fixture must be invoked by a
# RUN() in the same file's register_*_tests(). Catches the operator slip of
# adding a TEST() that then silently never runs (a green suite that proves
# nothing about the new fixture).
#
# The check is per-file (a fixture in test_sense.c is registered from
# register_sense_tests() in the same file — no cross-file lookup) and globs
# tests/test_*.c, so a new test file is covered with no hand-list here.
# test_main.c is exempt: it only dispatches to register functions.
#
# Single source of truth for this gate: CI (.github/workflows/ci.yml) and
# the local pre-push preflight (scripts/preflight.sh) both call this.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FAIL=0

for f in "$ROOT"/tests/test_*.c; do
    [ "$(basename "$f")" = "test_main.c" ] && continue
    DEFINED=$(grep -E '^TEST\(' "$f" | sed -E 's/^TEST\(([^)]+)\).*/\1/') || true
    [ -z "$DEFINED" ] && continue
    for name in $DEFINED; do
        # RUN(name); lives inside register_*_tests() in the same file.
        if ! grep -qE "[[:space:]]+RUN\($name\);" "$f"; then
            echo "Orphaned test in ${f#"$ROOT"/}: TEST($name) defined but not registered with RUN()"
            FAIL=1
        fi
    done
done

if [ "$FAIL" -eq 1 ]; then
    echo ""
    echo "Each TEST() fixture must be invoked by a RUN() call in the same"
    echo "file's register_*_tests() function. See"
    echo "tests/test_sense.c::register_sense_tests for the pattern."
    exit 1
fi
echo "OK: all TEST() fixtures are registered"
