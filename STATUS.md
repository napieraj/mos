# v0.4.0-dev — Application Status

**Tree state:** v0.4.0-dev. Pure test suite green under `-Wall
-Wextra -Wpedantic -Werror` (exact count reported by `make test`).
Watch
mode landed; the DiscRecording pivot (directory/doorbell substrate,
watch-all, mos_query_disc_info) landed 2026-06-10 — see CHANGELOG.
Schema family migration complete. Memory-safety and
security reviews passed. CI runs unit tests, strict
adapter compile, sanitizer build, and amalgamation tests on every
push. Hardware validation against BH16NS55 / WH16NS60 / A1379 — the
same matrix that was the v2 gate — has not yet happened against the
v0.4.0-dev binary; the first tag gates on it (plus the DR
falsification rows in INTEGRATION_HARNESS.md).

The v0.3.1-dev cycle added: the empirical notification probe (since
consolidated into `mos probe`, 2026-06-11), conservative wake-dispatch
extension in `src/mos_watch.c` (PropertyChange + IsTerminated only;
self-trigger family deferred to v0.4 pending probe results), the
`mos_internal_bsd_unit_matches` pure helper
consolidating BSD-name partition matching, and three review-driven
fixes: the watch-by-index identity race is closed (validated handle
threads service identity end-to-end instead of re-resolving by BSD
name); the BSD partition predicate now strictly requires `'s'`
followed by digits (rejecting "disk4s", "disk4sx", and similar);
and the `mos.event.v1` schema is now a `oneOf` union per event
kind with explicit `not`-clauses for disallowed cross-kind fields.
See ROADMAP.md v0.3.1-dev section for the self-trigger doctrine.

The v2 contract lock is superseded. Per-document schema versioning
replaces the v2/v3 per-CLI-invocation pinning model; see
`ARCHITECTURE.md §10` for the CLI shape ADR and `AGENTS.md` for the
rewritten JSON evolution policy. The v2-lock historical record
lives in git; `doc/research/2026-04-27-v2-contract-design.md` is
preserved as the research artifact behind that design.

## Shipped to v0.3-dev

**Schema family** replaces the v2 envelope:

- `mos.state.v1` — one-shot success envelope.
- `mos.error.v1` — failure envelope, nested
  `error.{code, message, context, recoverable}` plus top-level
  `exit_code`. Replaces flat `error` + `error_message`.
- `mos.list.v1` — `--list --json` envelope wrapping the array
  (was a bare array).
- `mos.event.v1` — NDJSON event records emitted by `--watch --json`.

Per-document `schema` field carries name and version. `--json`
takes no argument; `--json=anything` returns `EX_USAGE` with a
diagnostic naming `mos.state.v1`.

**Field rename history:** `is_rw` → `is_rewritable` (v0.3.0, since
removed). The `is_rewritable` field was introduced as a rename of an
earlier `is_rw` boolean; the field itself was removed entirely in
v0.3.1-dev (see "Field removal" below).

**Field removal:** `is_rewritable` (and its `is_authoring` internal
backing) removed from `mos_state_result`, `mos_watch_event`,
`mos_device_info_t`, `mos.state.v1`, `mos.list.v1`, `mos.event.v1`,
and all CLI rendering paths. The field was always-true for any drive
mos could see (Apple's kernel filters non-authoring drives out before
mos can reach them via SCSITaskUserClient — see ARCHITECTURE.md §9.1),
misleadingly-named (kext authoring flag ≠ MMC media-class rewritability),
and depended on an unstable Apple-internal kext property. v0.4's
GET CONFIGURATION supported-profile work answers the genuine "can this
drive write rewritable media" question via MMC.

**Sysexits exit codes** replace the 0/1/2 triplet: 0 EX_OK, 64
EX_USAGE, 66 EX_NOINPUT, 69 EX_UNAVAILABLE, 70 EX_SOFTWARE, 71
EX_OSERR, 74 EX_IOERR, 75 EX_TEMPFAIL. Semantic shift: `unknown`
is now an observation (exit 0), not a failure (was exit 1).

