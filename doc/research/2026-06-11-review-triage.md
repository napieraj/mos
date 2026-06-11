# 2026-06-11 — review triage

Triage record for the post-DR-pivot review round. Two sources feed it:

1. An external review (uploaded-tree, Linux environment) delivered
   2026-06-11 with five findings, triaged below. The reviewer also
   reported their own check matrix (Debug + GCC ASan/UBSan + strict
   `-W… -Werror` builds, 219/219 portable tests; schema fixtures;
   doc-staleness; dist regeneration clean). Those results are
   reported-by-reviewer; locally reproduced on this branch: pure
   suite green, dist/ in sync after regeneration.
2. Four in-session review agents (general core, CLI/contracts,
   adversarial watch, memory safety) — findings to be appended to
   this file when triaged.

Every entry below was verified against the working tree
(branch `claude/watch-dedup-dr-refs-7fqt8g`) before triage, per
CLAUDE.md rule 6. Severity is the triager's, not the reviewer's.
Dispositions follow AGENTS.md Process rule 2: behavior changes need
more than a review point; comment/test alignment does not.

## External review findings

### E1. `list --json` carries display-safe identity; `status`/`watch --json` carry raw

**Verified.** `list_row` stores identity after `mos_safe_ascii`
(cli/common.c:208-210) and `emit_list_json` re-escapes the stored
form (cli/common.c:262-270) — documented as deliberate at
cli/common.c:200-207. But `status --json` and `watch --json` pass
RAW identity through the JSON escaper (cli/status.c:104→148-159,
cli/watch.c:67→121-129; their `mos_safe_ascii` call sites are the
human emitters only). For printable identity the three agree; for
hostile bytes the same device renders differently across commands
(`\\x1b…` literal text in list vs JSON-escaped raw bytes in
status/watch).

- Severity: medium (cross-command JSON contract divergence).
- Disposition: **maintainer decision required** — either (a) store
  raw in `list_row` and apply `mos_safe_ascii` only at the table
  emitter, aligning list with status/watch, or (b) keep
  stored-sanitized and document the list fields as display-safe in
  schema + README. Pre-first-tag, the schema ADR permits an in-place
  v1 change either way. Option (a) is the cleaner data contract;
  the in-tree comment defends double-escaping's safety but never
  addresses cross-command consistency, which is the actual finding.

**Provenance audit (2026-06-11, settles the "is it pre-sanitized"
question).** Hypothesis checked: post-DR-pivot the identity strings
are the same data drutil shows, so perhaps the platform already
sanitizes them to printable ASCII and the divergence is unreachable.
**Refuted at the canonical source.** The full chain:

1. Drive → kernel: `IOSCSIPeripheralDeviceNub::InterrogateDevice`
   (apple-oss-distributions/IOSCSIArchitectureModelFamily,
   IOSCSIArchitectureModel/IOSCSIPeripheralDeviceNub.cpp) `bcopy`s
   the raw INQUIRY vendor/product/revision fields at SPC field
   width, NUL-terminates, and calls `StripWhiteSpace`
   (SCSILibraryRoutines.cpp) — which, verified verbatim, removes
   **trailing 0x20 bytes only**. No printable-range check, no
   control-byte filtering, anywhere on the path to
   `kIOPropertySCSIVendorIdentification` /
   `…ProductIdentification` / `…ProductRevisionLevel`. The only
   other byte-level effect is truncation at an embedded NUL
   (`OSString::withCString`).
