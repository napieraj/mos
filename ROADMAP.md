# ROADMAP

Forward-looking only — the plan, not the record. What shipped and the
decision back-and-forth that got us here live in the `AGENTS.md` ADRs, the
dated `doc/research/` notes, `doc/history/` (frozen), and git history;
design rationale is in `ARCHITECTURE.md`. This file states what is *not yet
built* and does not relitigate what is.

**Reality check.** mos has never run on a real drive. The pure decision
layer is exercised in CI, but every IOKit / MMC / DiskArbitration assumption
below is off-Mac supposition until the reference rig confirms it. Treat
unbuilt items as hypotheses, not commitments — including the ones written as
if settled.

---

## The bar — spec conformance (not drive agreement)

mos is a spec-defined-MMC library for embedding. Its correctness criterion is
canonical accuracy against MMC, not agreement with any one drive: a single
drive can neither validate conformance nor define it, and baking in what one
happens to do would break the spec-only contract. That bar is
drive-independent and is held by the pure suite + fuzz (the GET CONFIGURATION
walker against MMC §5.2, `state_from_sense_closed` against the ASC/ASCQ table,
hostile inputs exhausted).

The one thing NOT spec-derivable, and so the only standing outstanding check:
the Apple-framework layer mos sits on — whether the `MMCDeviceInterface`
convenience wrappers behave as the headers imply, whether the PREVENT/ALLOW
bit survives handle close, whether `kIOMessageServicePropertyChange`
self-triggers a probe loop. These need *a* Mac and *a* drive — any drive
answers them; they are macOS plumbing, not drive conformance — and they are a
one-time adapter smoke test, a ship-discipline item, **not a design gate**.
Run it before tagging a release shipped; do not block design on it.

Coverage matrix the decision layer must classify correctly (drive-independent,
spec-derived fixtures; real captures optional): pressed audio CD vs pressed
data CD-ROM (both profile 0x0008 — the data-vs-audio split is the canonical
drutil-fails case mos exists to solve, decided by READ TOC CONTROL byte, not
GET CONFIGURATION); pressed DVD-Video (0x0010); pressed BD-ROM (0x0040); blank
CD-R (0x0009) / DVD-R (0x0011) / DVD+R (0x001B); M-Disc BD-R (0x001E); empty
closed tray; open tray.

Optional fixture realism (not the gate). Reference rig: BH16NS55 + WH16NS60
1.00 firmware, OWC Mercury Pro (ASMedia ASM1153E bridge). Capture into
`tests/fixtures/bh16ns55-wh16ns60-1.00/`: `sg_get_config -H`, `sg_inq -e`,
`sg_logs`, `sg_readcap -H`, and READ DISC INFORMATION via `sg_raw` (0x51).
These corroborate the spec-derived fixtures; they gate nothing.

## Standing constraints (carry forward into every item below)

- **Schema freeze.** `mos.*.v1` documents are mutable in place until the
  first tag that ships them; the freeze — and the any-new-shape-is-`.v2`
  rule — takes effect at that tag. Canonical: the AGENTS.md schema ADR.
- **Division of labour.** DR enumerates and hands over cheap coarse status (a
  passive, GESN-fed snapshot "not guaranteed current"); mos owns the
  synchronous, fully-checked state machine and the deep rip-relevant metadata
  DR omits. The MMC state engine must not become a DR passthrough.

---

## Now — v0.4 — finish the typed surface, drop `raw_cdb`

The typed verbs that justify removing the raw passthrough, plus the removal.
`metadata`, `drive`, `features`, `tray`, and `capacity` shipped — the
reserved-name surface is now empty (the reserved-name machinery retired with
it). What remains:

- **Remove `mos_raw_cdb()`** — once the typed verbs cover the diagnostic
  cases the raw passthrough is legacy, and its removal is the major-version
  justification. What remains is the removal decision itself.

