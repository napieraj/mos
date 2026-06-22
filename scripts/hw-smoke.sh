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
#       sections: static empty tray disc watch errors capture all
#   scripts/hw-smoke.sh capture            # capture hex fixtures per disc type;
#                                          # interactive setup asks scrub/raw + save-log
#   scripts/hw-smoke.sh capture --raw      # scripted opt-out of the serial scrub (private)
#   SMOKE_CAPTURE_DIR=/path scripts/hw-smoke.sh capture   # output root (grouped vendor/model)
#   (advanced) --salt=KEY / MOS_SCRUB_SALT: BLAKE2b key for the scrub.
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

# ---- timing + run log --------------------------------------------------------
# START_ISO is the wall-clock start in the SAME RFC 3339 UTC form the C side
# stamps capture/probe records with (cli/probe.c format_rfc3339_utc), so the
# script's timeline lines up with the C `ts` fields. SECONDS (bash builtin, reset
# at main) drives the T+mm:ss markers. When LOGFILE is set (capture phase) every
# emitted line is appended to it, T+-prefixed, so the log reads as a timeline.
START_ISO=""; LOGFILE=""
# Serial scrub (capture only). Interactive: the capture-setup prompt asks whether
# to scrub. Non-interactive / scripted: --raw (or SMOKE_RAW=1) forces byte-perfect
# (no scrub). RAW_FORCED records that --raw came from the CLI so setup won't
# re-ask. The salt is a quiet runtime knob only (--salt= / MOS_SCRUB_SALT), not a
# prompt — a BD-writer serial barely warrants it, but it's there for the paranoid.
RAW="${SMOKE_RAW:-0}"; RAW_FORCED=0; SCRUB_SALT="${MOS_SCRUB_SALT:-}"
SAVE_LOG=1           # capture: write the run log + manifest (asked in setup)
CAP_PARTIAL=0        # capture: set when a step is skipped / the run ends early
tplus() { printf 'T+%02d:%02d' $((SECONDS / 60)) $((SECONDS % 60)); }
lg() { [ -n "$LOGFILE" ] || return 0; printf '%s  %s\n' "$(tplus)" "$*" >> "$LOGFILE" 2>/dev/null; }

hdr()  { printf '\n%s━━ %s %s%s%s\n' "$B$C" "$1" "$D" "$(tplus)" "$Z"; lg ""; lg "== $1 ($(tplus)) =="; }
info() { printf '%s   %s%s\n' "$D" "$*" "$Z"; lg "     $*"; }
ok()   { PASS=$((PASS+1)); printf '   %sPASS%s %s\n' "$G" "$Z" "$1"; lg "PASS $1"; }
bad()  { FAIL=$((FAIL+1)); FAILED_NAMES="$FAILED_NAMES\n   - $1"; printf '   %sFAIL%s %s\n' "$R" "$Z" "$1"; lg "FAIL $1"; }
skp()  { SKIP=$((SKIP+1)); printf '   %sSKIP%s %s%s%s\n' "$Y" "$Z" "$1" "${2:+  — }" "${2:-}"; lg "SKIP $1${2:+ — $2}"; }

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
    lg "\$ mos $*  (rc=$RC)"
    lg "$(printf '%s\n' "$OUT" | sed 's/^/       /')"
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
HAVE_PY=0; HAVE_SCHEMA=0; VALIDATOR=""; FIELD=""
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
    # FIELD (JSON field extractor) needs only python3 stdlib — set it up whenever
    # python3 is present, independent of the jsonschema stack, so the field reads
    # and the capture decode work even when schema validation can't.
    if command -v python3 >/dev/null 2>&1; then
        HAVE_PY=1
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
    fi
    bootstrap_deps
    if have_deps; then
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
    else
        info "no python3/jsonschema/rfc3339-validator — schema checks SKIP"
    fi
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
    # expected may be a space-separated set of acceptable schema names.
    case " $expected " in
        *" $got "*) ok "$desc → valid $got"; return 0 ;;
        *) bad "$desc: expected $expected, got $got (a valid but unexpected document type)"; return 1 ;;
    esac
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

# the drive's current state string, with or without the JSON validator present.
current_state() {
    if [ -n "$FIELD" ]; then "$MOS" state --json 2>/dev/null | python3 "$FIELD" state 2>/dev/null
    else "$MOS" state 2>/dev/null | grep -i '^ *State' | head -1 | sed 's/.*[Ss]tate[: ]*//' | tr -dc 'a-z_'; fi
}

# registry_id a given selector resolves to (from `mos state <sel> --json`). The
# stable identity to compare selectors by — the human/JSON identity ROWS differ
# by how you selected (Index populated when picked by index, etc.), so comparing
# full output gives false mismatches; registry_id is invariant.
state_regid() { [ -n "$FIELD" ] || return 1; "$MOS" state "$@" --json 2>/dev/null | python3 "$FIELD" registry_id 2>/dev/null; }

