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

- `2026-06-10-gesn-single-poll-rebuttal.md` — Dated rebuttal of the
  2026-05-29 note per this README's append rule: the 2026-05-30
  state-detection redesign adopted raw GESN issuance for the tray
  bit because GetTrayState's masking is header-provable, while the
  note's broader claim (no multi-signal GESN polling) still stands.

(An entry for a `2026-04-26-doctrine-review.md` note previously
appeared in this index; that file was never committed and the entry
was removed 2026-06-10.)