**Watch mode** (`-w` / `--watch`):

- Plain-text: one state-change line per event.
- JSON: NDJSON `mos.event.v1` records, one per line, fflush'd.
- IOKit `kIOGeneralInterest` notification for drive detach →
  `device_removed` event → clean exit (EX_OK).
- SIGINT / SIGTERM handled with bounded exit latency.
- Env knobs: `MOS_WATCH_STABLE_MS` (default 2000),
  `MOS_WATCH_TRANSITION_MS` (default 200).

**Additive fields:** `current_profile_name` (MMC-6 §5.4 table)
accompanies the hex-string `current_profile`. Optional
`latency_ms` on `mos.event.v1` envelopes only — the one-shot
`mos.state.v1` envelope has no such field (its closed field set
would reject it; see the JSON evolution ADR in `AGENTS.md`).

## Memory-safety and security review

**Memory safety review passed.** Two reference-counting bugs in
`find_service_by_bsd_name` (use-after-release on `parent`, leak of
`cur` on counter exit) found and fixed. `mos_watch_close` cleanup
order corrected to remove-source → release-token → destroy-port →
release-svc (Apple's documented pattern, closes a window where a
late-dispatch callback could fire with dangling `refcon`).
Defensive open-failure guards in `watch_probe`,
`watch_open_common`, `mos_watch_open_by_index` against the
NULL-handle-with-MOS_OK contract-violation case.
`mos_watch_bsd_name()` accessor added for pump-failure attribution.

**Security review passed.** Threat model covered: hostile
drive (BadBRIDGE-class), hostile env, hostile CLI args, hostile
NDJSON consumer. Critical finding fixed: the standalone smoke probe
(`mos_probe`, retired 2026-06-11) was
printing drive-controlled `vendor` / `product` strings raw to
stdout — terminal-injection vector via ANSI / OSC sequences.
Local `print_safe()` helper added; vendor/product now escape
non-printable bytes as `\xNN`. Main `tools/mos.c` was already
clean via `print_safe_ascii` and `json_escape_str`. Long-running
watch loop verified leak-free per probe; all bounded buffers
verified NUL-safe; all format strings verified literal.

## Shipped in v0.3.1-dev

(Path note: entries below predate the 2026-06-10 cli/ restructure —
`tools/mos.c` is now `cli/{main,common,status,list,watch}.c`,
`tools/mos_cli_io` is `cli/io`, and `emit_watch_plain` no longer
exists (watch is NDJSON-only). The entries describe what shipped when
they were written; see CHANGELOG for the restructure.)

- **Subcommand surface** landed for the v0.3 names: `mos status`,
  `mos list`, `mos watch` are recognized; `mos capacity`,
  `mos identity`, `mos tray`, `mos speed`, `mos features` reserved
  with a typed-name diagnostic pointing at v0.4. The implicit-status
  form (`mos --bsd disk4`) is permanent and unchanged.
- **Drive-swap race window** in `mos_open_by_index` closed by
  `IORegistryEntryIDMatching` against the registry-entry ID captured
  during enumeration. Specifically: this closes the registry-mutation
  TOCTOU window between the enumerate and reopen passes WITHIN A
  SINGLE `mos_open_by_index` call. It does NOT claim that the same
  watch session survives a physical detach+reattach cycle —
  `kIOMessageServiceIsTerminated` correctly terminates the watch on
  detach (emitting `device_removed`), and the user starts a new watch
  session against the new IORegistryEntry. Registry-entry IDs are
  reissued per IORegistryEntry instance per XNU's
  `IORegistryEntry::init()`; "valid until reboot" is an upper bound,
  not a survival-across-reattach guarantee. The earlier wording of
  this entry in the v0.3.1-dev iterations conflated the two scopes;
  the audit trail for that refutation lives in the CHANGELOG's
  v0.3.1-dev review-pass entries (the dated research note it once
  pointed at was never committed).
