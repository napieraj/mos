# ROADMAP

Forward-looking only. What shipped, when, and the decision back-and-forth that
got us here live in `CHANGELOG.md`; design rationale lives in `ARCHITECTURE.md`;
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

## Now — v0.3 line

Shipped: frozen CLI/JSON contract (`ok`/`error`, `revision`), event-driven
`--watch`, the `schemas/` family (`state`/`error`/`list`/`event` v1), a long
correctness/hardening arc, and F1 — the `media_changed` event (see CHANGELOG);
pure suite green in CI. The typed APIs that originally defined v0.3 were never
built and have moved to v0.4. With F1 landed, the v0.3 event contract is
considered complete — but not frozen: per the schema-evolution ADR in
AGENTS.md (revision 2026-06-10, "when the freeze begins"), `mos.event.v1`
remains mutable in place until the first tag that ships it (zero external
consumers; the CI validation suite is the consumer until a real one
exists). The freeze, and with it the any-new-kind-is-`mos.event.v2` rule,
takes effect at that tag. (This paragraph previously declared v1 frozen
outright — corrected 2026-06-10 to match the ADR, which was already
exercised the same day by the in-place `registry_id` reshape.)

*The DiscRecording (`DRCore*`) substrate is the next arc — see "Architectural"
below. It is spec/SDK-driven and not hardware-gated, but it is a large adapter
rewrite with no local verification, so it lands as its own unit behind the
marshal-to-pure-struct boundary rather than piecemeal.*

*A GESN single-poll alternative was considered and deferred: the DiscRecording
substrate already exposes the coarse tray/media signals, so mos's value is the
state machine that interprets them fully, not a GESN source. See
`doc/research/2026-05-29-gesn-single-poll.md`.*

**Named-input resolution → walk up.** *Resolved 2026-06-10: the DR
pivot landed and `DRDeviceCopyDeviceForBSDName` dissolved this — the
walk-up was never built and now never will be. Preserved below as the
record of the pre-pivot reasoning.* Resolve `--bsd diskN` by
`IOBSDNameMatching` → the `IOMedia` node → up to its BlockStorageDevice; this
matches the canonical Mac tools and normalizes a `diskNsM` slice to its whole
disk for free. *Pre-pivot code did the opposite* — it enumerated the drive
classes and walked down to match the unit; walk-up was the planned change,
never validated, then dissolved. Discovery/enumeration stays (an empty drive
has no media node, so no name); per-poll reopen stays on
`IORegistryEntryIDMatching` (reassignment-safe).

---

## Architectural — the DiscRecording substrate (LANDED 2026-06-10,
## Phases 0–2b; hardware falsification pending)

*Status update: Phases 0–2b of
`doc/research/2026-06-10-dr-pivot-implementation-plan.md` shipped —
probe modes folded into mos_notification_probe (--dr-dump + DR event
legs), enumeration/identity/addressing on the DR snapshot, the DR
doorbell replacing DiskArbitration (which left the library link line
entirely), watch-static identity retiring the per-probe rehome, and
watch-all (`mos watch --all`, `device_appeared`, per-drive removal).
One deviation from the dies-list below: the IOMedia child walk
SURVIVES at open — it is the only source of the media_id (F1) swap
fingerprint, which DR has no key for. The text below is the pre-pivot
plan, preserved as the decision record.*

**Media info (drutil-parity + volume name).** Staged per
`doc/research/2026-06-10-media-info-design.md`: stage 0 (media_class
from the already-fetched profile + the ISO9660 PVD parser) is SHIPPED;
stage 1 wires `ReadDiscInformation` (decoder already exists) + the DA
mounted-volume name + the PVD pread fallback into a nullable `media`
object — all convenience methods and block-device reads, zero raw CDBs,
zero lock interaction, dual-length rule (O-4) on every reply; stage 2
(UDF names, CD-TEXT, capacity blocks) deferred with named falsifiers.
Identity rides stage 1 as TWO subject-pure documents: `mos metadata
--json` → `mos.metadata.v1` (disc only — the TOC is a required-nullable
field of `disc`, the fingerprint subtree consumers hash) and `mos drive
--json` → `mos.drive.v1` (static drive facts: identity, firmware,
INQUIRY serial — the durable drive key, since registry_id is
attachment-scoped — and the spec-grounded AACS/bus-encryption
capabilities). Drive↔disc join is consumer-side; the standalone
mos.toc.v1 plan and the metadata `drive` block were both withdrawn
pre-ship. TOC parser already shipped fail-closed with fuzz
surface 8. Named third-party ids (MusicBrainz/AccurateRip/dvdid/BDMV
hashes) are permanently consumer-side; LibreDrive status is a MakeMKV
property mos cannot see — mos surfaces the spec-grounded
drive.capabilities (AACS feature/version, bus encryption) instead.

Spec/SDK-driven, not hardware-gated — this can land now. Replace the IOKit
class-walk enumeration with DiscRecording: `DRCopyDeviceArray` →
`DRDeviceCopyInfo`. Consequences:

