#!/usr/bin/env bash
#
# disc-ingest.sh — route every disc to the right tool from ONE mos read.
#
# This is the fully worked version of the barebones snippet in the project
# README's "Shell integration" section. It exists to show what `mos` is for:
# a single `mos metadata --json` document tells you what a disc IS — audio CD,
# sealed M-DISC archive, blank recordable, encrypted video — and that is
# enough to hand it to the tool that already owns the job. mos identifies;
# nothing here re-queries the drive to find out what it is holding.
#
# The tools it routes to (install what you need; each branch is independent):
#   - jq           field extraction from mos's JSON                (required)
#   - openssl      MusicBrainz Disc ID hash                        (audio CD)
#   - curl         MusicBrainz release lookup                      (audio CD)
#   - redumper     byte-perfect, offset-corrected preservation dump (redump.org)
#   - ddrescue     error-tolerant 1:1 imaging of aging media       (GNU ddrescue)
#   - makemkvcon   decrypt + rip encrypted video titles            (MakeMKV)
#   - shasum/diskutil  built into macOS
#
# Beyond routing, it shows mos used as more than a classifier:
#   - an append-only INVENTORY keyed by the drive's durable serial + a hash of
#     mos's closed `disc` fingerprint subtree (the one dedup key that works
#     even for Blu-ray, where no standard disc ID exists);
#   - a per-archive SIDECAR manifest (the mos metadata + drive identity + image
#     checksum) — the provenance record an archivist keeps beside the .iso;
#   - a `mos tray` lifecycle (lock an idle drive during a rip so a stray
#     operator eject becomes an event not a retraction; eject when done);
#   - content-protection / region messaging from mos's own fields.
#
# Usage:
#   ./disc-ingest.sh                 # follow `mos watch`: act on every disc
#                                    #   that turns ready, hot-plug included
#   ./disc-ingest.sh <drive>...      # one-shot on the given drive selector(s)
#   ./disc-ingest.sh fingerprint <drive>   # print the dedup hash and exit
#   echo /dev/disk4 | ./disc-ingest.sh -   # read dev nodes from stdin
#
# Config (environment overrides):
#   RIPS_DIR / ARCHIVE_DIR   output roots          (default ~/Rips, ~/Archive)
#   INVENTORY                append-only JSONL log (default ~/disc-inventory.jsonl;
#                            set empty to disable)
#   SIDECAR=1                write <image>.mos.json + .sha256 beside archives
#   EJECT_WHEN_DONE=0        `mos tray eject` after a successful job (swap-me)
#   LOCK_DURING_RIP=0        `mos tray lock` an idle drive across the job
#   MINLENGTH=120            makemkvcon --minlength (seconds; drops menus/junk)
#   MB_USER_AGENT            MusicBrainz UA — PUT A REAL CONTACT HERE; the
#                            service rejects a bare curl UA, cap 1 req/s.
#   DRY_RUN=1                print the command each branch WOULD run
#
# 0BSD, like mos. Copy it, cut the branches you don't want, make it yours.

set -euo pipefail

RIPS_DIR=${RIPS_DIR:-"$HOME/Rips"}
ARCHIVE_DIR=${ARCHIVE_DIR:-"$HOME/Archive"}
INVENTORY=${INVENTORY-"$HOME/disc-inventory.jsonl"}
SIDECAR=${SIDECAR:-1}
EJECT_WHEN_DONE=${EJECT_WHEN_DONE:-0}
LOCK_DURING_RIP=${LOCK_DURING_RIP:-0}
MINLENGTH=${MINLENGTH:-120}
MB_USER_AGENT=${MB_USER_AGENT:-"mos-disc-ingest/1.0 ( you@example.com )"}
MOS=${MOS:-mos}

log()  { printf '[disc-ingest] %s\n' "$*" >&2; }
warn() { printf '[disc-ingest] WARN: %s\n' "$*" >&2; }

# run CMD... — honor DRY_RUN; otherwise exec the command. Keeps every branch
# testable without hardware: `DRY_RUN=1 ./disc-ingest.sh disk4`.
run() {
    if [ "${DRY_RUN:-0}" = 1 ]; then
        { printf '    would run:'; printf ' %q' "$@"; printf '\n'; } >&2
    else
        "$@"
    fi
}

