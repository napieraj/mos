# Research notes

Dated research artifacts that informed `mos`'s design decisions.
These are not active project documents — they're frozen snapshots
of what was known on a specific date, cited from `ROADMAP.md` and
`CLAUDE.md` as the basis for specific decisions.

If a finding here gets superseded by later observation, do not edit
the file in place. Append a new dated file rebutting the original on
the merits, same append-with-argument discipline that governs ADRs
in `AGENTS.md`. The chain of files is the audit trail.

## Index

- `2026-04-22-driverkit-investigation.md` — SDK header audit,
  kmutil showloaded, and otool analysis confirming SCSITaskLib and
  MMCDeviceInterface remain present and undeprecated on macOS 26.5
  beta. Cited from ARCHITECTURE.md §9.5.

- `2026-04-26-hlds-silicon-and-mmc.md` — cdrinfo.pl reverse-engineering
  community findings on HLDS BH16NS55/WH16NS60 silicon and MMC
  dispatcher behavior. Justifies the v0.3 typed API design choice
  to target MMC-6/SPC-4 conformance with three carved-out profile
  flags rather than chase HLDS-specific quirks.

- `2026-04-26-silicon-family-map.md` — Silicon family map across
  vendors (Pioneer, Panasonic, Sony Optiarc, Samsung/TSST, Plextor,
  Lite-On, ASUS, Yamaha) for v1.0 fixture coverage. Falsifies the
  Apple SuperDrive A1379 = Panasonic UJ-8A8 hypothesis (it's HLDS
  GX50N inside an Apple shell with Apple-firmware USB bridge).
  Concrete twelve-drive fixture set at €700–900 for €1.0 ship.

- `2026-04-27-v2-contract-design.md` — Design reasoning behind the
  late-April v2 contract lock (API surface, error enum, string
  lifetime, CLI envelope shape). Superseded by the v0.3 per-document
  schema family; preserved unmodified with a superseding banner as
  the research trail behind the lock.

- `2026-05-29-gesn-single-poll.md` — GESN as a single-command
  multi-signal source, weighed against the watch architecture and
  deferred. **Partially inverted the next day** — see the
  2026-06-10 rebuttal below; the pure-decoder discipline in its
  "if pursued" section was followed, the issuance verdict and its
  GetTrayState premise were not.

- `2026-06-10-drutil-contract.md` — drutil(1) drive-selection contract
  extracted from the ADC man page and mapped to `DRCoreDevice.h` keys;
  three-tier evidence rating for "mos --index matches drutil by
  construction"; the filter-chain grammar evaluated and rejected; the
  --index / --bsd / --registry-id selector decision with the
  identity/addressing/format role split.

- `2026-06-10-dr-pivot-feasibility.md` — Web-verified feasibility and
  advisability check on the DiscRecording substrate pivot against
  current macOS (15.5 SDK headers vendored and diffed, deprecation
  status, coverage parity, runloop/entitlement caveats, survival
  risk). Verdict: feasible and advisable for enumeration/status/watch
  with the MMC sense path retained. Same-day revision in the file:
  the original tray-validation gate was wrong-shaped — the vendored
  kernel source proves GetTrayState's failure collapse is
  kernel-level and undetectable from userspace, so positive hardware
  runs cannot bless DR's tray bit; the GESN hybrid is the design,
  not the fallback. Also records that public kernel source for the
  MMC kext froze at Tiger-era 139.0.2.

- `2026-06-10-dr-pivot-implementation-plan.md` — Phased implementation
  plan for the DR pivot under the "DR is the doorbell and the
  directory; MMC is the inspector" doctrine: probe tool + fixtures,
  enumeration/identity/addressing on the DR snapshot (drutil-parity
  index, registry-path→ID identity), the DR doorbell replacing DA on
  the existing watch pump (same-day revision: DA retires in Phase 2 —
  doorbells are latency-only over the poll floor and the kernel's own
  1000ms media poll), retirements, coexistence falsification runs.
  All four design decisions resolved in-file same day: INQUIRY
  retires (identity-only, source-verified), DA retires, index gets
  provenance-grade wording, and watch-all pulled INTO scope as Phase
  2b (snapshot-as-join needs no schema motion; the DR notification
  center is the bus the watch was always meant to sit on).

