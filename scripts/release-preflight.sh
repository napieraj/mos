#!/bin/sh
# release-preflight.sh — fail if a release archive contains anything that has
# no business in a distributable source package: machine-specific CMake build
# state, compiled objects, or the vendored Apple SDK headers (which are
# reference-only and not ours to redistribute under 0BSD).
#
# Usage:
#   scripts/release-preflight.sh path/to/mos-x.y.z.zip
#   scripts/release-preflight.sh path/to/extracted/dir
#
# Exit 0 = clean, 1 = forbidden entries found, 2 = usage/IO error.
#
# This exists because a real release shipped a full build/ tree with a
# CMakeCache.txt pinned to the build machine's absolute paths, which made
# `make test` fail from a fresh extraction. See CHANGELOG 2026-05-30.

set -eu

TARGET="${1:-}"
if [ -z "$TARGET" ]; then
    echo "usage: $0 <archive.zip | extracted-dir>" >&2
    exit 2
fi

# Forbidden path fragments (matched anywhere in each entry's path).
# Note: dist/ is intentionally allowed — it is the stb/sqlite-style
# single-file amalgamation, a documented release artifact (see dist/MANIFEST.txt).
patterns='
(^|/)build(/|-)
(^|/)CMakeCache\.txt$
(^|/)CMakeFiles/
\.o$
\.a$
(^|/)docs/apple/
'

if [ -f "$TARGET" ]; then
    listing=$(unzip -Z1 "$TARGET")
elif [ -d "$TARGET" ]; then
    listing=$(cd "$TARGET" && find . -type f | sed 's|^\./||')
else
    echo "preflight: no such file or directory: $TARGET" >&2
    exit 2
fi

found=0
for pat in $patterns; do
    hits=$(printf '%s\n' "$listing" | grep -E "$pat" || true)
    if [ -n "$hits" ]; then
        echo "preflight: FORBIDDEN entries match /$pat/:"
        printf '%s\n' "$hits" | sed 's/^/  /'
        found=1
    fi
done

if [ "$found" -ne 0 ]; then
    echo "preflight: FAIL — package is not clean for release."
    exit 1
fi

echo "preflight: ok — no build artifacts or vendored SDK headers in $TARGET"
exit 0
