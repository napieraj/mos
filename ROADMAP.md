# ROADMAP

Forward-looking only. What shipped, when, and the decision back-and-forth that
got us here live in `doc/history/CHANGELOG.md` (retired 2026-06-11, frozen); design rationale lives in `ARCHITECTURE.md`;
project rules in `AGENTS.md`. This file states the plan and does not relitigate
it.

**Reality check.** mos has never run on a real drive. The pure decision layer is
exercised in CI, but every IOKit / MMC / Disk-Arbitration assumption below is
off-Mac supposition until the reference rig confirms it. Treat unbuilt items as
hypotheses, not commitments — including the ones written as if settled.

---

## Gate — spec-conformance (the real bar)

mos is a spec-defined-MMC library for embedding (see the v0.4 "what mos is not
for" ADR). Its correctness criterion is canonical accuracy against MMC, not
agreement with any one drive: a single drive can neither validate conformance
nor define it, and baking in what one happens to do would break the spec-only
contract. That bar is drive-independent and largely met for the decision layer —
the pure suite + fuzz exercise the walker against MMC §5.2 and `state_from_sense_closed`
against the ASC/ASCQ table, hostile inputs exhausted. TUR, REQUEST SENSE, GET
CONFIGURATION, INQUIRY are the most-mapped commands in the optical world
(makemkvcon, libcdio, the kernel, sg3_utils); their return shapes are settled.

The one thing NOT spec-derivable, and so the only real outstanding check: the
Apple-framework layer mos sits on — whether the `MMCDeviceInterface` convenience
wrappers behave as the headers imply, whether the PREVENT/ALLOW bit survives
handle close, whether `kIOMessageServicePropertyChange` self-triggers a probe
loop. These need *a* Mac and *a* drive — any drive answers them, they are macOS
plumbing, not drive conformance — and they are a one-time adapter smoke test, a
ship-discipline item, not a design gate. Run it before tagging a release
shipped; do not block design on it.

Coverage matrix the decision layer must classify correctly (drive-independent,
spec-derived fixtures; real captures optional): pressed audio CD vs pressed data
CD-ROM (both profile 0x0008 — the data-vs-audio split is the canonical
drutil-fails case mos exists to solve, decided by READ TOC CONTROL byte, not GET
CONFIGURATION); pressed DVD-Video (0x0010); pressed BD-ROM (0x0040); blank CD-R
(0x0009) / DVD-R (0x0011) / DVD+R (0x001B); M-Disc BD-R (0x001E); empty closed
tray; open tray.

Optional fixture realism (not the gate). Reference rig: BH16NS55 + WH16NS60 1.00
firmware, OWC Mercury Pro (ASMedia ASM1153E bridge). Capture into
`tests/fixtures/bh16ns55-wh16ns60-1.00/`: `sg_get_config -H`, `sg_inq -e`,
`sg_logs` (LOG SENSE — there is no `sense` page acronym; use a real `-p` token
or omit `-p` for the supported-page list), `sg_readcap -H`, and READ DISC
INFORMATION via `sg_raw` (0x51; byte-2 disc-status field is the disc-completion
signal). These corroborate the spec-derived fixtures; they gate nothing.

---

## Standing context — landed substrate, open remainders

The v0.3 line shipped (contract, `--watch`, the schema family,
hardening, F1 `media_changed`) and the DiscRecording substrate landed
2026-06-10 (enumeration, identity, addressing, doorbell, watch-all);
DiskArbitration left the project entirely 2026-06-11. The decision
records — pre-pivot plan, dies/survives checklist, walk-up
dissolution, GESN single-poll deferral, probe consolidation, shipped
lists — are frozen in
`doc/history/2026-06-10-dr-pivot-decision-record.md`. What stays live
from that arc:

- **Schema freeze status.** `mos.*.v1` documents remain mutable in
  place until the first tag that ships them; the freeze (and the
  any-new-shape-is-`.v2` rule) takes effect at that tag. Canonical:
  the AGENTS.md schema ADR (revision 2026-06-10).
- **Held-handle identity refresh (open, lands in v0.4).** v0.3-line
  handles capture `bsd_unit`/`media_id` once at open, so a handle held
  across an insert reports READY with the open-time -1 (documented as
  open-time semantics in mos.h). Under DR, per-query freshness is a
  dictionary lookup — `kDRDeviceMediaBSDNameKey` in the
  `DRDeviceCopyStatus` media-info dict, the same dictionary the
  status-changed notification delivers (`doc/dr-field-mapping.md`,
  bsd_unit row) — not a registry walk.
- **Media info (open, lands in v0.4).** ReadDiscInformation shipped as
  `mos_query_disc_info()`; what remains is stage 1 of
  `doc/research/2026-06-10-media-info-design.md`: two subject-pure
  documents — `mos metadata --json` → `mos.metadata.v1` (disc only;
  TOC as required-nullable fingerprint subtree) and `mos drive
  --json` → `mos.drive.v1` (identity, firmware, INQUIRY serial,
  spec-grounded AACS/bus-encryption capabilities) — plus the
  mounted-volume name. Stage 2 (UDF names, CD-TEXT, capacity blocks)
  stays deferred with named falsifiers. Third-party ids
  (MusicBrainz/AccurateRip/dvdid/BDMV) are permanently consumer-side.
- **Division of labour (standing doctrine).** DR's status dict exposes
  coarse signals as a passive, GESN-fed snapshot that is "not
  guaranteed current." mos does not collapse its state engine into
  that dict; it owns the synchronous, fully-checked, corroborating
  state machine and the deep rip-relevant metadata DR omits. DR
  enumerates and hands over cheap coarse status; mos interprets and
  enriches. The MMC state engine must not become a DR passthrough.

- **Headless adapter emulation (phase 1 LANDED 2026-06-11; phase 2
  open).** A link-seam fake of the IOKit + DiscRecording C symbols
  (real CoreFoundation linked) runs the Apple adapter TUs headless
  against committed MMC fixtures. Phase 1 shipped
  (`tests/fake/mos_fake_apple.c`, `tests/test_adapter_phase1.c`, the
  `adapter-fake` CI job under ASan/UBSan): the one-shot paths —
  open/enumerate/query through the REAL `mos_scsi.c`/`mos_state.c`/
  `mos_dr.c` — across READY/EMPTY/OPEN/LOADING/EMPTY_OR_OPEN and the
  disc-info fixtures, with the §5.5 lock balance asserted both
  directions, the GESN CDB pinned byte-for-byte, and seam-contract
  O-1/O-3 moved from hardware-gated to CI (INTEGRATION_HARNESS
  falsification item 0 updated). **Phase 2 (watch lifecycle —
  `mos_watch.c` + notification/run-loop symbols) remains open**: it is
  a real state-modelling subsystem, not a shim. This is **additive** —
  it retires hardware-gated *status*, not the pure layer's adversarial
  fuzz or the exhaustive nub-invariant proof, which test a different
  (full-octet, hostile) domain and stay. Full build brief, seam
  inventory, phased plan, and the five-leg cross-validation recipe
  (against the circular-oracle problem) in
  `doc/research/2026-06-11-headless-adapter-emulation.md`.

---

## Now — v0.4 — typed APIs, tray verbs, drop `raw_cdb`

The typed surfaces that justify removing the raw passthrough, plus the removal.

**Five typed APIs**, each a spec-defined MMC command behind a subcommand
(currently reserved names returning a "not yet implemented" diagnostic):
`capacity` (READ CAPACITY / READ DISC INFORMATION), `identity` (INQUIRY), `tray`
(verbs below), `speed` (SET CD SPEED, read-only reporting where the drive
allows), `features` (GET CONFIGURATION descriptors). CDBs built from SDK
constants, never hand-packed. Two hardware-gated design axes: the firmware-policy
generation on `mos_open()` (the Pioneer December-2022 cutoff flips bus-encryption
and AACS behavior on identical silicon) and the Apple SuperDrive 0xEA wake gate
(research-gated).

**Drive selectors** — `--registry-id` joins `--index` and `--bsd` as a third,
separate flag (no shared grammar: a bare decimal would be ambiguous between
index and registry id). Role split: registry entry ID = attachment identity (the watch's
own reopen authority; survives media churn, BSD renaming, and media-less
periods within one plug session — a replug mints a new ID by construction,
xnu's monotone never-reused counter, which is exactly the watch's TOCTOU
defense), index =
addressing/interop (1-based, DR-array positional once the DR pivot lands;
drutil-aligned — see the tiering and falsifier in
`doc/research/2026-06-10-drutil-contract.md`), BSD name = an I/O format
drutil itself cannot select by. drutil's composable filter grammar (bus
keywords, vendor/product match, multi-drive fan-out) was evaluated and
**rejected** — bus keywords are a 2004 contract on all-USB 2026 hardware and
the Intel cliff (§9.5.1 in ARCHITECTURE), vendor matching fails the
identical-drive rig that is the actual multi-drive use case, and fan-out
contradicts the single-drive-verb composition doctrine. Rationale recorded in
the research note.

