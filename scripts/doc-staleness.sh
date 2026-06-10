#!/bin/sh
# doc-staleness.sh — grep gate for documentation drift.
#
# Why this exists: the 2026-06-10 audit found ~15 instances of prose
# documentation contradicting the tree, nearly all dating to three
# un-swept events (the 2026-05-30 state-detection redesign, the
# v0.3→v0.4 typed-API renumbering, test-count growth). The code,
# tests, schemas, and fixtures were fully consistent — CI forces them
# to be — while nothing forced a doc sweep. This script is that force:
# a deny list of strings that are only ever stale when they appear in
# LIVE documentation.
#
# Scope: live docs only. CHANGELOG.md and doc/research/ are dated
# archives whose old text is preserved BY RULE (append-only), so they
# are exempt. AGENTS.md's ADR chain preserves superseded entries the
# same way, so AGENTS is exempt from patterns that legitimately appear
# in preserved-then-rebutted text.
#
# Maintaining the list: when a doc sweep retires a stale claim, add
# its marker string here so it cannot come back. Keep patterns
# specific enough that a legitimate mention can't trip them; when a
# string has legitimate live uses, don't add it (this gate is a
# tripwire, not a linter).

set -u
FAIL=0

# Live documentation set (archives deliberately excluded).
LIVE_DOCS="README.md ARCHITECTURE.md CONTRIBUTING.md INTEGRATION_HARNESS.md \
ROADMAP.md STATUS.md examples/README.md schemas/README.md \
tests/fixtures/README.md doc/dr-field-mapping.md"

deny() {
    pattern="$1"; reason="$2"; files="${3:-$LIVE_DOCS}"
    # shellcheck disable=SC2086
    hits=$(grep -nE -e "$pattern" $files 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "STALE DOC MARKER: /$pattern/ — $reason"
        echo "$hits" | sed 's/^/    /'
        FAIL=1
    fi
}

# Dead hardcoded test counts (live count belongs in CI output only).
deny '\b(59/59|89/89|92/92|96/96|103/103|116/116|174/174|177/177)\b' \
     "hardcoded pure-test count; cite CI instead"

# CLI flags that never shipped / were removed.
deny '\-\-raw\b'     "no --raw CLI flag exists; mos_raw_cdb is C API only"
deny '\-\-verbose\b' "no --verbose CLI flag exists"

# Pre-redesign mechanisms (removed 2026-05-30).
deny 'mos_internal_state_from_sense[^_]' \
     "renamed to mos_internal_state_from_sense_closed in the redesign"
deny 'single-shot UA retry|exactly once UA retry|UA retry in mos_query_state' \
     "the UA retry was removed; TUR is single-shot (ARCHITECTURE §4.1)"

# Deleted test scaffolding.
deny 'EXPECT_BYTES' "macro deleted with mos_cdb.c cleanup"

# Renamed/retired identifiers (fourth review: the gate could not catch
# renames until the dead names were added here — do so at every rename).
deny 'matches_self_or_partition' \
     "renamed to mos_internal_bsd_unit_matches"
deny 'stream_id' \
     "retired in favor of registry_id + stream_open_ms (session identity)"

# Stale version anchors in live docs.
deny 'tagging v0\.2\.0' "v0.2.0 is long tagged; say 'before any release tag'"
deny 'v0\.3 typed API|v0\.3 introduces' \
     "typed APIs and tray verbs moved to v0.4"

if [ "$FAIL" -eq 1 ]; then
    echo ""
    echo "Documentation contains markers known to be stale against the tree."
    echo "Fix the doc (or, if the string gained a legitimate live use,"
    echo "remove its pattern from scripts/doc-staleness.sh with a rationale)."
    exit 1
fi
echo "OK: no stale documentation markers in live docs"
