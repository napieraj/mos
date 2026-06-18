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
#   - diskutil     unmount before raw imaging (macOS, built in)
#
# Usage:
#   ./disc-ingest.sh                 # follow `mos watch`: act on every disc
#                                    #   that turns ready, hot-plug included
#   ./disc-ingest.sh <drive>...      # one-shot on the given drive selector(s)
#                                    #   (index / registry_id / diskN — see
#                                    #   `mos`'s "Selecting a drive")
#   echo /dev/disk4 | ./disc-ingest.sh -   # read dev nodes from stdin
#
# Config (environment overrides):
#   RIPS_DIR        where rips/dumps land           (default ~/Rips)
#   ARCHIVE_DIR     where .iso archives land         (default ~/Archive)
#   MB_USER_AGENT   MusicBrainz UA — PUT A REAL CONTACT HERE; the service
#                   rejects a bare curl UA and rate-limits to 1 req/s.
#
# DRY_RUN=1 prints the command each branch WOULD run instead of running it —
# the easy way to see the routing without a drive attached.
#
# 0BSD, like mos. Copy it, cut the branches you don't want, make it yours.

set -euo pipefail

RIPS_DIR=${RIPS_DIR:-"$HOME/Rips"}
ARCHIVE_DIR=${ARCHIVE_DIR:-"$HOME/Archive"}
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

# is_video_volume MOUNTPOINT — does this mounted disc carry a video layout?
# mos names the disc CLASS (cd/dvd/bd) but cannot tell a movie DVD from a data
# DVD — that distinction lives in the filesystem, which mos refuses to read
# from sectors (scope doctrine). So the consumer peeks: a DVD-Video has
# VIDEO_TS, a Blu-ray has BDMV, HD DVD has HVDVD_TS. This is exactly the
# division mos draws — it identifies the disc; you inspect the mount.
is_video_volume() {
    [ -n "$1" ] || return 1
    [ -d "$1/VIDEO_TS" ] || [ -d "$1/BDMV" ] || [ -d "$1/HVDVD_TS" ]
}

# need TOOL BRANCH — soft-require a tool; if missing, explain and skip the
# branch rather than abort the whole run (a box may have makemkvcon but not
# redumper, and still want the video branch).
need() {
    if have "$1"; then return 0; fi
    warn "$1 not found — skipping the $2 step (install it to enable)"
    return 1
}