# stop a backgrounded `mos watch` WITHOUT hanging: it stops on SIGINT (not the
# default SIGTERM `kill` sends), so send INT, wait briefly, then SIGKILL if it is
# still alive, and only then reap. An unbounded `wait` on a watcher that ignores
# the signal is the classic hang.
stop_watch() {
    p="$1"
    kill -INT "$p" 2>/dev/null
    n=0; while [ "$n" -lt 10 ]; do kill -0 "$p" 2>/dev/null || break; sleep 0.5; n=$((n+1)); done
    kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null
    wait "$p" 2>/dev/null
}

# ---- fixture capture (mos probe --capture) -----------------------------------
# Sanitized vendor+product token for fixture filenames (e.g. HL-DT-ST_BD-RE_WH16NS60).
# Short model token for filenames. Product strings carry a class prefix that is
# noise for identity, and INQUIRY fields are space-padded with arbitrary runs:
# "BD-RE  WH16NS60   " -> "WH16NS60". awk '{print $NF}' takes the last whitespace-
# delimited field, robust to multiple/leading/trailing spaces. Falls back to the
# whole product then "drive".
drive_model() {
    p=""
    [ -n "$FIELD" ] && p="$("$MOS" drive --json 2>/dev/null | python3 "$FIELD" product 2>/dev/null)"
    [ -z "$p" ] && p="$(list_field product 2>/dev/null)"
    [ -z "$p" ] && p="drive"
    last="$(printf '%s' "$p" | awk '{print $NF}')"   # last field; collapses any whitespace runs
    [ -z "$last" ] && last="$p"
    printf '%s' "$last" | tr -dc 'A-Za-z0-9._-'
}

# mos version token for filenames, derived in-shell from `mos --version`
# ("mos 0.4.0" -> "0.4.0"; strips any leading non-version words). Falls back to
# "x" when --version is unavailable.
mos_ver() {
    v="$("$MOS" --version 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+\.[0-9]' | head -1)"
    [ -z "$v" ] && v="x"
    printf '%s' "$v" | tr -dc 'A-Za-z0-9._-'
}

# Vendor token for the capture subdirectory (HL-DT-ST, Pioneer, …), sanitized.
drive_vendor() {
    v=""
    [ -n "$FIELD" ] && v="$("$MOS" drive --json 2>/dev/null | python3 "$FIELD" vendor 2>/dev/null)"
    [ -z "$v" ] && v="$(list_field vendor 2>/dev/null)"
    [ -z "$v" ] && v="unknown"
    printf '%s' "$v" | tr ' /' '__' | tr -dc 'A-Za-z0-9._-'
}

# Firmware revision token for filenames — fixtures are firmware-specific, so the
# drive's PRODUCT_REVISION_LEVEL ("1.00") belongs in the name. "x" when absent.
drive_revision() {
    r=""
    [ -n "$FIELD" ] && r="$("$MOS" drive --json 2>/dev/null | python3 "$FIELD" revision 2>/dev/null)"
    [ -z "$r" ] && r="x"
    printf '%s' "$r" | tr ' /' '__' | tr -dc 'A-Za-z0-9._-'
}

# A probe-usable selector: diskN when media is present, else the list Index
# (probe takes index or BSD, never registry_id).
capture_selector() {
    bn="$(list_field bsd_node 2>/dev/null)"
    if [ -n "${bn:-}" ] && [ "$bn" != null ]; then printf '%s' "${bn##*/}"; return; fi
    ix="$(list_field index 2>/dev/null)"; [ -z "$ix" ] && ix=1
    printf '%s' "$ix"
}

# Disc-type label for tracking + filenames. Sets DISC_TYPE; returns 1 = "done".
DISC_TYPE=""; DISC_TYPES_DONE=""
ask_disc_type() {
    if [ "${SMOKE_NONINTERACTIVE:-0}" = 1 ]; then DISC_TYPE="${DISC_TYPE:-disc}"; return 1; fi
    printf '\n%sWhich disc is loaded?%s  (used for fixture names + tracking)\n' "$B" "$Z"
    cat <<EOF
   1) uhd_bd   2) bd      3) mdisc    4) audio_cd
   5) dvd      6) cd      7) blank_bd  8) other (type a label)
   d) done with discs
EOF
    printf '%s> %s' "$D" "$Z"; read -r t || t=d
    case "$t" in
        1) DISC_TYPE='uhd_bd' ;; 2) DISC_TYPE='bd' ;; 3) DISC_TYPE='mdisc' ;;
        4) DISC_TYPE='audio_cd' ;; 5) DISC_TYPE='dvd' ;; 6) DISC_TYPE='cd' ;;
        7) DISC_TYPE='blank_bd' ;;
        8) printf '   label: '; read -r DISC_TYPE
           DISC_TYPE="$(printf '%s' "$DISC_TYPE" | tr ' /' '__' | tr -dc 'A-Za-z0-9_.-')"
           [ -z "$DISC_TYPE" ] && DISC_TYPE=other ;;
        d|D|"") return 1 ;;
        *) DISC_TYPE=other ;;
    esac
    return 0
}