2. Kernel → DR: closed source. The SDK header (DRCoreDevice.h,
   §11's public mirror) promises only a CFString "extracted from
   the device" for `kDRDeviceVendorNameKey` /
   `kDRDeviceProductNameKey` / `kDRDeviceFirmwareRevisionKey` —
   no encoding, charset, or sanitization contract.
3. DR → mos: `mos_internal_dr_copy_string` (src/mos_dr.c:58-71)
   converts via `CFStringGetCString(kCFStringEncodingUTF8)`, which
   preserves control bytes (0x01–0x1F are valid UTF-8).

Consequence: ESC (0x1b) and friends survive drive firmware → mos
buffers end-to-end; drutil's output looks clean because conforming
firmware puts printable ASCII in INQUIRY fields (an SPC SHOULD),
not because anything enforces it — exactly the conforming-drive
assumption the scope doctrine's adversarial-input clause refuses.
The `mos_safe_ascii` defense for terminal output stays load-bearing,
the E1 divergence is reachable with hostile firmware, and the
decision between (a) and (b) cannot be resolved by appeal to
platform sanitization.

**Version bound on link 1 (corrected twice, same day).** The
kernel-side evidence is from the LAST PUBLISHED source of the
family: apple-oss-distributions/IOSCSIArchitectureModelFamily's
newest tag is 139.0.2 (February 2005; `main` tracks it — including
the IOSCSIMultimediaCommands / PeripheralDeviceType05 optical
driver). A first version of this note framed that as "21 years
unverifiable" — uncalibrated, and the tree already held the
calibration (doc/research/2026-06-11-headless-adapter-emulation.md,
"drift calibration": the current 26.4 SDK still ships SCSITaskLib.h
with Copyright 2001-2009 and availability markers stopping at 10.6
— the userspace SAM surface byte-frozen since ~2009, a strong prior
of implementation quiescence; a prior, not a proof. Companion
evidence: DRCoreDevice.h byte-stable 10.13→15.5,
2026-06-10-dr-pivot-feasibility.md §2). The calibrated reading
STRENGTHENS the audit's conclusion rather than hedging it: the
last-published source sanitizes nothing, and the quiescence prior
says the shipping kext still behaves like that source — so
"no platform sanitization, then and now" is the best-evidence
conclusion, with residual drift (~2005-2009 window, plus the
binaries-change-behind-frozen-headers caveat) owned by the hardware
falsification leg like every other kernel-predicate claim. The bar
for relaxing any identity-byte defense is unchanged: positive
evidence (disassembly or Apple doc), dated, recorded here. The
§5.5-citations-are-Tiger-vintage observation is already on record
in the same artifact as a candidate doctrinal annotation; nothing
new to add from this audit.

### E2. "Profile-class change" fallback compares raw profile codes

**Verified.** `profile_class_changed_without_id`
(src/mos_watch_core.c:441-444) fires on ANY non-zero profile change
without media identity, so a same-class different-profile swap
(DVD-ROM 0x10 → DVD-R 0x11) fires too — the comment and name say
"class", and tests cover only cross-class (0x08→0x10,
tests/test_watch_core.c:345) and same-profile-no-fire
(tests/test_watch_core.c:373). The comment's own can't-see example
("DVD-R → DVD-R", src/mos_watch_core.c:435) is a same-PROFILE swap,
so the prose is internally loose, not just misaligned with code.

- Severity: low. The broader behavior is almost certainly the
  desired one: a profile flip with no identity available means a
  different disc; suppressing same-class flips would hide real swaps.
- Disposition: align name/comment/tests to the actual (any-profile)
  semantics — rename to `profile_changed_without_id`, reword the
  comment, add a same-class different-profile test pinning the fire.
  No behavior change.

### E3. `mos_cli_json_str` does unchecked `need + 1`

**Verified as written** (cli/io.c:21-27). `need + 1` could wrap only
if the escape of an in-memory string measured SIZE_MAX bytes — not
constructible on the target (identity fields are 8/16/4 bytes;
probe-path CF properties are kernel-bounded). No `mos_json_escape`
error sentinel exists to check today.

- Severity: nit (theoretical hardening).
- Disposition: cheap saturating guard, fold into a cleanup pass.

### E4. probe timestamp formatter ignores `clock_gettime`/`gmtime_r` failures

**Verified** (`format_rfc3339_utc`, cli/probe.c:104-117; reviewer's
line range matches). Failure requires libc-level breakage
unreachable on the target (64-bit time_t); the path is the
diagnostic `mos probe` subcommand.

- Severity: nit.
- Disposition: zero-init + return-check, fold into a cleanup pass.
  (CLAUDE.md's wrong-target-hardening note applies: this is
  developer-facing diagnostic tooling, not the adversarial-input
  surface.)

### E5. `mos_internal_dr_copy_identity_for_service` assumes non-NULL buffers when cap > 0

**Verified** (src/mos_dr.c:222-224). Internal helper, single in-tree
caller passes real stack arrays (src/mos_scsi.c); `NULL, nonzero`
would be a caller bug, not a reachable state.

- Severity: nit (internal precondition undocumented).
- Disposition: add `vendor && vcap`-style guards or a one-line
  precondition comment, fold into a cleanup pass.

## In-session agent findings

Four agents, all completed 2026-06-11; every entry below re-verified
against the tree by the triager before recording. Duplicates across
agents are cross-referenced, recorded once. The two reviews that ran
the pure suite independently report 219/219 green.

### Agent A — library core + public API

**A1. `mos.h` header claims raw access "is not on the default
state-query path" — false since the GESN redesign.** Verified:
include/mos.h:5-7 vs the not-ready branch (mos_state_core.c →
`mos_internal_mmc_get_tray_state` → `mos_raw_cdb` →
`ObtainExclusiveAccess`, src/mos_scsi.c). ARCHITECTURE §3 already
states the opposite, correctly. mos.h:275 ("no exclusive access
required") has the same gap. Severity: medium (public contract
prose). Disposition: doc fix — align mos.h with ARCHITECTURE §3
and note the brief not-ready-path lock.

**A2. `mos_scsi_status.h:13-16` justifies its own existence with a
false claim** — Apple's SCSITask.h does define RESERVATION_CONFLICT
/ TASK_SET_FULL / ACA_ACTIVE (verified by the agent against a
fetched macOS 15.5 SDK header; values identical to ours, behavior
unaffected). The valid no-SDK-dependency justification already
leads the comment. Severity: low. Disposition: strike the
"incomplete set" sentence.

**A3. ARCHITECTURE.md body still marks shipped `mos_query_disc_info`
as planned.** Verified: heading at §4.4 updated, body at
ARCHITECTURE.md:291-292 and :422 still say "planned" / "not yet
implemented"; the API ships (src/mos_scsi.c, tests/test_discinfo.c).
Severity: low. Disposition: doc fix.

**A4. `mos_error_is_recoverable` docstring omits `MOS_ERR_IO` from
both lists** (include/mos.h:573-578; impl returns false,
src/mos_strings.c). Severity: low. Disposition: one-word doc fix.

**A5. `MOS_ERR_UNSUPPORTED` carries two contradictory meanings.**
Verified: include/mos.h:108 "command not supported by drive" vs
mos_strings.c:51 "not implemented in this build"; both meanings
live (kIOReturnUnsupported map vs the get_features stub). A drive
rejection reads as a build limitation. Severity: low. Disposition:
maintainer decision — reword the description string to cover both,
or split the enum (v0.next; the error-code string set is open under
mos.error.v1, but the description is consumer-visible).

**A6. `mos_internal_toc_parse` has zero production callers** —
test/fuzz-only, the orphan shape the get_features stub exists to
avoid; referenced by the v0.4 media-info design. Severity: nit.
Disposition: deliberate decision either way; not a removal proposal.

**A7. Duplicate registry-id accessor** (src/mos_scsi.c:178 public vs
:189-191 internal twin, identical bodies). Severity: nit.
Disposition: cleanup pass.

Agent A reports the decision tree, sense table, GESN gates, config
walker, TOC bounds, disc-info layout, raw-CDB lock pairing, IOReturn
map, sysexits, and BSD-name domain guards checked line-by-line
against ARCHITECTURE/tests with no correctness findings.

### Agent B — CLI + contracts

**B1. Contract test pins the INVERSE of the published `bsd`
contract.** Verified: tests/cli/test_cli.sh:179-180 comment "Field
name is bsd_name, not bare 'bsd'" + assert_not_contains '"bsd":',
while the emitter (cli/common.c:113), the schema
(mos.error.v1 `bsd`), and the positive fixture all say `bsd`. The
assertion passes only because that test's failure path legitimately
omits the field. Same fossil in the mos.error.v1.json:5 prose
("bsd_name field is conditional") and the negative-fixture filename
(schemas/negative/mos.event.v1.missing_bsd_name.json). Severity:
medium — someone "fixing" the emitter to match the test comment
breaks the schema. Disposition: fix the test comment + assertion
rationale, schema prose, and (optionally) fixture name. Doc/test
text only; no behavior change.

**B2. Bare positional drive is rejected as "unknown subcommand",
contradicting the documented grammar.** Verified: README.md:22-29
and `print_usage` (cli/main.c:37-43) declare `mos [subcommand]
[drive] [options]` with the drive positional "like diskutil info
disk4", but the dispatch (cli/main.c:183) intercepts every bare
non-dash first word, so `mos 2` / `mos disk4` exit EX_USAGE.
Untested either way. Severity: medium. Disposition: maintainer
decision — make dispatch fall through to drive parsing when the
word has index/BSD shape, or fix the documented grammar.

**B3. validate.py's enum drift guard skips mos.list.v1.** Verified:
schemas/validate.py:55-57 covers state/event/prev_state only;
mos.list.v1's `drives[].state` enum is emitted from the same
`mos_state_description()` (cli/common.c) but unchecked — and
AGENTS.md's schema ADR claims lockstep. Severity: medium-low (CI
gap, latent). Disposition: extend validate.py; tooling-only, safe
to fix.

