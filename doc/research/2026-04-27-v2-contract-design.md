# mos v2 Contract Design Research

> **SUPERSEDED — historical research artifact.** This note captures
> the design reasoning that produced the v2 contract lock in late
> April 2026. The v2 contract was applied (see git history of that
> period) and then itself superseded by the v0.3 schema family in
> May 2026. The current contract is per-document schema versioning
> (`mos.state.v1`, `mos.error.v1`, `mos.list.v1`, `mos.event.v1`),
> not per-CLI-invocation versioning (`--json=v2`). See
> `ARCHITECTURE.md §10` for the current CLI shape ADR and
> `AGENTS.md` for the rewritten JSON evolution policy. This file
> is preserved unmodified for its historical value as the research
> trail behind the v2 lock — do not rewrite it; cite it as
> as-of-2026-04-27 reasoning if relevant.

**Date:** 2026-04-27
**Status:** Pre-hardware-validation; informs v0.2.0 lock and v0.3 deferred items
**Scope:** Library API surface (`mos_state_result`, opaque handles, error enum, string lifetime) and CLI wire format (`mos --json` envelope shape).

This note consolidates external prior-art research with project-internal review.
The research surveyed mature pure-C library conventions (libcurl, libgit2,
sqlite, libsystemd, libudev, libarchive, OpenSSL), JSON envelope specs
(JSON-RPC 2.0, JSON:API, Google JSON style guide, JSend, RFC 9457), and
CLI structured-output patterns (kubectl, lsblk, ip, journalctl, brew, cargo,
git --porcelain, AWS CLI). The review checked the research against the
actual `include/mos.h`, `tools/mos.c`, `AGENTS.md`, and `ROADMAP.md` to
catch recommendations that were already implemented or that conflicted
with project doctrine.

The consolidated lock list is at §1. The analysis trail is at §2 onward.

## 1. v2 lock list

Items 1–6 are **doctrine-safe**: desk-research-settleable, no behavioral
change pending hardware validation. Item 7 captures structural questions
deferred to v0.3, to be informed by the hardware-validation fixture
corpus.

A separate question — whether even the doctrine-safe items should wait
for hardware validation — is named at §1.1.

### 1.1 Doctrine-safe items (apply pre-hardware)

1. **Drop `state` from the failure envelope.** Currently the failure
   envelope emits `"state": "unknown"`, conflating "we queried and the
   drive returned a state we couldn't classify" with "we never queried
   at all." JSON-RPC 2.0, JSON:API 1.1, and Google's JSON style guide
   all enforce structural disjointness between success-shape and
   failure-shape envelopes for exactly this reason. The `"unqueried"`
   sentinel proposed in earlier discussion is strictly worse than
   dropping the field — it adds a value with the meaning "ignore me."
   Dropping is the right resolution.

2. **Plain-text mode disjointness.** Bare `mos` currently emits
   `unknown\n` for both unclassifiable-but-queried results and
   hard-failure paths (open failed, query failed). Same conflation as
   #1, propagated to the plain-text consumer. Resolution: emit
   `unknown\n` only for genuinely unclassifiable results;
   route hard failures to stderr-only via the existing `--verbose`
   diagnostic message plus exit 1, with no stdout output. Shell
   consumers who want to distinguish use exit code; consumers who
   only care about state get a clean answer.

3. **Pin error-enum storage type via CF_ENUM idiom.** A bare
   `typedef enum { ... } mos_error;` lets the C compiler choose the
   underlying integer type. With `-fshort-enums`, Swift's importer
   behavior, or bindgen's `rustified_enum` flavor (which is UB on
   out-of-set values), this matters for FFI cleanliness. The fix:
   `typedef enum mos_error : int32_t { MOS_OK = 0, ... } mos_error;`
   This is the macOS-native CF_ENUM idiom and gives Swift importers
   a proper enum. Five-line change, zero behavior cost, real FFI
   payoff.

4. **Include `bsd_name` in failure envelope when `mos_open_by_*` succeeded
   but query failed.** Surfaces the partially-open path so consumers can
   distinguish "no drive at index N" from "drive at index N exists but
   its query failed." Doctrine-safe; no library-to-drive behavior change,
   only which already-computed internal state the failure envelope
   surfaces. (Note: the field name is `bsd_name`, not `bsd` — see lock
   list item 7 below for the project-wide rename that happened alongside
   the v2 contract work and produced this name.)

5. **Schema-evolution policy in writing.** Pin in `ROADMAP.md` or
   `docs/abi.md`: additive-only within a major version (consumers
   ignore unknown fields per the kubectl / lsblk / rustc JSON
   convention); breaking changes gated by an explicit `--json=v3`
   flag (the git `--porcelain=v2` / brew `--json=v2` / cargo
   `--format-version` pattern). Both patterns earn their keep across
   the surveyed corpus; combining them gives mos a forward-compatible
   default with a clean breaking-change escape hatch.

6. **Document stdout-for-failure-JSON as deliberate JSON-RPC-style
   symmetry.** Currently the failure envelope goes to stdout, which
   is the minority pattern in CLI-land (AWS, kubectl, lsblk emit
   structured errors to stderr). The stdout choice has a real benefit
   the original research undersold: `mos --json | jq '.error // .state'`
   works as one pipeline regardless of outcome, because both the
   success and failure envelopes arrive on the same stream. Exit code
   1 still fires on failure for consumers that prefer to gate on `$?`
   and treat stdout as success-only — they get the conventional
   ergonomic without losing the structural property. Document the
   choice in `tools/mos.c`'s contract block, the README contract
   section, and the `--help` output so the divergence from
   AWS/kubectl/lsblk is visible-and-deliberate rather than
   accidental.

