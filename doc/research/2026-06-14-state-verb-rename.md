# `status` → `state`, and the digit-gated default subject

Status: **D1/D2 IMPLEMENTED 2026-06-14** (verb rename `status`→`state` +
digit-gated default subject, across cli/, CMakeLists, .github/workflows/
ci.yml, the emit-fixtures harness, tests/cli/test_cli.sh, README +
ARCHITECTURE + CONTRIBUTING + INTEGRATION_HARNESS + schemas docs, and the
AGENTS.md ADR). **D3 (state enrichment) PLANNED.** Pre-first-tag, no
external consumers — the verb surface is mutable in place (same window the
JSON-schema ADR's mutable-in-place clause relies on).

## The gap

Two unrelated-looking ergonomic facts, one root cause.

1. **`mos status` emits `mos.state.v1`.** The verb is the lone token in
   the whole vocabulary that says *status* — everything else says
   *state*: the schema (`mos.state.v1`), `mos_query_state`,
   `mos_state_description`, the `mos_state` enum, the human `State:` row,
   and the project name mac-optical-**state**. The cli-design doctrine is
   one-vocabulary / one-grep (it renamed the list header `#`→`Index`
   purely to match the flag, and keeps enum strings verbatim rather than
   prettify them — 2026-06-10-cli-design.md). By that rule `status` is
   the outlier and the verb↔schema mismatch is a real wart.

2. **`mos 2` does not select drive 2.** Today (cli/main.c:203) any bare
   first word is a subcommand *candidate*; `"2"` matches no verb and
   falls to the `else` at :242 → `unknown subcommand: 2`. The positional
   subject only works *after* a verb (`mos status 2`). So the most
   obvious thing a user types for "state of drive 2" — `mos 2` — errors.

Both dissolve if the default verb is `state` and a bare selector routes
straight to it.

3. **`state` under-sells what is free.** `mos list` is N drives × a
   *slice* of each drive's state; `state` of one drive already shows a
   SUPERSET of that slice (Index, State, Volume, BSD, Vendor, Product,
   Revision — plus Sense, Profile, Registry). But mos copies two
   kernel-cached DR dicts per device (`DRDeviceCopyInfo` +
   `DRDeviceCopyStatus`, src/mos_dr.c:163-175) and reads only FOUR keys
   out of them (identity ×3 + media BSD name). The rest of those dicts —
   blank/erasable, session/track counts, capacity blocks, physical
   interconnect — is sitting there at zero device I/O, and `state` is the
   verb that should fuse it.

## Decision

**D1 — rename the verb `status` → `state`, clean break (no alias).**
`mos state [drive]` becomes the default verb; `status` is removed, not
aliased. Matches house style for surface changes (killed `--brief`,
removed watch plain-mode — one string, one grep; 2026-06-10-cli-design.md).
After this, `mos state` → `mos.state.v1`: verb and schema name finally
agree. The library's `*_status` identifiers (`mos_disc_status`,
`mos_scsi_status`, the tray outcome strings) are a DIFFERENT axis —
disc/SCSI status is not the state verb — and are explicitly out of
scope. Do not let the grep noise pull them into the rename.

**D2 — a bare selector is the default subject: `mos <selector>` ⇒
`mos state <selector>`.** `mos 2`, `mos 4295032831`, `mos disk4`,
`mos /dev/disk4` all report that drive's state with no verb word. Bare
`mos` (zero args) is UNCHANGED: it still hits the cli/main.c:188
early-return (usage + hint, EX_USAGE, no hardware touched — the
2026-06-12 "an intent-free invocation must not touch hardware" rule).
The distinction is intent: a selector IS the intent, bare `mos` is not.

**D3 — `state` is the canonical cheap dashboard; enrich it with the
cheapest mechanism per fact.** This REFINES (does not yet close) the
cost-boundedness fork. `state` becomes the fused quick-look that surfaces
the popular slices of `metadata` / `capacity` / `drive`. Two corrections
from 2026-06-13-disc-tools-state-survey.md (§4, §6) and
2026-06-10-media-info-design.md:374, against an earlier draft of this
section that over-simplified the cost model:

1. **There are TWO kernel caches on macOS, not one — I anchored only on
   the first.** (a) The DiscRecording `DRDeviceCopyInfo`/`CopyStatus`
   dicts mos already copies (src/mos_dr.c:163-175): identity,
   interconnect, and the media-info sub-dict (blank/erasable, session/
   track counts, blocks used/free/overwritable). (b) The IOKit
   *media-node* registry: **`kIOCDMediaTOCKey`** on the **`IOCDMedia`**
   node — a cached **full-TOC** blob (POINT A0/A1/A2, ADR/control, MSF),
   zero commands, no lock, read by libcdio's `lib/driver/osx.c`. That is
   the CD-TOC-from-cache path.

