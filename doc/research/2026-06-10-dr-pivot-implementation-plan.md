# DR pivot — implementation plan (enumeration, watch, callbacks)

Companion to `2026-06-10-dr-pivot-feasibility.md` (and its same-day
revision) and `doc/dr-field-mapping.md`. Headers vendored at
`docs/apple/DiscRecording/` (15.5 SDK). Target: v0.4.

## Doctrine (one sentence per layer)

**DR is the doorbell and the directory; MMC is the inspector.** DR
supplies discovery, identity, addressing, and wake events; the
TUR⊕GESN state core remains the sole authority on drive state (the
§5.5 nub gate runs on TUR sense bytes DR does not expose — feasibility
note, revision item 3). DR data never decides a state; DR events only
schedule probes. Scope doctrine is unchanged: mos still authors
exactly one raw CDB (GESN); DR is not a command author, it is a
substrate above the same kext.

New Apple-adapter TU `src/mos_dr.c` (links DiscRecording + CF + IOKit).
`mos_pure` stays CF-free; the adapter extracts DR dictionaries into a
plain-C `mos_internal_dr_snapshot` struct at the seam, and everything
below that struct is pure and Linux-testable.

## Phase 0 — probe tool + fixtures (hardware feeds, never steers)

- `tools/mos_dr_probe.c`, standalone diagnostic in the
  `mos_notification_probe.c` mold (deliberately links DiscRecording/CF
  only, not mos_core, so it observes through none of mos's
  abstractions): dump `DRCopyDeviceArray` order, per-device
  `DRDeviceCopyInfo`/`CopyStatus` as XML plists, and a timestamped
  `kDRDevice{Appeared,Disappeared,StatusChanged}` stream.
- CMake `MOS_BUILD_DR_PROBE` (mirror `MOS_BUILD_NOTIFICATION_PROBE`),
  CI compiles it so the TU can't bitrot.
- One hardware capture session on Tahoe → committed fixtures (plist
  dumps + extracted plain-struct forms) with a dated
  `tests/fixtures/README.md` entry, per the hardware ADR.
- Phase 1 does NOT block on this: the dict→struct extraction is built
  to the vendored header's documented key types, with synthetic
  fixtures; real captures later validate or falsify (and become the
  regression set).

## Phase 1 — enumeration / identity / addressing (one-shot + list)

`src/mos_dr.c`:
- `mos_internal_dr_copy_snapshot()` → ordered array of
  `{ registry_id, bsd_unit, vendor, product, revision }`:
  - index = DR array position + 1 — **drutil parity by construction**
    (drutil-contract note tier-1 evidence), retiring the registry-ID
    sort approximation in `mos_enumerate_devices`.
  - `kDRDeviceIORegistryEntryPathKey` → `IORegistryEntryFromPath` →
    `IORegistryEntryGetRegistryEntryID` — the one surviving IOKit step
    (dr-field-mapping §identity); keeps `registry_id`/`media_id` (F1)
    and `mos.event.v1` unchanged.
  - `kDRDeviceMediaBSDNameKey` → existing `parse_bsd_unit` (absent ⇒
    -1, same media-scoped semantics as today).
  - vendor/product/revision from `DRDeviceCopyInfo` — retires
    per-open INQUIRY (decision 1 below).
- Rewired public paths (signatures and ABI unchanged):
  - `mos_enumerate_devices` walks the snapshot.
  - `mos_open_by_index` = snapshot[i] → registry path/ID →
    `mos_internal_open_service` — no re-enumeration.
  - `mos_open_by_bsd_name` = `parse_bsd_unit` pre-gate (preserves the
    malformed→`invalid_arg`/64 vs absent→`no_device`/66 contract) →
    `DRDeviceCopyDeviceForBSDName` → path → open. Deletes the
    walk-down enumeration; the never-implemented walk-up
    (ARCHITECTURE "superseded") dies unbuilt.
- CLI: `collect_and_query` consumes ONE snapshot and opens rows by
  registry ID. This deletes the three deferred review findings in one
  move: the O(n²) per-row re-enumeration, the single-drive
  double-probe, and `resolve_index_of` (index comes from the snapshot
  row, not a second enumeration).
- Tests: snapshot-ordering/identity logic pure-tested on constructed
  structs; CLI contract tests unchanged (zero-drive CI behavior is
  identical — DR returns an empty array on runners, same as IOKit
  matching today); `nub_invariant_check` untouched.

## Phase 2 — watch: DR doorbells on the existing pump

