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
  `tests/fixtures/README.md` entry, per the hardware ADR. The capture
  includes, per drive, both identity-string forms (INQUIRY-trimmed vs
  `DRDeviceCopyInfo` CFStrings) for the decision-1 byte-shape diff.
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
    per-open INQUIRY (decision 1, resolved: retire).
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

## Phase 2 — watch: DR doorbell replaces DA (revised same day)

The watch keeps its architecture: private-mode CFRunLoop pump, poll
engine as authority, per-probe open/close by registry ID, TUR⊕GESN
probe. DR changes only who rings the doorbell — and DA retires here,
not in a follow-up:

- Add `DRNotificationCenterCreateRunLoopSource` to the existing
  `MOS_WATCH_RUN_LOOP_MODE` runloop (same single-thread contract;
  callbacks fire only inside `mos_watch_next_event`'s pump).
- `kDRDeviceStatusChangedNotification` → `mos_internal_watch_notify_wake`
  (the same pull-the-poll-forward path the DA callback used). It is
  device-scoped, so it also wakes on tray-open / no-media drives where
  DA's media-scoped, bsd_unit-filtered wake matched nothing
  (`mos_watch.c` ~:348) — strictly better scope match.
- **DA session deleted in this phase.** Rationale (resolves former
  decision 2): polling is the correctness floor by the watch's own
  contract — poll-only is an already-shipped degraded mode
  (`mos_watch.c` ~:341) — so a doorbell source is latency-only, and
  the worst case for any missed wake is `stable_poll_ms` (2s) on top
  of the kernel's own 1000ms media-poll quantization
  (`IOSCSIMultimediaCommandsDevice.cpp:2528`, vendored). The event
  that must never be missed is removal, which rides
  `kIOGeneralInterest`, not DA. Two doorbells would buy bounded
  latency insurance at the price of a second setup/teardown/failure
  matrix; one doorbell + poll floor is the same shape the watch has
  today. If DR notification setup fails, the watch falls back to
  poll-only — the identical, documented fallback DA had.
- `kIOGeneralInterest` remains the terminal-removal source unchanged.
  DR's `Disappeared` notification is NOT additionally registered —
  redundant with a proven direct-IOKit mechanism; revisit only if the
  falsification runs show a removal `kIOGeneralInterest` misses.
- Poll engine: untouched rates. Do NOT lengthen transition/stable
  polling on the promise of DR doorbells — that's a tuning decision
  for after falsification runs show doorbell coverage/latency
  (iterate up on evidence). The integration-matrix doorbell run is
  observational (latency/coverage measurement), not a gate.
- Events/schemas: `mos.event.v1` unchanged (registry_id, states, error
  codes all as today). No schema work in this pivot.
- Net effect on `mos_watch.c`: DA setup/teardown, the
  description-changed callback, and the bsd_unit wake filter are
  deleted; DR source + one callback added. Net-negative LOC, and the
  DiskArbitration framework dependency drops from the watch path
  entirely (link line: it remains only if nothing else uses DA —
  check at implementation; if so, remove `-framework DiskArbitration`
  from CMake/README/amalgamate-header in the same commit).

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

## Decisions (all resolved same day)

1. **INQUIRY: retire in Phase 1.** Condition was "unless it gives us
   something the pivot doesn't" — verified no: `mos_internal_mmc_inquiry`
   (mos_scsi.c:707) extracts exactly vendor/product/revision, all three
   covered by `DRDeviceCopyInfo` keys; no peripheral-type validation to
   lose (IOKit class matching already owns device-type filtering), and
   INQUIRY failure is already the non-fatal empty-strings path DR-key
   absence maps onto. Residual: DR's pre-parsed CFStrings vs the SPC-4
   space-trimmed copies may differ in byte shape (consumer-visible in
   JSON identity fields) — Phase 0 captures both forms per drive and
   diffs; existing SCSI-width buffers remain the truncation bound.
2. **DA retires in Phase 2.** Doorbells are latency-only (poll is the
   correctness floor, kernel media discovery is 1000ms-quantized
   regardless), so the keep-until-proven-superset hedge bought bounded
   insurance at the cost of a second wake mechanism's failure matrix.
   See Phase 2.
3. **Index contract: provenance wording, not a stability promise.**
   mos.h + README document "index = position in the system's device
   array — the same array drutil enumerates — stable only within an
   enumerate→open window." Parity is stated as provenance (both
   readers of one array; cannot diverge); cross-run order stability is
   promised by neither substrate and stays explicitly unpromised,
   matching the existing snapshot-index semantics.
4. **Multi-device watch: out of scope, not foreclosed.** Guard against
   foreclosure: the DR StatusChanged callback filters by registry ID
   at the callback level (a parameter, not a structural assumption),
   so a future watch-all mode widens the filter rather than rewiring
   the pump. Honest cost note: after the schema freeze, watch-all's
   new event kinds are a `mos.event.v2`-class change (the v1 event
   enum is closed and drift-pinned by schemas/validate.py) — deferral
   prices the feature, it does not block it.

## Sizing / order

Phase 0 ≈ a 150-line tool + CMake/CI lines. Phase 1 ≈ 250–350 lines
in `mos_dr.c` plus rewiring deletions. Phase 2 ≈ 100–150 lines in
`mos_watch.c`. Phase 3 is net-negative LOC. Strictly in order, each
phase landing green (with dist regen) before the next; hardware
sessions at Phase 0 (capture) and after Phase 2 (falsification),
neither blocking the code phases.