**Tray control verbs** — `mos tray {eject, close, lock, unlock}`, built on the
three-level MMC removal-gating model (PREVENT ALLOW MEDIUM REMOVAL, Prevent
field, MMC-6 §6.x):

- **Level 0 — unlocked** (Prevent `00b`): front-panel button and host LoEj
  both eject freely.
- **Level 1 — locked** (Prevent `01b`): button inert; a bare eject fails at
  the drive with sense `5/53/02` MEDIA REMOVAL PREVENTED; cleared by
  reset/power-cycle.
- **Level 2/3 — persistent prevent** (Prevent `10b`/`11b`): locked, and a
  button press is converted into a host-visible GESN Media event,
  **EjectRequest (event code 1)** — the soft-eject protocol. The drive asks;
  the orchestrator decides. (Event code verified against Linux `sr.c`, which
  maps it to `DISK_EVENT_EJECT_REQUEST`; the same Media class mos's GESN
  decoder already parses, so surfacing `eject_requested` through `--watch` is
  a natural v0.4+ event-stream extension once the persistent verbs exist.)

Verb mapping: `eject` = polite, lock-respecting LoEj — a `5/53/02` refusal is
a *reported fact*, not an error to defeat; `eject --force` = unlock-then-eject
in one open/close cycle, which is byte-for-byte the kernel's own
`EjectTheMedia` sequence (unconditional `PREVENT_ALLOW(0)` then
`START_STOP_UNIT` LoEj — apple-oss kernel source; the OS's eject is
force-shaped, mos just makes the distinction explicit); `lock` = code 1
(`--persistent` → 3); `unlock` = code 0. PREVENT/ALLOW is reinstated because
a real caller now exists: an orchestrating consumer needs to lock idle drives
so a stray eject can't disturb an unattended operation — and persistent
prevent is what makes the button *cooperative* instead of merely dead. The
orchestrator owns the bit lifecycle; mos exposes the primitive. **Open and
gating:** `SetTrayState` shares GetTrayState's structural blindness
(ARCHITECTURE §9.9) and `SetMediaAccessPermission`'s availability is
SDK-version-bounded, so the verbs tilt toward raw CDBs via the §4 transport;
whether the prevent bit survives handle close, and the persistent-prevent
behavior matrix per drive, are the hardware gates in `ARCHITECTURE.md`.
Contract is mechanism-facts-only: mos reports what the command did and the
gating level it observed; the consumer derives whether it is a safety
interlock (it is not). Note the deliberate contrast with `drutil tray eject`,
which is documented unmount-then-eject *policy* — mos does not unmount for
you (doc/research/2026-06-10-drutil-contract.md).