- vendor / product / revision become free at enumerate time
  (`DRDeviceCopyInfo` keys), no open and no MMC round-trip — `mos list`
  shows full drive identity at ZERO commands, and `mos.drive.v1`
  shrinks to the two fields DR does not cache (INQUIRY serial;
  AACS/bus-encryption capabilities). DRDevice's historical non-burner
  blindness is moot: the §9.1 attach rule already blocks
  SCSITaskUserClient on read-only drives, so DR's enumeration set
  approximately equals the openable set — and pure readers are
  unobtainable in 2026 anyway. Burner-only IS the reachable universe.
- `--index` matches drutil order by construction (drutil *is* the DiscRecording
  CLI), so the index contract stops being ours to define.
- Identity stays robust: `kDRDeviceIORegistryEntryPathKey` resolves to a
  registry entry; per-poll reopen keeps using `IORegistryEntryIDMatching`
  (reassignment-safe), unchanged.
- **Subsumes the v0.3 walk-up item** — `DRDeviceCopyDeviceForBSDName` dissolves
  the `--bsd diskN` resolution question entirely.

**Dies / survives (the pivot's deletion checklist).** DIES: the
class-walk enumerator (`mos_enumerate_devices` two-phase collect/sort);
the IOMedia child walk (`mos_scsi.c:52` unit resolution with its
normal/degraded cases); `mos_open_by_bsd_name`'s walk-up; and the
selection-time TOCTOU class — name/index re-resolution that could
MISDIRECT to a different device after hotplug. DR device refs go stale
on detach (fails safe, disappeared notification) but cannot silently
become another device, so the dangerous TOCTOU class is structurally
gone. SURVIVES, deliberately: the watch's per-poll reopen via
`IORegistryEntryIDMatching` (the raw GESN still needs a real
io_service_t; the registry ID remains the reassignment-safe attachment
pin and the F1 swap-fingerprint substrate — poll-time defense, distinct
from selection-time); the pure bsd-name normalizers in reduced form
(`--bsd rdisk4` / `/dev/` forms are user input needing normalization
before `DRDeviceCopyDeviceForBSDName`); the entire open/probe seam
(convenience ops + raw GESN) unchanged. Net: v0.3's enumeration and
name-resolution adapter code is a museum piece — do not smoke-test it,
do not fix bugs in it; the durable adapter surface is open/probe +
watch loop + new DR glue.
- **Fixes held-handle identity staleness structurally** (third review,
  finding 2): v0.3 captures `bsd_unit`/`media_id` once at open, so a handle
  held across an insert reports READY with the open-time -1 (documented as
  open-time semantics in mos.h). Under DR the framework tracks media facts
  itself — `kDRDeviceMediaBSDNameKey` in the `DRDeviceCopyStatus` media-info
  dict, the same dictionary the status-changed notification delivers — so
  per-query freshness becomes a dictionary lookup, not a registry walk mos
  writes and later deletes (`doc/dr-field-mapping.md`, bsd_unit row). Also
  the `--index` alignment above stays drutil-tier per the evidence rating in
  `doc/research/2026-06-10-drutil-contract.md` (Documented / Inferred /
  Undocumented; falsifier in STATUS).

Division of labour — what mos *is* on top of DR. DR's status dict already exposes
the coarse signals (tray-open, busy, media present / in-transition / none) as a
passive, GESN-fed snapshot that is "not guaranteed current." mos does **not**
collapse its state engine into that dict; it owns (a) the synchronous,
fully-checked, corroborating state machine the snapshot lacks, and (b) the deep
rip-relevant metadata DR omits — media manufacturer / Media ID (ATIP/ADIP/PIC),
BCA (UHD/LibreDrive), layer & physical structure (READ DISC STRUCTURE),
AACS/region. DR enumerates and hands over cheap coarse status; mos interprets and
enriches. The MMC state engine must not become a DR passthrough. (The same
adapter smoke test in the gate covers DR's actual runtime behavior — any Mac+drive.)

---

## Next — v0.4 — typed APIs, tray verbs, drop `raw_cdb`

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

**Documentation polish.** `CHANGELOG.md` regenerated cleanly from git tags
(Keep-a-Changelog); `mos_open_by_index` race documented as a known limitation and
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
          └─► event contract frozen (mos.event.v1)

DR substrate ─► next arc (own unit, marshal-to-pure-struct boundary)

v0.4 ─► typed APIs shipped ─► tray verbs ─► raw_cdb removed ─► API stable

v1.0 ─► multi-drive fixtures ─► integration tests ─► CHANGELOG ─► ship
```

Largest risk: mistaking a drive capture for the correctness bar. mos is correct
iff it is spec-accurate, and that is verified drive-independently (pure suite +
fuzz). The only thing a Mac+drive establishes is that the Apple-framework adapter
is plumbed correctly — a one-time smoke test, not a design gate. Don't hold the
spec-driven work (DR substrate, typed APIs) hostage to it.