# capture_run <disctype> <state> <outdir> <model>
# Run `mos probe --capture <sel>`; for each reply, UNLESS --raw, replace the real
# serial bytes with the false serial and recompute the mos sha256 over the
# MODIFIED bytes — THEN write the .bin and the scrubbed .ndjson line (modify-then-
# write: the raw reply, which may carry the real serial, is a temp, never
# committed). With --raw the bytes are written byte-perfect (real serial intact).
capture_run() {
    cdt="$1"; cstate="$2"; cout="$3"; cmodel="$4"; cver="$5"; crev="$6"
    sel="$(capture_selector)"
    ndj="$cout/mos-v${cver}-${cmodel}-${crev}-${cdt}-${cstate}.ndjson"
    raw="$(mktemp -t mos_cap.XXXXXX)"
    printf '%s   $ mos probe --capture %s   (%s / %s)%s\n' "$D" "$sel" "$cdt" "$cstate" "$Z"
    lg "\$ mos probe --capture $sel   ($cdt / $cstate)"
    "$MOS" probe --capture "$sel" >"$raw" 2>/dev/null
    if ! grep -q 'mos.capture.v0' "$raw" 2>/dev/null; then
        if grep -qi 'not built' "$raw" 2>/dev/null; then bad "capture $cdt/$cstate: probe not built in (configure -DMOS_CLI_PROBE=ON)"
        else bad "capture $cdt/$cstate: no mos.capture.v0 output"; fi
        rm -f "$raw"; return
    fi
    out="$(python3 - "$raw" "$ndj" "$cout" "$cdt" "$cstate" "$cmodel" "$SERIAL_REAL" "$SERIAL_FALSE" "$RAW" "$cver" "$START_ISO" "$crev" <<'PY'
import sys, json, hashlib, os
raw, ndj, outdir, dt, state, model, real, false, rawmode, ver, started, rev = sys.argv[1:13]
rawmode = (rawmode == "1")
real_hex = real.encode().hex(); false_hex = false.encode().hex()
# fixture filename: mos-v<ver>-<model>-<revision>-<command>-<disctype>-<state>.bin
def binname(cmd): return "mos-v%s-%s-%s-%s-%s-%s.bin" % (ver, model, rev, cmd, dt, state)
wrote=empty=err=scr=0; outlines=[]
for line in open(raw):
    line=line.strip()
    if not line: continue
    try: d=json.loads(line)
    except Exception: continue
    if d.get("schema")!="mos.capture.v0": outlines.append(line); continue
    cmd=d.get("command","cmd"); sn=d.get("sense",{}) or {}
    # structured provenance so the manifest reads fields, not filenames
    d["mos_version"]=ver; d["model"]=model; d["revision"]=rev; d["disc_type"]=dt; d["state"]=state; d["run_started"]=started
    if not d.get("ok"):
        outlines.append(json.dumps(d)); print("  . %-24s error=%s"%(cmd,d.get("error"))); err+=1; continue
    reply=d.get("reply","") or ""
    if not reply:
        outlines.append(json.dumps(d)); print("  . %-24s no data (sense %s/%s/%s)"%(cmd,sn.get("sk"),sn.get("asc"),sn.get("ascq"))); empty+=1; continue
    tag=""
    if (not rawmode) and real and real_hex in reply:
        reply=reply.replace(real_hex,false_hex); d["reply"]=reply; d["serial_scrubbed"]=True; scr+=1; tag=" [serial scrubbed]"
    rb=bytes.fromhex(reply)
    d["sha256"]=hashlib.sha256(rb).hexdigest()
    bn=binname(cmd); d["fixture"]=bn
    outlines.append(json.dumps(d))
    open(os.path.join(outdir,bn),"wb").write(rb)
    print("  + %-24s %5d B  sha=%s -> %s%s"%(cmd,len(rb),d["sha256"][:12],bn,tag)); wrote+=1
open(ndj,"w").write("\n".join(outlines)+"\n")
print("SUMMARY %d written, %d no-data, %d error, %d serial-scrubbed"%(wrote,empty,err,scr))
PY
)"
    rm -f "$raw"
    printf '%s\n' "$out" | sed 's/^/     /'
    lg "$(printf '%s\n' "$out" | sed 's/^/     /')"
    ok "capture $cdt/$cstate -> $(basename "$ndj") + .bin files"
}