The watch keeps its architecture: private-mode CFRunLoop pump, poll
engine as authority, per-probe open/close by registry ID, TUR⊕GESN
probe. DR changes only who rings the doorbell:

- Add `DRNotificationCenterCreateRunLoopSource` to the existing
  `MOS_WATCH_RUN_LOOP_MODE` runloop (same single-thread contract;
  callbacks fire only inside `mos_watch_next_event`'s pump, like DA
  today).
- `kDRDeviceStatusChangedNotification` → `mos_internal_watch_notify_wake`
  (same pull-the-poll-forward path as the DA callback). Net new
  coverage: DR's notification is **device-scoped**, so it wakes on
  tray-open / no-media drives where DA's media-scoped wake cannot —
  closing the documented poll-only gap (`mos_watch.c` ~:348).
- `kDRDeviceDisappearedNotification` → terminal-removal wake,
  backstopping `kIOGeneralInterest` (keep both initially; they're one
  registration each and removal is the event a watch must never miss).
- Poll engine: untouched rates. Do NOT lengthen transition/stable
  polling on the promise of DR doorbells — that's a tuning decision
  for after falsification runs show doorbell coverage/latency
  (iterate up on evidence).
- DA session: keep in v0.4 alongside DR; retire in a follow-up only
  after the integration matrix shows DR's wake is a superset. Two
  doorbells are cheap; a missed wake is not.
- Events/schemas: `mos.event.v1` unchanged (registry_id, states, error
  codes all as today). No schema work in this pivot.

## Phase 3 — retirements + docs

- Delete: enumeration walk in `mos_open_by_bsd_name`, registry-ID
  sort, `resolve_index_of`, per-open INQUIRY (per decision 1), and the
  `collect_and_query` re-open path.
- ARCHITECTURE: new substrate section (DR directory/doorbell + MMC
  inspector), §11 gains the DR header provenance; seam-census and
  dr-field-mapping get dated follow-ups (state rows: NOT adopted, per
  the feasibility revision).
- AGENTS scope doctrine: append a dated entry recording that DR does
  not change the command surface (still one raw CDB).
- ROADMAP: v0.4 entry; INTEGRATION_HARNESS gains the matrix rows below.

## Build / CI / dist

- `mos_core` additionally links `-framework DiscRecording` — CMake
  target, README/CONTRIBUTING/amalgamate-header link lines, and
  `homebrew` caveats all updated in the same commit (doc-staleness
  gate will hunt stragglers).
- `scripts/amalgamate.sh`: add `mos_dr.c` to the weave list; guard
  checks unchanged (new TU keeps the 1+1 include-guard shape).
  dist/ regenerated in the same commit (CI byte-identity).
- CI: strict-adapter job adds the new TU; symbol-hygiene check covers
  the new object; macOS jobs link DiscRecording from the runner SDK
  (present in 15.5 — verified).

## Coexistence analysis (required: we hold an exclusive lock sometimes)

DR status/notification APIs take no exclusive access (header-verified,
feasibility note §4). Open question: whether DR's machinery issues its
own convenience commands around notification registration — if so,
mos's temporary exclusive GESN window could make *DR* mis-observe
(the §9.7 collapse, on DR's side of the fence). Falsification run for
the Phase 0/2 hardware session: `mos_dr_probe` notification stream +
`mos --watch` running concurrently through tray cycles; diff the
timelines; commit as fixture. Matrix additions: DR-array order vs
`drutil list` (parity claim), doorbell latency vs poll schedule,
GESN-failing bridge if one enters the fixture set.

## Decisions needing sign-off

1. **INQUIRY retirement** in Phase 1 (identity from CopyInfo) vs
   keeping it one release as a fallback. Plan assumes retire; the
   seam keeps the slot if we keep it.
2. **DA retirement** deferred (plan keeps DA+DR in v0.4) — confirm.
3. **Index contract wording**: document "index = DR device-array
   order (drutil-identical)" in mos.h + README once Phase 1 lands.
4. Multi-device watch via `kDRDeviceAppearedNotification`: out of
   scope for v0.4 (single-target watch unchanged).

## Sizing / order

Phase 0 ≈ a 150-line tool + CMake/CI lines. Phase 1 ≈ 250–350 lines
in `mos_dr.c` plus rewiring deletions. Phase 2 ≈ 100–150 lines in
`mos_watch.c`. Phase 3 is net-negative LOC. Strictly in order, each
phase landing green (with dist regen) before the next; hardware
sessions at Phase 0 (capture) and after Phase 2 (falsification),
neither blocking the code phases.