7. **Project-wide rename `bsd` → `bsd_name`.** Aligns with Apple's
   `system_profiler -xml` machine-readable output, which is the
   nearest peer (also a macOS device-info-as-JSON producer). Touches
   the public C accessor (`mos_device_info_bsd` →
   `mos_device_info_bsd_name`), JSON field (`"bsd"` → `"bsd_name"`),
   internal helpers (`mos_internal_normalize_bsd` →
   `mos_internal_normalize_bsd_name`, `mos_internal_bsd_is_whole_shape`
   → `mos_internal_bsd_name_is_whole_shape`), and the
   `mos_open_by_bsd` → `mos_open_by_bsd_name` open function. CLI flag
   `--bsd` retained for shell ergonomics — the rename is about output
   field naming and C surface alignment with `system_profiler`, not
   command-line UI. The local-variable `bsd` inside
   `mos_internal_copy_bsd()` is intentionally not renamed: different
   scope, different semantic role (CFTypeRef holding the IOKit
   property reference, not the resolved string). Doctrine-safe;
   public-API rename without behavior change. The simultaneous
   addition of `mos_handle_bsd_name(h)` accessor (used by item 4's
   partial-failure surfacing) follows the same naming convention.

### 1.2 Deferred to v0.3 hardware-validation pass

The following items are real questions but their right answers
depend on observations the project does not yet have. The
BH16NS55/WH16NS60/A1379 fixture corpus is the input that resolves
each one; deferring them to v0.3 is the doctrine-correct move.

1. **`is_rw` field disposition** (keep, drop, or rename to
   `has_writer_interface`). Per `ARCHITECTURE.md` §9.1, read-only
   drives don't get a `SCSITaskUserClient` attached on macOS, which
   means the code path that populates `mos_state_result` cannot run
   for them — making `is_rw` structurally always-true on the query
   path. If hardware validation confirms this invariant across the
   matrix, the field carries no information and should be dropped.
   The A1379 SuperDrive is the most likely candidate to violate the
   invariant; observation against the matrix is the tipping point.

2. **`current_profile` representation** (hex string `"0x0010"` vs
   decimal integer `16`). Hex preserves alignment with the entire
   MMC stack (Apple headers, T10 spec, cdrom_id, libcdio, drive
   firmware logs). Decimal aligns with JSON-native number semantics
   and JSON-RPC/gRPC numeric-code conventions. v2 keeps hex; the
   right resolution depends on the shape of the test fixture
   corpus. If fixtures are dominated by T10-spec cross-reference
   work, hex preserves direct readability. If consumer code
   pattern-matches against numeric constants, decimal is cleaner.

3. **Explicit `"status": "ok" | "error"` discriminator** vs.
   presence-of-error-key. Current presence-of-key has solid prior
   art (JSON-RPC 2.0, JSON:API, Google JSON style guide). Adding
   `status` would help typed-language consumers (Rust serde tagged
   unions, pydantic discriminated unions, zod) at the cost of a
   redundant field in the success case. The tipping observation is
   whether hardware produces ambiguous middle-ground states (drive
   returned partial data, query succeeded but populated fields are
   unreliable) — if yes, `status` can carry a third `"partial"`
   value that presence-of-key cannot. If no middle ground emerges
   from the validation pass, presence-of-key is sufficient and the
   redundancy of `status` is not earned.

4. **NDJSON envelope shape for v0.3 `--watch` streaming.** v2
   single-record envelope is by design; v0.3 streaming is a
   separate envelope decision that may not inherit cleanly from v2.
   v2 reserves field names (`event_type`, `timestamp`, `sequence`,
   `event_id`) so v0.3 can claim them as per-event metadata
   without conflict. Two plausible v0.3 shapes: per-event
   v2-record-with-added-metadata at the top level (preserves the
   schema-evolution forward-compat rule — consumers ignoring
   unknown fields will parse a v0.3 watch line correctly), or a
   wrapper envelope (`{"event": "state_changed", "payload": {v2
   record}}`) that breaks the additive convention cleanly. The
   first is preferred per the analysis but the actual choice is a
   v0.3 design problem driven by what events the underlying
   notification source (DiskArbitration callbacks, IOKit interest
   notifications) emits.

## 2. Closing notes

The lock list above represents the doctrine-safe ground-truth
intersection between (a) external desk research on prior-art C
library and CLI conventions, and (b) project-internal review that
caught two corrections to the original research output:

- The original research recommended adding `mos_strerror` — but
  the function already exists as `mos_error_description` in
  `src/mos_strings.c` with the static-lifetime guarantee. The
  recommendation was an artifact of working from the README rather
  than the header.
- The original research recommended documenting threading and
  string-lifetime contracts in the header — but both are already
  present in `mos.h`'s file-level comment and the
  `mos_state_result` typedef-comment. Same artifact-of-source
  shape: the reviewer worked from the public-facing docs, not the
  header itself.

These corrections are absorbed into the lock list above by their
absence — the items don't appear because they don't need to be done.
The remaining items earn their place through evidence-anchored
analysis rather than pattern-match against generic library
conventions.

The four v0.3-deferred items are the genuinely consequential
design choices still open. None of them is settleable from desk
research alone; all four require observation against real hardware
behavior. Per `AGENTS.md`: "Don't change behavior based on a
review alone... review points become comment updates or v0.next
flags until hardware validation is in." The deferral is the rule,
not an exception to it.

When the BH16NS55/WH16NS60/A1379 fixtures are recorded, return to
items 1.2.1 through 1.2.3 with the data in hand. The decision
criteria are already named here; only the inputs are missing.
Item 1.2.4 (NDJSON envelope) is independent of the validation
pass and unblocks once v0.3 design starts.