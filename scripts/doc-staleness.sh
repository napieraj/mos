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
# Scope: live docs only. doc/history/ and doc/research/ are dated
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

# Guard: this gate enumerates docs with `git ls-files`, which returns NOTHING
# outside a git work tree (a tarball/export, or run from the wrong directory).
# An empty file list makes every deny() below grep nothing and the script fall
# through to its unconditional "OK" — a false pass on a tripwire. So require a
# real work tree with tracked docs first; "cannot enumerate" is a hard error
# (exit 2), never a silent pass.
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "doc-staleness: not inside a git work tree — cannot enumerate docs via git ls-files." >&2
    echo "  Run from a git checkout; refusing to report a vacuous pass." >&2
    exit 2
fi
md_files=$(git ls-files '*.md')
if [ -z "$md_files" ]; then
    echo "doc-staleness: 'git ls-files *.md' returned no tracked docs — broken/empty checkout." >&2
    echo "  Refusing to report a vacuous pass." >&2
    exit 2
fi

# Whitespace hardening (Option B): split every file list on NEWLINE only, not
# the default space/tab/newline. `git ls-files` emits one path per line, so a
# tracked doc whose name contains a space is not torn apart by word-splitting
# in the loop or the deny() greps below; all file lists in this script are
# newline-joined to match. (A literal newline in a name git quotes via
# core.quotePath, so it can't silently slip through.)
nl='
'
IFS=$nl

# Live documentation set, by EXCLUSION: every tracked *.md is checked by
# default, so a new doc is covered the moment it lands — the fail-safe
# direction for a staleness tripwire (an inclusion list silently misses a
# doc nobody remembered to add). Exempt only the docs that preserve
# superseded claims as records and would false-positive on their own
# history:
#   - AGENTS.md — the ADR chain, append-only (rebuts decisions in place).
#   - CLAUDE.md — the failure-mode log; net flat/shrinking, not append-
#     only, but it quotes past wrong states as evidence, so it is not
#     held to current-state truth either.
#   - doc/research/ and doc/history/ — dated archives.
# To exempt a new doc, justify it here as one of these record types;
# everything else is held to current-state truth.
LIVE_DOCS=""
for f in $md_files; do
    case "$f" in
        AGENTS.md|CLAUDE.md)          continue ;;
        doc/research/*|doc/history/*) continue ;;
        *) LIVE_DOCS="${LIVE_DOCS}${f}${nl}" ;;
    esac
done
# If every tracked *.md is exemption-filtered, LIVE_DOCS is empty and the
# default-target deny()s would grep nothing — the same vacuous-pass class as an
# empty md_files, one layer deeper. Refuse it. (Unreachable today: live docs
# exist; this closes the residual edge.)
if [ -z "$LIVE_DOCS" ]; then
    echo "doc-staleness: every tracked *.md is exemption-filtered — no live docs to check." >&2
    echo "  Refusing to report a vacuous pass." >&2
    exit 2
fi

deny() {
    pattern="$1"; reason="$2"; files="${3:-$LIVE_DOCS}"
    # </dev/null: if $files is ever empty, grep must not fall back to reading
    # stdin (which would hang on a tty or match nothing silently).
    # shellcheck disable=SC2086
    hits=$(grep -nE -e "$pattern" $files 2>/dev/null </dev/null)
    if [ -n "$hits" ]; then
        echo "STALE DOC MARKER: /$pattern/ — $reason"
        echo "$hits" | sed 's/^/    /'
        FAIL=1
    fi
}

# Dead hardcoded test counts (live count belongs in CI output only).
deny '\b(59/59|89/89|92/92|96/96|103/103|116/116|174/174|177/177)\b' \
     "hardcoded pure-test count; cite CI instead"

# Retired by the DR pivot (2026-06-10): mos no longer issues INQUIRY,
# enumerates by class walk, plans a walk-up, or links DiskArbitration.
deny 'Issues up to four MMC commands' \
     "command count is three since the DR pivot retired INQUIRY"
deny 'walk-up is the planned change' \
     "walk-up dissolved by DRDeviceCopyDeviceForBSDName"
deny 'DARegisterDiskDescriptionChangedCallback' \
     "the watch's DA wake retired in DR pivot Phase 2a"

# CLI flags that never shipped / were removed.
deny '\-\-raw\b'     "no --raw CLI flag exists; raw CDB issuance is internal (mos_internal_raw_cdb)"
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
# The public mos_raw_cdb() passthrough was retired (2026-06-19): the function
# is now internal-only (mos_internal_raw_cdb), and diagnostic capture moved to
# `mos probe --capture`. A bare `mos_raw_cdb` in a LIVE doc is stale; the
# internal name has its own substring so this never matches it, and the
# append-only records (AGENTS/CLAUDE/research/history) are exempt by scope.
deny 'mos_raw_cdb\b' \
     "public passthrough retired; use mos_internal_raw_cdb (internal) or 'mos probe --capture'"
deny 'stream_id' \
     "retired in favor of registry_id + stream_open_ms (session identity)"

# Stale version anchors in live docs.
deny 'tagging v0\.2\.0' "v0.2.0 is long tagged; say 'before any release tag'"
deny 'v0\.3 typed API|v0\.3 introduces' \
     "typed APIs and tray verbs moved to v0.4"

# Probes consolidated into 'mos probe' (cli/probe.c, MOS_CLI_PROBE),
# 2026-06-11. ROADMAP.md is excluded: its preserved Phase-0 status text
# and the dated consolidation append legitimately name the dead tools
# to rebut them (same append-with-argument rule as AGENTS).
deny 'mos_notification_probe|tools/mos_probe|MOS_BUILD_(NOTIFICATION_)?PROBE|\-\-dr-dump' \
     "probes consolidated into 'mos probe' (cli/probe.c, MOS_CLI_PROBE), 2026-06-11" \
     "README.md
ARCHITECTURE.md
CONTRIBUTING.md
INTEGRATION_HARNESS.md
schemas/README.md
tests/fixtures/README.md
doc/dr-field-mapping.md"

if [ "$FAIL" -eq 1 ]; then
    echo ""
    echo "Documentation contains markers known to be stale against the tree."
    echo "Fix the doc (or, if the string gained a legitimate live use,"
    echo "remove its pattern from scripts/doc-staleness.sh with a rationale)."
    exit 1
fi
echo "OK: no stale documentation markers in live docs"
