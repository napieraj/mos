# `status` → `state`, and the digit-gated default subject

Status: PLANNED 2026-06-14. Pre-first-tag, no external consumers — the
verb surface is mutable in place (same window the JSON-schema ADR's
mutable-in-place clause relies on). This note is the plan + the ADR text
to append to AGENTS.md when the code lands.

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

**D3 — `state` is the canonical cheap dashboard: enrich from the kernel
cache, never from new commands.** This RESOLVES the previously-deferred
cost-boundedness fork. `state` becomes the fused quick-look that surfaces
the popular facts other verbs charge MMC commands for — but ONLY the ones
the kernel already caches, read off the DR Info/Status dicts mos already
copies. The cost line, crisp and enforceable: **`state` issues no device
command beyond the state core's own (TUR ⊕ at most one GESN); every
enrichment field is a kernel-cached dict read with zero device
round-trip.** No new MMC walk, no new exclusive lock, no disc sector
read. The deep, authoritative forms stay in their command-paying sources
(`metadata`, `capacity`, `drive`, `features`); a cached fact in `state`
and its commanded twin in `metadata` is projection-vs-source, and the
staleness gap — DR's Status dict is "the kernel's GESN-fed snapshot, not
guaranteed current" (2026-06-10-media-info-design.md:282) — is the honest
price of cheapness, stated, with the subcommand as where ground truth is
paid for. This is the TOC call inverted by intent, not contradicted: TOC
stays on the convenience COMMAND because `metadata` is the authoritative
record; `state`'s job is the cheap snapshot, so for `state` the cache is
the correct source.

### Enrichment inventory — cache-available (fold in) vs command-only (stays)

| Fact | Kernel-cached? | Today's source | `state` action |
|---|---|---|---|
| vendor / product / revision | yes (Info) | state core (DR) | already shown |
| media_class | derived from profile | state core | already shown |
| **physical interconnect** (USB/SATA/TB) | yes (Info) | nowhere in mos | **ADD** — popular, currently unsurfaced |
| **is_blank / is_erasable** | yes (Status MediaInfo) | `metadata` READ DISC INFO | **ADD** (snapshot) |
| **session_count / track_count** | yes (Status MediaInfo) | `metadata` READ DISC INFO | **ADD** (snapshot) |
| **blocks_used / free / overwritable** | yes (Status MediaInfo) | `capacity` (READ CAPACITY / RTI) | **ADD** (snapshot) |
| tray_open | yes (Status, stale-able) | state core GESN | already encoded in the `state` enum — no separate field |
| full disc_status (complete/appendable) | partial | `metadata` READ DISC INFO | NO — needs the command |
| toc / disc_structure / cdtext / track_info | no | `metadata` commands | NO |
| AACS / speeds / mechanical / error_recovery / serial | no | `drive` commands | NO |
| full MMC feature list | no | `features` GET CONFIG | NO |

Exact JSON shape of the added fields (a `media` sub-object? top-level
scalars? a `cache_snapshot` envelope to mark them snapshot-not-commanded)
is a v1-schema decision to settle when this lands — pre-tag mutable, so it
costs schema + examples + negatives + emitter + README in one commit. The
key design constraint: a consumer must be able to tell a snapshot field
from a commanded one, so the staleness contract is legible (candidate:
group them so their provenance is structural, not per-field prose).

Verify items (Mac, falsifier-class per the hardware-role ADR): which
Status MediaInfo keys populate in which media states (blank vs pressed vs
unmounted); whether the single-open `state` path holds the Status dict or
needs one extra `DRDeviceCopyStatus` (still zero device I/O); the
staleness envelope per field (how stale `blocks_free` can read
mid-format). Each surprise lands as a fixture + dated note, never a
per-drive special-case.

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

- **`state` issues no new device command.** D3 enriches from the kernel
  cache only — no MMC walk, no lock, no sector read. The state core's
  command budget (TUR ⊕ at most one GESN) is unchanged; the deep records
  (`metadata`, `capacity`, `drive`, `features`) remain the command-paying
  authoritative sources.
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
slice; it gains the popular facts other verbs charge MMC commands for —
but only those the kernel already caches (blank/erasable, session/track
counts, capacity blocks, physical interconnect — read off the DR
Info/Status dicts mos already copies, src/mos_dr.c:163-175). Hard cost
boundary: `state` issues no device command beyond the state core's own;
all enrichment is zero-device-I/O cache reads. `metadata`/`capacity`/
`drive`/`features` stay the command-paying authoritative sources; the
cached-vs-commanded duplication is projection-vs-source with the DR
snapshot's staleness stated, not hidden.
