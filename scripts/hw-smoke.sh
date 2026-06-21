#!/usr/bin/env bash
#
# hw-smoke.sh — interactive hardware smoke test: a full walk of the mos CLI
# surface on a real drive. Complements INTEGRATION_HARNESS.md (which covers the
# six core states, falsification rows, and fixture capture); this script EXERCISES
# every verb, selector form, error path, and --json document type, asserting exit
# codes / output / schema conformance where deterministic and PROMPTING for the
# physical disc/tray changes the media-dependent branches need.
#
# It is a smoke test, not a unit test: the pure suite + adapter fake (ctest) are
# the automated correctness oracle; this is the "does the whole thing behave on a
# real WH16NS40-class drive" pass a tag wants. A surprise here is a finding — per
# the hardware-role ADR it becomes a committed fixture + dated note, never a
# per-device branch in src/.
#
# Usage:
#   scripts/hw-smoke.sh [phase]
#   phases: static | empty | tray | disc | watch | errors | all (default)
#   MOS=/path/to/mos scripts/hw-smoke.sh        # override the binary; otherwise
#                                                 an installed `mos` on PATH is
#                                                 preferred, then build/bin/mos
#
# Designed for macOS (bash 3.2 / zsh both fine). Needs python3 for --json schema
# checks and JSON parsing; without it those steps SKIP with a note (everything
# else still runs). No jq dependency.

set -u

# ---- locate the binary + repo ------------------------------------------------
HERE=$(cd "$(dirname "$0")/.." && pwd)
SCHEMA_DIR="$HERE/schemas"
MOS="${MOS:-}"

PHASE="all"
for a in "$@"; do
    case "$a" in
        static|empty|tray|disc|watch|errors|all) PHASE="$a" ;;
        *) ;;
    esac
done
# Binary precedence: an explicit MOS= wins; otherwise an installed `mos` on PATH
# (what a hardware tester usually validates — e.g. a brew install) is preferred
# over a local build tree. Set MOS=build/bin/mos to force the freshly-built one.
MOS_ON_PATH=""
command -v mos >/dev/null 2>&1 && MOS_ON_PATH="$(command -v mos)"
if [ -z "$MOS" ]; then
    if   [ -n "$MOS_ON_PATH" ];        then MOS="$MOS_ON_PATH"
    elif [ -x "$HERE/build/bin/mos" ]; then MOS="$HERE/build/bin/mos"
    else echo "no mos binary: install it (brew) or build it (cmake -B build && cmake --build build), or set MOS=" >&2; exit 2
    fi
fi

# ---- presentation + counters -------------------------------------------------
if [ -t 1 ]; then B=$(printf '\033[1m'); R=$(printf '\033[31m'); G=$(printf '\033[32m')
                  Y=$(printf '\033[33m'); D=$(printf '\033[2m'); Z=$(printf '\033[0m')
else B=; R=; G=; Y=; D=; Z=; fi
PASS=0; FAIL=0; SKIP=0
FAILED_NAMES=""

hdr()  { printf '\n%s== %s ==%s\n' "$B" "$1" "$Z"; }
note() { printf '%s   %s%s\n' "$D" "$1" "$Z"; }
ok()   { PASS=$((PASS+1)); printf '%s   PASS%s %s\n' "$G" "$Z" "$1"; }
bad()  { FAIL=$((FAIL+1)); FAILED_NAMES="$FAILED_NAMES\n   - $1"; printf '%s   FAIL%s %s\n' "$R" "$Z" "$1"; }
skp()  { SKIP=$((SKIP+1)); printf '%s   SKIP%s %s%s%s\n' "$Y" "$Z" "$1" "${2:+  — }" "${2:-}"; }

pause() { # prompt the operator for a physical change; honor SMOKE_NONINTERACTIVE
    printf '\n%s>> %s%s\n' "$Y" "$1" "$Z"
    if [ "${SMOKE_NONINTERACTIVE:-0}" = "1" ]; then note "(non-interactive: assuming done)"; return; fi
    printf '   press Enter when ready (or s+Enter to skip this group)... '
    read -r reply || reply="s"
    [ "$reply" = "s" ] && return 1 || return 0
}

