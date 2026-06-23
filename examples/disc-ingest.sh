#!/usr/bin/env bash
#
# disc-ingest.sh — route every disc to the right tool from ONE mos read.
#
# This is the fully worked version of the barebones snippet in the project
# README's "Shell integration" section. It exists to show what `mos` is for:
# a single `mos metadata --json` document tells you what a disc IS — audio CD,
# a finalized M-DISC archive, blank recordable, encrypted video — and that is
# enough to hand it to the tool that already owns the job. mos identifies;
# nothing here re-queries the drive to find out what it is holding.
#
# The file is laid out so the DELEGATION reads first: config, then `main` and
# the `ingest_*` branch dispatchers (the "what mos says -> what tool runs"
# logic), then the helper functions the branches delegate to. Read top-down to
# see the routing; drop into the helpers when you care how a step works.
#
# The tools it routes to (install what you need; each branch is independent):
#   - jq           field extraction from mos's JSON                (required)
#   - openssl      MusicBrainz Disc ID hash                        (audio CD)
#   - curl         MusicBrainz lookup, cover art, AccurateRip DB   (audio CD)
#   - redumper     byte-perfect, offset-corrected preservation dump (redump.org)
#   - ddrescue     error-tolerant 1:1 imaging of aging media       (GNU ddrescue)
#   - makemkvcon   decrypt + rip encrypted video titles            (MakeMKV)
#   - shasum/diskutil  built into macOS
#
# Beyond routing, it shows mos used as more than a classifier:
#   - audio CDs are NAMED from a real MusicBrainz release lookup (artist / album
#     / year / tracklist), cross-checked against the disc's own TOC lengths, with
#     cover art + per-track tags + every ecosystem's disc id beside the rip;
#   - video discs are PLANNED from `makemkvcon -r info` (durations, chapters, the
#     main feature) and gated on a protection prediction from mos's drive caps;
#   - data discs are imaged 1:1 with ddrescue, verified against mos's reported
#     capacity AND its disc_structure geometry;
#   - an append-only INVENTORY + a provenance manifest (sha256 of each output)
#     keyed by the drive serial and a hash of mos's `disc` fingerprint subtree.
#
# This is a REFERENCE, not a product: it demonstrates what mos's JSON unlocks,
# with as few knobs as possible. It reports and identifies; it does not control
# the drive (no tray/eject — that is mos's separate control surface).
#
# Usage:
#   ./disc-ingest.sh                 # follow `mos watch`: act on every disc
#                                    #   that turns ready, hot-plug included
#   ./disc-ingest.sh <drive>...      # one-shot on the given drive selector(s)
#   ./disc-ingest.sh identify <drive>      # full plan, read-only — no rip, no writes
#   ./disc-ingest.sh drive <drive>         # drive identity + community-DB lookup keys
#   ./disc-ingest.sh fingerprint <drive>   # print the dedup hash and exit
#   echo /dev/disk4 | ./disc-ingest.sh -   # read dev nodes from stdin
#
# Config (environment overrides):
#   RIPS_DIR / ARCHIVE_DIR   output roots          (default ~/Rips, ~/Archive)
#   INVENTORY                append-only JSONL log (default ~/disc-inventory.jsonl;
#                            set empty to disable)
#   MINLENGTH=120            makemkvcon --minlength (seconds; drops menus/junk)
#   MB_USER_AGENT            MusicBrainz UA — PUT A REAL CONTACT HERE; the
#                            service rejects a bare curl UA, cap 1 req/s.
#   DRY_RUN=1                print the command each branch WOULD run (read-only
#                            probes — MusicBrainz, makemkvcon info — still run so
#                            the plan is REAL; nothing is written or ripped)
#
# 0BSD, like mos. Copy it, cut the branches you don't want, make it yours.

set -euo pipefail

RIPS_DIR=${RIPS_DIR:-"$HOME/Rips"}
ARCHIVE_DIR=${ARCHIVE_DIR:-"$HOME/Archive"}
INVENTORY=${INVENTORY-"$HOME/disc-inventory.jsonl"}
MINLENGTH=${MINLENGTH:-120}
MB_USER_AGENT=${MB_USER_AGENT:-"mos-disc-ingest/1.0 ( you@example.com )"}
MOS=${MOS:-mos}

# Internal constants (not knobs — a reference keeps its surface small).
MB_INC="artist-credits+recordings+release-groups+labels"   # MusicBrainz subqueries
COVER_SIZE=500                            # Cover Art Archive thumb (250|500|1200)
LENGTH_TOLERANCE=5                        # seconds of slack on the TOC-vs-MB check
# AccurateRip offsets via the DiscImageCreator TSV mirror (tab-delimited, no
# bot-blocking, no HTML parse): "VENDOR - MODEL <TAB> offset <TAB> n <TAB> pct%".
AR_OFFSET_URL="https://raw.githubusercontent.com/saramibreak/DiscImageCreator/master/Release_ANSI/driveOffset.txt"
AR_DB_CACHE="${TMPDIR:-/tmp}/disc-ingest.aroffsets.$$"
MB_THROTTLE_STAMP="${TMPDIR:-/tmp}/disc-ingest.mbstamp.$$"
trap 'rm -f "$AR_DB_CACHE" "$MB_THROTTLE_STAMP"' EXIT

# --- tiny plumbing the delegation leans on --------------------------------
log()  { printf '[disc-ingest] %s\n' "$*" >&2; }
warn() { printf '[disc-ingest] WARN: %s\n' "$*" >&2; }

# run CMD... — honor DRY_RUN; otherwise exec the command. Keeps every branch
# testable without hardware: `DRY_RUN=1 ./disc-ingest.sh disk4`. Read-only
# probes (mos, MusicBrainz, makemkvcon info) are NOT wrapped in run() — they
# execute even in DRY_RUN so the printed plan reflects the real disc.
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

# =========================================================================
# THE DELEGATION
# One `mos metadata --json` read per disc, then route on what it IS. `main`
# picks the invocation mode; `ingest_one` does the read + dispatch; each
# `ingest_*` owns one disc kind. The helpers they call are defined below.
# =========================================================================

# main: identify / fingerprint / offset subcommands, one-shot over args, dev
# nodes on stdin (-), or follow `mos watch`.
main() {
    have jq || { warn "jq is required"; exit 1; }
    have "$MOS" || { warn "'$MOS' not found on PATH (set MOS=...)"; exit 1; }

    if [ "${1:-}" = identify ]; then        # full plan, read-only, no rip/writes
        [ -n "${2:-}" ] || { warn "usage: $0 identify <drive>"; exit 64; }
        DRY_RUN=1                            # terminal path — script exits after
        ingest_one "$2"; return
    fi

    if [ "${1:-}" = drive ]; then           # drive identity + community-DB keys
        [ -n "${2:-}" ] || { warn "usage: $0 drive <drive>"; exit 64; }
        drive_report "$2"; return
    fi

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
    # Every ready event already carries media_type and writable, read ZERO-MMC
    # off the kernel media node — no second query. We log them as a cheap preview
    # and still hand the disc to ingest_one for the authoritative metadata read.
    # A consumer that only wanted finalized discs could gate right here instead,
    # e.g. add `and .writable == false` to skip blank/appendable recordables, or
    # `and (.media_type|test("rom$"))` for pressed media — no metadata call.
    # (writable is a bool, so use an explicit null test: jq's `//` treats false
    # as empty, so `.writable // "?"` would wrongly map false to "?".)
    "$MOS" watch | jq --unbuffered -r '
        select(.event != "error" and .state == "ready")
        | [ .bsd_node,
            (.media_type // "?"),
            (.writable | if . == null then "?" else tostring end) ]
        | @tsv' |
    while IFS=$'\t' read -r dev mtype writable; do
        # media_type is the RICH profile axis — ROM vs write-once vs rewritable
        # (cd_rom/cd_r/cd_rw, dvd_minus_r/dvd_plus_r, dvd_ram, bd_rom/bd_r/bd_re,
        # ...), finer than the coarse class and free on the event. Switch on it
        # for a zero-MMC first cut: a pressed disc is read-only inventory, a
        # write-once is an archive candidate, a rewritable is scratch/reuse. The
        # authoritative routing still happens in ingest_one (it needs the TOC,
        # disc_info, and the mount), but a consumer could route entirely here.
        local tier
        case "$mtype" in
            *_rom)            tier=pressed-ROM ;;
            *_rw|*_re|*_ram)  tier=rewritable ;;
            *_r)              tier=write-once ;;
            *)                tier=unknown-type ;;
        esac
        log "ready: $dev — ${mtype} ($tier, writable=$writable)"
        ingest_one "$dev"
    done
}

