#!/usr/bin/env bash
#
# hw-smoke.sh — interactive hardware smoke test for the mos CLI.
#
# What this is: a guided, MENU-DRIVEN walk of the whole CLI surface on a real
# drive. You pick a section, the script tells you exactly what to do physically
# (empty the tray, insert a CD, …), runs the relevant commands, and reports a
# concise PASS/FAIL for each — adapting its assertions to whatever disc/tray
# state it actually finds rather than demanding a fixed script.
#
# What this is NOT: the automated correctness oracle. The pure suite + adapter
# fake (`ctest`, `build/bin/mos_tests`) own that. This is the "does the whole
# thing behave on a real WH16NS60-class drive" pass a tag wants. Per the
# hardware-role ADR, a surprise here is a DELIVERABLE: capture it with
# `mos probe --capture <drive>` and file it as a fixture + dated note — never a
# per-device branch in src/.
#
# Usage:
#   scripts/hw-smoke.sh                 # interactive menu (recommended)
#   scripts/hw-smoke.sh <section>       # run one section non-interactively
#       sections: static empty tray disc watch errors all
#   MOS=/path/to/mos scripts/hw-smoke.sh         # force a specific binary
#   SMOKE_NONINTERACTIVE=1 scripts/hw-smoke.sh all   # assume "ready" at prompts
#   SMOKE_INSTALL_DEPS=0 scripts/hw-smoke.sh     # don't auto-install python deps
#
# macOS-targeted (bash 3.2 / zsh both fine). The --json schema checks want
# python3 + jsonschema + rfc3339-validator (the same stack CI uses,
# schemas/requirements-ci.txt); they SKIP cleanly when those are absent. No jq.

set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
SCHEMA_DIR="$HERE/schemas"

# ---- binary selection --------------------------------------------------------
# Precedence: an explicit MOS= wins; otherwise the freshly-built build/bin/mos
# (what you are validating for a tag) is preferred over an installed `mos` on
# PATH. The old script preferred PATH, which silently tested a STALE brew binary
# against the current repo's schemas — the surest way to a screen of false
# schema failures. We surface the alternative instead of choosing it silently.
MOS_ON_PATH=""; command -v mos >/dev/null 2>&1 && MOS_ON_PATH="$(command -v mos)"
MOS_BUILT="$HERE/build/bin/mos"
MOS="${MOS:-}"
if [ -z "$MOS" ]; then
    if   [ -x "$MOS_BUILT" ]; then MOS="$MOS_BUILT"
    elif [ -n "$MOS_ON_PATH" ]; then MOS="$MOS_ON_PATH"
    else
        echo "no mos binary found." >&2
        echo "  build it:  cmake -B build && cmake --build build" >&2
        echo "  or install it (brew), or pass MOS=/path/to/mos" >&2
        exit 2
    fi
fi

# ---- presentation ------------------------------------------------------------
if [ -t 1 ]; then
    B=$(printf '\033[1m'); R=$(printf '\033[31m'); G=$(printf '\033[32m')
    Y=$(printf '\033[33m'); C=$(printf '\033[36m'); D=$(printf '\033[2m'); Z=$(printf '\033[0m')
else B=; R=; G=; Y=; C=; D=; Z=; fi

PASS=0; FAIL=0; SKIP=0; FAILED_NAMES=""

hdr()  { printf '\n%s━━ %s %s\n' "$B$C" "$1" "$Z"; }
info() { printf '%s   %s%s\n' "$D" "$*" "$Z"; }
ok()   { PASS=$((PASS+1)); printf '   %sPASS%s %s\n' "$G" "$Z" "$1"; }
bad()  { FAIL=$((FAIL+1)); FAILED_NAMES="$FAILED_NAMES\n   - $1"; printf '   %sFAIL%s %s\n' "$R" "$Z" "$1"; }
skp()  { SKIP=$((SKIP+1)); printf '   %sSKIP%s %s%s%s\n' "$Y" "$Z" "$1" "${2:+  — }" "${2:-}"; }

