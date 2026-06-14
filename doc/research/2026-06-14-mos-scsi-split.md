# Splitting `src/mos_scsi.c`: refactor-feasibility pass (June 2026)

Refactor-prep report. **Question (user):** `src/mos_scsi.c` is the largest
TU in the tree (~1295 lines). Would splitting it improve readability, where
is the seam, and what does an executing session need to do it safely?
**Method:** inventory of the existing file against its own section headers,
symbol-coupling analysis across the proposed boundary, and a touch-list of
every build-graph and CI reference that names the file. No code moves in
this pass — this file is the deliverable, plus the handoff prompt at the
end. Line numbers are as of commit `be2314d`; they drift, so the executing
session **re-greps**, it does not trust the numbers here.

## Why split (and why this is readability, not architecture)

`mos_scsi.c` is the IOKit shell: it owns the device handle and is the only
TU that talks to `SCSITaskLib` / `MMCDeviceInterface`. That is one
*responsibility*, but it is three *layers* stacked in one file, and the
file's own `/* ---- ... ---- */` headers already name the boundaries:

1. **Device lifecycle** (~27–466): CF/registry helpers
   (`mos_internal_cf_number_u64`, `mos_internal_bsd_unit`),
   `mos_enumerate_devices`, the `mos_device_info_*` / `mos_handle_*`
   accessors, `mos_open_*` / `mos_open_device` / `mos_close`,
   `mos_internal_refresh_media_identity`.
2. **Command transport primitives** (~468–666): `mos_internal_ioreturn_to_mos_error`
   and the `mos_internal_mmc_*` convenience wrappers (TUR, GET CONFIG
   current-profile, GET tray-state) the **state core** (`src/mos_state.c`)
   sits on, plus `mos_raw_cdb` (the single `ObtainExclusiveAccess` site,
   ~1176) and its xfer-dir guard.
3. **Typed query surface** (~668–1175): the ~12 `mos_query_*` verbs
   (`disc_info`, `toc`, `cdtext`, `drive_caps`, `enumerate_features`,
   `disc_id`, `physical_structure`, `track_info`, `capacity`, `drive_perf`,
   `mode_caps`, `error_recovery`) plus their private helpers
   (`mos_internal_get_perf`, `mos_internal_mode_sense10`). Every one is the
   same shape: issue a command, hand the raw reply to a pure decoder in
   `src/mos_<feature>.c`, cache in the handle, return an accessor pointer.

Layer 3 is the clean lift. It is ~500 lines of repeated
issue→decode→cache→return, it is what grows every time a new MMC verb is
added, and it does not define any symbol that layers 1–2 call back into.
Moving it to **`src/mos_query.c`** leaves `mos_scsi.c` as "the handle and
the wire" (~800 lines) and gives the verb surface its own file. Length is
not the argument (`mos_result.c` is 600 lines of flat accessors and reads
fine); the argument is that a reader opening `mos_scsi.c` for an open/close
question currently scrolls through twelve unrelated decode wrappers.

## The seam is clean — one shared static to expose

`struct mos_handle` is defined in **`src/mos_internal.h`** (not private to
`mos_scsi.c`), so a new `mos_query.c` that `#include "mos_internal.h"`
reaches `h->mmc`, `h->std`, `h->disc_info`, every cached field — no
accessor plumbing needed. That is the load-bearing feasibility fact; it is
why this is a lift, not a rewrite.

Cross-boundary symbol analysis (verify before moving):

- **Into the new TU, from layer 2:** the moved verbs call
  `mos_internal_ioreturn_to_mos_error` (today a `static` in `mos_scsi.c`,
  ~532) and the `mos_internal_mmc_*` wrappers. The `mmc_*` set is **already
  declared in `mos_internal.h`** (~170–174). The ioreturn mapper is the
  **one** symbol the split must newly expose: move its prototype into
  `mos_internal.h` and drop `static`. (Confirm no third static is shared —
  grep each moved function body for `static`-defined callees that stay in
  `mos_scsi.c`.)
- **Stay in `mos_scsi.c`:** `mos_raw_cdb` and the xfer-dir guard. The query
  verbs that need a raw CDB (if any survive the inventory — most use
  convenience methods) call `mos_raw_cdb` through its existing
  `mos_internal.h` declaration. Keeping `mos_raw_cdb` put preserves
  **AGENTS §3 / scope-doctrine layer 1**: it must remain the SINGLE
  `ObtainExclusiveAccess` call site. The split must not duplicate it or add
  a second lock site — verify with `grep -n ObtainExclusiveAccess src/*.c`
  before and after (count stays at 1).
- **Move with the verbs:** `mos_internal_get_perf`, `mos_internal_mode_sense10`
  — private to the perf / mode-caps verbs, no other caller.

## Build-graph touch-list (every place that names the file)

A new `src/mos_query.c` must be added everywhere `mos_scsi.c` is compiled or
concatenated, or the link breaks / the amalgamation goes stale (the
`dist/` byte-identity gate fails CI):

- **`CMakeLists.txt`** — four targets list `src/mos_scsi.c`:
  `mos_core` (~182), and the three `MOS_BUILD_ADAPTER_FAKE` executables
  (`mos_adapter_oneshot_tests` ~406, `mos_emit_fixtures` ~440,
  `mos_adapter_watch_tests` ~476). Add `src/mos_query.c` to each.