2. **The cached CD TOC is RICHER than the convenience command, but CD-only
   and a fallback.** Full-TOC/MSF form, not the format-0 LBA shape
   `mos_internal_toc_parse` consumes (needs a separate CDTOC parser +
   MSF→LBA), stale-able (cached at media-detection, not per query), and
   **no DVD/BD equivalent** (only `kIODVDMediaTypeKey`, a type string).
   Survey §6 verdict: CD-only zero-command corroboration/fallback, not the
   primary path — "kernel-cached disc state on macOS is a CD-era artifact,
   not a general mechanism." So it is one cheap CD-only option, not a
   general free TOC source.

Consequence for the cost line: the genuinely zero-command set is the
DR-dict fields + the CD-only cached TOC. The most *popular* enrichments
the survey ranks — capacity/NWA, speeds, mechanical/lock — are **lock-free
convenience READS** (`ReadTrackInformation`, `GetPerformance`,
`ModeSense10`; survey §4 confirmed these are convenience methods, no lock,
no privilege), one device round-trip each, NOT cache reads. So "fuse the
specialities cheaply" forces a genuine choice about how cheap `state` must
stay (the fork below).

### Enrichment menu, by true cost (cheapest first)

| Tier | Fact | Mechanism | Cost |
|---|---|---|---|
| 0 | vendor/product/revision, **interconnect**, **is_blank/erasable**, **session/track count**, **blocks used/free/overwritable** | DR Info/Status dict (already copied) | zero command |
| 0 (CD) | full-TOC | `kIOCDMediaTOCKey` / `IOCDMedia` | zero command, CD-only, needs a CDTOC/MSF parser |
| 1 | capacity / NWA | `ReadTrackInformation` (CM) | 1 read, no lock |
| 1 | read/write speeds | `GetPerformance` (CM) | 1 read, no lock |
| 1 | mechanical type / lock-supported / **live locked bit** / buffer | `ModeSense10` 0x2A (CM) | 1 read, no lock |
| 1 | DVD physical / copyright / mfr structure | `ReadDVDStructure` / `ReadDiscStructure` (CM) | 1 read, no lock |
| — | full disc_status enum, cdtext, AACS, serial, full feature list | `metadata`/`drive`/`features` commands | stays in its verb |

Tier 0 is unambiguously `state`'s (free, snapshot-honest). Tier 1 is the
open question. Everything below stays the command-paying source's.
`media_class` is already shown (derived from profile); `tray_open` is
already encoded in the `state` enum, not a separate field.

### Cost line — RESOLVED: `state` already commands and locks, so enrich eagerly (one-shot)

The a/b/c fork drafted here rested on a false premise — that `state` has a
"zero-command, one-open" hot path worth protecting. It does not.
`mos_state_core.c` ALWAYS issues a convenience TUR (`:72`, the single-shot
presence probe) and, on the not-ready branch, takes exclusive access and
fires the **raw GESN** tray probe (`:13-14`, `:136` — the one raw CDB,
under the §5.5 lock). So `state` is already a command-issuing,
sometimes-locking verb; the Tier-1 lock-free convenience reads are
INCREMENTAL within a modality it already uses, and strictly CHEAPER than
the GESN branch it already runs (no lock).

**Decision: one-shot `mos state` enriches EAGERLY.** It issues the
applicable Tier-1 reads (capacity/NWA, speeds, mechanical), profile-class-
gated exactly as `metadata.c` gates today (no read fired against media
that must reject it), each fail-soft to null (the standing enrichment
doctrine — a failed read is a null field, never an error). **No `--full`
flag:** the categorical reason to hide enrichment behind a flag is gone,
and a latency escape hatch is not worth a second output mode pre-tag.

The one real residual is LATENCY (each read is a device round-trip), and
it is handled by the VERB SPLIT, not a flag: **`watch` / `mos.event.v1`
stays Tier-0** (state-core/cache only). The event stream is the actual hot
loop — polled every `MOS_WATCH_STABLE_MS` — and must not multiply
round-trips or bloat the event schema. Enrichment is a property of the
on-demand `state` SNAPSHOT, not the stream: rich one-shot `state`, lean
`watch`.