# step "physical instruction" → 0 proceed, 1 skip-this-section.
# Enter = proceed, s = skip section, q = quit to menu/exit. Honors
# SMOKE_NONINTERACTIVE (always proceeds).
step() {
    printf '\n%s➤  %s%s\n' "$B$Y" "$1" "$Z"
    if [ "${SMOKE_NONINTERACTIVE:-0}" = 1 ]; then info "(non-interactive: proceeding)"; return 0; fi
    printf '   %s[Enter] go   [s] skip   [q] quit%s ' "$D" "$Z"
    read -r r || r="q"
    case "$r" in s|S) return 1 ;; q|Q) return 2 ;; *) return 0 ;; esac
}

# ---- command runner: one invocation, displayed and captured ------------------
OUT=""; RC=0
run() {  # run <mos-args...>; fills OUT/RC and prints a compact view
    OUT="$("$MOS" "$@" 2>&1)"; RC=$?
    printf '%s   $ mos %s%s\n' "$D" "$*" "$Z"
    printf '%s\n' "$OUT" | sed 's/^/       /' | head -14
}

expect_rc()   { if [ "$RC" = "$1" ]; then ok "$2"; else bad "$2 (exit $RC, wanted $1)"; fi; }
# ERE so `a|b` alternation is portable across GNU and BSD (macOS) grep.
expect_text() { if printf '%s' "$OUT" | grep -Eqi -- "$1"; then ok "$2"; else bad "$2 (missing /$1/)"; fi; }
expect_nonzero() { if [ "$RC" -ne 0 ]; then ok "$1 (exit $RC)"; else bad "$1 (expected non-zero exit)"; fi; }
expect_any() {  # expect_any "desc" pat1 pat2 ...; PASS if any pattern matches OUT
    d="$1"; shift
    for p in "$@"; do printf '%s' "$OUT" | grep -qi -- "$p" && { ok "$d"; return; }; done
    bad "$d (none of: $*)"
}

# ---- schema validation (python3 + jsonschema, matching CI) -------------------
# Schemas are self-contained, so we validate each document against its single
# schema file with Draft202012Validator + FormatChecker — the exact mechanism
# schemas/validate.py and CI use (no deprecated RefResolver).
HAVE_SCHEMA=0; VALIDATOR=""; FIELD=""
have_deps() {
    command -v python3 >/dev/null 2>&1 \
        && python3 -c 'import jsonschema, rfc3339_validator' >/dev/null 2>&1
}
bootstrap_deps() {
    [ "${SMOKE_INSTALL_DEPS:-1}" = 1 ] || return 0
    have_deps && return 0
    command -v python3 >/dev/null 2>&1 || {
        info "python3 not found — --json schema checks will SKIP"; return 0; }
    req="$SCHEMA_DIR/requirements-ci.txt"
    info "installing jsonschema + rfc3339-validator (schemas/requirements-ci.txt)…"
    pin() { python3 -m pip install "$@" \
              || python3 -m pip install --user "$@" \
              || python3 -m pip install --break-system-packages "$@"; }
    if [ -f "$req" ]; then
        pin --require-hashes -r "$req" >/dev/null 2>&1 \
            || pin jsonschema rfc3339-validator >/dev/null 2>&1 \
            || info "install failed — schema checks will SKIP"
    else
        pin jsonschema rfc3339-validator >/dev/null 2>&1 \
            || info "install failed — schema checks will SKIP"
    fi
}
setup_validator() {
    bootstrap_deps
    have_deps || { info "no python3/jsonschema/rfc3339-validator — schema checks SKIP"; return; }
    HAVE_SCHEMA=1
    VALIDATOR="$(mktemp -t mos_validate.XXXXXX)"
    cat > "$VALIDATOR" <<'PY'
import sys, json
from jsonschema import Draft202012Validator, FormatChecker
sdir, name = sys.argv[1], sys.argv[2]
try:
    schema = json.load(open("%s/%s.json" % (sdir, name)))
except FileNotFoundError:
    sys.exit(2)
try:
    doc = json.load(sys.stdin)
except Exception as e:
    print("not JSON: %s" % e, file=sys.stderr); sys.exit(1)
errs = sorted(Draft202012Validator(schema, format_checker=FormatChecker())
              .iter_errors(doc), key=lambda e: list(e.path))
for e in errs[:6]:
    print("%s (at %s)" % (e.message, list(e.path)), file=sys.stderr)
sys.exit(1 if errs else 0)
PY
    FIELD="$(mktemp -t mos_field.XXXXXX)"
    cat > "$FIELD" <<'PY'
import sys, json
try: d = json.load(sys.stdin)
except Exception: sys.exit(1)
for k in sys.argv[1:]:
    if isinstance(d, dict) and k in d: d = d[k]
    else: sys.exit(1)
print("" if d is None else d)
PY
}
json_field() { [ -n "$FIELD" ] && printf '%s' "$1" | python3 "$FIELD" "${@:2}" 2>/dev/null; }