# Grab the drive serial ONCE, up front, on an empty drive (the cleanest read:
# feature 0108h via GET CONFIGURATION is non-exclusive and reads on an empty
# drive), and derive the false serial + keyed-BLAKE2b commitment so every capture
# is scrubbed INLINE. Sets SERIAL_REAL/SERIAL_FALSE/SERIAL_HASH; writes
# SERIAL-SCRUB.txt. Skipped under --raw. The real serial is never printed/logged.
SERIAL_REAL=""; SERIAL_FALSE=""; SERIAL_HASH=""
serial_arm() {
    sdir="$1"
    if [ "$RAW" = 1 ]; then
        info "--raw: serial hashing DISABLED — fixtures/logs are BYTE-PERFECT and contain"
        info "your REAL serial. Do NOT post these publicly. (Drop --raw to scrub.)"
        lg "RAW MODE: serial scrub disabled; output is byte-perfect (contains real serial)"
        return
    fi
    [ "$HAVE_PY" = 1 ] || { skp "serial scrub" "needs python3"; return; }
    SERIAL_REAL="$("$MOS" drive --json 2>/dev/null | python3 "$FIELD" serial 2>/dev/null)"
    if [ -z "$SERIAL_REAL" ]; then
        info "drive reports no serial (feature 0108h absent/blank) — captures left unscrubbed."
        info "If your drive only carries the serial in the INQUIRY tail, weigh --raw knowingly."
        return
    fi
    setup="$(python3 - "$SERIAL_REAL" "$SCRUB_SALT" <<'PY'
import sys, hashlib
real, salt = sys.argv[1], sys.argv[2]
salt_key = salt.encode()[:64]
def _d(x): return hashlib.blake2b(x, key=salt_key).digest()
seed = _d(real.encode())
def _ks(s):
    b = _d(s); i = 0
    while True:
        if i >= len(b): b = _d(b); i = 0
        yield b[i]; i += 1
def _draw(ks, n):
    lim = 256 - (256 % n)
    for x in ks:
        if x < lim: return x % n
    return 0
U = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"; L = U.lower(); D = "0123456789"
ks = _ks(seed)
false = "".join(D[_draw(ks,10)] if c.isdigit() else U[_draw(ks,26)] if "A"<=c<="Z" else L[_draw(ks,26)] if "a"<=c<="z" else c for c in real)
print(false); print(seed.hex())
PY
)"
    SERIAL_FALSE="$(printf '%s' "$setup" | sed -n '1p')"
    SERIAL_HASH="$(printf '%s' "$setup" | sed -n '2p')"
    salt_line="no (set --salt= or MOS_SCRUB_SALT for brute-force resistance)"
    [ -n "$SCRUB_SALT" ] && salt_line="yes"
    {
        printf 'Serial scrub - the real drive serial was replaced in every captured fixture\n'
        printf 'with a deterministic false serial of the same length and structure.\n\n'
        printf 'false_serial  : %s\n' "$SERIAL_FALSE"
        printf 'serial_blake2b: %s\n' "$SERIAL_HASH"
        printf 'salt_used     : %s\n' "$salt_line"
        printf 'started       : %s\n\n' "$START_ISO"
        printf 'Algorithm: keyed BLAKE2b (RFC 7693, community/non-NSA, ChaCha-based core)\n'
        printf 'with the salt as key; false_serial mirrors the real serial structure\n'
        printf '(letter/digit positions) via a custom keystream over the digest -\n'
        printf 'reproducible from the hash, real serial NOT derivable (preimage resistance).\n'
        printf 'Reverse-validate (use the SAME salt you ran with):\n'
        printf "    python3 -c 'import hashlib,sys; print(hashlib.blake2b(sys.argv[2].encode(),\\\\\n"
        printf "      key=sys.argv[1].encode()[:64]).hexdigest())' '<SALT>' '<REAL_SERIAL>'\n"
    } > "$sdir/SERIAL-SCRUB.txt"
    info "serial scrub armed - replacement $SERIAL_FALSE (keyed BLAKE2b); real serial not shown/logged"
    ok "serial grabbed up front on the empty drive"
}