- **`eject_requested` watch event** — the cooperative soft-eject the tray
  `lock --persistent` verb sets up: under Persistent Prevent the operator
  button raises a GESN EjectRequest instead of ejecting. Surfacing it on
  `--watch` needs an edge-triggered GESN event-drain distinct from the
  state-diff machinery, and is hardware-gated on whether the event survives
  the kernel's own GESN poll. Design + the rig-check-first build order:
  `doc/research/2026-06-13-eject-request-watch-event.md`.

- **Held-handle identity refresh** — v0.3-line handles capture
  `bsd_unit`/`media_id` once at open, so a handle held across an insert
  reports READY with the open-time -1 (documented as open-time semantics in
  `mos.h`). Under DR, per-query freshness is a dictionary lookup
  (`kDRDeviceMediaBSDNameKey` in the `DRDeviceCopyStatus` media-info dict —
  `doc/dr-field-mapping.md`), not a registry walk.

- **Transitional-state poll escalation** (contingent on hardware evidence).
  A drive persistently classifying EMPTY_OR_OPEN/UNKNOWN (GESN failing through
  a bridge) polls at transition rate — 200 ms cycles — indefinitely, with no
  analog of the error path's 200→2000 ms backoff. Cadence is behavior, so per
  the hardware ADR this waits for an observed case, not a review point.

- **Deferred signal-source work** (designed, parked pending the hardware
  capture): three-tier backoff cadence, a GESN-first probe with cached state,
  and the full signal-source hierarchy (wake → GESN → TEST UNIT READY). The
  `--watch` wake source shipped; these refine its cadence and add fallbacks.

- **Stage-2 media info** (deferred with named falsifiers in the design
  addendum): UDF volume names, BG format status, and — within CD-TEXT,
  whose album-level Title/Performer shipped 2026-06-14 — per-track
  titles, the other field types, and multi-language blocks. Third-party
  ids (MusicBrainz / AccurateRip / dvdid / BDMV) are permanently
  consumer-side.

- **Parked test/robustness remainders:**
  - *EPIPE-path CLI tests* — `mos_cli_stdout_finalize`'s errno-freshness
    argument (cli/io.c) is reasoned, not tested: force EPIPE and non-EPIPE
    stdout failures on the one-shot and watch paths and pin the EX_IOERR
    vs pipe-closed exit split. Harness work, no behavior change.
  - *All-watch directory-rescan fallback* — `mos_watch_open_all` fails
    honestly when the DR doorbell can't be set up. If hardware sessions ever
    observe `DRNotificationCenterCreate` failing in practice, add a
    slow-cadence `mos_internal_dr_copy_snapshot` reconciliation pass; build it
    on that evidence, not before.

**Open empirical questions** (need the rig): does
`kIOMessageServicePropertyChange` self-trigger a probe loop; does brief
exclusive access contend with another application holding the drive; does GET
EVENT STATUS NOTIFICATION report tray/media events reliably across drive
families. (The prevent-bit-survives-close question is now spec-resolved —
per-I_T-nexus state, a SCSITaskUserClient close clears nothing; what the rig
can still *falsify* is whether a given mechanism physically honors prevent,
the per-drive cooperative-button matrix, and a non-conformant bridge dropping
the nexus on close.)

---

## Later — v1.0 — production-ready, embeddable

- **Multi-drive fixtures.** The MT1959HWDN unification means one drive per
  silicon family covers many SKUs. Honest budget for full architectural
  coverage is €700–900 across ~eight rows (see canonical drives below).
  Acquisition order: Apple SuperDrive first (discontinued Aug 2024 →
  secondary-market only), Lite-On iHAS124 W second (cheap, validates the
  Renesas/NEC dispatcher path), Pioneer third (firmware-date discipline),
  Plextor PlexWriter Premium 2 fourth (only acquirable Sanyo silicon), rest
  opportunistically.

- **Decision-tree integration tests.** Largely in place
  (`tests/test_state_core.c`, the headless adapter-fake suite); what remains
  is filling any matrix gaps the rig surfaces.