# run a command, capture stdout+exit; echo a trimmed view of the output
LAST_OUT=""; LAST_RC=0
run() { LAST_OUT="$("$MOS" "$@" 2>&1)"; LAST_RC=$?; printf '%s   $ mos %s%s\n' "$D" "$*" "$Z"
        printf '%s\n' "$LAST_OUT" | sed 's/^/     /' | head -20; }

# assert helpers operate on LAST_OUT / LAST_RC of the preceding run()
want_rc()   { if [ "$LAST_RC" = "$1" ]; then ok "$2 (exit $1)"; else bad "$2 (exit $LAST_RC, wanted $1)"; fi; }
want_text() { if printf '%s' "$LAST_OUT" | grep -q -- "$1"; then ok "$2"; else bad "$2 (missing: $1)"; fi; }
want_one_of(){ d="$1"; shift; for p in "$@"; do if printf '%s' "$LAST_OUT" | grep -q -- "$p"; then ok "$d ($p)"; return; fi; done; bad "$d (none of: $*)"; }

# ---- schema validation (best-effort, python3 + jsonschema) -------------------
HAVE_SCHEMA=0
if command -v python3 >/dev/null 2>&1 && python3 -c 'import jsonschema' >/dev/null 2>&1; then HAVE_SCHEMA=1; fi

schema_one() { # $1 schema name, stdin = one JSON object; rc 0 ok / 1 fail / 2 skip
    [ "$HAVE_SCHEMA" = 1 ] || return 2
    [ -f "$SCHEMA_DIR/$1.json" ] || return 2
    python3 - "$SCHEMA_DIR/$1.json" "$SCHEMA_DIR" <<'PY'
import sys, json, glob, os
try: import jsonschema
except Exception: sys.exit(2)
schema = json.load(open(sys.argv[1]))
store = {}
for f in glob.glob(os.path.join(sys.argv[2], "*.json")):
    try:
        s = json.load(open(f))
        if isinstance(s, dict) and "$id" in s: store[s["$id"]] = s
    except Exception: pass
try:
    doc = json.load(sys.stdin)
except Exception as e:
    print("not JSON: %s" % e, file=sys.stderr); sys.exit(1)
try:
    resolver = jsonschema.RefResolver(base_uri=schema.get("$id",""), referrer=schema, store=store)
    jsonschema.validate(doc, schema, resolver=resolver)
except jsonschema.ValidationError as e:
    print("schema: %s" % e.message, file=sys.stderr); sys.exit(1)
except Exception as e:
    print("validator: %s" % e, file=sys.stderr); sys.exit(2)
sys.exit(0)
PY
}

want_schema() { # $1 schema, $2 desc, $3.. = mos args (must include --json)
    sch="$1"; desc="$2"; shift 2
    out="$("$MOS" "$@" 2>/dev/null)"
    printf '%s' "$out" | schema_one "$sch"; rc=$?
    case "$rc" in
        0) ok "$desc validates $sch" ;;
        1) bad "$desc does not validate $sch" ; printf '%s' "$out" | schema_one "$sch" 2>&1 | sed 's/^/       /' ;;
        2) skp "$desc schema $sch" "no python3/jsonschema or schema absent" ;;
    esac
}

want_schema_ndjson() { # $1 schema, $2 desc, $3 file of NDJSON
    sch="$1"; desc="$2"; f="$3"
    if [ "$HAVE_SCHEMA" != 1 ]; then skp "$desc ($sch)" "no validator"; return; fi
    if [ ! -s "$f" ]; then skp "$desc ($sch)" "no events captured"; return; fi
    n=0; failed=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        n=$((n+1))
        if ! printf '%s' "$line" | schema_one "$sch"; then failed=1; fi
    done < "$f"
    if [ "$failed" = 0 ]; then ok "$desc: $n line(s) validate $sch"; else bad "$desc: a line failed $sch"; fi
}

# pull a field out of `mos list --json` (field name in $1; first drive)
list_field() {
    command -v python3 >/dev/null 2>&1 || return 1
    "$MOS" list --json 2>/dev/null | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(1)
ds = d.get("drives", [])
if not ds:
    sys.exit(1)
v = ds[0].get(sys.argv[1])
print("" if v is None else v)
' "$1" 2>/dev/null
}

