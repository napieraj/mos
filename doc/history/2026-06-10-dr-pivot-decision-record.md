# DR-pivot decision record + v0.3-line completion (frozen)

RETIRED to history 2026-06-11. This is the "Now — v0.3 line" and
"Architectural — DiscRecording substrate" content that lived in
ROADMAP.md until its forward-only sweep: shipped lists, the preserved
pre-pivot plan, the dies/survives checklist, the named-input walk-up
dissolution, the GESN single-poll deferral, and the probe
consolidation argument. Frozen as written; the open remainders it
staged (held-handle refresh, media-info stages, division-of-labour
doctrine, schema-freeze status) live on in ROADMAP.md v0.4. Do not
update.

---

## Now — v0.3 line

Shipped: frozen CLI/JSON contract (`ok`/`error`, `revision`), event-driven
`--watch`, the `schemas/` family (`state`/`error`/`list`/`event` v1), a long
correctness/hardening arc, and F1 — the `media_changed` event (see doc/history/CHANGELOG.md);
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

### Probe consolidation (2026-06-11) — supersedes Phase 0's "tool inventory stays at two"

The Phase-0 disposition (implementation plan §retirements: "tool
inventory stays at two with a clean split: mos_probe = LIBRARY-path
smoke, mos_notification_probe = substrate observer") is superseded.
What changed: standalone opt-in binaries with their own arg parsing
and zero contract-test coverage were the project's one remaining
drift surface — CI compiled them but nothing ran them, and their
interfaces sat outside the CLI's validation matrix. Both tools are
now the `mos probe` subcommand (`cli/probe.c`, compiled in under
`MOS_CLI_PROBE`, default ON; retiring it is one default flip and the
OFF build is kept green by a dedicated CI leg): `mos probe <drive>`
is the notification event stream (NDJSON, `mos.probe.v0` — renamed
from `mos.notification_probe.v0`), `mos probe --dump` the one-shot DR
dictionary capture (renamed from `--dr-dump`). The smoke tool is
retired outright, not relocated — post-pivot, `mos list` + `mos
status` exercise the identical public-API path, so the duplication
was itself drift surface.

Two trades, recorded: (1) the observer no longer builds when the
library is broken — it lives inside the binary that links `mos_core`;
the observation *code path* stays raw (events come straight from
IOKit/DiscRecording callbacks, no mos abstraction). (2) The probe's
DiskArbitration legs are retired with it, and DA leaves the project
entirely — the legs survived the pivot only as the falsification
control arm, but no design decision depends on doorbell completeness
(doorbells are latency-only over the poll floor; the kernel itself
polls media at 1000 ms), so the control arm had no consumer. The
AGENTS.md scope addendum's "keeps DA legs" clause is rebutted by a
dated append there. `mos_internal_bsd_unit_matches` consequently has
no in-tree consumers (kept, with tests, as the pinned matching rule —
candidate for removal if no DA-shaped filtering returns).

**Media info (drutil-parity + volume name).** *(Progress 2026-06-10:
the ReadDiscInformation half shipped as the typed C API —
`mos_query_disc_info()` + accessors, built to the committed fixtures,
on-demand only, never the state path. The metadata/drive JSON
documents and volume-name work below remain open.)* Staged per
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
  Undocumented; falsifier in INTEGRATION_HARNESS.md).

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