# check_doc "expected.schema" "desc" "<json output>"
# Detects the document's own `schema` field, validates against THAT schema, and
# compares it to the expected one — so an error envelope where a state document
# was expected reads "got mos.error.v1" instead of a cryptic validation failure.
check_doc() {
    expected="$1"; desc="$2"; doc="$3"
    got="$(printf '%s' "$doc" | python3 "$FIELD" schema 2>/dev/null)"
    if [ -z "$got" ]; then
        if [ "$HAVE_SCHEMA" = 1 ]; then bad "$desc: output is not a JSON document with a schema field"
        else skp "$desc schema" "no validator"; fi
        return 1
    fi
    if [ "$HAVE_SCHEMA" != 1 ]; then skp "$desc schema ($got)" "no validator"; return 0; fi
    errs="$(printf '%s' "$doc" | python3 "$VALIDATOR" "$SCHEMA_DIR" "$got" 2>&1)"; rc=$?
    case "$rc" in
        2) skp "$desc schema ($got)" "schema file absent"; return 0 ;;
        1) bad "$desc: does not validate $got"; printf '%s\n' "$errs" | sed 's/^/         /' | head -6; return 1 ;;
    esac
    if [ "$got" = "$expected" ]; then ok "$desc → valid $got"; return 0
    else bad "$desc: expected $expected, got $got (a valid but unexpected document type)"; return 1; fi
}

# pull a field from the first drive of `mos list --json`. Uses python3 -c so the
# piped JSON reaches stdin (a heredoc here would override the pipe — SC2259).
list_field() {
    [ -n "$FIELD" ] || return 1
    "$MOS" list --json 2>/dev/null | python3 -c '
import sys, json
try: d = json.load(sys.stdin)
except Exception: sys.exit(1)
ds = d.get("drives", [])
if not ds: sys.exit(1)
v = ds[0].get(sys.argv[1])
print("" if v is None else v)
' "$1" 2>/dev/null
}

# ---- cleanup: never leave the tray locked or open ----------------------------
cleanup() {
    [ -n "$VALIDATOR" ] && rm -f "$VALIDATOR"
    [ -n "$FIELD" ] && rm -f "$FIELD"
    "$MOS" tray unlock >/dev/null 2>&1   # unlock clears BOTH Prevent states
}
trap cleanup EXIT INT TERM

# =============================================================================
# SECTIONS
# =============================================================================

phase_static() {
    hdr "STATIC — no hardware"
    if [ -x "$HERE/build/bin/mos_tests" ]; then
        t="$("$HERE/build/bin/mos_tests" 2>&1 | tail -1)"
        case "$t" in *", 0 failed"*) ok "pure suite: $t" ;; *) bad "pure suite: $t" ;; esac
    else skp "pure suite" "build/bin/mos_tests not built"; fi

    if [ "$HAVE_SCHEMA" = 1 ] && [ -f "$SCHEMA_DIR/validate.py" ]; then
        if python3 "$SCHEMA_DIR/validate.py" >/dev/null 2>&1; then ok "schemas/validate.py (examples + drift guards)"
        else bad "schemas/validate.py"; python3 "$SCHEMA_DIR/validate.py" 2>&1 | tail -8 | sed 's/^/         /'; fi
    else skp "schemas/validate.py" "no python3/jsonschema/rfc3339-validator"; fi

    run --version;          expect_rc 0 "mos --version"
    run --help;             expect_rc 0 "mos --help"
    run;                    expect_rc 64 "bare mos → EX_USAGE (64)"
    run definitelynotaverb; expect_rc 64 "unknown subcommand → EX_USAGE (64)"
    run state --json=oops;  expect_rc 64 "--json takes no argument → EX_USAGE (64)"
}