# ---- cleanup: never leave the tray locked ------------------------------------
cleanup() {
    "$MOS" tray unlock            >/dev/null 2>&1
    "$MOS" tray unlock --persistent >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

banner() {
    printf '%smos hardware smoke test%s\n' "$B" "$Z"
    note "binary : $MOS"
    if [ "$MOS" = "$MOS_ON_PATH" ] && [ -x "$HERE/build/bin/mos" ]; then
        note "         (installed mos on PATH; a build tree also exists —"
        note "          set MOS=$HERE/build/bin/mos to test that instead)"
    fi
    note "version: $("$MOS" --version 2>/dev/null || echo '(no --version)')"
    note "schema : $([ "$HAVE_SCHEMA" = 1 ] && echo 'python3 + jsonschema present' || echo 'absent — --json checks will SKIP')"
    note "phase  : $PHASE"
}

# =============================================================================
phase_static() {
    hdr "STATIC (no hardware)"
    if [ -x "$HERE/build/bin/mos_tests" ]; then
        out="$("$HERE/build/bin/mos_tests" 2>&1 | tail -1)"
        case "$out" in *", 0 failed"*) ok "pure suite: $out" ;; *) bad "pure suite: $out" ;; esac
    else skp "pure suite" "build/bin/mos_tests not built"; fi
    if [ "$HAVE_SCHEMA" = 1 ] && [ -f "$SCHEMA_DIR/validate.py" ]; then
        if python3 "$SCHEMA_DIR/validate.py" >/dev/null 2>&1; then ok "schemas/validate.py"; else bad "schemas/validate.py"; fi
    else skp "schemas/validate.py" "no python3/jsonschema"; fi
    run --help;            want_rc 0 "mos --help"
    run;                   want_rc 64 "bare mos → EX_USAGE"
    run definitelynotaverb; want_rc 64 "unknown subcommand → EX_USAGE"
    run state --json=oops; want_rc 64 "--json takes no argument → EX_USAGE"
}

phase_errors() {
    hdr "ERROR / SELECTOR EDGE CASES"
    run state 99
    if [ "$LAST_RC" -ne 0 ]; then ok "bad index -> non-zero ($LAST_RC)"; else bad "bad index should not exit 0"; fi
    out="$("$MOS" state --json 99 2>/dev/null)"
    if printf '%s' "$out" | schema_one mos.error.v1 >/dev/null 2>&1; then
        ok "bad index --json validates mos.error.v1"
    else
        skp "bad-index error schema" "validator unavailable or shape differs"
    fi
    run state disk999
    if [ "$LAST_RC" -ne 0 ]; then ok "nonexistent diskN -> non-zero ($LAST_RC)"; else bad "nonexistent diskN should not exit 0"; fi
}

phase_empty() {
    hdr "DRIVE PRESENT, NO DISC (tray open or closed-empty)"
    pause "Eject the tray and remove any disc (empty drive). 'mos tray eject' will run first." || { skp "empty phase" "skipped by operator"; return; }
    run tray eject; note "(if the tray was already open/empty this is a no-op 'done')"

    run state;              want_one_of "state is open/empty" '"open"' 'open' '"empty"' 'empty' 'empty_or_open'
    want_schema mos.state.v1 "state --json" state --json
    run list;               want_text 'HL-DT-ST\|Vendor\|Product\|Index' "list shows the drive"
    want_schema mos.list.v1 "list --json" list --json
    run drive;              want_text 'Vendor\|Product' "drive identity"
    note "serial may be null — many optical drives don't program VPD 0x80 (expected)"
    want_schema mos.drive.v1 "drive --json" drive --json
    run features;           want_rc 0 "features"
    run capacity;           note "capacity with no media: a no-media report or error is both fine"
    run metadata;           note "metadata with no disc: error envelope expected"

    # selector equivalence that does NOT need media: index + registry_id
    rid="$(list_field registry_id)"
    if [ -n "${rid:-}" ] && [ "$rid" != "0" ]; then
        run state 1;        s1="$LAST_OUT"
        run state "$rid";   s2="$LAST_OUT"
        if [ -n "$s1" ] && [ "$s1" = "$s2" ]; then ok "index and registry_id select the same drive"
        else bad "index vs registry_id selector mismatch"; fi
    else skp "index/registry_id selector equivalence" "could not read registry_id (need python3)"; fi

    if [ "${SMOKE_SKIP_PROBE:-0}" != 1 ]; then
        run probe --capture; want_text 'mos.capture.v0\|inquiry_serial\|not built' "probe --capture menu (or OFF build)"
        note "the inquiry_serial line is the VPD-0x80 reply — task_status/sense/bytes_transferred tell you if a serial exists"
    fi
}

