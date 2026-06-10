# Full-tree review (2026-06-10) — 21 angles, line-by-line

Maximum-depth review of the whole tree at 0.4.0-dev (post-DR-pivot,
post-`mos_query_disc_info`). Method: 21 independent review agents —
eleven file-area assignments reading every line of their files
function by function, four test-suite audits reviewing the tests as
code, six cross-cutting analyses (amalgamated-TU, concurrency/signals,
adversarial input, CF/IOKit object lifecycle, docs-vs-tree truth,
schemas-as-contract) — each armed with the vendored ground truth
(`docs/apple/DiscRecording/*.h`, `IOSCSIArchitectureModelFamily`
kernel + UserClientLib source). Every surviving candidate was then
verified against the primary source before disposition. Fixes landed
as commit `46be3d7`.

## Verdicts by area

CLEAN (no findings after exhaustive pass):
- Pure foundation (mos_pure.c/h, mos_sense.c, mos_scsi_status.h) —
  every parser bound re-derived; all fail-closed properties hold.
- State decision core (mos_state_core.c, mos_state.c) — the §5.5 nub
  gate re-verified against the vendored kernel's PollForMedia
  predicate; every decision-tree branch traced; no unhandled
  (status, sense) combination.
- DR adapter + glue (mos_dr.c, mos_internal.h, mos_strings.c,
  mos_result.c, mos_config.c, mos_discinfo.c).
- CLI dispatch + common (main.c, common.c/h) — full flag/selector/
  subcommand validation matrix enumerated; 64/65-drive cap arithmetic
  traced.
- DR/CF API conformance — every call vs the vendored headers:
  signatures, Add/RemoveObserver symmetry, Copy/Get ownership on all
  paths, dictionary key nesting, teardown order.
- CF/IOKit lifecycle ledger — every acquisition API's every call site
  balanced on every control path.
- Amalgamated TU (dist/mos.c) — no duplicate file-scope identifiers,
  preprocessor balance, declaration order, byte-identical header.
- Adversarial input — all seven external-byte ingress vectors traced
  to sinks (with one wrong sub-claim, see Contradiction below).
- Concurrency/signals — eight candidates raised, all refuted on
  verification (e.g. the strtoul-ERANGE concern is caught by the
  existing range check: ULONG_MAX > 3600000).

## Real bugs found and FIXED

