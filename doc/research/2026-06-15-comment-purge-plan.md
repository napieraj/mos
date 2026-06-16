# Comment purge plan (supersedes 2026-06-11)

**Date:** 2026-06-15. **Status:** in execution.

The 2026-06-11 plan landed its Phase 0 (the doctrine now in
`CONTRIBUTING.md` §Comment doctrine) and stopped. Phases 1–4 never
ran, and the tree roughly doubled afterward — `include/mos.h` 657→1184
lines, and ~10 `src/`/`cli/` files (`mos_tray`, `mos_da`,
`mos_modepage`, `mos_physstruct`, `mos_discstruct`, `mos_trackinfo`,
`mos_perf`, `cli/tray`, `cli/capacity`, `cli/metadata`) arrived never
governed by the tiers. Measured 2026-06-15: 2,352 leading-comment
lines / 12,164 source lines = 19.3% (trailing comments uncounted, so
the real share is higher).

This plan executes the original cuts against the grown tree, plus two
maintainer mandates from 2026-06-15: spec/safety blocks are tightened,
never gutted (the AGENTS.md ADRs protect spec tables and adversarial-
input contracts); and every surviving comment is re-read and rewritten
in place, not mechanically sliced.

## Ground rules (the canonical set; mirrored into CONTRIBUTING.md)

1. **Budget scales with audience.** `include/mos.h` keeps full caller
   contracts (only doc external consumers get). Internal seam headers
   get 1–3 line contracts. `.c` internals get why-only.
2. **No changelog in code.** No dates, `v0.x`, "supersedes", "(Commit
   D)", session/audit provenance. Durable rationale moves to a doc
   with a `§` pointer; the rest is deleted. A `_Static_assert`
   message containing "no longer matches …" is a runtime guard, not
   history — it stays.
3. **No "how it used to be."** The code is present tense. "the earlier
   behavior is retired", "old fixed 4096 buffer", "has bitten us
   before" → delete.
4. **No explaining the obvious.** Comments restating the next line go.
5. **One home per fact.** Header↔`.c` duplication collapses to one
   copy (contract in the header for public API, in the `.c` for
   internals).
6. **Survivors are rewritten by hand.** Not sliced. A comment that
   lives gets re-read in context and tightened.
7. **Spec tables and safety contracts are tightened, not removed.**
   One spec byte-layout table and one safety contract per parser,
   protected by the hardware/spec ADRs.

## Phases (executed as one pass, reviewed as one diff)

- **A — doctrine.** Sharpen CONTRIBUTING.md §Comment doctrine with
  rules 2/3/6/7 made explicit. This doc.
- **B — header↔.c dedup + tiering.** `cli/io.h↔io.c`,
  `cli/human.h↔human.c`, `mos_pure.h↔{sense,config,discinfo,…}.c`,
  `include/mos.h↔mos_result.c`.
- **C — history out of code.** Strip dated/changelog/provenance text;
  leave `§` pointers where rationale is durable.
- **D — narration + stale file headers.**
- **E — new ungoverned files** get the tiers.
- **F — spec/safety `.c`: tighten only.**
- **G — regenerate + verify.** `scripts/amalgamate.sh`,
  `scripts/doc-staleness.sh`, build + ctest green, before/after counts
  in the commit message (not in live docs — hardcoded counts are a
  staleness deny-pattern).

## Refusals (unchanged from 2026-06-11)

No deletion of kernel citations, safety contracts, ownership notes, or
spec tables to hit a line target. No hand-edits to `dist/` (regenerate
from source). No rewriting of AGENTS.md/CLAUDE.md/ARCHITECTURE.md
history.