Independent of the tier decision: the JSON must let a consumer tell a
snapshot (cached) field from a commanded one (candidate: group by
provenance so it is structural, not per-field prose), and
`metadata`/`capacity`/`drive` stay the authoritative sources — the
cached-vs-commanded duplication is projection-vs-source with DR's
staleness ("the kernel's GESN-fed snapshot, not guaranteed current",
media-info-design.md:282) stated, not hidden.

**CD cached full-TOC (`kIOCDMediaTOCKey`): BANKED, not built** (owner's
call). Survey §6 rates it CD-only, stale-able, fallback-grade, and it
needs a separate CDTOC + MSF→LBA parser; the universal convenience RTOC
stays the TOC route. It is the kernel-authored fallback to reach for only
if the convenience RTOC disappoints on real hardware — a hardware-
contingent option, not a stage deliverable.

Verify items (Mac, falsifier-class per the hardware-role ADR): which DR
Status MediaInfo keys populate in which media states (blank vs pressed vs
unmounted); whether the single-open `state` path holds the Status dict or
needs one extra `DRDeviceCopyStatus` (still zero device I/O); the
per-field staleness envelope (how stale `blocks_free` reads mid-format);
and for Tier 1, the SDK selector signatures (survey §8 — convenience-method
*existence* is header-confirmed, signatures to verify before wiring). Each
surprise lands as a fixture + dated note, never a per-drive special-case.

## The dispatch rule: digit-gated, no verb-table lookup