**Remove `mos_raw_cdb()`** — once the typed APIs cover the diagnostic cases the
raw passthrough is legacy; removal is the major-version justification. (The
`goto cleanup` consolidation this item once demanded already landed — the
eighth review pass collapsed the setup-failure releases onto a single
`setup_failed:` label; the function is down to three `ReleaseExclusiveAccess`
call sites. What remains here is only the removal decision itself.)

**"What mos is not for"** → a new AGENTS.md ADR. mos and its typed APIs are for
spec-defined MMC only. Out: firmware flashing, AACS/LibreDrive microcode, raw
descrambled sector reads, BD+ key extraction, and vendor write-quality features
that superficially resemble state — PureRead (Pioneer), AudioMaster (Yamaha),
GigaRec/AutoStrategy (Plextor). Those belong to higher-layer media libraries; mos's
acquire-on-call / release-on-return lock discipline is built to coexist with
them, not host them.

**Deferred signal-source work** (designed, parked pending the hardware capture):
three-tier backoff cadence, a GESN-first probe with cached state, and the full
signal-source hierarchy (Disk-Arbitration wake → GESN → TEST UNIT READY). The
`--watch` DA wake source shipped; these refine its cadence and add layered
fallbacks.

**Open empirical questions** (need the rig): does
`kIOMessageServicePropertyChange` self-trigger a probe loop on real hardware;
does the prevent bit survive `SCSITaskUserClient` close; does brief exclusive
access contend with another application holding the drive; does GET EVENT STATUS
NOTIFICATION report tray/media events reliably across drive families.