# Build one postable capture-manifest.md aggregating run metadata, a results
# table, and the FULL command logs (the mos.capture.v0 NDJSON, already serial-
# scrubbed + sha-recomputed inline at capture time). Reads structured provenance
# fields off each record (disc_type/state/command/sha256/fixture), not filenames.
build_manifest() {
    mout="$1"; mmodel="$2"; mver="$3"; mrev="$4"; mvendor="$5"; mstatus="${6:-complete}"
    [ "$HAVE_PY" = 1 ] || return 0
    python3 - "$mout" "$mmodel" "$mver" "$mrev" "$mvendor" "$START_ISO" "$RAW" "$(basename "$LOGFILE")" "$mstatus" <<'PY'
import sys, os, json, glob, datetime
mout, model, ver, rev, vendor, started, rawmode, logname, status = (sys.argv + [""]*9)[1:10]
raw = (rawmode == "1")
mans = sorted(glob.glob(os.path.join(mout, "*.ndjson")))
rows, logs = [], []
for m in mans:
    body = open(m).read()
    logs.append((os.path.basename(m), body))
    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("schema") != "mos.capture.v0":
            continue
        dt = d.get("disc_type", "?"); st = d.get("state", "?")
        cmd = d.get("command", "?")
        if not d.get("ok"):
            outcome, nb, sha, fx = "error:" + str(d.get("error")), "", "", ""
        elif not d.get("reply"):
            sn = d.get("sense", {}) or {}
            outcome = "no-data %s/%s/%s" % (sn.get("sk"), sn.get("asc"), sn.get("ascq"))
            nb, sha, fx = "0", "", ""
        else:
            outcome = "ok"
            nb = str(d.get("bytes_transferred", ""))
            sha = (d.get("sha256", "") or "")[:12]
            fx = d.get("fixture", "")
        rows.append((dt, st, cmd, outcome, nb, sha, fx))

scrub = ""
sp = os.path.join(mout, "SERIAL-SCRUB.txt")
if os.path.exists(sp):
    scrub = open(sp).read()

now = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
out = ["# mos capture manifest (%s)\n\n" % status.upper()]
if status != "complete":
    out.append("> **PARTIAL run** — not every disc/state was captured (steps skipped or "
               "no disc loaded). The fixtures present are valid; the matrix is incomplete.\n\n")
if raw:
    out.append("**RAW MODE — NOT scrubbed.** These fixtures/logs are byte-perfect and "
               "CONTAIN THE REAL DRIVE SERIAL. Keep private; do not post publicly.\n")
else:
    out.append("SAFE TO POST: the drive serial is scrubbed from every fixture/log below "
               "and replaced with a deterministic false serial (keyed BLAKE2b); the mos "
               "sha256 checksums are recomputed over the scrubbed bytes.\n")
out.append("\n## Run\n\n| field | value |\n|---|---|\n")
out.append("| status | %s |\n| generated | %s |\n| started | %s |\n| mos version | %s |\n"
           "| vendor | %s |\n| model | %s |\n| revision | %s |\n| run log | %s |\n"
           % (status, now, started, ver, vendor, model, rev, logname))
if scrub:
    out.append("\n## Serial scrub\n\n```\n%s```\n" % scrub)
out.append("\n## Results (%d command record(s))\n\n" % len(rows))
out.append("| disc | state | command | outcome | bytes | sha256… | fixture |\n")
out.append("|---|---|---|---|---|---|---|\n")
for r in rows:
    out.append("| %s | %s | %s | %s | %s | %s | %s |\n" % r)
out.append("\n## Full logs (mos.capture.v0 NDJSON)\n")
for base, body in logs:
    out.append("\n### %s\n\n```json\n%s\n```\n" % (base, body.rstrip("\n")))

mf = os.path.join(mout, "capture-manifest.md")
open(mf, "w").write("".join(out))
print("manifest: %s (%d records, %d log file(s))" % (mf, len(rows), len(logs)))
PY
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
    info "  Standards / Mechanical / Error Recovery / Write Protect round out the static facts"
    for row in Serial Interconnect Protection Profiles Standards Mechanical "Write Protect"; do
        if printf '%s' "$OUT" | grep -q "$row:"; then info "  ✓ $row present"
        else info "  · $row absent (null/omitted — acceptable)"; fi
    done
    check_doc mos.drive.v1 "drive --json" "$("$MOS" drive --json 2>/dev/null)"

    run features; expect_text 'BSD' "features names the drive (BSD row — added this branch)"
    expect_text 'Code|Cur|Persist' "features shows the MMC feature table"
    check_doc mos.features.v1 "features --json" "$("$MOS" features --json 2>/dev/null)"

    run capacity; info "no media → a no-media capacity report or an error envelope are both fine"
    run metadata; info "no disc → a mos.metadata.v1 doc with null rows (the disc facts are unreadable),"
    info "or a mos.error.v1 envelope on a hard failure — both acceptable."
    check_doc "mos.metadata.v1 mos.error.v1" "metadata --json (no disc)" "$("$MOS" metadata --json 2>/dev/null)"

    # Selector equivalence (no media): this branch completes the explicit-flag set
    # with --registry. Compare the registry_id each form resolves to (NOT the raw
    # output — the identity rows differ by selector), so this is a true "same
    # drive" check across positional index/registry + --index/--registry.
    rid="$(list_field registry_id)"
    if [ "$HAVE_SCHEMA" = 1 ] && [ -n "${rid:-}" ] && [ "$rid" != 0 ]; then
        r1="$(state_regid 1)"; r2="$(state_regid "$rid")"
        r3="$(state_regid --index 1)"; r4="$(state_regid --registry "$rid")"
        if [ "$r1" = "$rid" ] && [ "$r2" = "$rid" ] && [ "$r3" = "$rid" ] && [ "$r4" = "$rid" ]; then
            ok "index/registry positionals + --index/--registry all resolve to registry_id $rid"
        else bad "selector forms resolve differently (idx=$r1 reg=$r2 --index=$r3 --registry=$r4, want $rid)"; fi
    else skp "selector equivalence" "need python3 + registry_id"; fi
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

        # lock on a ready disc: if it's MOUNTED, macOS already armed the removal
        # lock and the CDB can't take exclusive access → already_locked (a no-op
        # success); if it's ready-but-UNMOUNTED, the CDB issues and sets the basic
        # Prevent → done. Both are correct — accept either, and release a lock we
        # actually set so the tray is left clean for the eject step / next pass.
        run tray lock
        case "$OUT" in
            *already_locked*) ok "tray lock on a mounted disc → already_locked" ;;
            *done*) ok "tray lock on a ready (unmounted) disc → done (basic Prevent set)"
                    "$MOS" tray unlock >/dev/null 2>&1; info "released it again to leave the tray unlocked" ;;
            *) bad "tray lock on a ready disc: expected already_locked or done ($(printf '%s' "$OUT" | grep -i outcome | tr -s ' '))" ;;
        esac

        # 3) selector equivalence with media (diskN exists): every form must resolve
        # to the same registry_id (compare identity, not selector-dependent output).
        bn="$(list_field bsd_node)"; rid="$(list_field registry_id)"
        if [ "$HAVE_SCHEMA" = 1 ] && [ -n "${bn:-}" ] && [ "$bn" != null ] && [ -n "${rid:-}" ] && [ "$rid" != 0 ]; then
            dn="${bn##*/}"
            r1="$(state_regid "$dn")"; r2="$(state_regid "$bn")"
            r3="$(state_regid --bsd "$dn")"; r4="$(state_regid --registry "$rid")"
            if [ "$r1" = "$rid" ] && [ "$r2" = "$rid" ] && [ "$r3" = "$rid" ] && [ "$r4" = "$rid" ]; then
                ok "diskN / /dev/diskN / --bsd / --registry all resolve to registry_id $rid"
            else bad "selector forms resolve differently (got $r1/$r2/$r3/$r4, want $rid)"; fi
        else skp "selector equivalence" "need python3 + a mounted disc"; fi

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
    if step "WATCH stream (~20s): set an empty baseline, then you generate events."; then
        # Baseline: if a disc is already loaded, eject it FIRST so the stream
        # starts from a known empty drive — then the operator inserts (and may
        # eject again) during the window to produce a clean lifecycle. Without
        # this, a disc that's already in defeats the insert transition.
        st="$(current_state)"
        case "$st" in
            *ready*|*loading*|*busy*|*formatting*|*media_unreadable*)
                info "a disc is already loaded (state: $st) — ejecting it to set an empty baseline"
                run tray eject
                ;;
            *) info "no disc loaded (state: ${st:-unknown}) — baseline already empty" ;;
        esac
        evf="$(mktemp -t mos_watch.XXXXXX)"
        "$MOS" watch --json >"$evf" 2>/dev/null & wpid=$!
        info "watching (pid $wpid) for ~20s — INSERT a disc now (then you may eject it)…"
        sleep 20; stop_watch "$wpid"
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
        stop_watch "$wpid"
        "$MOS" tray close >/dev/null 2>&1
    fi
}

