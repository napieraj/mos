#!/bin/sh
# preflight.sh — run the OS-independent CI gates locally, before a push, so
# operator slips are caught here instead of by CI after a round-trip:
#
#   - dist/ amalgamation stale relative to src/   (the most common slip)
#   - a TEST() fixture added but never RUN()-registered
#   - documentation drift (known-stale markers)
#   - README examples out of sync with schemas / cli emit_human
#   - the pure unit suite failing
#
# These all run on any platform (text/parse/pure-C). The macOS-only gates —
# the CLI compile, the strict-adapter -Werror leg, the amalgamated macOS
# test — cannot run here and stay CI's job. CI remains the authoritative
# backstop; this is the fast local mirror.
#
# Used by the pre-push hook (.githooks/pre-push, enable with `make hooks`)
# and runnable by hand (`make preflight` or scripts/preflight.sh). Exit 0
# when every gate passes, 1 otherwise.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

fail=0
step() { printf '\n=== %s ===\n' "$1"; }
# Run a gate without tripping an early exit; record failure, keep going so
# one push attempt surfaces every problem, not just the first.
run()  { if "$@"; then :; else fail=1; echo "FAILED: $*"; fi; }

step "dist/ amalgamation in sync with src/"
run ./scripts/amalgamate.sh --check

step "test registration (TEST() <-> RUN())"
run ./scripts/check-test-registration.sh

step "documentation staleness"
run sh ./scripts/doc-staleness.sh

step "CONTRIBUTING.md file tree in sync with the repo"
run python3 ./scripts/check-file-tree.py

step "CLI docs (completions + man) in sync with the command table"
run python3 ./scripts/gen-cli-docs.py --check

step "README <-> schema/code contract"
# check_readme.py self-skips (prints a note, exits 0) when jsonschema is
# absent, so this is best-effort locally and authoritative in CI.
run python3 ./schemas/check_readme.py

# The script + YAML linters CI runs (the lint-scripts job). Mirror them
# locally when installed; skip-with-note otherwise (CI is the backstop,
# same convention as the cmake check below). [Comment kept off the leading
# "shellcheck" word — that prefix is parsed as a shellcheck directive.]
step "shell scripts (shellcheck)"
if command -v shellcheck >/dev/null 2>&1; then
    run shellcheck scripts/*.sh .githooks/pre-push
else
    echo "SKIP: shellcheck not installed — CI's lint-scripts job runs it"
fi

step "workflow YAML (yamllint)"
if command -v yamllint >/dev/null 2>&1; then
    run yamllint -c .yamllint .github/workflows/ci.yml .github/dependabot.yml .yamllint
else
    echo "SKIP: yamllint not installed — CI's lint-scripts job runs it"
fi

step "pure unit tests"
if command -v cmake >/dev/null 2>&1; then
    if cmake -S . -B build/preflight -DCMAKE_BUILD_TYPE=Debug >/dev/null 2>&1 \
       && cmake --build build/preflight --target mos_tests -j >/dev/null 2>&1; then
        run ctest --test-dir build/preflight --output-on-failure
    else
        fail=1; echo "FAILED: pure unit test build"
    fi
else
    echo "SKIP: cmake not found — CI runs the unit tests"
fi

if [ "$fail" -ne 0 ]; then
    echo
    echo "preflight: FAIL — fix the gate(s) above before pushing."
    echo "(CI would reject the same; this just caught it locally.)"
    exit 1
fi
echo
echo "preflight: ok — all local gates pass."
echo "(macOS-only gates — CLI compile, strict-adapter, amalgamated test — still run in CI.)"