- **`scripts/amalgamate.sh`** — `dist/mos.c` concatenates `src/*.c` in an
  explicit order (~192 emits `mos_scsi.c`). Add a `mos_query.c` block.
  Order: `mos_query.c` after `mos_scsi.c` (it depends on the now-exposed
  `mos_internal_ioreturn_to_mos_error`, but C has no ordering requirement
  across file-scope functions, so placement is cosmetic — keep it adjacent
  to `mos_scsi.c`). **Run `./scripts/amalgamate.sh` and commit `dist/`** in
  the same commit; the CI gate compares byte-for-byte.
- **`.github/workflows/ci.yml`** — the `strict-adapter-build` leg compiles
  a fixed adapter-TU list (~527) under SDK-drift `-Werror`. Add
  `src/mos_query.c` there (it contains Apple SDK signatures, so it belongs
  in this leg, not the pure leg). The comment at ~482 enumerating the
  adapter TUs gets the new name too.
- **`AGENTS.md` / `CMakeLists.txt` comments** — the "`mos_core` — IOKit
  shell (mos_scsi.c, ...)" descriptions (~128) should name `mos_query.c`.

No schema, no public header (`include/mos.h`), no ADR change — the public
`mos_query_*` API is byte-identical; this is a file boundary, not a
contract change. Symbol hygiene is preserved automatically (every moved
symbol is already `mos_`/`mos_internal_`-prefixed; the CI `nm` check covers
the `mos_core` archive regardless of which TU defines a symbol).

## Risks, non-goals, and the stop rule

- **Not a behavior change.** Pure relocation. The proof obligation is
  *identical test output*, not new tests: `ctest` green on macOS, the
  adapter-fake + emit-validate jobs green, `dist/` byte-identical. If any
  test changes, the move was not pure — back out, don't patch the test
  (AGENTS Process rule 1).
- **Don't expand the boundary.** Resist moving layer-2 primitives "while
  you're in there" — the `mmc_*` wrappers stay with the transport because
  the state core depends on them and the file stays conceptually "handle +
  wire." One file out, not a reshuffle.
- **Verify, don't trust the line numbers above.** They are commit-pinned
  and will have drifted. Re-establish the seam by grepping the section
  headers and the `mos_query_` prototypes first.
- **Single commit, reversible.** The whole split is one commit (move +
  build-graph + amalgamation regen). Keep it isolated from feature work so
  a `git revert` is clean if a hardware run later surfaces something.

## Open question for the executing session to resolve first

Do any surviving `mos_query_*` verbs author a raw CDB via `mos_raw_cdb`, or
do they all use `MMCDeviceInterface` convenience methods? If all-convenience
(the likely case — `ReadDiscInformation`, `ReadTableOfContents`,
`GetConfiguration`, `ReadDiscStructure`, `ModeSense10`), then `mos_query.c`
never touches `ObtainExclusiveAccess` and the §3 invariant is trivially
preserved (it lives entirely in `mos_scsi.c`'s `mos_raw_cdb`). If one does,
note it explicitly in the commit message so the lock-site reasoning stays
auditable. Settle this by inventory (grep each moved body for `mos_raw_cdb`)
before moving a line — it decides whether the split touches the project's
one load-bearing invariant at all.

---

## Handoff prompt for the executing session

> Split `src/mos_scsi.c` into `src/mos_scsi.c` (device handle + command
> transport) and a new `src/mos_query.c` (the typed `mos_query_*` verb
> surface). This is a **pure relocation** — no behavior change, no public
> API change, no schema change. Read `doc/research/2026-06-14-mos-scsi-split.md`
> first; it has the seam, the symbol analysis, and the build-graph
> touch-list.
>
> INVENTORY BEFORE MOVING (re-grep — the doc's line numbers have drifted):
> 1. Re-locate the seam from the `/* ---- ... ---- */` headers and the
>    `mos_query_` prototypes. Confirm the move set is exactly the
>    `mos_query_*` verbs plus their private statics
>    (`mos_internal_get_perf`, `mos_internal_mode_sense10`) and nothing the
>    state core or lifecycle calls back into.
> 2. Grep each function you intend to move for `static`-defined callees that
>    will stay behind. The doc predicts exactly one
>    (`mos_internal_ioreturn_to_mos_error`); if there are more, expose each
>    in `src/mos_internal.h` and list them in the commit message.
> 3. Resolve the open question: do any moved verbs call `mos_raw_cdb`? Run
>    `grep -n ObtainExclusiveAccess src/*.c` and confirm the count is 1
>    before AND after the split.
>
> THEN:
> - Move the verb surface + its statics to `src/mos_query.c` (it
>   `#include "mos_internal.h"`; `struct mos_handle` lives there).
> - Expose `mos_internal_ioreturn_to_mos_error` in `mos_internal.h`, drop
>   its `static`.
> - Add `src/mos_query.c` to all four CMake targets, the `strict-adapter`
>   CI source list, and `scripts/amalgamate.sh`; regen `dist/` with
>   `./scripts/amalgamate.sh` and commit it.
> - Update the `mos_core` description comments that enumerate the IOKit-shell
>   TUs.
>
> PROVE IT (the deliverable is identical behavior):
> - Pure suite still builds/runs on Linux (the strict-pure invocation in
>   ci.yml); `dist/` byte-identical to regeneration (the amalgamation gate);
>   `python3 schemas/validate.py` green. macOS-only legs (ctest,
>   adapter-fake, emit-validate, strict-adapter) can't run off-Mac — say so
>   and let CI judge them; do not claim they pass.
> - One isolated commit. If any test output changes, the move was not pure:
>   revert and re-inventory, do not edit the test.