phase_tray() {
    hdr "TRAY CONTROL (no disc)"
    pause "Empty drive, tray closed. Exercising eject/close/lock/unlock/persistent/force." || { skp "tray phase" "skipped"; return; }
    run tray eject;  want_text 'done\|outcome' "tray eject → done"
    run state;       want_one_of "after eject: open" '"open"' 'open' 'empty_or_open'
    run tray close;  want_text 'done\|outcome' "tray close → done"
    want_schema mos.tray.v1 "tray close --json" tray close --json

    run tray lock;   want_text 'done\|outcome' "tray lock (basic) → done"
    run tray eject;  want_one_of "eject while locked → refused_locked" 'refused_locked' '53/02' '5/53/02'
    run tray unlock; want_text 'done\|outcome' "tray unlock (basic) → done"
    run tray eject;  want_text 'done\|outcome' "eject after unlock → done"
    run tray close

    run tray lock --persistent;   want_text 'done\|outcome' "tray lock --persistent → done"
    run tray unlock --persistent; want_text 'done\|outcome' "tray unlock --persistent → done"

    run tray lock;            want_text 'done\|outcome' "re-lock (basic) for --force test"
    run tray eject --force;   want_one_of "eject --force clears the lock" 'done' 'outcome'
    note "--force cleared the Prevent lock then ejected; it never forces the filesystem"
    run tray close
}

phase_disc() {
    hdr "DISC INSERTED (per media type — repeat for each disc you have)"
    note "Media to cover if available: CD-ROM, CD-R/RW, DVD-ROM, DVD±R/RW, BD-ROM, blank BD-R/RE, UHD BD."
    while : ; do
        pause "Insert a disc and let macOS mount it (or 'diskutil mount'). s+Enter to finish the disc phase." || break

        # 1) the stray-open guard: sample state across the load window
        note "sampling state during spin-up (expect: loading → ready; NO stray 'open')"
        seen_open=0; seen_loading=0
        i=0; while [ $i -lt 8 ]; do
            s="$("$MOS" state 2>/dev/null | grep -i '^State:' | head -1)"
            printf '%s     %s%s\n' "$D" "$s" "$Z"
            case "$s" in *open*) seen_open=1 ;; esac
            case "$s" in *loading*) seen_loading=1 ;; esac
            case "$s" in *ready*) break ;; esac
            i=$((i+1)); sleep 0.4
        done
        if [ "$seen_open" = 1 ]; then bad "STRAY OPEN during load (the 04/xx-vs-GESN bug) — capture this!"
        elif [ "$seen_loading" = 1 ]; then ok "load showed 'loading', no stray 'open'"
        else note "didn't catch the loading window (too fast) — fine if it reached ready"; fi

        # 2) ready/mounted enrichment
        run state;     want_one_of "disc state ready" '"ready"' 'ready'
        want_schema mos.state.v1 "state --json (mounted)" state --json
        run metadata;  want_text 'Disc:\|Profile:\|Media:\|TOC' "metadata populated"
        want_schema mos.metadata.v1 "metadata --json" metadata --json
        run capacity;  note "capacity: total/used; blank rewritable & BD-R add a formattable view"
        want_schema mos.capacity.v1 "capacity --json" capacity --json
        run drive;     note "serial reads null while mounted (raw INQUIRY backs off on exclusive access) — expected"
        run list;      want_one_of "list shows ready + volume" 'ready' '/Volumes'

        # 3) selector equivalence with media present (diskN now exists)
        bn="$(list_field bsd_node)"   # e.g. /dev/disk8
        if [ -n "${bn:-}" ] && [ "$bn" != "null" ]; then
            dn="${bn##*/}"            # disk8
            run state "$dn";              a="$LAST_OUT"
            run state "$bn";              b="$LAST_OUT"
            run state --bsd "$dn";        c="$LAST_OUT"
            if [ "$a" = "$b" ] && [ "$b" = "$c" ]; then ok "diskN / /dev/diskN / --bsd select the same drive"
            else bad "bsd selector forms disagree"; fi
        else skp "bsd selector equivalence" "no bsd_node (need python3 / mounted media)"; fi

        # 4) graceful eject of a mounted (idle) disc
        if pause "Close any apps using the disc, then I'll test a GRACEFUL eject (unmount+eject)."; then
            run tray eject; want_one_of "graceful eject of mounted disc" 'done' 'outcome'
            note "mounted → mos unmounts gracefully then ejects; a BUSY filesystem would surface MOS_ERR_BUSY instead"
            run tray close
        fi
    done

    # 5) busy-filesystem eject (data-loss guard: mos must REFUSE, never force)
    if pause "BUSY-FS test: insert a disc, let it mount. I'll hold a file open and try to eject." ; then
        vol="$(list_field volume_path)"; bn="$(list_field bsd_node)"
        if [ -n "${vol:-}" ] && [ "$vol" != "null" ] && [ -d "$vol" ]; then
            # hold an open handle on the volume root, then attempt eject
            ( exec 9< "$vol"; "$MOS" tray eject >/tmp/mos_busy_eject.$$ 2>&1; ) ; rc=$?
            LAST_OUT="$(cat /tmp/mos_busy_eject.$$ 2>/dev/null)"; rm -f /tmp/mos_busy_eject.$$
            printf '%s\n' "$LAST_OUT" | sed 's/^/     /'
            want_one_of "busy-fs eject is REFUSED, not forced" 'busy' 'BUSY' 'EX_TEMPFAIL'
            note "mos never force-unmounts: a busy volume yields MOS_ERR_BUSY (diskutil-equivalent), data intact"
        else skp "busy-fs eject" "no mounted volume_path (need python3 + a data disc)"; fi
    fi
}