# ingest_one <drive-selector> — the dispatcher. One metadata read, then route.
ingest_one() {
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
    local node vol class profile status erasable manufacturer mediaid disctype audio preemph sessions
    eval "$(printf '%s' "$meta" | jq -r '@sh "
        node=\(.bsd_node // "")
        vol=\(.volume_path // "")
        class=\(.disc.media_class // "")
        profile=\(.disc.current_profile // "")
        status=\(.disc.disc_info.status // "")
        erasable=\(.disc.disc_info.erasable // false | tostring)
        manufacturer=\(.disc.disc_structure.manufacturer_id // "")
        mediaid=\(.disc.disc_structure.media_type_id // "")
        disctype=\(.disc.disc_structure.disc_type_id // "")
        audio=\([.disc.toc.tracks[]? | select(.data == false)] | length)
        preemph=\([.disc.toc.tracks[]? | select(.pre_emphasis == true)] | length)
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
        "manufacturer=${manufacturer:-} audio=$audio sessions=$sessions vol=${label:-none}"

    # 1. Audio CD — identify, then byte-perfect dump.
    if [ "$class" = cd ] && [ "$audio" -gt 0 ]; then
        ingest_audio_cd "$sel" "$meta" "$drive_json" "$node_n" "$label" "$sessions" "$preemph"
        return 0
    fi

    # 2. Blank / appendable — nothing to read; report it is ready to write.
    if [ "$status" = blank ] || [ "$status" = appendable ]; then
        ingest_blank "$sel" "$meta" "$drive_json" "$class" "$profile"
        return 0
    fi

    # 3. Video DVD/BD — its TOC is drive-fabricated and identity-useless, so no
    #    CD-style id. makemkvcon (running the disc's BD-Java) is the authority on
    #    "is it a movie". A 0-title result is data, so it falls through.
    if { [ "$class" = dvd ] || [ "$class" = bd ]; } \
       && { is_video_volume "$vol" || [ -z "$vol" ]; }; then
        if ingest_video "$sel" "$meta" "$drive_json" "$raw" "$label"; then
            return 0
        fi
        # ingest_video returned 1 (no titles) — fall through to the archive branch
    fi

    # 4. Mounted data disc — image it 1:1.
    if [ -n "$vol" ]; then
        ingest_data "$sel" "$meta" "$drive_json" "$node_n" "$raw" "$label" \
            "$erasable" "$manufacturer" "$mediaid" "$disctype"
        return 0
    fi

    log "no rule for class=${class:-unknown} status=${status:-?} vol=none — nothing to do"
}

# --- Branch: audio CD -----------------------------------------------------
# Identify (MusicBrainz -> CD-TEXT -> label), cross-check the match against the
# disc's TOC lengths, name the rip Artist/Album (Year), then byte-perfect dump.
ingest_audio_cd() {                         # <sel> <meta> <drive> <node_n> <label> <sessions> <preemph>
    local sel="$1" meta="$2" drive_json="$3" node_n="$4" label="$5" sessions="$6" preemph="$7"

    [ "$sessions" -gt 1 ] && log "CD-Extra: $sessions sessions — data session not auto-extracted"
    # mos derives pre_emphasis per track from the Q-channel control nibble.
    # Pre-emphasised audio rips bright unless de-emphasised: redumper PRESERVES
    # the flag in its cuesheet (correct for archival), while WAV/FLAC tools
    # (whipper/EAC) apply the 50/15us de-emphasis curve. Flag it so it isn't
    # silently lost — best-effort, like the tools that read it from the TOC.
    [ "$preemph" -gt 0 ] && log "pre-emphasis: $preemph track(s) flagged — preserve the flag (redumper) or de-emphasise (whipper/EAC)"

    # Resolve identity. MusicBrainz is authoritative; CD-TEXT is the disc's own
    # offline answer; the mounted label is the last resort.
    local mbjson="" artist="" album="" year="" mbtracks="" source="label"
    if have curl && have openssl; then
        mbjson=$(mb_fetch "$meta")
        if [ -n "$mbjson" ]; then
            local rel; rel=$(mb_release "$mbjson" || true)
            if [ -n "$rel" ]; then
                IFS=$'\t' read -r artist album year mbtracks <<<"$rel"
                source="musicbrainz"
            fi
        fi
    else
        need openssl "MusicBrainz lookup" || true
    fi
    if [ -z "$album" ]; then                # CD-TEXT fallback
        artist=$(printf '%s' "$meta" | jq -r '.disc.cdtext.performer // ""')
        album=$(printf '%s'  "$meta" | jq -r '.disc.cdtext.title // ""')
        [ -n "$album" ] && source="cd-text"
    fi
    [ -z "$album" ]  && { album="${label:-Unknown Album}"; source="label"; }
    [ -z "$artist" ] && artist="Unknown Artist"

    if [ "$source" = musicbrainz ]; then
        log "release: $artist — $album${year:+ ($year)} [$mbtracks tracks, MusicBrainz]"
    else
        log "release: $artist — $album${year:+ ($year)} [$source — MusicBrainz had no match]"
    fi

    # Cross-check the lookup against THIS disc: the disc id is an exact function
    # of the TOC, but a fuzzy `toc=` match or the wrong disc of a set can slip a
    # release whose track lengths don't fit. Flag it rather than mis-tag.
    local length_check="not_checked"
    if [ "$source" = musicbrainz ]; then
        length_check=$(length_consistency_check "$meta" "$mbjson")
        [ "$length_check" = mismatch ] && \
            warn "MusicBrainz release is INCONSISTENT with the disc TOC lengths — review before trusting tags"
    fi

    offset_and_fingerprint "$meta" "$drive_json"

    # Every ecosystem's disc id from the ONE mos TOC read — the spotlight: no
    # second tool touches the drive to compute these.
    local ids; ids=$(disc_ids "$meta")
    log "disc ids: freedb=$(jq -r .freedb <<<"$ids")" \
        "accuraterip=$(jq -r '.accuraterip_id1+"-"+.accuraterip_id2' <<<"$ids")"

    # Lay the rip out as Artist/Album (Year)/ — the layout a music library
    # expects, named from the lookup, not from the anonymous diskN.
    local a_dir b_dir base out
    a_dir=$(sanitize_component "$artist")
    b_dir=$(sanitize_component "$album")
    if [ -n "$year" ]; then
        base=$(sanitize_component "$album ($year)"); out="$RIPS_DIR/$a_dir/$base"
    else
        base="$b_dir"; out="$RIPS_DIR/$a_dir/$b_dir"
    fi

    if need redumper "CD dump"; then
        run mkdir -p "$out"
        # redumper writes <image-name>.{bin,cue,log,...} into <image-path>.
        run redumper --drive="$node_n" --image-path="$out" --image-name="$base"
    fi

    # All the disc ids beside the rip (incl. the AccurateRip results URL) — a
    # tagger / accuraterip verifier reads one file, computed from mos's TOC.
    printf '%s\n' "$ids" | jq . 2>/dev/null | write_text_file "$out/disc-ids.json" || true

    # The tracklist + raw lookup beside the rip: the metadata a tagger consumes.
    if [ -n "$mbjson" ] && [ "$source" = musicbrainz ]; then
        printf '%s\n' "$mbjson" | jq . 2>/dev/null | write_text_file "$out/musicbrainz.json" || true
        { printf '%s — %s%s\n\n' "$artist" "$album" "${year:+ ($year)}"
          mb_tracklist "$mbjson"; } | write_text_file "$out/tracklist.txt"
        fetch_cover_art "$mbjson" "$out"        # front cover, keyed on the release MBID
        write_tags "$mbjson" "$out"             # per-track NN.tags for metaflac
    elif printf '%s' "$meta" | jq -e '.disc.cdtext.tracks | length > 0' >/dev/null 2>&1; then
        { printf '%s — %s (CD-TEXT)\n\n' "$artist" "$album"
          cdtext_tracklist "$meta"; } | write_text_file "$out/tracklist.txt"
    fi

    # A mismatch leaves a breadcrumb in the rip dir so a human can resolve it.
    if [ "$length_check" = mismatch ]; then
        { printf 'MusicBrainz match is INCONSISTENT with this disc'\''s TOC track lengths.\n\n'
          printf 'Resolved as: %s — %s%s\n' "$artist" "$album" "${year:+ ($year)}"
          printf 'Disc id: %s\n' "$(discid_from_meta "$meta")"
          printf 'Re-check / attach the correct release at:\n'
          printf '  https://musicbrainz.org/cdtoc/attach\n'
        } | write_text_file "$out/NEEDS_REVIEW.txt"
    fi

    inventory_append "$meta" "$drive_json" audio_cd \
        "$(jq -nc --arg a "$artist" --arg b "$album" --arg y "$year" \
                  --arg s "$source" --arg lc "$length_check" --argjson ids "$ids" \
            '{artist:$a, album:$b, year:(($y|select(.!="")) // null),
              id_source:$s, length_check:$lc,
              freedb:$ids.freedb, accuraterip:($ids.accuraterip_id1+"-"+$ids.accuraterip_id2)}')"
    write_manifest "$meta" "$drive_json" "$out" audio_cd
}

# --- Branch: blank / appendable -------------------------------------------
# Nothing to read; report it is ready to write, with the current write feature
# codes (mos features) and — for blank CD-R/RW — the ATIP pre-groove identity.
ingest_blank() {                            # <sel> <meta> <drive> <class> <profile>
    local sel="$1" meta="$2" drive_json="$3" class="$4" profile="$5"
    log "blank/appendable $class (profile ${profile:-?}) — ready to write"
    "$MOS" features "$sel" --json 2>/dev/null \
        | jq -c '{current_features: [.features[]? | select(.current) | .code]}' || true
    # mos surfaces the CD-R/RW pre-groove (ATIP) identity: maker code + type.
    if printf '%s' "$meta" | jq -e '.disc.atip != null' >/dev/null 2>&1; then
        printf '%s' "$meta" | jq -r '.disc.atip
            | "[disc-ingest] ATIP: \(if .disc_type==1 then "CD-RW" else "CD-R" end), "
              + "lead-in \(.lead_in.min):\(.lead_in.sec):\(.lead_in.frame) (maker code), "
              + "\(if .unrestricted_use then "unrestricted" else "restricted-use" end)"' >&2 || true
    fi
    inventory_append "$meta" "$drive_json" blank
}

# --- Branch: video DVD/BD -------------------------------------------------
# Plan from makemkvcon, name the dir from the disc, identify the main feature,
# write a titles manifest, then decrypt + rip. Returns 0 if it handled the disc,
# 1 if makemkvcon found no titles (so the caller falls through to data archive).
ingest_video() {                            # <sel> <meta> <drive> <raw> <label>
    local sel="$1" meta="$2" drive_json="$3" raw="$4" label="$5" class
    class=$(printf '%s' "$meta" | jq -r '.disc.media_class // ""')

    protection_note "$meta"
    # Dual-layer DVD: the layer break (end_sector_l0) is where a one-pass image
    # would seam; makemkvcon handles it, but it's worth surfacing.
    printf '%s' "$meta" | jq -e '.disc.disc_structure.physical.num_layers == 2' >/dev/null 2>&1 \
        && log "dual-layer: layer break at sector $(printf '%s' "$meta" | jq -r '.disc.disc_structure.physical.end_sector_l0')"

    # Predict decryptability from the disc protection + the DRIVE's capability
    # (mos drive) before spending a multi-minute scan — the spotlight guard. We
    # only WARN: makemkvcon is the authority, so the prediction informs, never
    # blocks (a stale drive-cap reading shouldn't veto a rip).
    local verdict; verdict=$(predict_protection "$meta" "$drive_json")
    [ "${verdict%%:*}" = block ] && warn "protection: ${verdict#block:} (trying makemkvcon anyway)"

    need makemkvcon "video rip" || return 0     # can't rip without it; treat as handled

    local info name titles
    info=$(makemkv_info "$raw")
    makemkv_check_messages "$info"              # surface AACS/decrypt errors verbatim
    titles=$(printf '%s\n' "$info" | sed -n 's/^TCOUNT:\([0-9]*\).*/\1/p' | head -n1)
    if [ -z "${titles:-}" ] || [ "$titles" -eq 0 ]; then
        log "makemkvcon found 0 titles — not a movie; trying a data archive"
        return 1                                # fall through to the archive branch
    fi

    name=$(makemkv_disc_name "$info")
    name=${name:-$label}
    local name_dir; name_dir=$(sanitize_component "${name:-disc}")
    local out="$RIPS_DIR/$name_dir"

    # Parse every title; rank by duration. The main feature is the longest title
    # UNLESS MakeMKV flagged one "FPL_MainFeature" (its verdict on the real movie
    # behind a fake-playlist obfuscated disc), which wins outright.
    local rows main_title="" main_sec=0 fpl_title="" t sec dur chap bytes oname tname
    rows=$(makemkv_titles "$info")
    log "video $class: \"${name:-untitled}\" — $titles title(s) -> $out"
    while IFS=$'\t' read -r t sec dur chap bytes oname tname; do
        [ -n "$t" ] || continue
        [ "$oname" = - ] && oname=""; [ "$tname" = - ] && tname=""   # undo sentinels
        log "  title $t: $dur, $chap chapter(s), $(human_bytes "$bytes")${tname:+  [$tname]}${oname:+  ->  $oname}"
        case "$tname" in FPL_MainFeature*) fpl_title="$t" ;; esac
        if [ "$sec" -gt "$main_sec" ]; then main_sec="$sec"; main_title="$t"; fi
    done <<<"$rows"
    if [ -n "$fpl_title" ]; then
        log "main feature: title $fpl_title (MakeMKV FPL_MainFeature — fake-playlist verdict)"
        main_title="$fpl_title"
    elif [ -n "$main_title" ]; then
        log "main feature: title $main_title (${main_sec}s, longest)"
    fi

    run mkdir -p "$out"

    # Manifest: the makemkvcon plan + the disc name, beside the rip.
    { printf '# %s\n# title\tseconds\tlength\tchapters\tbytes\toutput\tname\n' "$name"
      printf '%s\n' "$rows"; } | write_text_file "$out/titles.tsv"

    # Rip every title that passed --minlength (box set + extras); the main
    # feature is identified above and recorded, but we don't drop the rest.
    run makemkvcon mkv --minlength="$MINLENGTH" "dev:$raw" all "$out"

    inventory_append "$meta" "$drive_json" video \
        "$(jq -nc --arg n "$name" --argjson tc "${titles:-0}" --arg mt "${main_title:-}" \
                  --arg pv "${verdict%%:*}" \
            '{disc_name:$n, title_count:$tc, main_title:(($mt|select(.!="")) // null),
              protection_predict:$pv}')"
    write_manifest "$meta" "$drive_json" "$out" video
    return 0
}

# --- Branch: mounted data disc --------------------------------------------
# Image it 1:1 with ddrescue (a mapfile lets a second pass retry only the
# sectors an aging disc failed; dd aborts on the first error). mos reports; the
# consumer unmounts and reads the sectors. `mos capacity` is the size oracle.
# The M-DISC subcase is a label: MILLEN/MR1 is the registered Millenniata
# Manufacturer/Media ID and is BLU-RAY ONLY (it lives in the BD Disc Information
# structure), so a DVD M-DISC carries no disc_structure and is archived as
# ordinary write-once data.
ingest_data() {                             # <sel> <meta> <drive> <node_n> <raw> <label> <erasable> <mfr> <mid> <disctype>
    local sel="$1" meta="$2" drive_json="$3" node_n="$4" raw="$5" label="$6"
    local erasable="$7" manufacturer="$8" mediaid="$9" disctype="${10}"

    local kind="data disc"
    [ "$erasable" = false ] && kind="write-once data disc"
    [ "$manufacturer" = MILLEN ] && [ "$mediaid" = MR1 ] && kind="M-DISC ($disctype)"
    log "$kind on volume $label: archiving to ISO"
    local img="$ARCHIVE_DIR/${label:-disc}.iso"
    local size; size=$("$MOS" capacity "$sel" --json 2>/dev/null | jq -r '.media_bytes // empty')

    # Preservation oracle from mos's disc_structure.physical (DVD/HD-DVD): the
    # exact data-area sector extent and the dual-layer break. A DVD-9 image must
    # be the right TOTAL sector count, and an OTP disc has a layer break at
    # end_sector_l0 that a one-pass tool must know about — mos reports both, so
    # the consumer verifies completeness against the disc's own geometry instead
    # of re-probing. (BD geometry isn't in physical; capacity is the BD oracle.)
    local phys_bytes=""
    if printf '%s' "$meta" | jq -e '.disc.disc_structure.physical != null' >/dev/null 2>&1; then
        phys_bytes=$(printf '%s' "$meta" | jq -r '.disc.disc_structure.physical
            | ((.end_sector - .start_sector + 1) * 2048)')
        printf '%s' "$meta" | jq -e '.disc.disc_structure.physical.num_layers == 2' >/dev/null 2>&1 \
            && log "dual-layer (OTP): layer break at sector $(printf '%s' "$meta" | jq -r '.disc.disc_structure.physical.end_sector_l0') — image must span both layers"
        log "expected data-area extent: $phys_bytes bytes ($(human_bytes "$phys_bytes")) from disc_structure.physical"
    fi

    run mkdir -p "$ARCHIVE_DIR"
    need ddrescue "archive imaging" || return 0
    run diskutil unmountDisk "$node_n"        # ddrescue reads the raw node
    run ddrescue -b2048 -n     "$raw" "$img" "$img.map"   # fast pass, no split
    run ddrescue -b2048 -d -r3 "$raw" "$img" "$img.map"   # retry the gaps
    if [ "${DRY_RUN:-0}" != 1 ] && [ -f "$img" ]; then
        local got; got=$(stat -f%z "$img")
        if [ -n "$size" ] && [ "$got" = "$size" ]; then
            log "verified: $img == $size bytes (mos capacity)"
        elif [ -n "$size" ]; then
            warn "size mismatch: $img is $got, mos capacity says $size"
        fi
        # Second, independent check against the DVD data-area extent.
        if [ -n "$phys_bytes" ] && [ "$got" != "$phys_bytes" ]; then
            warn "image $got bytes vs disc_structure data-area $phys_bytes — runout/padding is normal, a large gap is not"
        fi
    fi
    write_sidecar "$meta" "$drive_json" "$img"
    inventory_append "$meta" "$drive_json" archive
}

# =========================================================================
# HELPERS the branches delegate to
# =========================================================================

# sanitize_component STR — make a string safe as a single path component:
# drop control bytes, replace the filesystem-hostile set with spaces, collapse
# and trim whitespace, cap the length. Never empty (falls back to "Untitled").
# A disc title is disc-controlled text, not a path — this is the gate.
sanitize_component() {
    local s
    s=$(printf '%s' "${1:-}" \
        | LC_ALL=C tr -d '\000-\037' \
        | sed 's#[/\\:*?"<>|]# #g; s/^ *//; s/ *$//; s/  */ /g')
    s=${s:0:120}
    printf '%s' "${s:-Untitled}"
}

# human_bytes N — round a byte count to GiB/MiB for a log line.
human_bytes() {
    awk -v b="${1:-0}" 'BEGIN{
        if(b>=1073741824) printf "%.1f GiB", b/1073741824;
        else if(b>=1048576) printf "%.0f MiB", b/1048576;
        else printf "%d B", b }'
}

# is_video_volume MOUNTPOINT — does this mounted disc carry a video layout?
# mos names the disc CLASS (cd/dvd/bd) but cannot tell a movie DVD from a data
# DVD — that distinction lives in the filesystem, which mos refuses to read
# from sectors (scope doctrine). So the consumer peeks: a DVD-Video has
# VIDEO_TS, a Blu-ray BDMV, HD DVD HVDVD_TS. mos identifies; you inspect.
is_video_volume() {
    [ -n "$1" ] || return 1
    [ -d "$1/VIDEO_TS" ] || [ -d "$1/BDMV" ] || [ -d "$1/HVDVD_TS" ]
}

# --- Audio identity: MusicBrainz Disc ID + lookup -------------------------
# A MusicBrainz Disc ID is a pure function of disc.toc: first/last track, then
# 100 frame offsets (each track and the lead-out as its LBA + 150), SHA-1'd,
# then URL-safe base64. Verified byte-for-byte against libdiscid (the reference
# disc -> 49HHV7Eb8UKF3aQiNmu1GR8vKTY-). The freedb/CDDB and AccurateRip ids
# derive from the same disc.toc client-side too — note AccurateRip keys on RAW
# LBAs where MusicBrainz/CDDB use LBA+150.
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

# toc_query_string <metadata-json> — the MusicBrainz `toc=` parameter: first,
# last, lead-out then every track start, all as LBA+150, joined with '+'.
toc_query_string() {
    printf '%s' "$1" | jq -r '.disc.toc as $t
        | [ $t.first_track, $t.last_track, $t.leadout_lba + 150 ]
          + [ $t.tracks[].start_lba + 150 ] | join("+")'
}

# disc_ids <metadata-json> — EVERY audio-CD disc identifier that is a pure
# function of mos's TOC, derived from ONE mos read with no tool touching the
# drive. This is the spotlight: mos ships the TOC primitive and a consumer keys
# MusicBrainz, freedb/CDDB, AND AccurateRip off it. The framings differ (verified
# against libdiscid + whipper): MusicBrainz and CDDB use LBA+150 (the 150-frame
# lead-in pregap), AccurateRip uses RAW LBAs. The AccurateRip ids also yield the
# results-DB URL where a consumer fetches the expected per-track checksums +
# confidence (still no audio read — mos's TOC alone locates the record). The two
# AccurateRip ids cover AUDIO tracks only (a CD-Extra data track is excluded);
# freedb counts all tracks, as the CDDB spec does. MusicBrainz id needs openssl.
disc_ids() {                                # disc_ids <metadata-json>
    local mb=null
    have openssl && mb="\"$(discid_from_meta "$1")\""
    printf '%s' "$1" | jq -c --argjson mb "$mb" '
        def digitsum(n): (n|tostring)|explode|map(.-48)|add;
        def tohex8: . as $n | [range(0;8)]|reverse|map(($n/pow(16;.)|floor)%16)
                    | map(if .<10 then .+48 else .+87 end)|implode;
        .disc.toc as $t
        | [ $t.tracks[] | select(.data==false) ] as $au
        | (($t.tracks|map((.start_lba/75|floor)+2|digitsum(.))|add)) as $sum
        | (($t.leadout_lba/75|floor) - ($t.tracks[0].start_lba/75|floor)) as $total
        | (((($sum%255)*16777216) + ($total*256) + ($t.tracks|length)) % 4294967296 | tohex8) as $fd
        | ((($au|map(.start_lba)|add) + $t.leadout_lba) % 4294967296 | tohex8) as $a1
        | ((($au|map((if .start_lba==0 then 1 else .start_lba end) * .track)|add)
             + ($t.leadout_lba * (($au|length)+1))) % 4294967296 | tohex8) as $a2
        | { musicbrainz: $mb, freedb: $fd, accuraterip_id1: $a1, accuraterip_id2: $a2,
            accuraterip_url: ("http://www.accuraterip.com/accuraterip/"
              + $a1[7:8] + "/" + $a1[6:7] + "/" + $a1[5:6]
              + "/dBAR-" + (($au|length)|tostring|("00"+.)[-3:])
              + "-" + $a1 + "-" + $a2 + "-" + $fd + ".bin") }'
}

# mb_throttle — MusicBrainz hard-caps anonymous clients at 1 request/second.
# Sleep out the remainder of the last second before returning.
mb_throttle() {
    local now last
    now=$(date +%s)
    if [ -f "$MB_THROTTLE_STAMP" ]; then
        last=$(cat "$MB_THROTTLE_STAMP" 2>/dev/null || echo 0)
        [ "$now" -le "$last" ] && sleep 1
    fi
    date +%s >"$MB_THROTTLE_STAMP"
}

# mb_fetch <metadata-json> — resolve the release as JSON. Passing the raw TOC
# beside the disc ID lets MusicBrainz fuzzy-match a disc it doesn't yet know and
# hand back a submission URL; the TOC is the same offsets the id hashes. Echoes
# the JSON body on stdout (empty on miss/failure). Runs even in DRY_RUN (a
# read-only GET) so the resolved name is the REAL one.
mb_fetch() {                                # mb_fetch <metadata-json>
    local id toc url
    id=$(discid_from_meta "$1")
    toc=$(toc_query_string "$1")
    log "MusicBrainz disc id: $id"
    url="https://musicbrainz.org/ws/2/discid/$id?inc=$MB_INC&cdstubs=no&toc=$toc&fmt=json"
    mb_throttle
    curl -fsS -A "$MB_USER_AGENT" "$url" 2>/dev/null || true
}

# mb_release <mb-json> — TSV "artist<TAB>album<TAB>year<TAB>tracks" of the first
# release the lookup returned, or nothing. The artist-credit array is joined
# with its own joinphrases (so "A feat. B" reads correctly); the year is the
# leading 4 digits of the release date.
mb_release() {                              # mb_release <mb-json>
    [ -n "${1:-}" ] || return 1
    printf '%s' "$1" | jq -r '
        (.releases // [])[0] as $r
        | if $r == null then empty else
            [ ([ $r."artist-credit"[]? | (.name // "") + (.joinphrase // "") ] | add // "Unknown Artist"),
              ($r.title // "Unknown Album"),
              (($r.date // "")[0:4]),
              ([ $r.media[]?.tracks[]? ] | length | tostring) ]
            | @tsv
          end'
}

# mb_tracklist <mb-json> — "NN. Title  [m:ss]" lines from the first release.
mb_tracklist() {                            # mb_tracklist <mb-json>
    printf '%s' "$1" | jq -r '
        def pad2: tostring | if length < 2 then "0" + . else . end;
        (.releases // [])[0] as $r
        | $r.media[]?.tracks[]?
        | (if .length then (.length / 1000 | floor) else null end) as $s
        | "\(.position | pad2). \(.title // "?")"
          + (if $s != null then "  [\($s / 60 | floor):\($s % 60 | pad2)]" else "" end)'
}

# fetch_cover_art <mb-json> <outdir> — pull the front cover from the Cover Art
# Archive, keyed on the RELEASE MBID the MusicBrainz lookup returned (releases[0]
# .id). CAA /release/{mbid}/front-NNN is a 307 redirect to archive.org, so -L is
# required; a 404 means no art, where we fall back to the release-GROUP cover
# (many releases inherit art only at the group level). Saved as cover.jpg in the
# album dir (the sidecar headless servers read). Spotlight: the MBID is resolved
# from mos's TOC — the disc itself located its own artwork.
fetch_cover_art() {                         # fetch_cover_art <mb-json> <outdir>
    local relid rgid f="$2/cover.jpg"
    relid=$(jq -r '.releases[0].id // ""' <<<"$1")
    rgid=$(jq -r '.releases[0]["release-group"].id // ""' <<<"$1")
    [ -n "$relid" ] || return 0
    if [ "${DRY_RUN:-0}" = 1 ]; then log "would fetch cover art (release $relid) -> $f"; return 0; fi
    have curl || return 0
    if curl -fLs -A "$MB_USER_AGENT" -o "$f" "https://coverartarchive.org/release/$relid/front-$COVER_SIZE" && [ -s "$f" ]; then
        log "cover art -> $f"
    elif [ -n "$rgid" ] && curl -fLs -A "$MB_USER_AGENT" -o "$f" "https://coverartarchive.org/release-group/$rgid/front-$COVER_SIZE" && [ -s "$f" ]; then
        log "cover art (release-group) -> $f"
    else
        rm -f "$f" 2>/dev/null || true
        log "no cover art on the Cover Art Archive for this release"
    fi
}

# write_tags <mb-json> <outdir> — one portable tag file per track (NN.tags), in
# metaflac's KEY=value line format, ready for `metaflac --import-tags-from`. The
# field names are exactly what Picard/beets/Navidrome round-trip (Vorbis
# comments + the MUSICBRAINZ_* id set). Per-track ARTIST falls back to the album
# artist; a various-artists disc carries its own track artist-credit. Note
# MUSICBRAINZ_TRACKID holds the RECORDING mbid (historical), the per-release
# track id is MUSICBRAINZ_RELEASETRACKID. Empty-valued fields are dropped.
write_tags() {                              # write_tags <mb-json> <outdir>
    if [ "${DRY_RUN:-0}" = 1 ]; then log "would write per-track tags -> $2/NN.tags (metaflac --import-tags-from)"; return 0; fi
    local pos
    jq -r '.releases[0].media[]?.tracks[]?.position' <<<"$1" | while read -r pos; do
        [ -n "$pos" ] || continue
        jq -r --argjson pos "$pos" '
            .releases[0] as $r
            | ($r."artist-credit" | map(.name + (.joinphrase // "")) | add) as $aa
            | ($r.media[] | select(any(.tracks[]?; .position == $pos))) as $m
            | ($m.tracks[] | select(.position == $pos)) as $t
            | [ "TITLE="          + ($t.title // ""),
                "ARTIST="         + ((($t."artist-credit" // $r."artist-credit") | map(.name + (.joinphrase // "")) | add)),
                "ALBUM="          + ($r.title // ""),
                "ALBUMARTIST="    + $aa,
                "TRACKNUMBER="    + ($t.number // ($t.position | tostring)),
                "TOTALTRACKS="    + ($m."track-count" | tostring),
                "DISCNUMBER="     + ($m.position | tostring),
                "DATE="           + ($r.date // ""),
                "LABEL="          + (($r."label-info"[0].label.name) // ""),
                "CATALOGNUMBER="  + (($r."label-info"[0]."catalog-number") // ""),
                "BARCODE="        + ($r.barcode // ""),
                "MUSICBRAINZ_ALBUMID="        + ($r.id // ""),
                "MUSICBRAINZ_RELEASEGROUPID=" + (($r."release-group".id) // ""),
                "MUSICBRAINZ_RELEASETRACKID=" + ($t.id // ""),
                "MUSICBRAINZ_TRACKID="        + (($t.recording.id) // "")
              ] | map(select(endswith("=") | not)) | .[]' <<<"$1" \
            > "$(printf '%s/%02d.tags' "$2" "$pos")"
    done
    log "per-track tags -> $2/NN.tags (apply with: metaflac --import-tags-from=NN.tags track.flac)"
}

# cdtext_tracklist <metadata-json> — the disc's OWN tracklist from CD-TEXT, the
# offline fallback when MusicBrainz has no match. Sparse (only titled tracks).
cdtext_tracklist() {                        # cdtext_tracklist <metadata-json>
    printf '%s' "$1" | jq -r '
        (.disc.cdtext.tracks // [])[]?
        | select(.title != null or .performer != null)
        | "\(.track). \(.title // "")"
          + (if .performer then "  — \(.performer)" else "" end)'
}

# length_consistency_check <metadata-json> <mb-json> — cross-check the
# MusicBrainz release against the disc IN THE DRIVE. The disc id is an EXACT
# function of the TOC, so an id match is self-consistent — but a fuzzy `toc=`
# fallback match, or the wrong medium picked from a multi-disc set, can hand
# back a release whose track lengths don't fit this disc. We derive each track's
# length from the TOC (sector span / 75 frames-per-second) and compare it to
# MusicBrainz's track length (ms) within LENGTH_TOLERANCE seconds. A count or
# length mismatch is flagged rather than silently trusted. Echoes "ok" or
# "mismatch" on stdout; logs the offending tracks to stderr.
length_consistency_check() {               # <metadata-json> <mb-json>
    local meta="$1" mb="$2" n toc mb_secs
    n=$(printf '%s' "$meta" | jq '[.disc.toc.tracks[]?] | length')
    toc=$(printf '%s' "$meta" | jq -r '
        .disc.toc as $t | ($t.tracks | sort_by(.start_lba)) as $tr
        | [ range(0; ($tr|length)) as $i
            | { track: $tr[$i].track,
                sec: (((if $i+1 < ($tr|length) then $tr[$i+1].start_lba else $t.leadout_lba end)
                        - $tr[$i].start_lba) / 75) } ]
        | .[] | "\(.track)\t\(.sec)"')
    # Pick the medium whose track-count matches the disc; fall back to the first.
    mb_secs=$(printf '%s' "$mb" | jq -r --argjson n "$n" '
        (.releases // [])[0] as $r
        | (((($r.media // []) | map(select((.tracks|length) == $n)) | .[0]) // $r.media[0])) as $m
        | $m.tracks[]? | "\(.position)\t\(if .length then (.length / 1000) else -1 end)"' 2>/dev/null)
    [ -n "$mb_secs" ] || { echo ok; return 0; }   # no MB lengths -> nothing to check
    printf '%s\n@\n%s\n' "$toc" "$mb_secs" | awk -v tol="${LENGTH_TOLERANCE:-5}" '
        /^@$/ { side=2; next }
        side!=2 { toc[$1]=$2; ntoc++; next }
        { mb[$1]=$2; nmb++ }
        END {
            bad=0
            if (nmb>0 && ntoc!=nmb) {
                printf "[disc-ingest] WARN: track-count mismatch: TOC %d vs MusicBrainz %d\n", ntoc, nmb > "/dev/stderr"
                bad=1 }
            for (t in toc) {
                if (!(t in mb) || mb[t] < 0) continue
                d = toc[t] - mb[t]; if (d < 0) d = -d
                if (d > tol) {
                    printf "[disc-ingest] WARN: track %d length TOC %.0fs vs MusicBrainz %.0fs (delta %.0fs)\n", t, toc[t], mb[t], d > "/dev/stderr"
                    bad=1 } }
            print (bad ? "mismatch" : "ok")
        }'
}

# fingerprint <metadata-json> — sha256 of mos's CLOSED `disc` subtree, the
# uniform per-disc dedup key. The subtree is a fixed, sorted-by-jq key set
# designed to be hashed; it is the only stable identity for Blu-ray/data discs
# that carry no MusicBrainz/CDDB id.
fingerprint() { printf '%s' "$1" | jq -Sc '.disc' | sha256; }

# sha256 of stdin (or a file via redirection), hash only. macOS ships shasum.
sha256() { shasum -a 256 | awk '{print $1}'; }

# --- AccurateRip drive READ OFFSET, paired with the disc fingerprint ------
# The offset (the per-drive sample correction that makes an audio rip
# bit-identical across drives — AccurateRip/EAC/whipper) is NOT a value any
# drive reports: there is no MMC command, mode page, or IOKit property for it.
# It lives in AccurateRip's community DB, keyed on the drive IDENTITY mos DOES
# report. So mos ships the key (vendor/product); this is the consumer lookup —
# the boundary ROADMAP.md draws ("AccurateRip … permanently consumer-side").
# Source is the DiscImageCreator TSV mirror: "VENDOR - MODEL <TAB> offset <TAB>
# n <TAB> pct%", tab-delimited (no HTML parse, no bot-blocking — needs only curl).

# accuraterip_offset <drive-json> — echo this drive's AccurateRip read offset
# ("+6"), or nothing if the drive is unlisted. The DB is fetched once and cached.
accuraterip_offset() {
    local vendor product ar_vendor key
    vendor=$(printf '%s'  "$1" | jq -r '.vendor  // ""')
    product=$(printf '%s' "$1" | jq -r '.product // ""')
    [ -n "$product" ] || return 1
    # AccurateRip lists the normalized vendor, not the raw INQUIRY string.
    case "$vendor" in
        HL-DT-ST) ar_vendor="LG Electronics" ;;
        JLMS)     ar_vendor="Lite-ON" ;;
        Matshita) ar_vendor="Panasonic" ;;
        TSSTcorp) ar_vendor="Toshiba Samsung" ;;
        *)        ar_vendor="$vendor" ;;
    esac
    key="$ar_vendor - $product"                  # AR name column is "VENDOR - MODEL"
    if [ ! -s "$AR_DB_CACHE" ]; then
        curl -fsSL "$AR_OFFSET_URL" >"$AR_DB_CACHE" 2>/dev/null || return 2
        [ -s "$AR_DB_CACHE" ] || return 2
    fi
    awk -F'\t' -v key="$key" '
        function norm(s){ s=tolower(s); gsub(/[ \t]+/," ",s); gsub(/^ +| +$/,"",s); return s }
        BEGIN{ nkey=norm(key) } norm($1)==nkey { print $2; exit }' "$AR_DB_CACHE"
}

# offset_and_fingerprint <metadata-json> <drive-json> — the rip-log pair: the
# drive's AccurateRip read offset together with the disc's TOC fingerprint. The
# offset corrects the drive; the fingerprint identifies the disc.
offset_and_fingerprint() {
    local off="" fp
    fp=$(fingerprint "$1")
    if have curl; then off=$(accuraterip_offset "$2" || true); fi
    case "$off" in
        "")         log "AccurateRip offset: unlisted — measure: whipper offset find" ;;
        "[Purged]") log "AccurateRip offset: [Purged] — no stable offset; measure 3 key discs" ;;
        *)          log "AccurateRip offset: $off samples (confirm: whipper offset find -o $off)" ;;
    esac
    log "disc TOC fingerprint: $fp"
}

# drive_report <selector> — "what is known about THIS drive": mos's identity,
# the AccurateRip read offset (the ONE community DB that is machine-readable,
# auto-resolved), and the lookup KEYS + canonical URLs for the human-only DBs
# (LibreDrive/MakeMKV, UHD crossflash, redump CD compat) — all keyed on exactly
# the identity mos reports. Honest by design: those three forum/wiki sources
# publish no machine-readable list (and 403 bots), so the script hands you the
# key to paste, not a scraped answer. A coarse local table flags known UHD
# models; `makemkvcon` (if present) is the live LibreDrive oracle.
drive_report() {                            # drive_report <selector>
    local sel="$1" d vendor product revision serial fw inter caps off
    d=$("$MOS" drive "$sel" --json 2>/dev/null) || { warn "mos drive failed for $sel"; return 1; }
    if [ "$(jq -r '.schema // ""' <<<"$d")" = mos.error.v1 ]; then
        warn "$sel: $(jq -r '.error.code // "error"' <<<"$d")"; return 0
    fi
    vendor=$(jq -r '.vendor // "?"' <<<"$d");   product=$(jq -r '.product // "?"' <<<"$d")
    revision=$(jq -r '.revision // "?"' <<<"$d"); serial=$(jq -r '.serial // "(none)"' <<<"$d")
    fw=$(jq -r '.firmware_date // "?"' <<<"$d")
    inter=$(jq -r '"\(.interconnect // "?")/\(.interconnect_location // "?")"' <<<"$d")
    caps=$(jq -r '(.protection // {}) | [to_entries[] | select(.value != null and .value != false) | .key] | join(", ")' <<<"$d")

    printf 'Drive: %s %s   (rev %s, firmware %s)\n' "$vendor" "$product" "$revision" "$fw"
    printf '  serial: %s    interconnect: %s    protection: %s\n' "$serial" "$inter" "${caps:-none}"

    if have curl; then off=$(accuraterip_offset "$d" 2>/dev/null || true); fi
    case "${off:-}" in
        "") printf '  AccurateRip read offset: unlisted (measure: whipper offset find)\n' ;;
        *)  printf '  AccurateRip read offset: %s samples (auto-resolved)\n' "$off" ;;
    esac

    printf '  Lookup keys for the human-only DBs (no public machine-readable list — paste these):\n'
    printf '    LibreDrive/MakeMKV : "%s %s" rev %s      https://forum.makemkv.com/forum/viewforum.php?f=19\n' "$vendor" "$product" "$revision"
    printf '    UHD crossflash     : %s rev %s (fw %s)   https://forum.makemkv.com/forum/viewtopic.php?t=19634\n' "$product" "$revision" "$fw"
    printf '    redump CD compat   : %s %s rev %s        http://wiki.redump.org/index.php?title=Optical_Disc_Drive_Compatibility:_CD\n' "$vendor" "$product" "$revision"
    case "$product" in
        *WH16NS60*|*BU40N*)               printf '  hint: %s is a known UHD-Official model (firmware-dependent)\n' "$product" ;;
        *WH16NS40*|*WH14NS40*|*BW-16D1HT*) printf '  hint: %s is a known UHD-Friendly model (firmware-dependent)\n' "$product" ;;
        *BDR-2*|*BDR-S*|*BDR-X*)          printf '  hint: Pioneer drive — UHD crossflash needs firmware pre-~Dec 2022 (fw %s)\n' "$fw" ;;
    esac
    have makemkvcon && printf '  live LibreDrive status: makemkvcon -r --cache=1 info disc:9999\n'
    return 0
}

# --- makemkvcon -r robot-mode parsing -------------------------------------
# `makemkvcon -r info dev:RAW` emits line-based records (apdefs.h
# AP_ItemAttributeId; codes are stable across versions per the MakeMKV docs):
#   TCOUNT:n                       — number of rippable titles
#   CINFO:attr,code,"value"        — disc-level attribute  (2 = disc name)
#   TINFO:title,attr,code,"value"  — title-level attribute (9 dur, 8 chapters,
#                                    11 bytes, 27 default output filename)
# Values are quoted with \" and \\ escaped. We split only on the structural
# commas (a title can contain a comma) and normalize into a tab table the shell
# can read. mos says "this is a DVD/BD"; makemkvcon (running the disc's own
# BD-Java) is the authority on whether it's a MOVIE and what the titles are.

# makemkv_info <raw-dev> — run the info pass once, echo its raw robot output.
# Runs in DRY_RUN too (read-only) so the plan is real.
makemkv_info() {
    makemkvcon -r --cache=1 --minlength="$MINLENGTH" --noscan info "dev:$1" 2>/dev/null || true
}

# makemkv_disc_name <info> — the disc name (CINFO attr 2), unescaped.
makemkv_disc_name() {
    printf '%s\n' "$1" | sed -n 's/^CINFO:2,[0-9]*,"\(.*\)"$/\1/p' | head -n1 \
        | sed 's/\\"/"/g; s/\\\\/\\/g'
}

# makemkv_titles <info> — one TSV row per title:
#   title <TAB> seconds <TAB> H:MM:SS <TAB> chapters <TAB> bytes <TAB> outname <TAB> name
# Sorted by title index. Durations are converted to seconds for ranking. The
# title NAME (id 2) is last so MakeMKV's "FPL_MainFeature" rename — its verdict
# on which playlist is the real movie behind a fake-playlist obfuscated disc —
# can be honored in selection.
makemkv_titles() {
    printf '%s\n' "$1" | awk -F '\t' 'BEGIN{OFS="\t"}
        function unq(s){ sub(/^"/,"",s); sub(/"$/,"",s);
                         gsub(/\\"/,"\"",s); gsub(/\\\\/,"\\",s);
                         gsub(/\t/," ",s); return s }
        function dsec(d, n,p){ n=split(d,p,":");
                         if(n==3) return p[1]*3600+p[2]*60+p[3];
                         if(n==2) return p[1]*60+p[2]; return p[1]+0 }
        /^TINFO:/ {
            rest=substr($0,7)
            i=index(rest,","); t=substr(rest,1,i-1);   rest=substr(rest,i+1)
            i=index(rest,","); a=substr(rest,1,i-1);   rest=substr(rest,i+1)
            i=index(rest,","); v=unq(substr(rest,i+1))
            seen[t]=1
            if(a==2)       name[t]=v
            else if(a==9)  { dur[t]=v; sec[t]=dsec(v) }
            else if(a==8)  chap[t]=v
            else if(a==11) bytes[t]=v
            else if(a==27) out[t]=v
        }
        END {
            n=0; for(t in seen) idx[n++]=t+0
            for(i=1;i<n;i++){ k=idx[i]; j=i-1;
                while(j>=0 && idx[j]>k){ idx[j+1]=idx[j]; j-- } idx[j+1]=k }
            # "-" sentinels for the two optionally-empty columns: a bare empty
            # field mid-row would be swallowed by the shell read loop (tab is
            # IFS-whitespace, so adjacent tabs collapse). The loop maps "-" back.
            for(i=0;i<n;i++){ t=idx[i];
                print t, (sec[t]+0), (dur[t]=="" ? "?" : dur[t]),
                      (chap[t]=="" ? "0" : chap[t]),
                      (bytes[t]=="" ? "0" : bytes[t]),
                      (out[t]=="" ? "-" : out[t]), (name[t]=="" ? "-" : name[t]) }
        }'
}

# makemkv_check_messages <info> — scan robot MSG lines for the hard-failure
# signatures (AACS host-cert revoked, undecryptable volume key, open failure)
# and surface them verbatim. makemkvcon has no rich error enum — the MSG text +
# a non-zero exit are the signal — so we grep the info pass for the decisive
# phrases before committing to a multi-minute rip.
makemkv_check_messages() {
    printf '%s\n' "$1" | sed -n 's/^MSG:[0-9]*,[0-9]*,[0-9]*,"\(.*\)",".*$/\1/p' \
        | grep -iE 'revoked|can.t be decrypted|failed to open|aacs' \
        | while IFS= read -r m; do warn "makemkvcon: $m"; done || true
}

# predict_protection <metadata-json> <drive-json> — from the disc's protection
# (mos metadata copyright) AND the DRIVE's protection CAPABILITY (mos drive),
# predict whether makemkvcon can decrypt BEFORE a multi-minute scan. An AACS
# disc on a drive that advertises no AACS (a plain DVD/BD-ROM unit, or a non-UHD
# drive on a UHD disc) is the fail-fast case; a CSS disc on a CSS-capable drive
# is green-lit. This is the spotlight: mos's drive-capability JSON gives a guard
# makemkvcon alone cannot. BD+ and UHD/AACS-2.0 are deliberately invisible to
# mos (disc-side VM / vendor microcode), so this predicts only to the standard-
# MMC line. Echoes "ok" or "block:<reason>".
predict_protection() {
    local meta="$1" drive="$2" prot class d_aacs d_css
    prot=$(jq -r '.disc.disc_structure.copyright.protection_name // ""' <<<"$meta")
    class=$(jq -r '.disc.media_class // ""' <<<"$meta")
    d_aacs=$(jq -r 'if .protection.aacs == null then "no" else "yes" end' <<<"$drive")
    d_css=$(jq -r 'if .protection.css == null then "no" else "yes" end' <<<"$drive")
    case "$prot" in
        aacs)
            [ "$d_aacs" = yes ] && echo ok \
                || echo "block:disc is AACS but the drive advertises no AACS capability (mos drive .protection.aacs is null) — wrong or incapable drive" ;;
        css_cppm|cprm)
            [ "$d_css" = yes ] && echo ok \
                || echo "block:disc is CSS/CPRM but the drive shows no CSS capability — likely the wrong drive" ;;
        *)
            if [ "$class" = bd ] && [ "$d_aacs" = no ]; then
                echo "block:Blu-ray video is typically AACS but the drive advertises no AACS capability"
            else echo ok; fi ;;
    esac
}

# --- Inventory / sidecars / tray ------------------------------------------

# inventory_append <metadata-json> <drive-json> <action> [extra-json] — one JSONL
# row per disc, keyed by the drive's durable serial (survives replug; registry_id
# does not) and the disc fingerprint. This is the catalog/dedup substrate. An
# optional 4th arg is a JSON object merged in (e.g. the resolved title).
inventory_append() {
    [ -n "$INVENTORY" ] || return 0
    local row extra="${4:-}"; [ -n "$extra" ] || extra='{}'
    row=$(jq -nc \
        --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        --arg action "$3" \
        --arg fp "$(fingerprint "$1")" \
        --argjson meta "$1" --argjson drive "$2" --argjson extra "$extra" '{
            ts: $ts, action: $action, fingerprint: $fp,
            serial:      ($drive.serial // null),
            product:     ($drive.product // null),
            class:       ($meta.disc.media_class // null),
            profile:     ($meta.disc.current_profile // null),
            volume_name: ($meta.disc.volume_name // null),
            manufacturer:       ($meta.disc.disc_structure.manufacturer_id // null)
        } + $extra')
    if [ "${DRY_RUN:-0}" = 1 ]; then log "inventory += $row"
    else printf '%s\n' "$row" >>"$INVENTORY"; fi
}

# write_manifest <metadata-json> <drive-json> <dir> <action> — the per-rip
# provenance record written INSIDE a rip directory (audio/movie): mos's full
# metadata + drive identity + fingerprint + a sha256 of EVERY output file in
# the dir (the redumper .bin/.cue/.log, the .mkv titles, the tracklist). This
# is the directory twin of write_sidecar's beside-the-.iso record, so all three
# rip branches leave the same checksummed provenance.
write_manifest() {
    local meta="$1" drive="$2" dir="$3" action="$4"
    if [ "${DRY_RUN:-0}" = 1 ]; then log "manifest -> $dir/manifest.json (sha256 of each output)"; return 0; fi
    [ -d "$dir" ] || return 0
    # hash every regular file in the dir except the manifest we're writing. This
    # is post-rip provenance: best-effort, and must never abort a finished rip —
    # so stat/sha failures degrade to 0/"" and the whole thing is guarded.
    local files
    files=$( cd "$dir" && find . -type f ! -name manifest.json -print0 \
        | while IFS= read -r -d '' f; do
              f=${f#./}
              jq -nc --arg name "$f" \
                     --argjson bytes "$(stat -f%z "$f" 2>/dev/null || echo 0)" \
                     --arg sha "$(sha256 <"$f" 2>/dev/null || true)" \
                     '{name: $name, bytes: $bytes, sha256: $sha}'
          done | jq -sc '.' ) || files='[]'
    jq -nc --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" --arg action "$action" \
        --arg fp "$(fingerprint "$meta")" --argjson files "${files:-[]}" \
        --argjson meta "$meta" --argjson drive "$drive" \
        '{captured_at: $ts, action: $action, fingerprint: $fp,
          files: $files, metadata: $meta, drive: $drive}' >"$dir/manifest.json" \
        || warn "manifest write failed for $dir (rip is intact)"
    return 0
}

# write_sidecar <metadata-json> <drive-json> <image-path> — the archivist's
# provenance record beside the image: mos's full metadata + drive identity +
# the image's own sha256 (skipped in DRY_RUN; it would read the whole image).
write_sidecar() {
    local sha="(dry-run)"
    if [ "${DRY_RUN:-0}" != 1 ] && [ -f "$3" ]; then sha=$(sha256 <"$3"); fi
    local doc
    doc=$(jq -nc --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" --arg sha "$sha" \
        --argjson meta "$1" --argjson drive "$2" \
        '{captured_at: $ts, image_sha256: $sha, metadata: $meta, drive: $drive}')
    if [ "${DRY_RUN:-0}" = 1 ]; then log "sidecar -> $3.mos.json"
    else printf '%s\n' "$doc" >"$3.mos.json"; fi
}

# write_text_file <path> — write a small sidecar (tracklist, manifest, marker)
# from stdin, honoring DRY_RUN.
write_text_file() {
    if [ "${DRY_RUN:-0}" = 1 ]; then log "write -> $1"; cat >/dev/null
    else cat >"$1"; fi
}

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

main "$@"