**B4. mos.error.v1 `exit_code` enum lists unreachable 77; usage
text omits reachable 70.** Verified: schema enum
[64,66,69,70,71,74,75,77] vs `mos_error_sysexit` range
{0,64,66,69,70,71,74,75} (src/mos_strings.c:123-138); usage
(cli/main.c:87-88) omits 70. Severity: low. Disposition: drop 77
from the schema (pre-tag, in place), add 70 to usage.

**B5. Schema prose misdescribes `bsd` against the project's own
naming standard.** Verified: mos.state.v1.json `bsd` description
says "Whole-disk BSD name (e.g. disk4)" with a `/^r?disk[0-9]+$/`
claim while its own `pattern` is `^/dev/disk[0-9]+$` and the
emitter sends the dev node; mos.error.v1.json:16 same. Severity:
low. Disposition: doc fix inside the schema files.

**B6. Banned synonym + stale version anchor in
INTEGRATION_HARNESS.md:387-390** ("device path", "v0.3's typed
APIs" — the possessive evades doc-staleness.sh's regex). Verified.
Severity: low. Disposition: doc fix; consider widening the
staleness regex.

**B7. schemas/README.md:64-65 cites emitter functions that don't
exist** (`list_callback`, `emit_watch_json`; actual:
`emit_list_json`, `emit_watch_ndjson` — grep confirms). Severity:
low/nit. Disposition: doc fix.

**B8. CLI identifiers violating the `mos_cli_` prefix rule;
`mos_error_to_code` is the worst** — a CLI function wearing the
library's reserved `mos_` prefix (cli/common.h:38), invisible to
the archive-scoped symbol-hygiene check; plus unprefixed externs
(`emit_list_json`, `open_sole_drive`, `resolve_index_of`, …).
Verified. Severity: low. Disposition: rename in a cleanup pass
(internal to the CLI binary; no external behavior).

**B9. Example fixture `context` string never produced by any
emitter** ("could not query state" vs the four real context
strings). Verified. Severity: nit (context is documented opaque).
Disposition: align fixture text.

**B10. CMakeLists.txt:30-32 comment misenumerates the pure layer**
(lists mos_pure.c twice; omits config/discinfo/result). Verified.
Severity: nit. Disposition: comment fix.

Agent B reports the machine-enforced surfaces clean: emitter↔schema
field sets exact (closed-set rule holds), escaping choke points
sound, watch NDJSON framing/signal handling pinned, dist/
regeneration byte-identical. The drift concentrates in prose INSIDE
contract artifacts — where doc-staleness.sh doesn't reach.

### Agent C — adversarial watch (post-DR-pivot)

**C1. All-mode treats DR Disappeared as removal authority; a
spurious Disappeared permanently evicts a live drive, and the
Appeared dedupe swallows DR's own correction.** Verified end to
end: dr_device_disappeared_callback calls
`mos_internal_watch_notify_removed` directly (src/mos_watch.c:632),
contradicting the file's own axiom ("DR data never decides state",
src/mos_watch.c:460); the slot frees only after device_removed is
taken (src/mos_watch_core.c:585-589) but `find` matches active-only
slots — terminated cores are still active — so a corrective
Appeared dedupes away (src/mos_watch.c:351-353); nothing rescans,
and StatusChanged deliberately joins nothing
(src/mos_watch.c:503-505). Whether DR can fire a spurious
Disappeared is an open empirical question (the pivot plan lists
delivery semantics as a falsification target). The agent's key
observation: switching to `notify_wake` gives IDENTICAL removal
latency for a real removal (reopen-by-registry-id returns NO_DEVICE
→ terminal; pinned by `watch_removed_via_reopen_failure`) while a
spurious one costs one probe — strictly dominant on the code's own
axioms. Severity: medium (the round's strongest finding).
Disposition: maintainer decision; the wake-not-remove variant is
recommended, with a fake-DR fixture test for the
spurious-Disappeared and terminated-slot-Appeared arms. Until
decided: a comment at the callback recording the trust assumption.

**C2. `mos_watch_open_all` snapshots the directory BEFORE
registering the Appeared observer.** Verified
(src/mos_watch.c:892-899): a device arriving in the gap is
permanently invisible — discovery "has no floor at all" by the
function's own comment — unless DR replays Appeared at
registration, which is unfalsified (the fake doesn't model replay;
no test covers the gap). Reversing the order closes the window by
construction; the existing dedupe already makes the overlap benign.
Severity: medium. Disposition: maintainer decision; low-risk
reorder + a fake-arrival-in-gap test.

**C3. "Ignored until a slot frees" (include/mos.h:500-501) promises
a recovery that doesn't exist.** Verified: the dropped Appeared is
never replayed, slot-free triggers no rescan, StatusChanged joins
nothing — the 17th drive needs a physical replug. Severity:
medium-low (contract prose vs exotic 17-drive reality).
Disposition: fix the doc now ("dropped for that plug session");
directory-rescan-on-free is a v0.next flag.

**C4. Persistently transitional states poll at transition rate
forever.** Verified: EMPTY_OR_OPEN and UNKNOWN classify
transitional (src/mos_watch_core.c:183-184), so a bridge where GESN
persistently fails probes at 200 ms — each a full open +
exclusive-lock GESN attempt + close — indefinitely, with no
escalation analogous to the error backoff
(src/mos_watch_core.c:347-369). Severity: medium-low. Disposition:
v0.next policy flag (cadence change is behavior; needs the
hardware-falsification lens per the hardware ADR).

**C5. Wake/backoff/fairness interaction under a wake storm.**
Verified components: `notify_wake` unconditionally zeroes the
deadline (defeating error backoff); the multiplexer returns on
first EMIT and re-enters at lowest id (starvation under sustained
low-id emits); an UNRESOLVED StatusChanged wakes every slot
(src/mos_watch.c:496-501) — and a failing drive is the likeliest
StatusChanged spammer. Self-healing, deliberate bounded-work trade,
but unexamined. Severity: low. Disposition: record as known trade
+ v0.next test items.

**C6. (= D3) `visited` bitmask silently requires
MOS_WATCH_ALL_CAP ≤ 64** (uint64_t, `1ull << i`; CAP is 16).
Severity: nit. Disposition: add
`_Static_assert(MOS_WATCH_ALL_CAP <= 64, …)` — matches the repo's
width-pinning idiom; cleanup pass.

**C7. All-mode StatusChanged calls CFRunLoopStop even when it woke
nothing** (resolved-but-unknown id falls through to the stop at
src/mos_watch.c:506; the single-target arm returns without
stopping). Verified. Severity: nit (one spurious pump).
Disposition: cleanup pass.

Agent C additionally attests (attack attempted, held): borrowed-
pointer lifetime including all-mode slot reuse, the run-loop gate,
the UINT64_MAX sleep sentinel, mono/wall clock separation, and the
dedupe-before-claim fix at HEAD. Its untested-edge-case list
(spurious Disappeared, terminated-slot Appeared, 17th drive,
fairness under repeated low-id emits, wake-all amplification,
timeout_ms 0/negative through the adapter, two watches sharing the
private mode, the multiplexer TERMINAL branch, sustained
EMPTY_OR_OPEN cadence) is the candidate list for the next test
batch.

### Agent D — memory safety

No high or medium findings; the hostile-byte parse surface and the
CF/IOKit lifetime surface hold under static review (trace record in
the agent report: dual-length rule at every device-length use, all
18 CF Create/Copy sites paired, the probe parent-walk balanced on
all five exits, watch teardown ordering, all four malloc sites, all
12 fixed-buffer copy sites pinned).

**D1. (= E4)** probe timestamp formatter — adds the observation that
the production twin `format_rfc3339` in src/mos_watch_core.c:86-113
DOES guard both failures, so the diagnostic path is an inconsistency,
not just a hardening nit. Disposition unchanged (cleanup pass).

**D2. `emit_list_table`/`emit_list_json` trust `n` against
MOS_CLI_LIST_CAP-sized arrays with no local clamp.** Verified: every
caller's `n` comes from `caq_cb`, which caps at MOS_CLI_LIST_CAP
(cli/common.c:282-291) — safe by construction, but the invariant is
non-local and the function is exported (cli/common.h). Severity:
nit. Disposition: one-line clamp/assert; cleanup pass.

**D3. (= C6)** CAP ≤ 64 static assert. **D4.** `mos_cli_safe_ascii`'s
fixed 4096 buffer can truncate mid-`\xNN` (stderr diagnostics only;
mos_safe_ascii itself fuzzed truncation-safe). Severity: nit.
**D5. (= E5)** mos_dr.c cap-without-pointer guard. Dispositions:
cleanup pass.

## Disposition summary

- **Needs maintainer decision (behavior):** E1 (JSON identity
  contract), B2 (bare-drive grammar), C1 (Disappeared trust —
  wake-not-remove recommended), C2 (observer/snapshot order), A5
  (UNSUPPORTED wording), C4 (transitional cadence, v0.next).
- **Fixable now, no behavior change:** E2 (rename+test), A1-A4
  (doc), B1 (test comment/schema prose), B3 (validate.py), B4
  (schema enum + usage), B5-B7, B10, C3 (mos.h wording), C6/D3
  (static assert).
- **Cleanup-pass batch (nits):** E3, E4/D1, E5/D5, A6, A7, B8, B9,
  C5 (tests), C7, D2, D4.
