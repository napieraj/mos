#!/bin/bash
# wait-and-classify.sh — minimal demonstration of mos in
# a shell pipeline. Waits for a disc, prints what it sees, exits.
#
# This is NOT a media tool. It's a ~15-line shape to show what
# integrating mos looks like. Copy these lines into your real pipeline.
#
# Polling note: this loop polls every 2 seconds. That is fine for a
# demonstration but not optimal — see `mos --watch` (shipped in
# v0.3-dev) for an event-driven alternative that opens a short-lived
# handle per probe and emits one JSON line per state transition. The
# watch design is cooperative with Finder, Disk Arbitration, and any
# concurrent application because no handle is held between probes.

set -euo pipefail

# Block until the drive reports READY. One mos invocation per loop
# iteration; tolerates transient failures (no drive, IOKit hiccup).
while :; do
    out=$(mos --json 2>/dev/null) || { sleep 2; continue; }
    state=$(printf '%s' "$out" | jq -r .state)
    [ "$state" = "ready" ] && break
    sleep 2
done

# Profile byte identifies the media class. See MMC-6 Table 89.
# mos emits the profile as a lowercase hex string (cli/status.c uses
# "0x%04x"), so the case patterns below MUST be lowercase — bash case
# is case-sensitive by default and `0x001A` would not match `0x001a`.
# DVD line covers single-layer AND dual-layer formats.
profile=$(printf '%s' "$out" | jq -r .current_profile)

case "$profile" in
    0x0008|0x0009|0x000a)
        echo "CD" ;;
    0x0010|0x0011|0x0012|0x0013|0x0014|0x0015|0x0016|0x0017|0x001a|0x001b|0x002a|0x002b)
        echo "DVD" ;;
    0x0040|0x0041|0x0042|0x0043)
        echo "Blu-ray" ;;
    *)
        echo "unknown profile: $profile"; exit 1 ;;
esac
