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

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INC="$ROOT/include"
SRC="$ROOT/src"
DIST="$ROOT/dist"

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

#include "mos.h"

HEADER

# The guard-stripper below collapses exactly ONE include-guard pair per
# header: it consumes the FIRST #endif after a guard opens. An interior
# preprocessor conditional inside a stripped header would have its #endif
# eaten instead — silently corrupting the weave (the macOS CI compile
# would catch it downstream, but the script itself would emit garbage
# without complaint). Refuse loudly instead of corrupting; if a header
# legitimately grows an interior conditional, upgrade strip_file to track
# nesting depth first.
for hdr in mos_scsi_status.h mos_pure.h mos_internal.h; do
    conds=$(grep -c '^#[[:space:]]*if' "$SRC/$hdr" || true)
    ends=$(grep -c '^#[[:space:]]*endif' "$SRC/$hdr" || true)
    if [ "$conds" -ne 1 ] || [ "$ends" -ne 1 ]; then
        echo "amalgamate: $SRC/$hdr has $conds #if*/#$ends #endif directives;" >&2
        echo "the guard-stripper handles exactly the 1+1 include guard." >&2
        echo "Upgrade strip_file to nesting-depth tracking before weaving it." >&2
        exit 1
    fi
done

# Helper: strip includes of mos.h / mos_internal.h / mos_pure.h /
# mos_scsi_status.h and their include guards, so concatenation doesn't
# produce duplicates.
strip_file() {
    awk '
        BEGIN { in_guard = 0 }

        # Drop library-local includes; weaving replaces them.
        /^#[[:space:]]*include[[:space:]]*"mos\.h"/             { next }
        /^#[[:space:]]*include[[:space:]]*"mos_internal\.h"/    { next }
        /^#[[:space:]]*include[[:space:]]*"mos_pure\.h"/        { next }
        /^#[[:space:]]*include[[:space:]]*"mos_scsi_status\.h"/ { next }

        # Collapse internal header include guards.
        /^#ifndef MOS_INTERNAL_H/    { in_guard = 1; next }
        /^#define MOS_INTERNAL_H/    { next }
        /^#ifndef MOS_PURE_H/        { in_guard = 1; next }
        /^#define MOS_PURE_H/        { next }
        /^#ifndef MOS_SCSI_STATUS_H/ { in_guard = 1; next }
        /^#define MOS_SCSI_STATUS_H/ { next }
        in_guard && /^#endif/ { in_guard = 0; next }

        { print }
    ' "$1"
}

{
    echo "/* ==== src/mos_scsi_status.h ==== */"
    strip_file "$SRC/mos_scsi_status.h"
    echo
    echo "/* ==== src/mos_pure.h ==== */"
    strip_file "$SRC/mos_pure.h"
    echo
    echo "/* ==== src/mos_internal.h ==== */"
    strip_file "$SRC/mos_internal.h"
    echo
    echo "/* ==== src/mos_sense.c ==== */"
    strip_file "$SRC/mos_sense.c"
    echo
    echo "/* ==== src/mos_pure.c ==== */"
    strip_file "$SRC/mos_pure.c"
    echo
    echo "/* ==== src/mos_config.c ==== */"
    strip_file "$SRC/mos_config.c"
    echo
    echo "/* ==== src/mos_discinfo.c ==== */"
    strip_file "$SRC/mos_discinfo.c"

    echo "/* ==== src/mos_discstruct.c ==== */"
    strip_file "$SRC/mos_discstruct.c"
    echo
    echo "/* ==== src/mos_cdtext.c ==== */"
    strip_file "$SRC/mos_cdtext.c"
    echo
    echo "/* ==== src/mos_physstruct.c ==== */"
    strip_file "$SRC/mos_physstruct.c"
    echo
    echo "/* ==== src/mos_trackinfo.c ==== */"
    strip_file "$SRC/mos_trackinfo.c"
    echo
    echo "/* ==== src/mos_perf.c ==== */"
    strip_file "$SRC/mos_perf.c"
    echo
    echo "/* ==== src/mos_modepage.c ==== */"
    strip_file "$SRC/mos_modepage.c"
    echo
    echo "/* ==== src/mos_vpd80.c ==== */"
    strip_file "$SRC/mos_vpd80.c"
    echo
    echo "/* ==== src/mos_result.c ==== */"
    strip_file "$SRC/mos_result.c"
    echo
    echo "/* ==== src/mos_state_core.c ==== */"
    strip_file "$SRC/mos_state_core.c"
    echo
    echo "/* ==== src/mos_watch_core.c ==== */"
    strip_file "$SRC/mos_watch_core.c"
    echo
    echo "/* ==== src/mos_state.c ==== */"
    strip_file "$SRC/mos_state.c"
    echo
    echo "/* ==== src/mos_watch.c ==== */"
    strip_file "$SRC/mos_watch.c"
    echo
    echo "/* ==== src/mos_strings.c ==== */"
    strip_file "$SRC/mos_strings.c"
    echo
    echo "/* ==== src/mos_dr.c ==== */"
    strip_file "$SRC/mos_dr.c"
    echo
    echo "/* ==== src/mos_da.c ==== */"
    strip_file "$SRC/mos_da.c"
    echo
    echo "/* ==== src/mos_scsi.c ==== */"
    strip_file "$SRC/mos_scsi.c"
    echo
    echo "/* ==== src/mos_query.c ==== */"
    strip_file "$SRC/mos_query.c"
    echo
    echo "/* ==== src/mos_serial.c ==== */"
    strip_file "$SRC/mos_serial.c"
    echo
    echo "/* ==== src/mos_tray.c ==== */"
    strip_file "$SRC/mos_tray.c"
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