# Interactive capture setup. Surfaces the three things to confirm before we touch
# the filesystem: (1) we will CREATE per-drive subdirectories under the output
# root (the mos repo root by default); (2) scrub the serial or keep byte-perfect;
# (3) save the run log + manifest. Returns 0 to proceed, 1 to cancel. Sets RAW
# and SAVE_LOG. Non-interactive uses the flag/env defaults. The salt stays a quiet
# runtime flag — never prompted.
capture_setup() {
    csbase="$1"
    printf '\n%sCapture setup%s — fixture + diagnostic capture\n' "$B" "$Z"
    info "Output root : $csbase"
    info "I will CREATE per-drive subdirectories there (vendor/model/) and write the"
    info "fixtures (and, if you choose, a run log + manifest) into them."
    if [ "${SMOKE_NONINTERACTIVE:-0}" = 1 ]; then
        info "(non-interactive: proceeding; scrub=$([ "$RAW" = 1 ] && echo off || echo on), save-log=$SAVE_LOG)"
        return 0
    fi
    printf '   %screate capture subdirectories under that path?  [Enter] yes   [q] cancel%s ' "$D" "$Z"
    read -r r || r=q
    case "$r" in q|Q) info "capture cancelled — nothing written"; return 1 ;; esac

    if [ "$RAW_FORCED" = 1 ]; then
        info "--raw: serial will NOT be scrubbed — byte-perfect output (keep it private)."
    else
        printf '   %sscrub the drive serial from fixtures & logs?  [Enter] yes (safe to share)   [n] no, byte-perfect%s ' "$D" "$Z"
        read -r r || r=y
        case "$r" in n|N) RAW=1 ;; *) RAW=0 ;; esac
    fi

    printf '   %ssave the run log + manifest alongside the fixtures?  [Enter] yes   [n] no%s ' "$D" "$Z"
    read -r r || r=y
    case "$r" in n|N) SAVE_LOG=0 ;; *) SAVE_LOG=1 ;; esac
    return 0
}