- `2026-06-10-full-tree-review.md` — 21-angle line-by-line review of
  the whole tree at 0.4.0-dev: per-area verdicts (nine areas clean,
  incl. the nub gate re-verified against vendored kernel source and a
  complete CF/IOKit lifecycle ledger), three real bugs found and
  fixed (human-path terminal injection, watch-all join demotion,
  schema-invalid index sentinel), the refuted-candidates record, and
  the agent-contradiction process note. Fixes in commit 46be3d7.

- `2026-06-10-gesn-single-poll-rebuttal.md` — Dated rebuttal of the
  2026-05-29 note per this README's append rule: the 2026-05-30
  state-detection redesign adopted raw GESN issuance for the tray
  bit because GetTrayState's masking is header-provable, while the
  note's broader claim (no multi-signal GESN polling) still stands.

- `2026-06-11-comment-refactor-plan.md` — Survey and phased plan for
  deduplicating the comment mass: per-file comment-density table,
  four bloat categories (header↔.c duplication, history-in-code,
  ABI-pin boilerplate, narration) with verified instances, a
  protected load-bearing category that the plan refuses to cut, a
  public-vs-internal commentary-tier rule plus the single-home rule
  (both destined for CONTRIBUTING.md), and five comment-only
  execution phases gated on tests + `dist/` regen.

- `2026-06-12-libredrive-probe-feasibility.md` — Full-session research
  report on replicating makemkvcon's LibreDrive status in `mos`: how
  makemkvcon determines status (closed `LibDriveIO`/`SDF.bin`), how `mos`
  models device identity/flags and its one-raw-CDB command surface, the
  scope/safety constraints any probe must meet, the Phase A feasibility
  verdict, and the fallback. Verdict: not recoverable from public sources
  — the status logic lives in MakeMKV's closed, unreleased
  `LibDriveIO`/`SDF.bin` (the firmware fingerprint is itself a proprietary
  blob); the only public substrate is the generation-specific MediaTek
  `0xF2`-series RAM debug/code-injection interface, undocumented for
  MT1959; and "Enabled" is inseparable from the RAM-microcode upload `mos`
  excludes. Independently fails ROADMAP.md:214,314 and the one-raw-CDB
  doctrine. Records the only honest fallback (a clearly-labelled
  identity-family heuristic, which is a guess, not detection). Track
  closed at Phase A.