- **Watch session identity preservation**: every `mos_watch_t`
  captures its target's IORegistry entry ID at construction (from
  the validated `io_service_t`) and uses
  `mos_internal_open_by_registry_id` for every probe reopen — NOT
  `mos_open_by_bsd_name`. The previous v0.3.1-dev refactor had
  closed the constructor race (notification identity preserved via
  retained `w->svc`) but left the per-probe path going back to
  IOBSDNameMatching, creating a two-identity bug: the watch could
  silently start probing a different physical drive that inherited
  the same BSD name while still holding the original notification
  service. Caught in external review; fixed by making
  registry-entry ID the single identity authority for the watch
  session lifetime. `w->bsd_name` is now used only as a display
  label and as the Disk Arbitration filter, not as the probe
  target. On `IORegistryEntryIDMatching` failure (entry terminated),
  the probe surfaces `MOS_ERR_NO_DEVICE` → watch core flips to
  terminal-removal → clean `device_removed` event.
- **Watch downstream-pipe close**: `mos --watch | head` and similar
  consumer-close patterns now exit cleanly with EX_OK instead of
  exit 141 or continuing to probe a closed pipe. SIGPIPE is ignored
  at CLI entry; `emit_watch_ndjson` and `emit_watch_plain` return a
  three-valued `watch_emit_status` (ok | pipe_closed | write_error)
  derived from `fflush()` return + `ferror(stdout)` + `errno`; the
  watch loop interprets `pipe_closed` as clean termination
  (`mos_watch_close` + return EX_OK) and `write_error` as a real
  output failure (`EX_IOERR`, 74). A v0.3.1-dev iteration of the
  SIGPIPE comment in `tools/mos.c::main` claimed this defense was in
  place when it wasn't — the comment described EPIPE surfacing
  through `fflush` return codes but the code never inspected them.
  Caught in external review; the comment and the implementation
  are now aligned.
- **0x7F escape in JSON output**: `mos_json_escape` now escapes 0x7F
  (DEL) alongside 0x00–0x1F per defense-in-depth against terminal-
  aware downstream consumers. Tested by the hostile-input render
  fixtures in `tests/test_render.c`.
- **`mos_probe` safe-print consolidation**: the local `print_safe`
  helper now delegates to `mos_safe_ascii` from the public API so
  CLI and probe share one tested rendering rule.
- **Strict adapter CI compile** (`strict-adapter-build` job):
  `src/mos_scsi.c`, `src/mos_watch.c`, `src/mos_state.c`, and the
  CLI/probe TUs of the day (`tools/mos.c` and the standalone probes,
  since restructured into `cli/`: CLI 2026-06-10, probes 2026-06-11)
  all compiled with `-Werror=implicit-function-declaration
  -Werror=incompatible-function-pointer-types -Werror=int-conversion
  -Werror=return-type` against the macOS 12.0 deployment target.

## Deferred to v0.4 / future

- **Hardware validation pass** against BH16NS55, WH16NS60, A1379
  SuperDrive. Same matrix that was the v2 gate; never executed
  against the v0.3.1-dev binary because the development environment
  has no IOKit.
- **Typed APIs**: `mos_get_disc_capacity`, `mos_get_disc_identity`,
  `mos_tray_control`, `mos_speed_control`, `mos_enumerate_features`.
  Each lands as a subcommand wired into the existing reserved-name
  diagnostic at `tools/mos.c` (ARCHITECTURE.md §10) with its own
  schema name (`mos.capacity.v1`, `mos.identity.v1`, etc.).