phase_capture() {
    hdr "CAPTURE — raw MMC hex fixtures (mos probe --capture)"
    if [ "$HAVE_PY" != 1 ]; then skp "capture" "needs python3 to decode hex → .bin"; return; fi

    # Output root defaults to the mos repo root (SMOKE_CAPTURE_DIR overrides). Ask
    # before creating anything (subdir permission, scrub-or-raw, save-log).
    capbase="${SMOKE_CAPTURE_DIR:-$HERE/hw-captures}"
    capture_setup "$capbase" || { skp "capture" "cancelled at setup"; return; }

    # Organize by vendor/model so many drives stay tidy; the filename carries the
    # rest (mos version, firmware revision, command, disc type, state), and the
    # whole run (bins + manifest + log) lands together under one drive dir.
    vendor="$(drive_vendor)"; model="$(drive_model)"; ver="$(mos_ver)"; rev="$(drive_revision)"
    outdir="$capbase/$vendor/$model"
    mkdir -p "$outdir" || { bad "capture: cannot create $outdir"; return; }
    if [ "$SAVE_LOG" = 1 ]; then
        LOGFILE="$outdir/mos-v${ver}-${model}-${rev}-capture-run.log"
        : > "$LOGFILE" 2>/dev/null
        lg "mos hw-smoke capture run"
        lg "started : $START_ISO   (UTC, same form as the C capture/probe ts)"
        lg "binary  : $MOS"
        lg "version : $ver   vendor: $vendor   model: $model   revision: $rev   raw: $RAW"
    else
        LOGFILE=""
    fi
    info "drive       : v$ver  $vendor/$model  fw $rev"
    info "fixtures    : mos-v$ver-$model-$rev-<command>-<disctype>-<state>.bin"
    info "output dir  : $outdir"
    if [ "$RAW" = 1 ]; then
        info "MODE: byte-perfect (serial NOT scrubbed) — output contains your real serial, keep PRIVATE."
    else
        info "MODE: serial-scrubbed (keyed BLAKE2b) — safe to share; file chosen .bin into"
        info "tests/fixtures/ per tests/fixtures/README.md (hardware-role ADR: reviewed, not auto-canonical)."
    fi
    [ "$SAVE_LOG" = 1 ] && info "run log     : $(basename "$LOGFILE")   (T+ timeline; started $START_ISO, UTC like the C ts)"
    info "Note: capture self-gates on exclusive access — a MOUNTED disc records BUSY;"
    info "the rich replies come from the auto-unmounted pass. I also race the spin-up"
    info "to grab a LOADING pass (becoming-ready replies) when the window is catchable."

    CAP_PARTIAL=0; cap_discs=0   # completeness tracking for the log/manifest marker

    # 0) Read the serial FIRST, on an empty drive — feature 0108h (GET
    #    CONFIGURATION) reads cleanly there, so we know what to scrub before any
    #    hex is captured. (Skipped under --raw.)
    if step "EMPTY the drive (no disc) so I can read the serial cleanly first."; then
        "$MOS" tray eject >/dev/null 2>&1
    fi
    serial_arm "$outdir"

    # 1) empty / open baseline: INQUIRY + GET CONFIGURATION return data with no
    #    media; the media commands record their no-media sense.
    if step "EMPTY-DRIVE capture: confirm the drive is empty, then continue."; then
        "$MOS" tray eject >/dev/null 2>&1
        st="$(current_state)"
        capture_run baseline "${st:-empty}" "$outdir" "$model" "$ver" "$rev"
    else
        CAP_PARTIAL=1   # no baseline → partial run
    fi

    # 2) per-disc-type: race the spin-up for a LOADING pass → capture MOUNTED
    #    (records the BUSY gate) → unmount → capture UNMOUNTED (productive
    #    replies) → eject to prep the swap.
    while : ; do
        [ "${SMOKE_NONINTERACTIVE:-0}" = 1 ] && break
        ask_disc_type || break
        if ! step "Insert the '$DISC_TYPE' disc NOW (press Enter the moment it goes in)."; then CAP_PARTIAL=1; continue; fi

        # LOADING pass: poll for the spin-up window (3–5s on many drives) and fire
        # a capture the moment the drive reports loading — grabs becoming-ready
        # replies. The disc is unmounted during load, so capture's exclusive access
        # is free. If the window is missed (already ready), skip it with a note.
        got_loading=0; i=0
        while [ $i -lt 16 ]; do
            case "$(current_state)" in
                *loading*) capture_run "$DISC_TYPE" loading "$outdir" "$model" "$ver" "$rev"; got_loading=1; break ;;
                *ready*|*open*|*empty*|*unreadable*|*fault*) break ;;
            esac
            sleep 0.3; i=$((i+1))
        done
        [ "$got_loading" = 0 ] && info "loading window not caught (already settled) — no loading pass"

        # settle to ready before the mounted pass
        i=0; while [ $i -lt 12 ]; do
            case "$(current_state)" in *ready*|*open*|*empty*|*unreadable*|*fault*) break ;; esac
            sleep 0.5; i=$((i+1))
        done

        # MOUNTED pass (as inserted) — records the exclusive-access BUSY behavior
        capture_run "$DISC_TYPE" mounted "$outdir" "$model" "$ver" "$rev"

        # UNMOUNTED pass — the canonical fixtures. Unmount the filesystem; the disc
        # stays in the drive (diskutil unmountDisk, not eject).
        bn="$(list_field bsd_node 2>/dev/null)"
        if [ -n "${bn:-}" ] && [ "$bn" != null ]; then
            if diskutil unmountDisk "${bn##*/}" >/dev/null 2>&1; then info "unmounted ${bn##*/} (disc still loaded)"
            else info "could not unmount ${bn##*/} — the unmounted pass may also report BUSY"; fi
        fi
        capture_run "$DISC_TYPE" unmounted "$outdir" "$model" "$ver" "$rev"

        DISC_TYPES_DONE="$DISC_TYPES_DONE $DISC_TYPE"; cap_discs=$((cap_discs + 1))
        info "disc types captured:${DISC_TYPES_DONE}"
        "$MOS" tray eject >/dev/null 2>&1 && info "ejected — ready for the next disc"
    done

    # Completeness marker for the log/manifest: a run with no disc captured, or any
    # skipped step, is PARTIAL — both are valid (this is a fixture AND diag tool),
    # but a consumer should know whether the matrix was fully walked.
    [ "$cap_discs" = 0 ] && CAP_PARTIAL=1
    cap_status="complete"; [ "$CAP_PARTIAL" = 1 ] && cap_status="partial"
    lg ""; lg "capture status: $cap_status ($cap_discs disc type(s); raw=$RAW)"

    # (Fixtures are already serial-scrubbed INLINE at capture time — born clean.)
    # Aggregate metadata + results + the scrubbed full logs into one postable file.
    if [ "$SAVE_LOG" = 1 ]; then
        mout="$(build_manifest "$outdir" "$model" "$ver" "$rev" "$vendor" "$cap_status")"
        [ -n "$mout" ] && ok "$mout"
    else
        info "run log + manifest NOT saved (your choice); fixtures + per-capture NDJSON kept."
    fi

    hdr "CAPTURE OUTPUT ($cap_status)"
    if command -v find >/dev/null 2>&1; then
        nbin=$(find "$outdir" -name '*.bin' 2>/dev/null | wc -l | tr -d ' ')
        nman=$(find "$outdir" -name '*.ndjson' 2>/dev/null | wc -l | tr -d ' ')
        info "$nbin .bin fixture(s) + $nman NDJSON log(s) in $outdir"
    fi
    if [ "$RAW" = 1 ]; then
        info "RAW: artifacts are BYTE-PERFECT and contain the real serial — keep them PRIVATE."
    else
        info "POSTABLE (serial-scrubbed inline, sha recomputed): capture-manifest.md aggregates"
        info "the full logs + results; SERIAL-SCRUB.txt records the false serial + BLAKE2b hash."
    fi
    info "File chosen .bin into tests/fixtures/ with a dated README entry"
    info "(tests/fixtures/README.md)."
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
        capture) phase_capture ;;
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
   7) capture  raw MMC hex → .bin fixtures, per disc type, serial-scrubbed
   a) all      run 1–6 in order (capture is separate, run it explicitly)
   s) summary so far     q) quit