have() { command -v "$1" >/dev/null 2>&1; }

# need TOOL BRANCH — soft-require a tool; if missing, explain and skip the
# branch rather than abort (a box may have makemkvcon but not redumper).
need() {
    if have "$1"; then return 0; fi
    warn "$1 not found — skipping the $2 step (install it to enable)"
    return 1
}

# sha256 of stdin (or a file via redirection), hash only. macOS ships shasum;
# Linux boxes running the pure tests have sha256sum.
sha256() { { shasum -a 256 2>/dev/null || sha256sum; } | awk '{print $1}'; }

# is_video_volume MOUNTPOINT — does this mounted disc carry a video layout?
# mos names the disc CLASS (cd/dvd/bd) but cannot tell a movie DVD from a data
# DVD — that distinction lives in the filesystem, which mos refuses to read
# from sectors (scope doctrine). So the consumer peeks: a DVD-Video has
# VIDEO_TS, a Blu-ray BDMV, HD DVD HVDVD_TS. mos identifies; you inspect.
is_video_volume() {
    [ -n "$1" ] || return 1
    [ -d "$1/VIDEO_TS" ] || [ -d "$1/BDMV" ] || [ -d "$1/HVDVD_TS" ]
}

# -------------------------------------------------------------------------
# Identity from the TOC mos already emits — no second tool touches the drive.
# A MusicBrainz Disc ID is a pure function of disc.toc: first/last track, then
# 100 frame offsets (each track and the lead-out as its LBA + 150), SHA-1'd,
# then URL-safe base64. Verified byte-for-byte against libdiscid (the
# reference disc -> 49HHV7Eb8UKF3aQiNmu1GR8vKTY-). The freedb/CDDB and
# AccurateRip ids derive from the same disc.toc client-side too — note
# AccurateRip keys on RAW LBAs where MusicBrainz/CDDB use LBA+150.
discid_from_meta() {                        # discid_from_meta <metadata-json>
    printf '%s' "$1" | jq -r '
        .disc.toc as $t
        | [ $t.first_track, $t.last_track, $t.leadout_lba + 150 ]
          + [ range(1;100) as $n
              | (first(($t.tracks[]|select(.track==$n).start_lba)) // -150) + 150 ]
        | .[]' |
    { read -r f; read -r l; s=$(printf '%02X%02X' "$f" "$l")
      while read -r o; do s="$s$(printf '%08X' "$o")"; done
      printf '%s' "$s" | openssl dgst -sha1 -binary | base64 | tr '+/=' '._-'; }
}

# mb_lookup <metadata-json> — resolve the release. Passing the raw TOC beside
# the disc ID lets MusicBrainz fuzzy-match a disc it doesn't yet know and hand
# back a submission URL; the TOC is the same offsets the id hashes.
mb_lookup() {                               # mb_lookup <metadata-json>
    local id toc
    id=$(discid_from_meta "$1")
    toc=$(printf '%s' "$1" | jq -r '.disc.toc as $t
        | [ $t.first_track, $t.last_track, $t.leadout_lba + 150 ]
          + [ $t.tracks[].start_lba + 150 ] | join("+")')
    log "MusicBrainz disc id: $id"
    run curl -fsS -A "$MB_USER_AGENT" \
        "https://musicbrainz.org/ws/2/discid/$id?toc=$toc&cdstubs=no&fmt=json"
}

# fingerprint <metadata-json> — sha256 of mos's CLOSED `disc` subtree, the
# uniform per-disc dedup key. The subtree is a fixed, sorted-by-jq key set
# designed to be hashed; it is the only stable identity for Blu-ray/data discs
# that carry no MusicBrainz/CDDB id.
fingerprint() { printf '%s' "$1" | jq -Sc '.disc' | sha256; }

# inventory_append <metadata-json> <drive-json> <action> — one JSONL row per
# disc, keyed by the drive's durable serial (survives replug; registry_id does
# not) and the disc fingerprint. This is the catalog/dedup substrate.
inventory_append() {
    [ -n "$INVENTORY" ] || return 0
    local row
    row=$(jq -nc \
        --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        --arg action "$3" \
        --arg fp "$(fingerprint "$1")" \
        --argjson meta "$1" --argjson drive "$2" '{
            ts: $ts, action: $action, fingerprint: $fp,
            serial:      ($drive.serial // null),
            product:     ($drive.product // null),
            class:       ($meta.disc.class // null),
            profile:     ($meta.disc.profile // null),
            volume_name: ($meta.disc.volume_name // null),
            maker:       ($meta.disc.disc_structure.manufacturer_id // null)
        }')
    if [ "${DRY_RUN:-0}" = 1 ]; then log "inventory += $row"
    else printf '%s\n' "$row" >>"$INVENTORY"; fi
}

# write_sidecar <metadata-json> <drive-json> <image-path> — the archivist's
# provenance record beside the image: mos's full metadata + drive identity +
# the image's own sha256 (skipped in DRY_RUN; it would read the whole image).
write_sidecar() {
    [ "$SIDECAR" = 1 ] || return 0
    local sha="(dry-run)"
    if [ "${DRY_RUN:-0}" != 1 ] && [ -f "$3" ]; then sha=$(sha256 <"$3"); fi
    local doc
    doc=$(jq -nc --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" --arg sha "$sha" \
        --argjson meta "$1" --argjson drive "$2" \
        '{captured_at: $ts, image_sha256: $sha, metadata: $meta, drive: $drive}')
    if [ "${DRY_RUN:-0}" = 1 ]; then log "sidecar -> $3.mos.json"
    else printf '%s\n' "$doc" >"$3.mos.json"; fi
}

# -------------------------------------------------------------------------
# Optional `mos tray` lifecycle. Locking an IDLE drive during a rip is the one
# thing no FOSS ripper does (they rely on the disc being busy) — it turns a
# stray operator eject into a reported event instead of a retraction mid-read,
# which is exactly what an autoloader robot wants. Eject-when-done is the
# universal "this disc is finished, swap it" signal.
tray_lock()  { [ "$LOCK_DURING_RIP" = 1 ] && run "$MOS" tray lock "$1" || true; }
tray_eject() { [ "$EJECT_WHEN_DONE" = 1 ] && run "$MOS" tray eject "$1" --force || true; }

# protection_note <metadata-json> — surface what the disc/drive say about
# content protection (a reported fact, never enforcement). DVD carries a
# per-disc copyright type + region; BD video is AACS by construction.
protection_note() {
    local prot region
    prot=$(printf '%s' "$1" | jq -r '.disc.disc_structure.copyright.protection_name // ""')
    region=$(printf '%s' "$1" | jq -r '.disc.disc_structure.copyright.region // ""')
    case "$prot" in
        css_cppm) log "protection: CSS/CPPM, region mask ${region:-?} — makemkvcon/libdvdcss will decrypt" ;;
        cprm)     log "protection: CPRM — makemkvcon will decrypt" ;;
        aacs)     log "protection: AACS — makemkvcon will decrypt" ;;
        none|"")  : ;;
    esac
}

# -------------------------------------------------------------------------
# The dispatcher. One metadata read, then route on what the disc IS.
# -------------------------------------------------------------------------
ingest_one() {                              # ingest_one <drive-selector>
    local sel="$1" meta
    if ! meta=$("$MOS" metadata "$sel" --json 2>/dev/null); then
        warn "could not read $sel (no media? busy?) — skipping"; return 0
    fi
    # mos routes its error envelope to stdout so one pipeline reads both.
    if [ "$(printf '%s' "$meta" | jq -r '.schema // ""')" = mos.error.v1 ]; then
        warn "$sel: $(printf '%s' "$meta" | jq -r '.error.code // "error"')"; return 0
    fi

    # @sh quotes every field for the shell, so a space in a path is safe.
    # erasable is coerced through tostring so a bool/null never trips @sh.
    local node vol class profile status erasable maker mediaid disctype audio sessions
    eval "$(printf '%s' "$meta" | jq -r '@sh "
        node=\(.bsd_node // "")
        vol=\(.volume_path // "")
        class=\(.disc.class // "")
        profile=\(.disc.profile // "")
        status=\(.disc.disc_info.status // "")
        erasable=\(.disc.disc_info.erasable // false | tostring)
        maker=\(.disc.disc_structure.manufacturer_id // "")
        mediaid=\(.disc.disc_structure.media_type_id // "")
        disctype=\(.disc.disc_structure.disc_type // "")
        audio=\([.disc.toc.tracks[]? | select(.data == false)] | length)
        sessions=\((.disc.session_layout // []) | length)
    "')"

    # Settle the mount: `mos watch` fires on READY, but DiskArbitration mounts
    # asynchronously after, so a data disc can arrive with volume_path still
    # empty. Re-read a few times before deciding video-vs-data (audio CDs never
    # mount, so don't wait on them).
    if { [ "$class" = dvd ] || [ "$class" = bd ]; } && [ -z "$vol" ]; then
        local _try
        for _try in 1 2 3 4; do
            sleep 0.5
            vol=$(printf '%s' "$("$MOS" metadata "$sel" --json 2>/dev/null)" \
                  | jq -r '.volume_path // ""')
            [ -n "$vol" ] && break
        done
    fi

    local node_n=${node#/dev/}              # diskN      (redumper, diskutil)
    local raw=${node:+/dev/r${node#/dev/}}  # /dev/rdiskN (ddrescue) — empty-safe
    local label=${vol##*/}                  # mounted volume name, if any
    local drive_json; drive_json=$("$MOS" drive "$sel" --json 2>/dev/null || echo null)
    log "$sel -> class=${class:-?} profile=${profile:-?} status=${status:-?}" \
        "maker=${maker:-} audio=$audio sessions=$sessions vol=${label:-none}"

    # 1. Audio CD — identify, then byte-perfect dump. A CD-Extra (audio in
    #    session 1, data in session 2 — session_layout length > 1) is flagged
    #    so the data session isn't silently dropped.
    if [ "$class" = cd ] && [ "$audio" -gt 0 ]; then
        [ "$sessions" -gt 1 ] && log "CD-Extra: $sessions sessions — data session not auto-extracted"
        if need curl "MusicBrainz lookup"; then mb_lookup "$meta" || warn "MB lookup failed"; fi
        tray_lock "$sel"
        if need redumper "CD dump"; then
            mkdir -p "$RIPS_DIR/$node_n"
            run redumper --drive="$node_n" --image-path="$RIPS_DIR/$node_n"
        fi
        inventory_append "$meta" "$drive_json" audio_cd
        tray_eject "$sel"
        return 0
    fi

    # 2. Blank / appendable — nothing to read; report it is ready to write,
    #    with the current write feature codes (mos features) and the formattable
    #    capacity (mos capacity) rather than raw MMC numbers alone.
    if [ "$status" = blank ] || [ "$status" = appendable ]; then
        log "blank/appendable $class (profile ${profile:-?}) — ready to write"
        "$MOS" features "$sel" --json 2>/dev/null \
            | jq -c '{current_features: [.features[]? | select(.current) | .code]}' || true
        inventory_append "$meta" "$drive_json" blank
        return 0
    fi

    # 3. Video DVD/BD — its TOC is drive-fabricated and identity-useless, so no
    #    CD-style id. makemkvcon -r (robot mode) is the authority on "is it a
    #    movie": TCOUNT is the rippable-title count (0 => data, fall through),
    #    and CINFO:2 (ap_iaName) is the disc name -> output dir. makemkvcon
    #    runs the disc's BD-Java itself, resolving fake-playlist obfuscation
    #    that defeats a naive "longest title" pick; --minlength drops menus.
    if { [ "$class" = dvd ] || [ "$class" = bd ]; } \
       && { is_video_volume "$vol" || [ -z "$vol" ]; }; then
        protection_note "$meta"
        if need makemkvcon "video rip"; then
            local info titles name out
            info=$(makemkvcon -r --cache=1 --minlength="$MINLENGTH" --noscan \
                       info "dev:$raw" 2>/dev/null || true)
            titles=$(printf '%s\n' "$info" | sed -n 's/^TCOUNT:\([0-9]*\).*/\1/p')
            # CINFO:id,code,"value": value is quoted with \" and \\ escaped.
            name=$(printf '%s\n' "$info" \
                 | sed -n 's/^CINFO:2,[0-9]*,"\(.*\)"$/\1/p' | head -n1 \
                 | sed 's/\\"/"/g; s/\\\\/\\/g')
            name=${name:-$label}; name=${name//\//_}    # disc name is not a path
            if [ "${titles:-0}" -gt 0 ]; then
                out="$RIPS_DIR/${name:-disc}"
                log "video $class: \"${name:-untitled}\" — $titles title(s) -> $out"
                tray_lock "$sel"
                mkdir -p "$out"
                run makemkvcon mkv --minlength="$MINLENGTH" "dev:$raw" all "$out"
                inventory_append "$meta" "$drive_json" video
                tray_eject "$sel"
                return 0
            fi
            log "makemkvcon found 0 titles — not a movie; trying a data archive"
            # deliberately NO return: fall through to the archive branch below
        else
            return 0                                  # can't rip without it
        fi
    fi

    # 4. Mounted data disc — anything left that mounted a data volume is data to
    #    archive: image it 1:1 with ddrescue (a mapfile lets a second pass retry
    #    only the sectors an aging disc failed; dd aborts on the first error).
    #    mos reports; the consumer unmounts and reads the sectors. `mos capacity`
    #    is the size oracle. The M-DISC subcase is a label: MILLEN/MR1 is the
    #    registered Millenniata Manufacturer/Media ID and is BLU-RAY ONLY (it
    #    lives in the BD Disc Information structure), so a DVD M-DISC carries no
    #    disc_structure and is archived as ordinary write-once data.
    if [ -n "$vol" ]; then
        local kind="data disc"
        [ "$erasable" = false ] && kind="write-once data disc"
        [ "$maker" = MILLEN ] && [ "$mediaid" = MR1 ] && kind="M-DISC ($disctype)"
        log "$kind on $vol: archiving to ISO"
        local img="$ARCHIVE_DIR/${label:-disc}.iso"
        local size; size=$("$MOS" capacity "$sel" --json 2>/dev/null | jq -r '.media_bytes // empty')
        mkdir -p "$ARCHIVE_DIR"
        if need ddrescue "archive imaging"; then
            tray_lock "$sel"
            run diskutil unmountDisk "$node_n"        # ddrescue reads the raw node
            run ddrescue -b2048 -n     "$raw" "$img" "$img.map"   # fast pass, no split
            run ddrescue -b2048 -d -r3 "$raw" "$img" "$img.map"   # retry the gaps
            if [ "${DRY_RUN:-0}" != 1 ] && [ -f "$img" ]; then
                local got; got=$(stat -f%z "$img" 2>/dev/null || stat -c%s "$img")
                if [ -n "$size" ] && [ "$got" = "$size" ]; then
                    log "verified: $img == $size bytes (mos capacity)"
                elif [ -n "$size" ]; then
                    warn "size mismatch: $img is $got, mos capacity says $size"
                fi
            fi
            write_sidecar "$meta" "$drive_json" "$img"
            inventory_append "$meta" "$drive_json" archive
            tray_eject "$sel"
        fi
        return 0
    fi

    log "no rule for class=${class:-unknown} status=${status:-?} vol=none — nothing to do"
}

# -------------------------------------------------------------------------
# main: a `fingerprint` subcommand, one-shot over args, dev nodes on stdin
# (-), or follow `mos watch`.
# -------------------------------------------------------------------------
main() {
    have jq || { warn "jq is required"; exit 1; }
    have "$MOS" || { warn "'$MOS' not found on PATH (set MOS=...)"; exit 1; }

    if [ "${1:-}" = fingerprint ]; then     # dedup hash, no side effects
        [ -n "${2:-}" ] || { warn "usage: $0 fingerprint <drive>"; exit 64; }
        fingerprint "$("$MOS" metadata "$2" --json)"; return
    fi

    if [ "$#" -gt 0 ] && [ "$1" != - ]; then
        for sel in "$@"; do ingest_one "$sel"; done
        return
    fi

    if [ "${1:-}" = - ]; then               # dev nodes piped in
        while read -r dev; do [ -n "$dev" ] && ingest_one "$dev"; done
        return
    fi

    # No args: drive the loop from mos's event stream. One event per
    # transition — an insert fires once, a swap fires media_changed, a
    # hot-plugged drive joins live, an eject doesn't end the stream.
    log "following 'mos watch' — insert a disc (Ctrl-C to stop)"
    "$MOS" watch | jq --unbuffered -r '
        select(.event != "error" and .state == "ready") | .bsd_node' |
    while read -r dev; do ingest_one "$dev"; done
}

main "$@"