1. **Terminal injection on the human output path** (the review's one
   security finding). cli/human.c is layout-only and prints verbatim;
   the two sites feeding it drive-controlled identity strings
   (status's Drive row, list's table rows) never escaped them — `mos
   status` / `mos list` in HUMAN mode wrote raw drive bytes to the
   tty while the JSON path escaped everything. A vendor field
   containing ESC[… injected ANSI/OSC sequences. Fixed at the
   construction sites via the pure mos_safe_ascii (\xNN for
   everything outside printable ASCII), with human.h now stating the
   sanitization contract and list_row storing the escaped form in
   widened buffers.

   Severity correction (owner, same day): post-pivot these strings
   are NOT raw MMC bytes — they pass through DR's closed parse
   (device → kernel → DRDeviceCopyInfo CFString → UTF-8), which
   plausibly launders the 8-bit C1 vector (encoding reinterpretation)
   and moved the parsing-level threats behind Apple's code; that is
   exactly why the INQUIRY-side parsing defenses were retired. The
   fix stands on three narrower legs: ESC (0x1B), the byte ANSI/OSC
   injection actually needs, is encoding-stable through every
   transform in the chain; the header promises only "extracted from
   the device" — an unverifiable intermediary, and the GetTrayState
   lesson is that unverifiable Apple convenience layers don't get
   trusted with safety properties; and the JSON path escapes these
   same strings, so the doctrine is all sinks or none. Disposition:
   defense-in-depth per doctrine, not a demonstrated exploit.

   Process note: a parallel cross-cutting agent had asserted this
   path was already escaped — the contradiction was settled by
   reading the file, not by majority (see Contradiction).
2. **watch-all join demotion** (contract bug). The multiplexer
   cleared join_pending on a joining slot's FIRST event of any kind;
   a hot-plugged drive whose probe failed initially (ERROR first)
   would later announce itself as a plain mid-stream `snapshot`
   instead of `device_appeared`. Now clears only on the actual
   relabel; pinned by a new error-first-join test.
3. **index:0 was schema-invalid** (contract mismatch, found
   independently by two agents). The emitter deliberately emits
   index 0 for "unresolvable at capture time" (documented in
   resolve_index_of) while mos.state.v1 required minimum:1 — the
   --bsd TOCTOU path produced invalid documents. Pre-tag, the schema
   was amended in place to the documented sentinel (minimum 0,
   description updated), with a positive example and a negative
   (-1) fixture.

## Contract/doc repairs

- mos.h: the "(when out is non-NULL)" phrasing on mos_query_state /
  mos_query_disc_info / mos_watch_next_event read as out-is-optional
  (the same phrase pattern marks genuinely-optional err_out
  elsewhere); all three now state out is REQUIRED.
- mos.event.v1's error-object description names the absent `context`
  field (not just the excluded `ok` code).
- CONTRIBUTING.md still described the retired tools/mos.c /
  mos_cli_io.c layout and called accessor results "fields"; updated.
- test_cli.sh's duplicated test numbering (two Test 16–19 blocks)
  renumbered.

## Hardening (defense beyond current reachability)

- mos_raw_cdb rejects NULL h->mmc; the exclusive-release comment now
  states the have_exclusive invariant instead of implying
  unconditional acquisition; failed QueryInterface defensively NULLs
  h->mmc against a COM-contract-violating plug-in.
- Notification probe: mach-timebase conversion divides first (the
  plain delta*numer overflowed uint64 on Intel timebases at
  multi-day uptimes), EX_USAGE replaces a bare exit 2, short plist
  writes during --dr-dump now warn on stderr.

## Test debt paid (from the test-suite audits)

New tests: deferred sense formats 0x71/0x73 (both layout variants);
config descriptor ending exactly at the trusted span; leading-zero
BSD names pinned as parse-numerically/render-canonical;
mos_state_result_registry_id and mos_watch_event_sense accessors
(previously zero coverage); multiplexer last-device-removal (stream
stays open at UINT64_MAX, never TERMINAL); cross-rate deadline
folding (min, not max); error-first join (with fix 2).

## Refuted candidates (recorded so they aren't re-litigated)

- strtoul ERANGE in getenv_uint — the 3600000 range check catches
  ULONG_MAX. — relaxed-CAS ordering — single-thread contract +
  monotonicity make relaxed sufficient. — slot-overwrite vs consumer
  pointers — callbacks fire only inside the NEXT next_event call,
  when prior event pointers are already contract-invalid. —
  emit_startup-before-listeners in the probe — failure exits non-zero,
  the startup line is a header. — wait-and-classify set -u — the
  capture assigns before `||` fires. — object==NULL asymmetry across
  DR callbacks — each callback's null-handling matches its
  responsibility (can't join/remove an unidentified device; CAN wake
  broadly). — QueryInterface-garbage reliance — standard COM
  contract (still hardened, above). — CFRunLoopAddSource silent
  failure — void API, Apple semantics; the degraded mode is the
  documented nanosleep fallback. — emit_json field order vs fixtures
  — JSON field order is non-contractual.

## Contradiction note (process)

The adversarial-input agent asserted the human path was escaped; the
escaper-audit agent quoted the raw fputs sites. Direct file read
settled it for the escaper agent. The lesson is the standing one:
agent consensus is not verification — the highest-severity claim of
a run gets primary-source confirmation regardless of how many agents
agree or disagree.

## Still open (deliberate, with owners)

- Hardware-only unknowns: unchanged list (registry-path shape, DR
  doorbell delivery, drutil parity, identity byte-shape, GESN-window
  coexistence, watch-all hot-plug ordering) — INTEGRATION_HARNESS.
- Deferred efficiency items from the PR#2 review: per-StatusChanged
  CopyInfo, single-target watch carrying all-mode state, 1-slot-all
  unification — re-open triggers recorded there.
- Low-value observations not acted on: fuzz length-sweep for GESN
  beyond 0..8 (decoder is bounds-gated; marginal), test-harness
  fail-fast mode (design choice), multi-line TEST() in the orphan
  grep (convention is single-line), validate.py brace-regex
  fragility (guarded by the FAIL-on-empty-extraction arm).
