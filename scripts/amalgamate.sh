#!/bin/sh
# amalgamate.sh — Produce a single-file, drop-in distribution of the C core.
#
# Output:
#   dist/mos.h    — public header, unchanged from include/
#   dist/mos.c    — concatenation of internal header + all src/*.c
#
# Consumers can drop those two files into their source tree. Compile
# mos.c as a regular translation unit; on the link step, add IOKit,
# CoreFoundation, DiscRecording, and DiskArbitration to the link
# line. No CMake, no
# submodule.
#
# This is the stb / SQLite integration model. See CONTRIBUTING.md for
# the non-amalgamated layout.

set -eu

# Deterministic collation so the src/*.c glob below weaves in the same order on
# every machine and CI (otherwise the committed dist/ and a regen diverge).
export LC_ALL=C

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INC="$ROOT/include"
SRC="$ROOT/src"
DIST="$ROOT/dist"

# The internal headers, woven in dependency order (each uses the ones before
# it). This is the ONE authoritative list — both the weave loop and the
# include-stripper below derive from it, so there is no second copy to drift.
# A manually-ordered list is the same shape SQLite's amalgamation builder uses
# (tool/mksqlite3c.tcl, the curated `flist`): the order encodes the dependency
# DAG and the pure-state-machines-before-IOKit-adapters reading order, which a
# directory glob cannot.
WEAVE_HEADERS="mos_scsi_status.h mos_pure.h mos_internal.h"

# Completeness guard: every internal header in src/ must be in WEAVE_HEADERS.
# A new src/*.h that nobody added here would otherwise leave its `#include
# "mos_new.h"` line stripped from the .c that uses it but its body never woven
# — an undefined-symbol link failure downstream in CI, far from the cause.
# Fail loudly here instead, naming the missing header. Runs BEFORE any output
# is written so a validation failure never leaves a half-written dist/.
for hdr_path in "$SRC"/*.h; do
    hdr=$(basename "$hdr_path")
    case " $WEAVE_HEADERS " in
        *" $hdr "*) : ;;
        *)
            echo "amalgamate: $hdr_path is not in WEAVE_HEADERS." >&2
            echo "Add it to the list (in dependency order) before weaving." >&2
            exit 1 ;;
    esac
done

# --check: regenerate into a scratch dir and DIFF against the committed
# dist/ instead of writing in place — side-effect-free, so the pre-push
# hook and scripts/preflight.sh can run it without dirtying the tree.
# Exit 1 on any drift (a src/ edit not followed by a regen — the common
# operator slip), exit 0 when in sync. Plain regeneration (no arg) is the
# in-place write the release/commit step uses.
CHECK=0
case "${1:-}" in
    --check) CHECK=1 ;;
    "")      : ;;
    *)       echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

if [ "$CHECK" -eq 1 ]; then
    OUT=$(mktemp -d "${TMPDIR:-/tmp}/mos-amalgamate.XXXXXX")
    trap 'rm -rf "$OUT"' EXIT
else
    OUT="$DIST"
fi
mkdir -p "$OUT"

# -- Public header: verbatim copy --------------------------------------
cp "$INC/mos.h" "$OUT/mos.h"

# -- Amalgamated implementation ---------------------------------------
H="$OUT/mos.c"
cat > "$H" <<'HEADER'
/*
 * mos.c — amalgamated single-file implementation of mac-optical-state.
 *
 * Build instructions for a consuming project:
 *
 *   Compile the implementation to an object file:
 *     cc -c mos.c -mmacosx-version-min=12.0
 *
 *   Link it into an executable (frameworks needed at link time only):
 *     cc mos.o your_main.o -o yourtool \\
 *        -framework IOKit \\
 *        -framework CoreFoundation \\
 *        -framework DiscRecording \\
 *        -framework DiskArbitration \\
 *        -mmacosx-version-min=12.0
 *
 * Or add both files (mos.h and this one) to your existing build system
 * and make sure IOKit, CoreFoundation, DiscRecording, and
 * DiskArbitration are on your link line, with the deployment target
 * pinned to macOS 12.0 to match the CMake build's
 * CMAKE_OSX_DEPLOYMENT_TARGET. Skipping -framework DiscRecording
 * fails to link at the DRCopyDeviceArray reference in mos_dr.c.
 *
 * DiskArbitration is OPTIONAL: compile with -DMOS_USE_DISKARBITRATION=0
 * and you may drop -framework DiskArbitration entirely. mos_query_volume
 * then always reports unmounted (volume name/path null) — the API, CLI,
 * and JSON shapes are unchanged. With the default (flag unset), the
 * DADiskCopyDescription reference in mos_da.c requires the framework.
 *
 * See mos.h for the API.
 * See https://github.com/napieraj/mos for source, tests,
 * and the non-amalgamated layout.
 */