- **Documentation polish.** A fresh root `CHANGELOG.md` (Keep-a-Changelog)
  generated from git tags at tag time; `mos_open_by_index` race documented as
  a known limitation with `mos_open_by_bsd` marked preferred; the
  lock-composability property made explicit in `ARCHITECTURE.md`.

---

## Canonical reference drives (fixture acquisition)

By silicon family — one drive per family covers many SKUs; multiples of one
family are wasted spend.

- **MediaTek MT1959HWDN (owned).** The reference rig speaks for the whole
  late-LG family — BH14/16NS\*, WH16NS\*, BU/BP slim & USB variants, and the
  ASUS/Buffalo/Dell/HP/Verbatim/Vinpower rebadges.
- **Renesas R8J32xxx (Pioneer).** BDR-XD08 / BDR-212. *Firmware-date
  discipline:* the December-2022 cutoff flips identical silicon between two MMC
  dispatcher behaviors (bus-encryption 0x1B→0x13, mandatory AACS handshake,
  READ DISC STRUCTURE starts refusing) — ask the seller for firmware version
  before buying.
- **Apple bridge + 0xEA gate.** A1379 SuperDrive — late units are `HL-DT-ST
  DVDRW GX50N` inside (the Mac-only enforcement and 0xEA wake live in the
  soldered USB bridge firmware). Discontinued 2024; buy in the cheap window.
  Genuine Panasonic UJ-8A8 is a separate row.
- **Renesas R8J32091NP (Lite-On outlier).** iHAS124 W — a second NEC-lineage
  dispatcher at €15–30.
- **Rest, for full coverage:** Panasonic MN103S (UJ-8A8 / UJ-265), Sanyo
  (PlexWriter Premium 2 / PX-716SA / PX-760SA), MediaTek MT18xx/MT19xx (Lite-On
  iHAS124 B/F, iHBS212), NEC/Renesas (AD-7260S / AD-7280S), MediaTek MT1887 USB
  (Samsung SE-208), ASUS Pioneer-OEM slim (SBW-06D2X-U), Yamaha CRW-F1 (exotic).

**Skip:** generic Chinese USB drives (unpredictable bridge chips → bad fixture
data); IDE-only drives (connection cost > fixture value); post-2017 "Optiarc
AD-5290S" (Vinpower-firmware Lite-On, not Sony silicon).

---

## Out of scope

mos and its typed verbs are for spec-defined MMC only. Out: firmware flashing;
AACS handshake / BD+ / key extraction / LibreDrive microcode (MakeMKV
territory); raw descrambled sector reads; vendor write-quality features that
superficially resemble state — PureRead (Pioneer), AudioMaster (Yamaha),
GigaRec/AutoStrategy (Plextor); M-Disc capability-bit detection (feature
0x0028 — speculative v1.x only); BD-R DL/TL/QL profile differentiation (all
0x001E today, fine for state); BD-RE rewritable state-machine variants (v1.x
if anyone asks); quiet/acoustic mode (unimplementable on HLDS via standard
MMC). Those belong to higher-layer media libraries; mos's acquire-on-call /
release-on-return lock discipline is built to coexist with them, not host them.

---

## Dependency graph (remaining work)

```
spec-conformance (the bar) ─► pure suite + fuzz green ─► decision layer correct
          └─► adapter smoke run on any Mac+drive ─► tag shipped (not a design gate)

v0.4 ─► raw_cdb removed ─► API stable
          └─► eject_requested watch event (rig-gated, optional)

v1.0 ─► multi-drive fixtures ─► integration-test gaps ─► CHANGELOG ─► ship
```

Largest risk: mistaking a drive capture for the correctness bar. mos is
correct iff it is spec-accurate, verified drive-independently (pure suite +
fuzz). The only thing a Mac+drive establishes is that the Apple-framework
adapter is plumbed correctly — a one-time smoke test, not a design gate. Don't
hold the spec-driven work hostage to it.