# -------------------------------------------------------------------------
# Audio-CD identity: a MusicBrainz Disc ID is a pure function of the audio
# TOC mos already emits, so no second tool touches the drive. jq builds
# libdiscid's hash input (first/last track, then 100 frame offsets — each
# track and the lead-out as its LBA + 150); openssl + URL-safe base64 finish
# it. Verified byte-for-byte against libdiscid: the reference disc (offsets
# 150,15363,32314,46592,63414,80489; lead-out 95462) -> 49HHV7Eb8UKF3aQiNmu1GR8vKTY-.
# -------------------------------------------------------------------------
mos_discid() {                              # mos_discid <cd-drive>
    "$MOS" metadata "$1" --json | jq -r '
        .disc.toc as $t
        | [ $t.first_track, $t.last_track, $t.leadout_lba + 150 ]
          + [ range(1;100) as $n
              | (first(($t.tracks[]|select(.track==$n).start_lba)) // -150) + 150 ]
        | .[]' |
    { read -r f; read -r l; s=$(printf '%02X%02X' "$f" "$l")
      while read -r o; do s="$s$(printf '%08X' "$o")"; done
      printf '%s' "$s" | openssl dgst -sha1 -binary | base64 | tr '+/=' '._-'; }
}

# mb_lookup <cd-drive> — resolve the release. Passing the raw TOC alongside
# the disc ID lets MusicBrainz fuzzy-match a disc it doesn't yet know and
# return a submission URL for it; the TOC is the same offsets the ID hashes.
mb_lookup() {                               # mb_lookup <cd-drive>
    local id toc
    id=$(mos_discid "$1")
    toc=$("$MOS" metadata "$1" --json | jq -r '.disc.toc as $t
        | [ $t.first_track, $t.last_track, $t.leadout_lba + 150 ]
          + [ $t.tracks[].start_lba + 150 ] | join("+")')
    log "MusicBrainz disc id: $id"
    run curl -fsS -A "$MB_USER_AGENT" \
        "https://musicbrainz.org/ws/2/discid/$id?toc=$toc&cdstubs=no&fmt=json"
}

# -------------------------------------------------------------------------
# The dispatcher. One metadata read, then route on what the disc IS.
# -------------------------------------------------------------------------
ingest_one() {                              # ingest_one <drive-selector>
    local sel="$1" meta
    if ! meta=$("$MOS" metadata "$sel" --json 2>/dev/null); then
        warn "could not read $sel (no media? busy?) — skipping"; return 0
    fi

    # @sh quotes every field for the shell, so a space in a path is safe.
    local node vol class profile status erasable maker mediaid disctype audio
    eval "$(jq -r '@sh "
        node=\(.bsd_node // "")
        vol=\(.volume_path // "")
        class=\(.disc.class // "")
        profile=\(.disc.profile // "")
        status=\(.disc.disc_info.status // "")
        erasable=\(.disc.disc_info.erasable)
        maker=\(.disc.disc_structure.manufacturer_id // "")
        mediaid=\(.disc.disc_structure.media_type_id // "")
        disctype=\(.disc.disc_structure.disc_type // "")
        audio=\([.disc.toc.tracks[]? | select(.data == false)] | length)
    "' <<<"$meta")"

    local disk=${node#/dev/}                 # diskN      (redumper, diskutil)
    local raw=${node/disk/rdisk}             # /dev/rdiskN (ddrescue, dd)
    local label=${vol##*/}                   # mounted volume name, if any
    log "$sel -> class=${class:-?} profile=${profile:-?} status=${status:-?}" \
        "maker=${maker:-} mediaid=${mediaid:-} audio=$audio vol=${label:-none}"

    # 1. Audio CD — identify, then take a byte-perfect, offset-corrected
    #    preservation dump. (A data CD has no audio tracks and falls through.)
    if [ "$class" = cd ] && [ "$audio" -gt 0 ]; then
        log "audio CD: $audio audio track(s)"
        if need curl "MusicBrainz lookup"; then mb_lookup "$sel" || warn "MB lookup failed"; fi
        if need redumper "CD dump"; then
            mkdir -p "$RIPS_DIR/$disk"
            run redumper --drive="$disk" --image-path="$RIPS_DIR/$disk"
        fi
        return 0
    fi

    # 2. Blank recordable — nothing to read; report that it is ready to burn.
    #    `mos features` confirms the matching write feature is current (e.g.
    #    0x0041 BD-R). This is the "ready for archival" signal, not an action.
    if [ "$status" = blank ] || [ "$status" = appendable ]; then
        log "blank/appendable $class (profile ${profile:-?}) — ready to write"
        # mos ships no feature name table, so the codes (e.g. 0x0041 = BD-R)
        # stay as MMC numbers — mapping is the consumer's (MMC-6 §5.3).
        "$MOS" features "$sel" --json 2>/dev/null \
            | jq -c '{current_features: [.features[]? | select(.current) | .code]}' || true
        return 0
    fi

    # 3. Video DVD/BD — its TOC is drive-fabricated and identity-useless (the
    #    schema says never key on it), so there is no CD-style disc-ID. mos
    #    named the class; makemkvcon decrypts and rips the titles. Two ways in:
    #    a mounted disc whose filesystem carries a video layout (VIDEO_TS /
    #    BDMV), or an UNMOUNTED dvd/bd — the common shape for video macOS won't
    #    mount (UHD BD, some DVD-Video). A mounted disc with NO video layout is
    #    data and falls through to the archive branch. (For a raw preservation
    #    image instead of an MKV rip, redumper dumps DVD/BD by BSD name too.)
    if { [ "$class" = dvd ] || [ "$class" = bd ]; } \
       && { is_video_volume "$vol" || [ -z "$vol" ]; }; then
        if need makemkvcon "video rip"; then
            # Robot mode (-r) is makemkvcon's machine-readable scan. Two lines
            # decide and name the rip: TCOUNT is the number of rippable titles
            # (0 = no movie here, so this is really data — fall through and
            # archive it), and CINFO:2 is the disc name. So makemkvcon itself
            # confirms "is it a movie" and supplies the output directory name.
            local info titles name out
            info=$(makemkvcon -r --cache=1 --noscan info "dev:$raw" 2>/dev/null || true)
            titles=$(sed -n 's/^TCOUNT:\([0-9]*\).*/\1/p' <<<"$info")
            name=$(sed -n 's/^CINFO:2,[0-9]*,"\(.*\)"$/\1/p' <<<"$info" | head -n1)
            name=${name//\//_}                       # a disc name is not a path
            if [ "${titles:-0}" -gt 0 ]; then
                out="$RIPS_DIR/${name:-${label:-disc}}"
                log "video $class: \"${name:-untitled}\" — $titles title(s) -> $out"
                mkdir -p "$out"
                run makemkvcon mkv "dev:$raw" all "$out"
                return 0
            fi
            log "makemkvcon found 0 titles — not a movie; trying a data archive"
            # deliberately NO return: fall through to the archive branch below
        else
            return 0                                  # can't rip without it
        fi
    fi

    # 4. Mounted data disc — anything left that mounted a data volume is data
    #    to archive: image it 1:1. ddrescue, not dd — a mapfile lets a second
    #    pass retry only the sectors an aging disc failed, where dd aborts on
    #    the first read error. mos reports; the consumer unmounts and reads the
    #    sectors (scope doctrine), and `mos capacity` is the size oracle the
    #    finished image is verified against.
    #
    #    The M-DISC subcase is just a label here: MILLEN/MR1 is the registered
    #    Millenniata Disc Manufacturer/Media ID, and it is BLU-RAY ONLY — it
    #    lives in the BD Disc Information structure, so a DVD M-DISC reports no
    #    disc_structure and is archived as ordinary write-once data.
    if [ -n "$vol" ]; then
        local kind="data disc"
        [ "$erasable" = false ] && kind="write-once data disc"
        [ "$maker" = MILLEN ] && [ "$mediaid" = MR1 ] && kind="M-DISC ($disctype)"
        log "$kind on $vol: archiving to ISO"
        local img="$ARCHIVE_DIR/${label:-disc}.iso"
        local size; size=$("$MOS" capacity "$sel" --json | jq '.media_bytes')  # reads mounted
        mkdir -p "$ARCHIVE_DIR"
        if need ddrescue "archive imaging"; then
            run diskutil unmountDisk "$disk"           # ddrescue reads the raw node
            run ddrescue -b2048 -n     "$raw" "$img" "$img.map"   # fast pass, no split
            run ddrescue -b2048 -d -r3 "$raw" "$img" "$img.map"   # retry the gaps
            if [ "${DRY_RUN:-0}" != 1 ] && [ -f "$img" ]; then
                if [ "$(stat -f%z "$img" 2>/dev/null || stat -c%s "$img")" = "$size" ]
                    then log "verified: $img == $size bytes (mos capacity)"
                    else warn "size mismatch: $img vs $size bytes from mos capacity"; fi
            fi
        fi
        return 0
    fi

    log "no rule for class=${class:-unknown} status=${status:-?} vol=none — nothing to do"
}

# -------------------------------------------------------------------------
# main: one-shot over args, dev nodes on stdin (-), or follow `mos watch`.
# -------------------------------------------------------------------------
main() {
    have jq || { warn "jq is required"; exit 1; }
    have "$MOS" || { warn "'$MOS' not found on PATH (set MOS=...)"; exit 1; }

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