**Deferred from the 2026-06-11 review intake** (three external passes;
fixes landed the same day, these are the parked remainders):

- **EPIPE-path CLI tests.** `mos_cli_stdout_finalize`'s errno-freshness
  argument (cli/io.c) is reasoned, not tested: force EPIPE and
  non-EPIPE stdout failures on the one-shot and watch paths and pin
  the EX_IOERR vs pipe-closed exit split. Test-harness work, no
  behavior change implied.
- **All-watch directory-rescan fallback.** `mos_watch_open_all` now
  fails honestly when the DR doorbell can't be set up (arrival
  discovery has no poll floor — see the open's comment). If hardware
  sessions ever observe `DRNotificationCenterCreate` or its run-loop
  source actually failing in practice, a slow-cadence
  `mos_internal_dr_copy_snapshot` reconciliation pass in the all-pump
  would restore soft-fail; build it on that evidence, not before.

---

## Later — v1.0 — production-ready, embeddable

Three readiness criteria from external review: multi-drive fixtures, decision-tree
integration tests, documentation polish. `--watch` (one of the original three)
already shipped.

**Multi-drive fixtures.** The MT1959HWDN unification means one drive per silicon
family covers many SKUs. Honest budget for full architectural coverage is
€700–900 across ~eight rows (see canonical drives below). Acquisition order:
Apple SuperDrive first (discontinued Aug 2024 → secondary-market only, pricing
curve is real), Lite-On iHAS124 W second (cheap, validates the Renesas/NEC
dispatcher path), Pioneer third (firmware-date discipline applies), Plextor
PlexWriter Premium 2 fourth (only acquirable Sanyo silicon), rest
opportunistically.

**Decision-tree integration tests.** Stub the MMC layer with a fake
`mos_handle_t` returning canned sense; exercise `mos_query_state` on a Linux CI
runner without hardware — GESN open/closed authority, sense-fork when GESN is
silent, the new fault/formatting/empty-or-open states, descriptor-format
sense, tray-open mid-read, disc swap mid-query. Largely implemented in
`tests/test_state_core.c` as of v0.3.1-dev (closes the reviewer's B+→A gap at
the function level).

