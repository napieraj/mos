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

- **Cost-boundedness of `state` is a SEPARATE, still-open decision.**
  This note renames the verb and adds the default subject; it does NOT
  decide whether `state` may issue disc reads to enrich the dashboard
  (the original fork). Recommendation stands: keep `state` cost-bounded
  (one open + state query + one DA volume read, zero disc/capability
  walks) — richer disc detail is `mos metadata`. Decide after the rename.
- **No field, schema, or library-API change.** `mos.state.v1` is
  byte-identical. This is verb-name + dispatch + help text only.
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

No field/schema/library-API change. The library's `*_status`
identifiers (`mos_disc_status`, `mos_scsi_status`, the tray outcome) are
a different axis and are untouched. Whether `state` may issue disc reads
to enrich the dashboard is a separate, deliberately-deferred decision.