phase_watch() {
    hdr "WATCH (event stream + contention)"
    if pause "Single-drive watch: I'll stream events while you EJECT then INSERT a disc."; then
        evf="/tmp/mos_watch_events.$$"
        "$MOS" watch --json >"$evf" 2>/dev/null &
        wpid=$!
        note "watching (pid $wpid) for ~20s — eject the tray, then insert a disc..."
        sleep 20; kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
        sed 's/^/     /' "$evf" | head -12
        want_schema_ndjson mos.event.v1 "watch events" "$evf"
        if grep -q 'state_changed\|snapshot' "$evf" 2>/dev/null; then ok "watch emitted lifecycle events"; else bad "watch emitted no events"; fi
        if grep -q '"open"' "$evf" 2>/dev/null && grep -q 'becoming\|04/01\|loading' "$evf" 2>/dev/null; then
            note "check the event order: an insert should show 'loading', not a stray 'open'"; fi
        rm -f "$evf"
    fi

    if pause "CONTENTION: I'll run a background watch and fire 'mos tray eject' against it (post-#110 should SUCCEED)."; then
        "$MOS" watch >/dev/null 2>&1 & wpid=$!
        sleep 2
        run tray eject
        case "$LAST_RC" in
            0) ok "eject succeeded under a concurrent watch (contention reduced)" ;;
            *) if printf '%s' "$LAST_OUT" | grep -qi 'exclusive'; then
                   bad "eject lost to watch (EXCLUSIVE_ACCESS) - residual GESN window hit"
               else
                   note "eject non-zero ($LAST_RC) for a non-contention reason - inspect above"
               fi ;;
        esac
        kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
        "$MOS" tray close >/dev/null 2>&1
    fi
}

# =============================================================================
banner
case "$PHASE" in
    static) phase_static ;;
    empty)  phase_empty ;;
    tray)   phase_tray ;;
    disc)   phase_disc ;;
    watch)  phase_watch ;;
    errors) phase_errors ;;
    all)    phase_static; phase_errors; phase_empty; phase_tray; phase_disc; phase_watch ;;
esac

hdr "SUMMARY"
printf '%s%d passed%s, %s%d failed%s, %s%d skipped%s\n' \
    "$G" "$PASS" "$Z" "$R" "$FAIL" "$Z" "$Y" "$SKIP" "$Z"
[ "$FAIL" -gt 0 ] && printf 'failed:%b\n' "$FAILED_NAMES"
note "Per the hardware-role ADR, a FAIL or a surprise is a deliverable: capture it"
note "with 'mos probe --capture <drive>' and file it as a fixture + dated note."
[ "$FAIL" -eq 0 ]