EOF
        printf '%s> %s' "$D" "$Z"; read -r pick || pick=q
        case "$pick" in
            1) phase_static ;; 2) phase_errors ;; 3) phase_empty ;;
            4) phase_tray ;; 5) phase_disc ;; 6) phase_watch ;;
            7) phase_capture ;;
            a|A) run_section all ;;
            s|S) summary ;;
            q|Q) break ;;
            *) info "pick 1–7, a, s, or q" ;;
        esac
    done
}

# ---- main --------------------------------------------------------------------
SECONDS=0
# Wall-clock start in the C side's RFC 3339 UTC form (ms precision via python when
# present; whole-second fallback otherwise) so the script timeline aligns with the
# capture/probe `ts` fields.
START_ISO="$(python3 -c 'import datetime; n=datetime.datetime.utcnow(); print(n.strftime("%Y-%m-%dT%H:%M:%S.")+("%03dZ"%(n.microsecond//1000)))' 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%S.000Z)"
setup_validator
banner

# Parse flags (--raw, --salt=VALUE) from anywhere in argv; the remaining non-flag
# word (if any) is the section. Flags override the SMOKE_RAW / MOS_SCRUB_SALT env.
ARG=""
for a in "$@"; do
    case "$a" in
        --raw)     RAW=1; RAW_FORCED=1 ;;
        --salt=*)  SCRUB_SALT="${a#--salt=}" ;;
        --*)       echo "unknown flag '$a' (--raw, --salt=VALUE)" >&2; exit 2 ;;
        *)         ARG="$a" ;;
    esac
done

case "$ARG" in
    static|errors|empty|tray|disc|watch|capture|all)
        run_section "$ARG"; summary; [ "$FAIL" -eq 0 ] ;;
    "")
        if [ "${SMOKE_NONINTERACTIVE:-0}" = 1 ]; then
            run_section all; summary; [ "$FAIL" -eq 0 ]
        else
            menu; summary; [ "$FAIL" -eq 0 ]
        fi ;;
    *)
        echo "unknown section '$ARG' (static|empty|tray|disc|watch|errors|capture|all)" >&2; exit 2 ;;
esac