/* Feature-test macros for the whole amalgamated translation unit. The
 * standalone TUs define these per-file ahead of their own includes;
 * concatenation would otherwise place a later TU's defines AFTER system
 * headers have already been processed — ineffective, and fragile against
 * reordering of the weave. Hoisted here so the amalgamated build sees the
 * same SDK surface as the standalone build. The per-file blocks below are
 * #ifndef-guarded, so they become no-ops. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* mos is implemented in C11: it uses _Static_assert and _Atomic. The CMake
 * build pins -std=c11 with C_STANDARD_REQUIRED, but this amalgamation is the
 * drop-in, no-CMake path (compile mos.c by hand), where an older dialect would
 * accept _Atomic/_Static_assert as compiler extensions and silently build a
 * subtly different library. Fail loudly instead. This is in mos.c only, never
 * mos.h, so a C99 (or older) application that includes the public header and
 * links is unaffected — the C ABI is dialect-agnostic. The __cplusplus arm is
 * skipped: compiling mos.c as C++ is a separate unsupported path. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#  if !defined(__cplusplus)
#    error "mos requires a C11 (or later) compiler: mos.c uses _Static_assert and _Atomic. Compile with -std=c11 or newer."
#  endif
#endif

#include "mos.h"

HEADER

# Helper: drop the library-local quoted includes (mos.h + the woven internal
# headers); weaving supplies their content exactly once. Include GUARDS are
# left intact deliberately: because each header is woven exactly once, its
# `#ifndef MOS_PURE_H … #endif` wrapper is harmless (the macro is defined once
# and never retested), and leaving it in means an interior `#if` inside a
# header is no longer something the weaver has to reason about — the same
# reason SQLite's amalgamation never strips guards either. STRIP_NAMES is built
# from WEAVE_HEADERS so the drop set and the weave list cannot diverge.
STRIP_NAMES="mos.h $WEAVE_HEADERS"
strip_file() {
    awk -v names="$STRIP_NAMES" '
        BEGIN { n = split(names, a, " "); for (i = 1; i <= n; i++) drop[a[i]] = 1 }

        # Drop a library-local quoted include (tolerates a trailing comment,
        # e.g. #include "mos.h"  /* for mos_state */); weaving replaces it.
        /^[[:space:]]*#[[:space:]]*include[[:space:]]*"[^"]+"/ {
            name = $0
            sub(/^[^"]*"/, "", name)
            sub(/".*/,     "", name)
            if (name in drop) next
        }

        { print }
    ' "$1"
}

# Weave the internal headers first (ordered: each uses the ones before it),
# then every src/*.c. A directory walk, so a new TU needs no edit here. The .c
# order is not significant — the headers above declare every cross-TU symbol —
# so the glob's (LC_ALL=C) order just keeps dist/ stable. No exclusions today.
{
    for hdr in $WEAVE_HEADERS; do
        echo "/* ==== src/$hdr ==== */"
        strip_file "$SRC/$hdr"
        echo
    done
    for f in "$SRC"/*.c; do
        echo "/* ==== src/$(basename "$f") ==== */"
        strip_file "$f"
        echo
    done
} >> "$H"

# The manifest is deterministic on purpose: dist/ is committed, and CI
# regenerates it expecting byte-identical output. No timestamps, no git
# state (a PR's synthetic merge commit would describe differently than
# the branch head) — the version is the MOS_VERSION_STRING consumers
# compile against.
MOS_VERSION=$(sed -n 's/^#define MOS_VERSION_STRING "\(.*\)"$/\1/p' "$INC/mos.h")
{
    echo "mac-optical-state amalgamated distribution"
    echo "Version:  ${MOS_VERSION:-unknown}"
    echo ""
    echo "Files:"
    echo "  mos.h — public API"
    echo "  mos.c — implementation (concatenated from src/)"
    echo ""
    echo "Build:"
    echo "  Compile:  cc -c mos.c -mmacosx-version-min=12.0"
    echo "  Link:     cc mos.o your_main.o -o yourtool \\"
    echo "               -framework IOKit \\"
    echo "               -framework CoreFoundation \\"
    echo "               -framework DiscRecording \\"
    echo "               -framework DiskArbitration \\"
    echo "               -mmacosx-version-min=12.0"
    echo ""
    echo "License: 0BSD (see repository)"
    echo "Source:  https://github.com/napieraj/mos"
} > "$OUT/MANIFEST.txt"

if [ "$CHECK" -eq 1 ]; then
    drift=0
    for f in mos.h mos.c MANIFEST.txt; do
        if ! diff -q "$DIST/$f" "$OUT/$f" >/dev/null 2>&1; then
            drift=1
            echo "  drift: dist/$f" >&2
            diff -u "$DIST/$f" "$OUT/$f" 2>/dev/null | sed -n '1,40p' >&2 || true
        fi
    done
    if [ "$drift" -eq 1 ]; then
        echo "ERROR: dist/ is stale relative to src/ + scripts/amalgamate.sh." >&2
        echo "Run ./scripts/amalgamate.sh and commit the result." >&2
        exit 1
    fi
    echo "OK: committed dist/ matches regeneration"
    exit 0
fi

echo "Wrote:"
echo "  $OUT/mos.h"
echo "  $OUT/mos.c"
echo "  $OUT/MANIFEST.txt"