- **Notification-source extensions**: dispatch on
  `kIOMessageServiceWasClosed`, `IsAttemptingOpen`, `BusyStateChange`
  remains deferred pending empirical characterization by
  `mos probe` — see ROADMAP.md v0.3.1-dev for
  the self-trigger analysis that drove the deferral.
- **Explicit SCSI command timeouts** via `mos_raw_cdb`'s
  `timeout_ms` for convenience methods. Mitigates hostile-drive
  DoS-against-self.

## Cross-cutting verification

- Pure tests: all passing (live count in the CI log) under `-Wall -Wextra -Wpedantic
  -Werror=implicit-function-declaration` against C11. Seven new tests
  in v0.3.1-dev pin the BSD-name self-or-partition matcher, including
  two regression tests for the malformed-suffix bug that the
  first-pass matcher (v0.3.1-dev intermediate) accepted.
- `_Static_assert` on `sizeof(mos_error) == sizeof(int32_t)`
  still in place; v0.3.1 added no `mos_error` variants with values
  outside the existing range.
- IOKit-side TUs: now compiled by the `strict-adapter-build` CI job
  on `macos-latest` with `-mmacosx-version-min=12.0` and four
  `-Werror=` promotions targeting SDK-signature drift classes. Prior
  STATUS.md text said "syntax-checked … full compile deferred"; that
  deferral is no longer accurate.
- `tests/cli/test_cli.sh`: rewritten for v0.3 surface (schema
  family, sysexits exit codes, unknown-as-EX_OK semantics). Now
  exercised by the `build-and-test` CI job against a real `mos`
  binary on `macos-latest` — the v0.3-dev "NOT executed against a
  real binary" note no longer applies.
- Watch loop runtime: NOT executed end-to-end against hardware. The
  pure watch state machine has 24 unit tests with fake ops
  (including regression fixtures for the v0.3-dev clock-domain and
  removal-path fixes); the IOKit-side adapter (`src/mos_watch.c`)
  is exercised by the strict-adapter-build job for type-correctness
  but not for runtime behavior. Hardware validation (gate, below)
  is the remaining outstanding step.

## Hardware validation gate

The v0.3.1-dev contract is stable for the gate. Validation should:

1. Full CMake build + `ctest` on an Apple-toolchain host.
2. Manual smoke per matrix drive in each state:
   - `mos --json`: confirm `"schema": "mos.state.v1"`,
     `"bsd"` (full dev node since the 2026-06-10 CLI redesign; was `"bsd_name"`), `"current_profile"`,
     `"current_profile_name"` populated.
   - `mos --json --index 99` (no-drive): confirm
     `mos.error.v1` envelope with nested
     `error.{code, message, context, recoverable}` and
     `exit_code: 66`.
   - Bare `mos --index 99`: confirm empty stdout, stderr
     diagnostic, exit 66.
   - `mos --json --watch --bsd diskN`: confirm initial
     `snapshot` event, then `state_changed` on tray / media
     transitions, then `device_removed` on detach.
3. If a query-failure can be deliberately induced, confirm
   partial-failure `mos.error.v1` envelope surfaces `bsd_name`
   correctly.
4. `mos --json=v2` (or any `=value`): confirm `EX_USAGE` (64)
   with diagnostic naming `mos.state.v1`.
5. Capture per-drive fixtures via
   `mos status --json > fixtures/<drive>/<state>.json` (plus
   `mos probe --dump` for the DR dictionaries).

Once 1-4 pass, v0.3 is shippable. Step 5 informs v0.4 typed-API
design.

### Falsification runs (post-2026-06-10 scope reduction)

The blanket "adapter unvalidated" framing has shrunk: the adapter seam
is now verified against the kernel's own source — GetTrayState masking,
the §5.5 nub invariant, TUR exclusivity, IOReturn pins, the GESN CDB
(ARCHITECTURE §9.7, §5.5, §11). What hardware still owes us is
**falsification + fixture acquisition**, never design input
(AGENTS.md doctrine):