phase_errors() {
    hdr "ERROR / SELECTOR PATHS — no hardware"
    run state 99;     expect_nonzero "bad index → non-zero"
    run state disk999; expect_nonzero "nonexistent diskN → non-zero"
    e="$("$MOS" state --json 99 2>/dev/null)"
    check_doc mos.error.v1 "bad index --json" "$e"
}

# Inspect whatever the drive currently reports, asserting the checks that fit
# the observed state. Sets the global STATE to the observed state string (display
# goes straight to stdout, so this is NOT called in a $(...) capture).
STATE=""
inspect_state() {
    run state;  expect_rc 0 "state exits 0 on an observed drive"
    s="$("$MOS" state --json 2>/dev/null)"
    check_doc mos.state.v1 "state --json" "$s"
    STATE="$(json_field "$s" state)"
    info "observed state: ${STATE:-<unknown>}"
}

phase_empty() {
    hdr "EMPTY DRIVE — no disc loaded"
    step "Eject the tray and remove any disc (a plain 'mos tray eject' runs first)." || return
    run tray eject; info "(already-open/empty drive → a no-op 'done')"

    inspect_state
    case "$STATE" in *open*|*empty*) ok "state is open/empty ($STATE)" ;;
        "") skp "state classification" "could not read state" ;;
        *) bad "expected open/empty, got '$STATE'" ;; esac

    run list;     expect_text 'Index|Vendor|Product' "list shows the drive"
    check_doc mos.list.v1 "list --json" "$("$MOS" list --json 2>/dev/null)"

    run drive;    expect_text 'Vendor|Product' "drive identity (vendor/product)"
    info "This branch's 'mos drive' is the full identity record — eyeball these rows:"
    info "  Serial       feature 0108h (GET CONFIGURATION); reads empty AND mounted, null if unprogrammed"
    info "  Interconnect bus + internal/external from DiscRecording (atapi / usb / firewire; null if DR omits)"
    info "  Protection   content schemes the drive can authenticate; Profiles = supported media classes"
    info "  Standards / Mechanical / Error Recovery round out the static facts"
    for row in Serial Interconnect Protection Profiles Standards Mechanical; do
        if printf '%s' "$OUT" | grep -q "$row:"; then info "  ✓ $row present"
        else info "  · $row absent (null/omitted — acceptable)"; fi
    done
    check_doc mos.drive.v1 "drive --json" "$("$MOS" drive --json 2>/dev/null)"

    run features; expect_text 'BSD' "features names the drive (BSD row — added this branch)"
    expect_text 'Code|Cur|Persist' "features shows the MMC feature table"
    check_doc mos.features.v1 "features --json" "$("$MOS" features --json 2>/dev/null)"

    run capacity; info "no media → a no-media capacity report or an error envelope are both fine"
    run metadata; info "no disc → mos.error.v1 envelope expected"
    check_doc mos.error.v1 "metadata --json (no disc)" "$("$MOS" metadata --json 2>/dev/null)"

    # Selector equivalence (no media): this branch completes the explicit-flag set
    # with --registry, so exercise positionals AND flags — all must pick one drive.
    rid="$(list_field registry_id)"
    if [ -n "${rid:-}" ] && [ "$rid" != 0 ]; then
        p_idx="$("$MOS" state 1 2>/dev/null)";          p_reg="$("$MOS" state "$rid" 2>/dev/null)"
        f_idx="$("$MOS" state --index 1 2>/dev/null)";   f_reg="$("$MOS" state --registry "$rid" 2>/dev/null)"
        if [ -n "$p_idx" ] && [ "$p_idx" = "$p_reg" ] && [ "$p_idx" = "$f_idx" ] && [ "$f_idx" = "$f_reg" ]; then
            ok "index/registry positionals and --index/--registry flags select the same drive"
        else bad "selector forms disagree (positional index/registry vs --index/--registry)"; fi
    else skp "selector equivalence" "could not read registry_id"; fi
}