**Documentation polish.** A fresh root `CHANGELOG.md` (Keep-a-Changelog)
generated from git tags at tag time — the review-era log is frozen at
doc/history/CHANGELOG.md; `mos_open_by_index` race documented as a known limitation and
`mos_open_by_bsd` marked preferred; the lock-composability property made explicit
in `ARCHITECTURE.md`.

---

## Canonical reference drives (fixture acquisition)

By silicon family — one drive per family covers many SKUs; multiples of one
family are wasted spend.

- **MediaTek MT1959HWDN (owned).** The reference rig speaks for the whole late-LG
  family — BH14/16NS\*, WH16NS\*, BU/BP slim & USB variants, and the
  ASUS/Buffalo/Dell/HP/Verbatim/Vinpower rebadges.
- **Renesas R8J32xxx (Pioneer).** BDR-XD08 / BDR-212. *Firmware-date discipline:*
  the December-2022 cutoff flips identical silicon between two MMC dispatcher
  behaviors (bus-encryption 0x1B→0x13, mandatory AACS handshake, READ DISC
  STRUCTURE starts refusing) — ask the seller for firmware version before buying.
- **Apple bridge + 0xEA gate.** A1379 SuperDrive — late units are `HL-DT-ST
  DVDRW GX50N` inside (HLDS mechanism; the Mac-only enforcement and 0xEA wake live
  in the soldered USB bridge firmware). Discontinued 2024; buy in the
  cheap-and-plentiful window. Genuine Panasonic UJ-8A8 is a separate row.
- **Renesas R8J32091NP (Lite-On outlier).** iHAS124 W — a second NEC-lineage
  dispatcher at €15–30.
- **Rest, for full coverage:** Panasonic MN103S (UJ-8A8 / UJ-265), Sanyo
  (PlexWriter Premium 2 / PX-716SA / PX-760SA), MediaTek MT18xx/MT19xx (Lite-On
  iHAS124 B/F, iHBS212), NEC/Renesas (AD-7260S / AD-7280S), MediaTek MT1887 USB
  (Samsung SE-208), ASUS Pioneer-OEM slim (SBW-06D2X-U), Yamaha CRW-F1 (exotic).

**Skip:** generic Chinese USB drives (unpredictable bridge chips → bad fixture
data); IDE-only drives (connection cost > fixture value); post-2017 "Optiarc
AD-5290S" (Vinpower-firmware Lite-On, not Sony silicon, despite the vendor string
— the real Sony Optiarc range stopped at AD-5280S-CB).

---

## Out of scope

AACS handshake / BD+ / key extraction (MakeMKV territory); firmware flashing;
M-Disc capability-bit detection (feature 0x0028 — speculative v1.x only); BD-R
DL/TL/QL profile differentiation (all 0x001E today, fine for state); BD-RE
rewritable state-machine variants (v1.x if anyone asks); quiet/acoustic mode
(unimplementable on HLDS via standard MMC).

---

## Dependency graph

```
spec-conformance (the bar) ─► pure suite + fuzz green ─► decision layer correct
          └─► adapter smoke run on any Mac+drive ─► tag shipped (not a design gate)

DR substrate (unblocked) ─► enumeration + free identity + coarse status
          └─► subsumes walk-up resolution

v0.3 ─► contract + --watch + schemas + hardening + F1 media_changed shipped ─► v1.0 candidate
          └─► event contract freezes at first tag (mos.event.v1)

DR substrate ─► next arc (own unit, marshal-to-pure-struct boundary)

v0.4 ─► typed APIs shipped ─► tray verbs ─► raw_cdb removed ─► API stable

v1.0 ─► multi-drive fixtures ─► integration tests ─► CHANGELOG ─► ship
```

Largest risk: mistaking a drive capture for the correctness bar. mos is correct
iff it is spec-accurate, and that is verified drive-independently (pure suite +
fuzz). The only thing a Mac+drive establishes is that the Apple-framework adapter
is plumbed correctly — a one-time smoke test, not a design gate. Don't hold the
spec-driven work (DR substrate, typed APIs) hostage to it.