The clean separator (owner's, 2026-06-14): **does `argv[1]` contain a
decimal digit anywhere?**

- **Contains a digit → positional subject.** Skip subcommand dispatch
  entirely; let the existing syntactic positional parser (cli/main.c:314)
  classify it (all-digits split on the registry floor → index|registry;
  else → bsd) under the default `state` verb.
- **No digit → subcommand candidate.** Existing dispatch, unchanged:
  match the verb table; unrecognized → `unknown subcommand`.

This rests on two invariants, both verified, not assumed:

1. **Every valid selector contains ≥1 digit.** index and registry_id are
   all-digit by construction; a whole-disk BSD form requires at least one
   digit — `mos_internal_bsd_name_is_whole_shape` (src/mos_pure.c:21-22,
   comment "Require at least one digit") rejects `disk`/`rdisk` with no
   trailing unit. So no selector is digit-free.
2. **Every subcommand name is digit-free.** state, list, watch, metadata,
   drive, features, tray, capacity, probe — all alphabetic today. This
   becomes a stated CONSTRAINT on future verb names: a verb may not
   contain a digit (a cheap, natural rule — command words don't carry
   numerals).

Why digit-gating beats the verb-table "selector-shaped" rule I first
sketched: it needs no array scan, and it RESOLVES rather than introduces
the typo friction. A mistyped verb is digit-free, so `mos statuss` /
`mos metadta` still route to subcommand dispatch and keep the precise
`unknown subcommand: statuss` diagnostic. The only token that could be
"a verb I meant but it has a digit" cannot exist, because verbs are
digit-free by invariant 2 — so there is no false-route on the verb side.
The residual: a digit-bearing garbage selector (`d1sk`) routes positional
and fails as a drive-open error rather than `unknown subcommand` — the
correct classification anyway (it is selector-shaped, not verb-shaped).

Flag-first invocations are untouched: `mos -i 2`, `mos --bsd disk4`
start with `-`, so the cli/main.c:203 block is already skipped and getopt
handles them under the default verb.

## What does NOT change

- **`state` reads no sectors and needs no elevation** — but it is NOT
  command-free or lock-free: it already issues a convenience TUR every
  query and takes exclusive access for the raw GESN on the not-ready tray
  fork (`mos_state_core.c`). One-shot `state` enriches eagerly with the
  Tier-1 lock-free reads (profile-gated, fail-soft) — incremental within
  that modality and cheaper than the GESN branch. `watch`/`event.v1` stays
  Tier-0; `metadata`/`capacity`/`drive`/`features` stay the authoritative
  sources.
- **The rename + dispatch (D1/D2) carry NO schema change** — that slice
  is verb-name + dispatch + help text only, `mos.state.v1` byte-identical.
  The D3 enrichment IS a `mos.state.v1` field addition (pre-tag mutable)
  and should land as its own commit AFTER D1/D2, so the rename is not
  entangled with a schema change.
- **Bare `mos` still errors** (no probing on intent-free invocation).
- **The multi-drive default is unchanged**: `mos state` with no selector
  and >1 drive → EX_USAGE mini-list (cli/status.c:231); with exactly one
  drive → sole-drive default. `mos <selector>` bypasses that branch.

## Implementation steps

1. **cli/main.c**
   - Add `static bool contains_digit(const char *s)`.
   - Guard the subcommand block (:203) with `&& !contains_digit(argv[1])`
     so a digit-bearing `argv[1]` falls through unshifted to getopt +
     the positional parser (default verb = `mos_cli_run_state`).
   - Rename the `"status"` dispatch arm (:206) to `"state"`; REMOVE
     `status` (clean break).
   - Usage text (:30): `status [drive]` → `state [drive]`; add one line
     teaching the bare-selector form (`mos 2`, `mos disk4`).
   - Bare-`mos` hint (:193) and the `unknown subcommand` "Recognized:"
     list (:245): `status` → `state`.
2. **cli/status.c → cli/state.c**, `mos_cli_run_query` →
   `mos_cli_run_state` (completes the vocabulary; the default-path
   function should name the verb). Update the prototype in cli/common.h
   and the call at cli/main.c:443. CMake source list updated.
3. **Tests** — tests/cli/test_cli.sh and tests/test_main.c: `mos status`
   → `mos state`; ADD cases for `mos 2` / `mos disk4` / `mos
   4295032831` resolving to state, and assert `mos statuss` still yields
   `unknown subcommand` (the preserved-diagnostic property). Golden human
   tests (tests/test_human.c) need no change (output identical).
4. **Docs** — README CLI section (`mos status` examples → `mos state`,
   document the bare-selector form); append a dated "superseded by
   2026-06-14" pointer to 2026-06-10-cli-design.md; this note.
5. **AGENTS.md** — append the ADR below when the code lands (house
   pattern: design note first, AGENTS ADR at implementation).

## Test plan

Pure/golden render tests unchanged (no output delta). New coverage is
dispatch-level in test_cli.sh: the digit-gate truth table — `mos 2`,
`mos disk4`, `mos /dev/disk4`, `mos 4295032831` → state path; `mos
state`, `mos list`, `mos drive` → verbs; `mos statuss`, `mos drivee` →
`unknown subcommand`; `mos` (bare) → EX_USAGE, no probe. Strict build +
full suite + ASan/UBSan per the per-batch gate.

## ADR (ready to append to AGENTS.md at implementation)

### Verb `state` replaces `status`; bare selector is the default subject (2026-06-14)

`mos status` emitted `mos.state.v1` — a verb↔schema mismatch against a
one-vocabulary CLI where every other token (schema, `mos_query_state`,
the `mos_state` enum, the `State:` row, the project name) says *state*.
Renamed `status` → `state`, clean break, no alias (house style for
surface changes). `mos state` now agrees with `mos.state.v1`.

Coupled change: `mos <selector>` with no verb word resolves to
`mos state <selector>` (`mos 2`, `mos disk4`). Dispatch separator is a
digit-anywhere test on `argv[1]`, not a verb-table lookup: every valid
selector carries ≥1 digit (index/registry all-digit; BSD form requires a
unit digit — mos_pure.c:21-22) and every verb name is digit-free (now a
stated constraint on future verb names). So digit-bearing ⇒ positional
subject, digit-free ⇒ subcommand candidate. This preserves the
`unknown subcommand` diagnostic for mistyped verbs (digit-free) instead
of misreading them as BSD names. Bare `mos` (zero args) is unchanged: it
still errors without touching hardware (the 2026-06-12 no-probe-on-
intent-free-invocation rule) — a selector is intent, bare `mos` is not.

The rename + dispatch carry no schema or library-API change. The
library's `*_status` identifiers (`mos_disc_status`, `mos_scsi_status`,
the tray outcome) are a different axis and are untouched.

Coupled direction (D3, separate commit): `state` becomes the canonical
cheap dashboard. It already shows a superset of `mos list`'s per-drive
slice. Tier 0 (zero command) folds in the DR-cache fields — interconnect,
blank/erasable, session/track counts, capacity blocks. Tier 1 —
capacity/NWA, speeds, mechanical via the lock-free convenience methods
(`ReadTrackInformation`/`GetPerformance`/`ModeSense10`) — lands EAGERLY in
one-shot `state` (profile-gated, fail-soft): `state` already issues TUR +
the raw GESN tray probe, so the lock-free Tier-1 reads are incremental and
cheaper than the GESN branch, and no `--full` flag is needed.
`watch`/`event.v1` stays Tier-0 (the hot loop). The CD cached full-TOC
(`kIOCDMediaTOCKey`) is banked as a fallback, not built. `metadata`/
`capacity`/`drive`/`features` stay the command-paying authoritative
sources; the cached-vs-commanded duplication is projection-vs-source with
the DR snapshot's staleness stated, not hidden.