phase_tray() {
    hdr "TRAY CONTROL — no disc"
    step "Empty drive, tray closed. Walks eject/close/lock/unlock and --force." || return

    run tray eject;  expect_text 'eject|done|outcome' "tray eject → done"
    inspect_state; case "$STATE" in *open*|*empty*) ok "after eject: open/empty ($STATE)" ;; *) info "state: $STATE" ;; esac
    run tray close;  expect_text 'close|done|outcome' "tray close → done"
    check_doc mos.tray.v1 "tray close --json" "$("$MOS" tray close --json 2>/dev/null)"

    info "lock = the basic Prevent (hard removal block); unlock clears BOTH states."
    info "On an empty drive the lock is a COLD lock — the default eject is refused,"
    info "and --force clears it. (--persistent is gone: lock is always the basic Prevent.)"
    run tray lock;          expect_text 'lock|done|outcome' "tray lock → done"
    run tray eject;         expect_any "eject of a COLD-locked tray → refused_locked" 'refused_locked' '53/02'
    run tray unlock;        expect_text 'unlock|done|outcome' "tray unlock → done"
    run tray eject;         expect_text 'eject|done|outcome' "eject after unlock → done"
    run tray close

    run tray lock;          expect_text 'lock|done|outcome' "re-lock for the --force test"
    run tray eject --force; expect_any "eject --force clears the cold lock" 'done' 'outcome'
    info "--force cleared the COLD Prevent lock then ejected; it never forces the filesystem."
    run tray close
}

phase_disc() {
    hdr "DISC LOADED — adaptive, repeat per media type"
    info "Cover if you have them: CD-ROM, CD-R/RW, DVD-ROM, DVD±R/RW, BD-ROM, blank BD-R/RE, UHD BD."
    # non-interactive can't swap discs, and `step` always proceeds there — cap the
    # insert loop at one pass so `SMOKE_NONINTERACTIVE=1 all` can't spin forever.
    disc_pass=0
    while : ; do
        [ "${SMOKE_NONINTERACTIVE:-0}" = 1 ] && [ "$disc_pass" -ge 1 ] && break
        disc_pass=$((disc_pass+1))
        step "Insert a disc and let macOS mount it (or 'diskutil mount diskN')." || break

        # 1) the stray-open guard — sample state across the spin-up window
        info "sampling state during spin-up (expect loading → ready; NO stray 'open')"
        seen_open=0; seen_loading=0; i=0
        while [ $i -lt 10 ]; do
            line="$("$MOS" state 2>/dev/null | grep -i 'State' | head -1)"
            printf '%s       %s%s\n' "$D" "$line" "$Z"
            case "$line" in *open*) seen_open=1 ;; esac
            case "$line" in *loading*) seen_loading=1 ;; esac
            case "$line" in *ready*) break ;; esac
            i=$((i+1)); sleep 0.4
        done
        if [ "$seen_open" = 1 ]; then
            bad "STRAY 'open' during load (the 04/xx-vs-GESN transient) — CAPTURE THIS with 'mos probe --capture'"
        elif [ "$seen_loading" = 1 ]; then ok "load showed 'loading', no stray 'open'"
        else info "spin-up too fast to catch 'loading' — fine if it reached ready"; fi

        # 2) ready-state enrichment
        inspect_state
        case "$STATE" in
            *ready*) ok "disc reached ready ($STATE)" ;;
            "") skp "ready enrichment" "no state read" ; continue ;;
            *) info "state is '$STATE' (not ready) — skipping ready-only checks"; continue ;;
        esac
        info "Speeds (GET PERFORMANCE) ride in 'mos state' on the ready branch:"
        run state; expect_text 'Speed' "state shows Speeds for a ready disc (or note if drive omits them)"

        run metadata; expect_text 'Disc|Profile|TOC|Track' "metadata populated"
        info "metadata rows here: Disc, Profile, Sessions (CD only), TOC, Track."
        check_doc mos.metadata.v1 "metadata --json" "$("$MOS" metadata --json 2>/dev/null)"
        run capacity; expect_text 'Media|Recordable|Formattable' "capacity shows a size view"
        info "capacity human uses the × glyph (blocks × B); blank rewritable & BD-R add a Formattable row."
        check_doc mos.capacity.v1 "capacity --json" "$("$MOS" capacity --json 2>/dev/null)"
        run drive;    expect_text 'Vendor|Product' "drive identity while mounted"
        info "Serial (0108h) + Interconnect read even while mounted — no exclusive-access back-off."

        # lock on a MOUNTED disc → already_locked (macOS armed the removal lock)
        run tray lock; expect_text 'already_locked' "lock on a mounted disc → already_locked"

        # 3) selector equivalence with media (diskN now exists): every form agrees
        bn="$(list_field bsd_node)"; rid="$(list_field registry_id)"
        if [ -n "${bn:-}" ] && [ "$bn" != null ]; then
            dn="${bn##*/}"
            s1="$("$MOS" state "$dn" 2>/dev/null)";   s2="$("$MOS" state "$bn" 2>/dev/null)"
            s3="$("$MOS" state --bsd "$dn" 2>/dev/null)"
            same=1; { [ "$s1" = "$s2" ] && [ "$s2" = "$s3" ]; } || same=0
            if [ -n "${rid:-}" ] && [ "$rid" != 0 ]; then
                s4="$("$MOS" state --registry "$rid" 2>/dev/null)"
                [ "$s3" = "$s4" ] || same=0
            fi
            if [ "$same" = 1 ]; then ok "diskN / /dev/diskN / --bsd / --registry select the same drive"
            else bad "selector forms disagree with media present"; fi
        else skp "selector equivalence" "no bsd_node (need a mounted disc + python3)"; fi

        # 4) graceful eject of an idle mounted disc
        if step "Close anything using the disc — I'll test a GRACEFUL eject (unmount+eject)."; then
            run tray eject; expect_any "graceful eject of a mounted disc" 'done' 'outcome'
            info "mounted → mos unmounts gracefully then ejects; a BUSY fs would surface MOS_ERR_BUSY."
            run tray close
        fi
    done

    # 5) busy-filesystem eject — mos must REFUSE, never force (data-loss guard)
    if step "BUSY-FS test: insert a disc, let it mount; I'll hold a file open then eject."; then
        vol="$(list_field volume_path)"
        if [ -n "${vol:-}" ] && [ "$vol" != null ] && [ -d "$vol" ]; then
            tmp="$(mktemp -t mos_busy.XXXXXX)"
            ( exec 9< "$vol"; "$MOS" tray eject >"$tmp" 2>&1 )
            OUT="$(cat "$tmp")"; rm -f "$tmp"
            printf '%s\n' "$OUT" | sed 's/^/       /' | head -8
            expect_any "busy-fs eject is REFUSED, not forced" 'busy' 'EX_TEMPFAIL'
            info "mos never force-unmounts: a busy volume yields MOS_ERR_BUSY (diskutil-equivalent), data intact."
        else skp "busy-fs eject" "no mounted volume_path (need a data disc + python3)"; fi
    fi
}