0. **Seam contract UNGUARDED clauses** (doc/seam-contract.md appendix —
   the three obligations the pure suite structurally cannot see, all
   adapter-shaped):
   - **O-1**: confirm the adapter zero-initializes `mos_state_result`
     before filling (read `mos_scsi.c:193` on the build host; assert a
     fresh query on an empty drive reports `current_profile: 0` not
     garbage).
   - **O-3**: identity-string lifetime — hold a result past a
     `mos_close`, read `vendor/product/revision` under ASan (the rehome
     helper makes this pass; the clause needs one on-Mac ASan run to be
     source-verified rather than fixture-asserted).
   - **V-1**: eject and re-insert while running one-shot queries;
     confirm `bsd_unit` flips to -1 and back driven by NODE absence
     (e.g. state `loading` with unit still -1 mid-spin-up is correct),
     not by the state enum — the fake derives unit FROM state, so this
     shape is untestable headlessly.

1. **Insert-under-watch** (pass/fail): `mos --watch` at default rates
   during a real insert; compare mount latency to a no-watch baseline,
   watch IORegistry for repeated nub teardown. Retires the §5.5
   backward-flip and UA slivers. While there: two rapid `mos --watch`
   opens on the same drive must show distinct `stream_open_ms` values
   (pins the per-process monotonicization end-to-end).
2. **Fixture acquisitions** for paths currently spec-only:
   descriptor-format sense (0x72/0x73) from a real device; a bridge
   that omits `media_id` (exercises the profile-fallback swap path);
   an LG stale-profile sequence (pins the §9 suppression with real
   bytes); opportunistically, a damaged disc to timestamp the kernel
   auto-eject corollary.
3. **Index-order comparison**: `drutil list -xml` vs `mos list`,
   repeated across hotplug (doc/research/2026-06-10-drutil-contract.md
   tiering; retires the Inferred tier).

4. **Multi-drive `mos watch` guard** (2026-06-11 fix, not headless-
   testable): with two drives attached and no selector, `mos watch`
   must print the mini-list to stderr and exit 64 — same contract as
   `mos status`. One drive: implied, as before.

5. **`bsd_unit` fallback branch on real bridges** (2026-06-11 fix):
   does any owned bridge actually expose its BSD name on a non-IOMedia
   node (taking `mos_internal_bsd_unit`'s fallback), and does that
   node's registry ID survive a disc swap? One `mos probe --dump`
   before/after a swap answers both. The branch now mints media_id 0
   (don't-infer-a-swap sentinel); a captured fixture either pins that
   or retires the question.

6. **DR doorbell setup failure in practice**: `mos_watch_open_all` now
   fails the open if `DRNotificationCenterCreate` / run-loop source
   creation fails. If a real Mac ever shows this failing, that
   observation funds the rescan-fallback decision parked in ROADMAP
   (2026-06-11 intake remainders).

A surprise observed on hardware lands as a committed `.bin` fixture and
the pure layer is built to the fixture — defenses generic, never
device-special-cased.

## Shipped 2026-06-10 (post-audit session)

- PVD/pread arc closed: privilege-footprint doctrine (AGENTS layer 3),
  no sector I/O, no sector parsers; stage 2 re-scoped to SCSI surfaces.
- CLI redesign implemented: cli/ one-file-per-command over
  cli/common, layout engine cli/human with byte-exact golden tests,
  bsd = full dev node everywhere, state.v1 += registry_id/index,
  two-phase list with per-entry containment, positional subject,
  watch NDJSON end to end, bare word and plain watch removed.
- Pre-tag mutation ritual vendored (scripts/mutation-pass.py); two
  survivors found and pinned (eject-set gap — the exhaustive checker drives the live core but runs outside the suite and harness;
  registry copy-through). 23/23 killed.
- Naming standard recorded (AGENTS): bsd_unit / bsd_name / dev node,
  Apple-canonical; banned synonyms swept.