- `2026-06-13-disc-tools-state-survey.md` — Ecosystem cross-reference
  (libcdio, MediaInfo, dvd+rw-mediainfo, ddrescue, redumper,
  xorriso/libburn, dvdisaster, cdrdao, sg3_utils, cdrom_id, drutil):
  what each surfaces as drive/disc state, by what mechanism, and the
  candidate enrichment paths tiered against the scope doctrine. Two
  load-bearing corrections: CONFIRMS MediaInfo is file-only (the
  "TOC from kernel cache" claim is libcdio's `kIOCDMediaTOCKey`,
  CD-only, full-TOC/MSF shape), and OVERRIDES a sub-agent claim that
  `GetPerformance`/`ReadTrackInformation` lack convenience wrappers
  (they do not — `ARCHITECTURE.md:834`), which moves capacity/speeds/
  mechanical-state from raw-CDB to convenience-method scope. Ranked
  recommendation: extend READ DISC STRUCTURE (DVD physical/copyright/
  mfr-ID), then capacity/NWA via `ReadTrackInformation`. Records
  redumper as proof the macOS raw path works on console privilege
  (mos's one-raw-CDB rule is self-imposed scope) and libburn's macOS
  MMC path as dead (disabled behind `GET_SCSI_FIXED`).

- `2026-06-13-tray-control-feasibility.md` — Full-session feasibility
  report on the v0.4 tray-control surface (eject/close/lock/unlock) for a
  ripping-robot workflow, verified against the tree + canonical sources
  (T10 04-349r1, Apple's `IOSCSIMultimediaCommandsDevice`). Verdict:
  feasible, the planned surface, and the robot's "lock survives handle
  close" requirement is spec-derived (per-I_T-nexus PREVENT state; a
  SCSITaskUserClient close is none of the SPC-4 clearing events; no
  voluntary kext unlock on release) — so the verbs are fire-and-forget,
  not a held session, and hardware's role is falsification, not design
  input. Basis for the implemented tray verbs and the AGENTS controller-
  verbs ADR.

- `2026-06-13-eject-request-watch-event.md` — Design note for the
  `eject_requested` watch follow-on (NOT implemented): why the
  Persistent-Prevent soft-eject is the one case state-observation cannot
  close (no state change by construction — edge vs level), what an
  edge-triggered GESN event-drain in the watch would touch, and the three
  constraints that make it hardware-gated (destructive single-consumer GESN
  queue, the kernel's own ~1000 ms GESN poll racing userspace, multi-watcher
  event theft). Build order: rig check first (does EjectRequest reach
  userspace at all, via GESN drain or a forwarded IOKit notification), code
  only if yes. No IPC daemon — the mechanism stays observation through the
  drive.

- `2026-06-13-read-capacity-feasibility.md` — Feasibility note for the
  reserved `capacity` verb. Header-confirmed against `SCSITaskLib.h` that
  `MMCDeviceInterface` has NO READ CAPACITY / READ FORMAT CAPACITIES
  convenience wrapper, so a capacity *command* is raw-CDB-only (exclusive
  access → BUSY on mounted media, the exact case capacity is wanted). Verdict:
  don't add a raw READ CAPACITY — whole-disk byte capacity is free from the
  kernel-computed `kIOMediaSizeKey`/`kIOMediaPreferredBlockSizeKey` on the
  IOMedia node mos already resolves (no command, no lock, works mounted), and
  the recordable/append-state view already ships via READ TRACK / DISC
  INFORMATION. READ FORMAT CAPACITIES `0x23` is the one raw-verb candidate
  (blank-media max formattable capacity, the gap the cheap paths miss),
  deferred behind a stated need + the layer-1 showing. Includes the
  no-new-command `mos.capacity.v1` surface sketch and pickup checklist.

- `2026-06-14-mos-scsi-split.md` — Refactor-feasibility pass for splitting
  the tree's largest TU (`src/mos_scsi.c`, ~1295 lines) into device-handle
  + transport vs. a new `src/mos_query.c` holding the typed `mos_query_*`
  verb surface. Confirms the seam is clean: `struct mos_handle` already
  lives in `src/mos_internal.h`, so the lift needs exactly one static
  exposed (`mos_internal_ioreturn_to_mos_error`) and `mos_raw_cdb` stays
  put as the single `ObtainExclusiveAccess` site (AGENTS §3). Pure
  relocation, no API/schema/ADR change. Includes the build-graph touch-list
  (four CMake targets, `amalgamate.sh`, the strict-adapter CI leg) and a
  ready-to-paste handoff prompt for the executing session.

- `2026-06-16-serial-vpd-0x80-feasibility.md` — Resolves the long-open
  stage-1 falsifier (carried in the 06-10 media-info-design and 06-13
  disc-tools-survey notes): does Apple's convenience `Inquiry`
  (`MMCDeviceInterface`) surface VPD page 0x80 (Unit Serial Number)?
  Answered NO from the header alone (no hardware) — the `Inquiry` pointer
  takes only `SCSICmd_INQUIRY_StandardData*`, no EVPD/PAGE_CODE parameter,
  and the file has no separate VPD/serial method; contrast `ModeSense10`
  (PC/PAGE_CODE) and `GetConfiguration` (RT). Both cheap paths also ruled
  out: `DRDeviceCopyInfo` has no serial key (dr-field-mapping), the
  standard-INQUIRY vendor tail (bytes 36+) is rejected as device-quirk
  special-casing, and the IOKit IORegistry path — checked because a serial
  could hide under a non-obvious key the way firmware hides under "revision"
  — defines `kIOPropertyProductSerialNumberKey` ("Serial Number") but the
  optical kernel stack (`IOSCSIPrimaryCommandsDevice` /
  `IOSCSIProtocolServices` / `IOATABlockStorageDevice`, source-verified)
  never populates it: it parses only standard INQUIRY, never VPD 0x80. So
  the serial is a raw INQUIRY VPD 0x80 under the AGENTS
  layer-1 raw-verb rule: showing (a) satisfied by the header, showing (b)
  the ObtainExclusiveAccess→BUSY-on-mounted analysis (benign — serial is a
  static fact, null-on-busy is graceful). Includes the CDB, pure-parser +
  fixture shape, and the build-when-a-consumer-exists recommendation. `mos`
  behavior unchanged (Process rule 2 / hardware-role ADR).

(An entry for a `2026-04-26-doctrine-review.md` note previously
appeared in this index; that file was never committed and the entry
was removed 2026-06-10.)