phase_watch() {
    hdr "WATCH — event stream + contention"
    if step "I'll stream watch events for ~20s while you EJECT, then INSERT a disc."; then
        evf="$(mktemp -t mos_watch.XXXXXX)"
        "$MOS" watch --json >"$evf" 2>/dev/null & wpid=$!
        info "watching (pid $wpid) — eject the tray, then insert a disc…"
        sleep 20; kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
        sed 's/^/       /' "$evf" | head -12
        if [ -s "$evf" ]; then
            n=0; failed=0
            while IFS= read -r ln; do
                [ -z "$ln" ] && continue; n=$((n+1))
                sch="$(printf '%s' "$ln" | python3 "$FIELD" schema 2>/dev/null)"
                if [ "$HAVE_SCHEMA" = 1 ] && [ -n "$sch" ]; then
                    printf '%s' "$ln" | python3 "$VALIDATOR" "$SCHEMA_DIR" "$sch" >/dev/null 2>&1 || failed=1
                fi
            done < "$evf"
            if [ "$HAVE_SCHEMA" != 1 ]; then skp "watch event schemas" "no validator"
            elif [ "$failed" = 0 ]; then ok "watch: $n event line(s) each validate their own schema"
            else bad "watch: an event line failed its schema"; fi
            if grep -Eq 'state_changed|snapshot' "$evf"; then ok "watch emitted lifecycle events"
            else bad "watch emitted no lifecycle events"; fi
            if grep -q '"open"' "$evf" && grep -Eqi 'becoming|04/01|loading' "$evf"; then
                info "review the order: an insert should show 'loading', not a stray 'open'"; fi
        else bad "watch produced no output"; fi
        rm -f "$evf"
    fi

    if step "CONTENTION: background watch + 'mos tray eject' against it (should SUCCEED post-#110)."; then
        "$MOS" watch >/dev/null 2>&1 & wpid=$!
        sleep 2
        run tray eject
        if [ "$RC" = 0 ]; then ok "eject succeeded under a concurrent watch (contention reduced)"
        elif printf '%s' "$OUT" | grep -qi 'exclusive'; then
            bad "eject lost to watch (EXCLUSIVE_ACCESS) — residual empty-drive GESN window"
        else info "eject non-zero ($RC) for a non-contention reason — inspect above"; fi
        kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
        "$MOS" tray close >/dev/null 2>&1
    fi
}

# =============================================================================
banner() {
    printf '%smos hardware smoke test%s\n' "$B" "$Z"
    info "binary : $MOS  ($("$MOS" --version 2>/dev/null || echo 'no --version'))"
    if [ "$MOS" = "$MOS_BUILT" ] && [ -n "$MOS_ON_PATH" ]; then
        info "note   : an installed mos is also on PATH ($MOS_ON_PATH);"
        info "         testing the freshly-built one. Pass MOS=$MOS_ON_PATH to test that."
    elif [ "$MOS" = "$MOS_ON_PATH" ] && [ -x "$MOS_BUILT" ]; then
        info "note   : a build tree also exists; testing the installed mos on PATH."
        info "         Pass MOS=$MOS_BUILT to test the freshly-built one."
    fi
    info "schema : $([ "$HAVE_SCHEMA" = 1 ] && echo 'python3 + jsonschema + rfc3339-validator present' || echo 'absent — --json checks SKIP')"
}

summary() {
    hdr "SUMMARY"
    printf '%s%d passed%s, %s%d failed%s, %s%d skipped%s\n' \
        "$G" "$PASS" "$Z" "$R" "$FAIL" "$Z" "$Y" "$SKIP" "$Z"
    [ "$FAIL" -gt 0 ] && printf 'failed:%b\n' "$FAILED_NAMES"
    info "Per the hardware-role ADR a FAIL or surprise is a deliverable: capture it with"
    info "'mos probe --capture <drive>' and file it as a fixture + a dated note."
}

run_section() {
    case "$1" in
        static) phase_static ;; errors) phase_errors ;; empty) phase_empty ;;
        tray)   phase_tray ;;   disc)   phase_disc ;;   watch) phase_watch ;;
        all)    phase_static; phase_errors; phase_empty; phase_tray; phase_disc; phase_watch ;;
    esac
}

menu() {
    while : ; do
        printf '\n%sChoose a section%s (set up the drive/disc to match):\n' "$B" "$Z"
        cat <<EOF
   1) static   no hardware: pure suite, schemas, help/exit codes
   2) errors   selector + usage error paths
   3) empty    drive present, NO disc — identity, serial, list, features
   4) tray     eject / close / lock / unlock / --force (no disc)
   5) disc     insert a disc — adaptive loading/ready/enrichment checks
   6) watch    event stream + watch/eject contention
   a) all      run 1–6 in order
   s) summary so far     q) quit
EOF
        printf '%s> %s' "$D" "$Z"; read -r pick || pick=q
        case "$pick" in
            1) phase_static ;; 2) phase_errors ;; 3) phase_empty ;;
            4) phase_tray ;; 5) phase_disc ;; 6) phase_watch ;;
            a|A) run_section all ;;
            s|S) summary ;;
            q|Q) break ;;
            *) info "pick 1–6, a, s, or q" ;;
        esac
    done
}

# ---- main --------------------------------------------------------------------
setup_validator
banner

ARG="${1:-}"
case "$ARG" in
    static|errors|empty|tray|disc|watch|all)
        run_section "$ARG"; summary; [ "$FAIL" -eq 0 ] ;;
    "")
        if [ "${SMOKE_NONINTERACTIVE:-0}" = 1 ]; then
            run_section all; summary; [ "$FAIL" -eq 0 ]
        else
            menu; summary; [ "$FAIL" -eq 0 ]
        fi ;;
    *)
        echo "unknown section '$ARG' (static|empty|tray|disc|watch|errors|all)" >&2; exit 2 ;;
esac
