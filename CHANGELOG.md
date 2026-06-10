# CHANGELOG

Historical record for mos, relocated out of ROADMAP.md (which is now
forward-only). This is a raw archive preserved for later parsing into a clean
Keep-a-Changelog format generated from git tags at v1.0 — dates and wording are
as they were written, not curated. Authoritative history is git; this is the
human-readable narrative. Live test counts are in CI, not here.

---

## 2026-06-10 — GitHub bring-up, the DiscRecording pivot (Phases 0–3), watch-all, mos_query_disc_info; version to 0.4.0-dev

The audited tree moved to `github.com/napieraj/mos` and CI ran for the
first time ever — four runs, four latent self-consistency bugs, all
fixed the same hour: a CLI test fixture (`diskdoesnotexist`) that
deterministically exercised the malformed-name path instead of the
no-device path it claimed; `setup-python '3.x'` resolving to 3.14 where
the hash-pinned rpds-py cannot build (interpreter now pinned); the
dist/ contradiction — CONTRIBUTING + CI demand a committed amalgamation
byte-identical to regeneration while .gitignore excluded it AND
amalgamate.sh stamped a timestamp making byte-identity impossible
(manifest now deterministic, dist committed); and the amalgamation
job's source list missing cli/human.c + tests/test_human.c.

A post-import review added the machine checks the repo's conventions
implied but didn't enforce: schemas/validate.py drift pins for error
codes, event kinds, and media classes against their C emit tables; a
single reserved-subcommand array; a shared profile-sentinel predicate.
Two of the review's own findings were falsified on implementation and
recorded as such (the mos_query_state deref-order "bug" was misread —
the guard exists; the -Wswitch claim was inverted — the CLI switches
were the pinned ones).

**The DiscRecording pivot** (research + plan in doc/research/
2026-06-10-dr-pivot-*.md, headers vendored from the 15.5 SDK, kernel
source vendored at IOSCSIArchitectureModelFamily-139.0.2 — the newest
Apple ever published, Tiger-era). Doctrine: DR is the doorbell and the
directory; MMC is the inspector. The kernel GetTrayState collapse
(every GESN failure → closed + kIOReturnSuccess, undetectable from
userspace) was re-derived from the vendored source mid-design and
killed the idea of trusting DR for state: the TUR⊕GESN core is
untouched and remains the sole state authority.

- Phase 0: DR observation modes folded INTO mos_notification_probe
  (--dr-dump plist capture + Appeared/Disappeared/StatusChanged event
  legs) — two probes, not three; mos_probe re-scoped as the
  library-path smoke.
- Phase 1: enumeration in DR device-array order (drutil parity by
  provenance; the registry-ID sort approximation retired), identity
  from DRDeviceCopyInfo (open-time INQUIRY retired with its fixed-
  width copier — replaced by width-pinned copies at the DR seam),
  --bsd via DRDeviceCopyDeviceForBSDName behind the unchanged
  parse_bsd_unit gate, public mos_open_device() for the one-snapshot
  pattern. The CLI's O(n²) re-enumeration, single-drive double-probe,
  and expensive resolve_index_of all died here. The IOMedia walk
  SURVIVES at open: it is the only source of the media_id (F1)
  fingerprint, which DR has no key for.
- Phase 2a: kDRDeviceStatusChangedNotification (device-scoped,
  registry-ID-filtered, fail-open) replaced the DA description-changed
  wake; DiskArbitration left the library link line entirely (the
  notification probe keeps DA legs as the falsification control arm).
  Identity became watch-static — captured once at open — retiring the
  per-probe re-home, its pure helper, and its ASan test; the lifetime
  CONTRACT is unchanged and still pinned by test_watch_lifetime.
- Phase 2b: watch-all. Pure multiplexer over up to 16 per-device
  cores (ascending-registry_id interleave, stream-global seq,
  per-slot non-terminal device_removed), five fixture tests, new
  device_appeared event kind added to mos.event.v1 IN PLACE (pre-tag,
  per the schema ADR — the ROADMAP line claiming v1 frozen was
  corrected against the ADR it contradicted), `mos watch --all`,
  Appeared/Disappeared observers load-bearing in all mode.
- Phase 3: live docs aligned (ARCHITECTURE §4/§5/§6/§8/§9/§11, ROADMAP
  status, AGENTS scope addendum — the pivot does not change the
  command surface, still exactly one raw CDB — dr-field-mapping
  outcome, INTEGRATION_HARNESS falsification rows) and three new
  staleness deny markers pin the retired claims.

A five-angle systematic review of the pivot (finders fed the vendored
DR headers as ground truth) returned three angles clean (API
conformance, multiplexer seam, CLI/schema); the rest produced four
fixes (a comment that misdescribed the bounded-copy truncation
behavior, one shared identity extractor, width-agreement pins, the
all-mode removal-latency contract sentence + a positional/--all test)
and four refutations, with three efficiency items deferred on recorded
rationale.

**v0.4 opens**: mos_query_disc_info() — the typed READ DISC
INFORMATION accessor README promised, built to the committed fixture
pair through the non-exclusive convenience method, never on the state
path. mos_disc_status is public and ABI-pinned; accessors are
NULL-tolerant; the out-of-band failure convention matches
get_current_profile. Version is 0.4.0-dev everywhere
(MOS_VERSION_STRING, CMake project VERSION); the first tag still
gates on the hardware session (INTEGRATION_HARNESS DR falsification
rows + the ROADMAP adapter smoke items).

---

## 2026-06-10 — Two-review convergence: kernel-source verification, watch hardening, session-identity normalization

Two independent adversarial reviews of the v0.3.1-dev tree converged on one
real pure-layer bug and a set of seam questions; everything verifiable was
then pinned to primary sources fetched from apple-oss-distributions rather
than taken on either review's word.

**Watch core.** Fixed F1: the no-change pump branch never refreshed the media
fingerprint, so a transient `media_id == 0` snapshot permanently disarmed
swap detection (adopt-any-nonzero fix; zero never overwrites known identity;
three regression tests proven to bite in both failure directions). Added
error backoff: consecutive identical probe errors double the retry interval
from transition rate to the stable cap — a held drive now emits a bounded
error stream instead of ~18k lines/hour — with reset on success or a
different error, and notify_wake still pulling forward. Adapter: the pump's
run-loop wait now requires `CFRunLoopGetCurrent() == w->run_loop` (documented
cross-thread misuse degrades to nanosleep instead of a busy-spin), and the
session-open wall timestamp is monotonicized per process.

**Session identity normalized.** The composite `stream_id` string
("diskN-<unix_ms>") is gone through three design rounds that each removed a
layer: the BSD prefix was the wrong identity (may not exist at open — the
empty-drive watch is the primary use case — and goes stale mid-stream); the
registry-ID composite's `rid` prefix defended against a confusion xnu makes
impossible (real entry IDs ≥ 2^32+256, the whole 32-bit space reserved); and
the composite itself was denormalization — JSON carries structure.
`mos.event.v1` now emits `registry_id` + `stream_open_ms` as two plain
integers; session identity is the pair; consumers wanting one key
concatenate. Registry ID is **attachment** identity: xnu mints it per attach
from a never-reused monotone counter, so it spans media-less periods but a
replug is a new ID by construction — which is the reopen path's TOCTOU
defense, now documented as such. Side effect: the pure event lost its last
core-owned borrowed-pointer field.

**Kernel-source verification** (ARCHITECTURE §5.5 new, §9.7 upgraded, §11):
GetTrayState masking confirmed verbatim ("Assume the tray is shut" →
`*trayState = 0` + success on ANY GESN failure; Apple's own success path
reads byte 5 with no NEA/class/length checks — mos's decoder is strictly
stronger). The nub invariant proven at both ends: the kernel's
`PollForMedia` nub predicate (TUR-GOOD or sense 00/00) is the same predicate
as mos's GESN-skip gate, so the not-ready exclusive lock cannot collide with
a mounted volume; corollary documented — the kernel auto-ejects on every
not-ready sense outside {04/00, 04/01, 3A/xx, 57/00, 04/04} plus
MEDIUM/HARDWARE ERROR, making `media_unreadable`/`device_fault` (and the
04/02 / 04/07 `loading` flavors) transient in practice. TUR exclusivity
confirmed (`SCSITaskUserClient` defaults to `kIOReturnExclusiveAccess`); the
state-core comment corrected from "should be impossible" to load-bearing.

**drutil contract research** (`doc/research/2026-06-10-drutil-contract.md`):
`-drive` is a composable filter chain (decimal = 1-based index into the
CURRENT candidate list; bus keywords; vendor/product exact match; fan-out on
multi-match), its vocabulary maps 1:1 to `DRDeviceCopyInfo` keys, and drutil
cannot select by BSD name at all. Index tiering: index≡list-row Documented,
list≡DR-array Inferred, array determinism Undocumented (falsifier:
`drutil list -xml` vs `mos list`). Filter grammar evaluated and REJECTED for
mos (bus keywords are a 2004 contract on all-USB 2026 hardware; vendor match
fails identical-drive rigs; fan-out contradicts single-drive verbs) — v0.4
selectors are three separate flags: `--index`, `--bsd`, `--registry-id`.
ROADMAP tray verbs rebuilt on the three-level removal-gating model (unlocked
/ locked / persistent-prevent, where a button press becomes a GESN
EjectRequest event — code 1, cross-checked against Linux `sr.c`); the
kernel's own eject is force-shaped (unconditional unlock + LoEj), so
mos's polite/`--force` split is a distinction the platform doesn't offer.

**Tooling and policy.** `amalgamate.sh` hardened (feature-test macros
hoisted to the prologue; loud refusal if a stripped header grows interior
conditionals). New `scripts/doc-staleness.sh` deny-list gate + CI job — the
audit found the machine-checked layer fully consistent while prose lagged
the 2026-05-30 redesign in ~15 places; this is the missing forcing function.
`test_watch_lifetime.c` now compiled AND run by CI. dist-drift CI check.
Schemas-win evolution ADR rewritten (closed field sets; state enum is the
open axis) plus a pre-ship mutability revision; stale UA-retry and Homebrew
ADRs superseded; falsification doctrine ADR added (hardware never changes
behavior directly — surprises land as committed fixtures). Doc sweep across
ARCHITECTURE / CONTRIBUTING / INTEGRATION_HARNESS / README / STATUS;
hardware gate rescoped to falsification + fixture acquisition; exit 77
documented as reserved; nits (unused strlcpy prototype, dead `_WIN32`
branch, stale feature-macro comments, inquiry/profile return-convention
rationale). Late-pass CLI fixes from a follow-up review: the watch's
next_event timeout slice is now capped at 500 ms independently of the poll
cadence — it is the SIGINT-latency bound, and coupling it to
MOS_WATCH_TRANSITION_MS inverted its own purpose at large env values (an
up-to-one-hour Ctrl-C stall at the 3600000 cap); getenv_uint diagnoses
set-but-invalid env values on stderr instead of silently falling back; the
CLI's duplicate /dev/ pre-strip removed (the library parse is the tested
normalization authority for disk4 / rdisk4 / /dev/ forms); redundant second
sense memset in mos_raw_cdb removed; tracked docs consolidated under doc/
(docs/ is now solely the local vendored-header area preflight strips).

Third independent review (Linux-side, including a -Wshadow/-Wconversion/
-Wsign-conversion/-fanalyzer pass the tree survived): pure watch core now
adopts the probe's bsd_unit itself on every successful pump — the
error/device_removed fallback unit previously depended on an undocumented
Apple-adapter write into core state, so any second adapter shipped the bug;
regression test proven to bite against the pre-fix core, adapter poke
retired. Watch-mode JSON error envelopes render compact single-line — the
NDJSON contract was broken at the reachable open-failure path while
one-shot mode keeps the pretty envelope; CLI contract tests updated to the
compact form plus a new single-line framing assertion (the old assertions
pinned the pretty form and would have failed). Held-handle open-time
identity semantics documented on mos_state_result_bsd_unit and in the
mos.state.v1 description rather than patched — the DR pivot fixes it
structurally (kDRDeviceMediaBSDNameKey is framework-tracked status), now
recorded as the pivot's fourth justification. The reviewer-verified
-Wshadow/-Wconversion/-Wsign-conversion cleanliness is locked into the
strict-pure CI job. Nits: misdirected "above" comment in mos_scsi.c;
--help now states the env-var domain (1..3600000 ms) and the
warn-and-fallback behavior.

Fourth review (PoC-backed, with a 26-mutant campaign — 24 killed): the one
real defect was format_rfc3339 emitting schema-invalid ts for clocks at or
past the year-10000 boundary (5-digit year; "" at gmtime_r extremes) — the
hostile-input discipline now applies to the clock too: values saturate to
9999-12-31T23:59:59Z, unconditionally schema-shaped, with three boundary
tests and the reviewer's contract PoC passing (their 8-property hostile
suite is 8/8 under ASan). The two mutation survivors became regression
tests, each re-verified to kill its mutant here: the GET CONFIGURATION
alignment guard's span-fits case (the existing misaligned test was
false-confidence — short buffer meant the bounds check rejected the
descriptor independently) and latency saturation against a backward
monotonic clock. Fuzz harness header corrected (five surfaces not four;
documented build one-liner now links — it omitted mos_config.c and
mos_discinfo.c; MOS_FUZZ_DISCINFO added to the CI smoke env, which was
silently running that phase at its full 500k default). Dead identifiers
swept (matches_self_or_partition → bsd_unit_matches; last stream_id
comment) and both retired names added to the staleness deny-list — the
gate now catches renames it previously could not. docs/apple/ citations
marked dev-tree-only with the §11 public mirrors as the artifact-consumer
evidence path; dr-field-mapping's line-number citations replaced with the
header's own section banners. Example script's DVD case gains 0x0017
(dvd_minus_rw_dl).

Seam-fidelity audit (commissioned; fourth reviewer): the flagship
exhaustive checker REFUTED the §5.5 nub-invariant equivalence on exactly
11 of 268,435,456 inputs — PollForMedia sets mediaFound on CC + ASC/ASCQ
00/00 independent of the sense key (the old §5.5 described the
intermediate flag, missing the shouldEjectMedia reset that IS the nub
decision), while mos's gate keyed on the full all-zero triple, so
CC+00/00 with a non-zero, non-ejected key (RECOVERED ERROR, ILLEGAL
REQUEST, UNIT ATTENTION, DATA PROTECT, reserved) took the exclusive lock
against a live nub. Kernel claims verified against the Apple source line
by line here before acting. Fixed by mirroring the kernel's FULL
predicate: CC+00/00 now routes to UNKNOWN without the lock for every
nub-preserving key and still probes for the four ejected keys
{0x2,0x3,0x4,0x8} — preserving device_fault/media_unreadable
classification, which the reviewer's own warning said the blunt fix
would lose. Checker vendored (tests/audit/, predicate updated to the
fixed gate, self-audit clean) and added to CI; exhaustive run exits 0;
two new tests pin both gate arms, the no-lock one proven to bite. §5.5
rewritten around flag-vs-decision. Remaining audit items closed
headlessly: error-path out-params are now contractually UNDEFINED and
the fakes POISON them (E-1 suite-enforced; resolves the adapter's
TUR-vs-tray/profile inconsistency without touching IOKit code);
the backward-clock flip is self-guarding (a no-op flip fails the
latency test — fixing a false-confidence pattern in a test added
earlier the same day); GESN class-mask width and descriptor-format
key-nibble mask pinned (the fixed-format mask was already pinned — a
small census correction). Seam contract and census vendored to doc/
with resolutions marked; the UNGUARDED residue {O-1, O-3, V-1} is now
step 0 of the hardware gate.

Forward defense for v0.4 RT=0 enrichment (the dual-length problem: a
hostile drive controlling both content and self-described length of a
variable-size reply): new pure primitive mos_internal_trusted_len pins
the rule — trusted parse region = min(allocated, transferred), device
claim clamps only downward, claim totals computed in uint64 so they
cannot wrap before the clamp. Four boundary/wrap tests, fuzz phase 6
(now six surfaces; properties: bounded by each authority, monotone in
the claim; MOS_FUZZ_TRUST knob + CI env), seam contract O-4 generalizing
O-2 from the fixed sense buffer to drive-sized replies, and both v0.4
TODO sites (GetConfiguration RT=0 buffer, ReadDiscInformation) annotated
to require the primitive. Landed before any consumer exists so the
enrichment work inherits the bound instead of inventing one. Also AGENTS
gains the three-layer scope doctrine (MMC-only kernel-authored command
surface with GESN as the lone justified raw CDB; no SPC ambition; full
adversarial input space).

Media info, stage 0 (design + everything headlessly shippable): the
identical-drives disambiguation problem gets its design doc
(doc/research/2026-06-10-media-info-design.md — field-by-field source
matrix; the entire drutil-parity MVP plus volume name needs ZERO raw
CDBs: ReadDiscInformation/ReadTrackInformation/ReadTableOfContents are
all kernel-authored convenience methods, the mounted volume name is
DiskArbitration which mos already links, and the unmounted fallback is
a block-device pread). Shipped now: media_class ("cd"/"dvd"/"bd"/
"hd_dvd") derived by new mos_profile_class from the profile the state
query already fetches — emitted in mos.state.v1 AND mos.event.v1 under
the same 0x0000-suppression as current_profile_name, closed-enum
schema'd, three fixtures migrated, two new negatives (bad class value;
class alongside the 0x0000 sentinel), totality test pairing the class
table to the name table so neither switch can grow without the other.
Also shipped: mos_internal_iso9660_volume_id (ECMA-119 PVD, layout
cross-checked against libcdio; hostile-input-hardened — short trusted
reads are "no answer", non-printables become '?', right-edge-only
padding trim) with three tests and fuzz phase 7 (now seven surfaces,
MOS_FUZZ_PVD knob + CI env), waiting for its stage-1 adapter caller.

TOC-as-disc-identity (the batch-rip dedup primitive):
mos_internal_toc_parse — MMC-6 format-0 READ TOC walk, dual-length rule
on the header Data Length, FAIL-CLOSED on duplicate/non-ascending/
reserved track numbers, tracks after lead-out, and partial trailing
descriptors (a half-parsed hostile TOC must not yield a falsely-stable
fingerprint). Two tests (realistic 3-track audio CD; six hostile
shapes incl. a lying Data Length clamped mid-descriptor) and fuzz
surface 8 (eight surfaces, MOS_FUZZ_TOC + CI env, success invariants:
<=99 strictly-ascending tracks). The mos.toc.v1 document shape is
decided in the design doc (schema file lands with the stage-1 `mos toc`
verb per schemas-win); its canonical serialization is itself the dedup
key — derivation runs consumer-ward, per the stream_id precedent.
LibreDrive note recorded: TOC reads are unprivileged on any firmware.

Identity design consolidated after the fingerprinting research review:
ONE document (mos.metadata.v1, behind a stage-1 `mos metadata` verb)
with the TOC as a required-nullable field of the `disc` fingerprint
subtree — the standalone mos.toc.v1 plan and the state.v1 `media`
object plan are both withdrawn pre-ship (each would have put disc facts
in two schemas; a split dedup key would also have forced consumers to
invent a canonical concatenation). state.v1 keeps media_class and
gains only volume_name in stage 1. Scope boundary written down: mos
emits bytes it read and hashes of bytes it read; every named
third-party identity (MusicBrainz, AccurateRip, dvdid, BDMV hashes) is
consumer-side forever. LibreDrive status determined to be a MakeMKV
property (drives report Enabled on unpatched firmware; enablement is
MakeMKV-database-gated), invisible to SCSI — mos surfaces the
spec-grounded drive.capabilities facts (AACS feature 0x010D presence/
version, bus-encryption capability) it actually can read.

Taxonomy revision after review: drive facts leave mos.metadata.v1
entirely — the governing rule is now explicit (a schema's fields belong
to its SUBJECT plus addressing context). mos.drive.v1 born (static
drive document: identity, firmware, INQUIRY unit serial as the durable
inventory key — registry_id being attachment-scoped — plus the
capabilities block), mos.metadata.v1 is pure disc, drive-disc join is
consumer-side. Net schema count unchanged (toc.v1 died, drive.v1 born).
State.v1 deliberately keeps capabilities OUT — static facts have no
place on the hot path. Post-DR-pivot cost table recorded: mos list
becomes a zero-command surface (DRDeviceCopyInfo kernel cache carries
vendor/product/revision/interconnect + write capabilities), drive.v1's
open is justified by exactly the two fields DR lacks (serial, AACS
capabilities), and the only mos-authored CDB anywhere remains the raw
GESN. DRDevice non-burner enumeration blindness determined MOOT rather than
falsifier-worthy: the §9.1 SCSITaskUserClient attach rule already
blocks read-only drives, so DR's enumeration boundary coincides with
the capability boundary mos lives inside — and pure readers are
unobtainable in 2026 regardless. Burner-only is the reachable universe. Pivot deletion checklist
recorded: the class-walk enumerator, IOMedia child walk, walk-up
resolution, and the selection-time (misdirection) TOCTOU class all die
with DR; the watch's registry-ID per-poll reopen, the pure bsd
normalizers (reduced), and the open/probe seam survive. Tag posture
agreed: v0.3 tags on spec-conformance as the pre-pivot baseline (after
the pure-layer mutation ritual); the never-executed enumeration glue
gets no hardware ceremony — the surviving open/probe path is exercised
organically when stage-1 verbs land on the Mac. The v0.4 signal stack
designed: DR status callbacks (drive level, payload-carrying), DA
(filesystem level), exclusivity state (client-contention busy, no
command), kernel cache (identity) — a four-tier probe ladder where
ready/busy resolve at zero commands, TUR covers
unmounted-present and device-internal busy, and the raw GESN fires only
on the empty-vs-open fork. mos's niche restated: honest synthesis on
the common path; the authoritative failure-honest tray bit — which no
platform layer offers current — on the rare one. Lock-hold frequency
drops to the disambiguation moment only. Niche extended with the
taxonomy point (DR media state is 3-valued; mos is 10-valued via sense
decode — formatting/loading/device_fault distinctions the platform
discards). volume_path (kDADiskDescriptionVolumePathKey) added to the
stage-1 design: state.v1 alongside volume_name, and metadata.v1 CONTEXT
(never the disc subtree — session fact, machine-local) — the field that
lets consumers compute dvdid/BDMV hashes themselves from metadata +
mount path, keeping filesystem hashing out of mos permanently. Mounted-media
command split recorded: convenience methods need no exclusivity, the
raw path is doubly unreachable under a mount (§5.5 gate + TUR-GOOD
short-circuit), so non-null volume_path certifies a lock-free capture;
rdiskN read permission flagged as a stage-1 verify for the
unmounted-fallback only — then the fallback itself was WITHDRAWN: the
/dev/rdiskN pread is a third I/O modality with a privilege footprint
(root:operator, TCC) mos refuses, and no in-scope replacement exists
(raw READ(10) needs the lock the nub forbids). unmounted discs
honestly report null volume fields. AGENTS scope doctrine gains layer
3: privilege footprint = the SCSITaskUserClient console grant and
nothing more. 
Correction: unmounted-present is the COMMON rip state (UDF BD/UHD
rarely auto-mounts, MakeMKV prefers it), not a fallback — and DR's
media-info dict was header-verified to carry NO volume/filesystem key
(physical facts only), so there is no in-scope path for mos to read a
volume name on an unmounted disc. Documented as a structural boundary:
mos does full SCSI-layer identity unmounted (class/TOC/disc-info/
capacity via convenience methods) and emits volume_name null, with the
public parsers (ISO now, UDF stage-2) as the consumer handoff for the
component already reading those sectors privileged.

---

## 2026-05-30 — State-detection redesign: TUR-presence / raw-GESN-tray / sense-enrich

Reworked the `mos_query_state()` decision core (`mos_state_core.c`) to the
converged division of labour. Convenience `TestUnitReady` runs first as the
presence-trusted primary, issued **once** — the UNIT ATTENTION drain loop is
gone (the kernel drains power-on/reset UA before mos holds a handle, unlike
the Linux first-toucher pattern; `MOS_TUR_MAX_ATTEMPTS` removed). A `GOOD`
status short-circuits to `READY` without ever taking the exclusive lock, so a
query never disturbs a mounted rip.

Only a not-ready TUR reaches the tray bit, and the tray bit is now a **raw GET
EVENT STATUS NOTIFICATION (0x4A)** under exclusive access via `mos_raw_cdb()`,
not the `GetTrayState` MMC convenience method — that wrapper hard-codes
closed+success on a GESN failure, masking it (ARCHITECTURE §3, §4.2, §9.7).
Taking exclusive access here is safe because not-ready ⇒ not mounted, so it
never hits the mounted-volume `kIOReturnBusy`. The GESN response is parsed by
a new pure, fuzz/ASan-checked decoder `mos_internal_gesn_media_door_open`
(NEA gate, Media-class check, device-reported full-span reject; door bit per
Linux `sr.c`).

GESN owns open/closed; the TUR sense only *enriches* the closed branch and
never flips GESN's verdict (the two invariants are pinned as tests). When GESN
gives no authoritative bit, the sense supplies the tray fork
(`3A/02`→open, `3A/01`→closed, generic `3A/00`→`empty_or_open`). The classifier
`mos_internal_state_from_sense` was renamed `…_from_sense_closed` and rewritten
to closed-branch semantics.

New `mos_state_enum` values appended (ABI-safe, accessor-only across the FFI):
`MOS_STATE_FORMATTING` (04/04), `MOS_STATE_MEDIA_UNREADABLE` (medium error /
57/00), `MOS_STATE_DEVICE_FAULT` (hardware error), `MOS_STATE_EMPTY_OR_OPEN`
(no medium, tray unobservable). Wired into `mos_strings.c` and
`schemas/mos.state.v1.json`. `get_tray_state` keeps its vtable signature; its
contract changed (OK = authoritative, error = sense-fork) and its Apple impl
is now the raw GESN.

Result model unchanged: negative `mos_error` return remains the "couldn't
reach the drive" axis; couldn't-lock-for-GESN collapses into the sense fork
(`empty_or_open` when ambiguous) with `MOS_OK`, since TUR already proved
reachability — no separate result enum added.

Tests: `test_state_core.c` scenarios rewritten for the inverted flow incl.
both GESN-authority invariants; `test_sense.c` rewritten for closed-branch
semantics + the new states + 8 GESN-decoder unit tests; `fuzz_pure.c` fuzzes
the GESN decoder. Pure layer green under `-Werror` (release + ASan), fuzz
ASan-clean. Apple-side GESN issuance is written but HW-gated (uncompilable
headless).

Surface-contract follow-up (external review): added the four new states to
`mos.event.v1.json` `state`/`prev_state` enums with positive fixtures
(formatting / media_unreadable / device_fault / empty_or_open); made
`watch_state_is_transitional()` exhaustive over every `mos_state_enum` value
(no `default`, so a future state is compile-loud under `-Wswitch`/`-Werror`) —
FORMATTING and EMPTY_OR_OPEN classed transitional, MEDIA_UNREADABLE and
DEVICE_FAULT stable — with a test pinning the poll class of all ten;
resurrected the dead `last_profile` field as a cross-class media_changed
fingerprint for bridges that expose no stable media_id (CD↔DVD↔BD swaps, with
the same-class-swap blind spot documented); refreshed CLI `--help` /
output-contract / plain-mode vocab and the public header state list; and
removed stale checked-in `build/` artifacts from the archive.

Code-review hardening (third external review):
- GET CONFIGURATION current-profile read no longer trusts a GOOD status
  alone: extraction moved to a pure, length-gated decoder
  (`mos_internal_config_current_profile`) that requires the reply's Feature
  Header Data Length to actually cover bytes 6-7, so a truncated GOOD response
  surfaces as "no profile" (MOS_ERR_IO, enrichment skips) rather than a silent
  0x0000 that reads like real "no media." Five unit tests + fuzz coverage; the
  Apple `get_current_profile` now routes through it. (The reviewer flagged
  this as the one finding that could yield a *wrong* observation rather than a
  degraded-but-honest one.)
- tools/mos.c: collapsed the redundant device_removed branch in
  emit_watch_ndjson (prev_state was written identically in both arms) to
  "always write prev_state; write state only when not removed"; guarded an
  empty-string argv[1] out of the subcommand branch.
- The review's UA-retry finding was already resolved (the drain loop was
  removed earlier; TUR is now a single shot).

Release-hygiene + remaining contract drift (second external review):
- README brought in line with the v0.3 reality — full ten-state output table
  with stable/transitional class annotations, the watch-poll table, the GESN
  row corrected to "raw CDB, not the GetTrayState wrapper," the no-exclusive-
  access claim narrowed to "temporary exclusive on the not-ready path only,"
  and `revision` added to the state-envelope field list.
- Schema-versioning policy decided and documented: the `state`/`prev_state`
  enum is an OPEN set within v1 — additive values are forward-compatible (not
  a schema-name break), and consumers must treat an unrecognized value as
  unclassifiable. Stated in README and in both schemas' enum `description`s.
- Drift guard: `schemas/validate.py` now extracts the state strings from
  `mos_state_description()` and fails if they don't match the `mos.state.v1`
  and `mos.event.v1` enums — the C↔schema divergence can't recur silently.
- `scripts/release-preflight.sh` rejects an archive (or tree) containing
  `build*/`, `CMakeCache.txt`, `CMakeFiles/`, `*.o`/`*.a`, or the vendored
  `docs/apple/` SDK headers; the release zip now excludes all build trees and
  `docs/apple/` (0BSD redistribution hygiene) and is gated through preflight.
  `dist/` is kept intentionally (the documented amalgamation artifact).



A full pass over every comment in src/, include/, and tools/ for necessity,
conciseness, and staleness, then a second full pass re-applying the matured
calibration from the start (the early leaf files had been done under a softer
early lens). Total comment lines ~1600 → ~1293, but the prose reduction is
larger than the count suggests — most of the work was rewriting
verbose-but-load-bearing blocks tight rather than deleting whole comments
(e.g. the 42-line adapter pointer-lifetime audit rule in mos_watch.c → 16,
mos.h's mos_raw_cdb doc 46 → 18). Calibration: explain the *why* only when
it is non-obvious to a reader who already knows C/IOKit/SCSI, and once —
deferring cross-file contracts to the header that owns them, and verifying
the owner actually carries the fact before pointing at it. Cut throughout:
migration/review archaeology ("Commit C/D", "prior to v0.3-dev", numbered
refactor histories), tombstones for moved/removed code, reproduced SDK
function signatures (visible at the call site), v0.4 research agendas (moved
to ROADMAP), questions the audience would never ask, and restatements of
`MOS_IO_AUTO` / obvious code. Kept (rewritten tight): spec byte-offset/
bit-layout tables, bounds/UB/wrap invariants, ownership/lifetime contracts,
the terminal-injection security rationale, and genuine "don't reintroduce
this" warnings.

Staleness corrected along the way: several watch-event-kind lists missing
`media_changed` (mos.h, mos.c, mos_watch_core.c); a `mos_internal_watch_pump`
doc naming a nonexistent `ops->now_ms` (renamed `mono_ms` in the two-clock
split); the mos_cli_io.c `json_str` comment describing a superseded
4096-byte-buffer impl; a `fill_event_state_fields` comment with the wrong
branch/field counts after F1; an orphaned `poll_ms_for_state` doc-comment; a
stale `lines 870-873` reference in an 833-line file; and (second pass) a
`mos_pure.h` struct-header pointer to per-field "load-bearing or scratch"
docs in mos_watch_core.c that do not exist there.

Two real bugs surfaced by reading the `if(APPLE)` tool files line-by-line
(these never compile on Linux — only the pure layer builds here — so they
had gone unnoticed and would fail the macOS `-Werror` build):

- `tools/mos.c`: `ev.kind` on a pointer → `mos_watch_event_kind(ev)`.
- `tools/mos_probe.c`: `r.sense_key`/`.asc`/`.ascq` on a
  `const mos_state_result *` → `mos_state_result_sense(r, ...)`.

Both are opaque-struct-refactor stragglers, fixed to use the accessors like
the rest of each file; syntax-checked against the public header, full proof
rides the Mac smoke test.

Also: moved a stray mid-file `#include <stdbool.h>` in mos_pure.h up to the
top include block.



The fifth and final additive `mos.event.v1` kind before the v1 freeze:
`media_changed`, emitted when the drive stays READY across two probes but the
disc was physically replaced. The fingerprint is the whole-disk IOMedia
registry entry ID (`IORegistryEntryGetRegistryEntryID`), **not** the profile
byte — a swap re-mints the id even for a same-profile DVD→DVD replacement, the
case a profile compare misses. `media_id == 0` is the no-media / unavailable
sentinel; the core compares it for equality only and never emits it on the wire.

- Pure core (`mos_watch_core.c`): the remembered snapshot grew from `last_state`
  to `last_state` + `last_media_id` + `last_profile`. The media_changed branch
  is evaluated after the state-change branch and gated on
  `state == READY && last_state == READY` with both ids present and differing,
  so a real state transition is always a `state_changed`, and an unknown id
  never fabricates a swap. Event payload is the new disc's, via the existing
  `fill_event_state_fields`.
- `mos_state_result` / `mos_state_env_t` gained an internal `media_id`; the
  state core propagates it exactly like `bsd_unit`.
- Schema: additive `media_changed` enum value + a dedicated `oneOf` branch
  (state and prev_state both `ready`, full media payload, no error). New positive
  example fixture. CLI emits the kind in both NDJSON and plain paths.
- Pure-tested headless: same-state swap emits media_changed; a same-disc poll in
  between emits nothing; a zero id never triggers; a state change with a new id
  stays state_changed.

Adapter capture (`mos_internal_bsd_unit` now also returns the chosen whole-disk
node's registry entry id into `h->media_id`) is written but **unverified** — it
is Apple-only and rides the one-time adapter smoke test like every other IOKit
path. Cost is free: the whole-disk child was already iterated there.



Before any stable-ABI claim: `mos_state_result` and `mos_watch_event` were
transparent output structs the caller stack-allocated and read by field, so
adding a field would change `sizeof` and let a newer library overrun an older
caller's storage. Both are now **opaque** — the public header carries only a
forward typedef plus `mos_<type>_<field>()` accessors; the full layout moved to
`src/mos_pure.h` (internal, still field-accessed by the core, the Apple fill
paths, and the pure tests). Fields now grow in place by appending to the hidden
struct, ABI-safe with no size/version/reserved negotiation — opacity is the
guarantee. This matches the already-opaque `mos_device_info_t`.

- `mos_query_state` / `mos_watch_next_event` now take `const mos_*_*  **out` and
  hand back a borrowed, handle/watch-owned pointer valid until the next call or
  close — the lifetime the borrowed strings already had, now covering the whole
  object. `*out` is NULL on non-success.
- New `src/mos_result.c`: pure accessors (NULL-object-tolerant), added to the
  amalgamation (they are public API, unlike the not-yet-issued parsers).
  `tests/test_result.c` pins them headless.
- `tools/mos.c` migrated to the accessors as the reference consumer.
- Pure suite green under release `-Werror` and ASan/UBSan. The two fill-path
  signature changes and the CLI migration compile macOS-side and ride the usual
  adapter smoke check.

**Landed since the 2026-05-15 entry** (pure-test suite is 116/116 as of
this date — a live count, not to be back-propagated into the dated
entries above, whose counts are accurate for their own dates):

- F2 — `revision` emitted by both renderers, in both `mos.state.v1`
  and `mos.event.v1`, with fixtures.
- F3 — one-shot stdout finalize unified with the watch path
  (`finalize_oneshot_stdout`), plus `finalize_failure_stdout` (preserves
  the failure code on `EPIPE`, `EX_IOERR` on a real write error).
- F5 — the DA-latency caveat scoped to the known-BSD-unit case only.
- F7 — the phantom `MOUNTED` state removed from ARCHITECTURE (mos's
  classification is mount-independent; mounted is still `READY`).
- `mos_cli_bsd_name` / `mos_bsd_name_format` now domain-reject a unit
  outside `[0, UINT32_MAX]` to NULL rather than truncating.
- errno discipline: NO clear on the read path (matches the header
  contract); `ferror` is authoritative; no `clearerr`.
- `fuzz_pure` wired into CMake (`MOS_BUILD_FUZZ`) and a CI job under
  ASan+UBSan.
- Schema fixtures: negative event fixtures corrected to top-level
  `bsd_name`; symmetric `error_with_revision`; `mos.list.v1` reduced to
  `bsd_name`-only (vendor/product removed).


**Docs.** A `doc/adr/` directory was briefly created to split these
decisions into ADRs, then reversed — ARCHITECTURE.md is again the single
source of truth and there are no standalone ADR files. Project-level
ADRs continue to live in AGENTS.md as before.

---

## ~2026-05-15 — v0.3.1-dev: notification-source extensions

### Extended interest-notification dispatches (PARTIALLY LANDED, REVISED 2026-05-15)

The watch's kIOGeneralInterest callback previously acted only on
kIOMessageServiceIsTerminated. Other message types were explicitly
ignored — comment in src/mos_watch.c said "not actionable for state
observation." That was wrong. The same callback also receives
several push notifications that MIGHT be actionable:

- **kIOMessageServiceWasClosed** — another consumer released the
  drive (the "read finished" signal we previously thought needed
  process-watching).
- **kIOMessageServiceIsAttemptingOpen** — another consumer is
  opening the drive.
- **kIOMessageServiceBusyStateChange** — service busy count
  changed (typically on open/close of any user-client).
- **kIOMessageServicePropertyChange** — registry properties
  changed. May reflect media-state transitions surfaced by the
  kernel's MMC stack.

Initial plan was to dispatch wake on all four. That plan was revised
after closer inspection of mos's own probe path: `watch_probe` calls
`mos_open_by_bsd_name`, which calls `IOCreatePlugInInterfaceForService`
with `kIOMMCDeviceUserClientTypeID` — that creates an
`IOMMCDeviceUserClient`, which IS exactly the user-client open/close
event that triggers IsAttemptingOpen, WasClosed, and BusyStateChange.

Three of the four dispatch cases would self-trigger a tight probe loop:

  pump probe → mos_open_by_bsd_name → kernel fires IsAttemptingOpen
    → watch_interest_callback → mos_internal_watch_notify_wake →
    pump returns to CFRunLoopRunInMode → mach message dispatch
    → immediate re-probe → loop

WasClosed and BusyStateChange would fire similarly on probe close.
The result would be max-rate probing — opposite of the intended cost
reduction.

**What actually LANDED (the safer subset):**

- kIOMessageServiceIsTerminated dispatch (unchanged — drive went away).
- kIOMessageServicePropertyChange dispatch (NEW — properties track
  drive-level state like profile / capacity, not client opens, so
  should not self-trigger; this is reasoned, not empirically verified).

**What was REVERTED:**

- kIOBusyInterest subscription. Removed. The only message it delivers
  is BusyStateChange, which we don't dispatch on (self-trigger). The
  empirical probe still subscribes to kIOBusyInterest for
  characterization purposes; the watch itself does not.

**Deferred to v0.4 pending empirical characterization:**

- IsAttemptingOpen / WasClosed / BusyStateChange dispatch. The
  notification probe (tools/mos_notification_probe.c) needs to
  characterize:
  1. Whether IOCreatePlugInInterfaceForService against an MMC drive
     reliably triggers these messages (highly likely but not 100%
     confirmed across all kext versions),
  2. What message_argument distinguishes our own task's open from
     another process's open, if anything,
  3. Whether a self-trigger suppression mechanism — counter-based
     ("expect N IsAttemptingOpens; ignore the next N") or timestamp-
     based ("ignore messages within X ms of our last open/close") —
     is feasible without missing genuine external events,
  4. Empirical rate of these messages during a long read by another
     process (does the noise scale with consumer activity?).

If the v0.4 plan ends up adding the dispatch with self-trigger
suppression, the implementation likely lives in the Apple adapter
(track open/close timestamps in `mos_watch_t`) rather than the pure
state machine (which has no concept of "our own task" — appropriate
for the pure / adapter split).

If the empirical probe reveals that IsAttemptingOpen / WasClosed
do NOT reliably fire for MMCDeviceInterface opens (e.g. because
the plug-in interface uses a different mechanism than IOServiceOpen),
then they ARE safe to dispatch on and v0.4 should land them without
suppression. The whole question is empirical.

### Empirical-probe diagnostic tool (LANDED)

`tools/mos_notification_probe.c` subscribes to every plausible
push-notification source for a given BSD-name drive and logs every
event as NDJSON (schema `mos.notification_probe.v0`). Subscribes to:

- kIOGeneralInterest on the resolved io_service_t
- kIOBusyInterest on the same service
- DARegisterDiskAppearedCallback (filtered to drive prefix)
- DARegisterDiskDisappearedCallback (same)
- DARegisterDiskDescriptionChangedCallback (same)

Each event carries monotonic timestamp (ms since probe start),
RFC 3339 wall-clock timestamp, source identifier, message type name
+ raw value, and BSD name. Runs until SIGINT. Output is line-flushed
for streaming into jq or archival.

Does NOT link against mos_core — uses raw IOKit / DA so observations
aren't filtered through mos's own abstractions. Useful for
characterizing what actually fires per drive class, independently
of mos's interpretation.

Build: `cmake -DMOS_BUILD_NOTIFICATION_PROBE=ON`. Output binary at
`./bin/mos_notification_probe`. Usage:
`./bin/mos_notification_probe disk4 > events.ndjson`.

---

## 2026-05-04 → 2026-05-14 — v0.3-dev: external review-pass landings

### v0.3-dev: external review pass landings

Date: 2026-05-14

Second external review of the v0.3-dev tree (after the first pass
that produced the schema family, sysexits exit codes, subcommand
dispatch, and hostile-input render tests) identified 10 findings.
All landed before the hardware integration matrix runs:

1. **Watch time-domain mixup (critical)** — Apple-side
   `watch_open_common` passed `wall_clock_ms()` (epoch ms, ~1.7T)
   as the scheduling `start_ms` while `now_ms` returned monotonic
   uptime ms (~thousands). The first `mos_internal_watch_pump`
   compared the two and returned `SLEEP_UNTIL` with a deadline
   ~55 years out — `mos --watch` would hang silently for the
   process lifetime. Pure unit tests missed it because the test
   fixture used small integer values for both clocks.

   Fix: split the `mos_watch_ops_t` vtable into `mono_ms`
   (scheduling) and `wall_ms` (timestamps, stream_id). The
   `mos_internal_watch_init` signature gained both `start_mono_ms`
   and `start_wall_ms`. Domain-mixup is now compile-loud, not
   silent. Test fixture rewritten to two clocks; regression test
   `test_clock_domains_separate` runs mono in the thousands and
   wall in the trillions to pin the contract.

2. **Watch `ts` wrong** — mechanical follow-on from (1):
   `format_rfc3339` was fed the same monotonic value used for
   scheduling, producing timestamps like `1970-01-01T00:00:12Z`.
   Fixed alongside (1): `fill_event_base` now reads `wall_ms`
   fresh each emit.

3. **`device_removed` gap** — two sub-issues:
   (a) `have_last_state` was overloaded as both "have observation"
   and "already emitted device_removed", so termination before any
   successful snapshot dropped the terminal event.
   (b) `MOS_ERR_NO_DEVICE` from probe was treated as a transient
   error; poll-only mode (no kIOGeneralInterest) would spin
   emitting `MOS_EVENT_ERROR` forever on unplug.
   Fix: dedicated `removed_event_emitted` sentinel; `NO_DEVICE`
   from probe routes through the terminal path with a
   `device_removed` event (`prev_state` = last observed, or
   unknown if none). Regression tests:
   `test_notify_removed_before_snapshot_still_emits_removed` and
   `test_probe_no_device_terminates_with_removed_event`.

4. **`_Static_assert` breaks C++ consumers** — `include/mos.h:88`
   used the C11 keyword inside an `extern "C"` block, which g++
   rejects. Fix: guard on `__cplusplus` and use C++11
   `static_assert` there. New CI job `cxx-consumer` compiles a
   one-line `#include "mos.h"` consumer under c++11/14/17/20 to
   prevent the regression.

5. **CLI ctest not registered on default configure** —
   `CMakeLists.txt:188` checked `MOS_BUILD_TESTS` before the
   `option(...)` declaration at `:209`, so a fresh configure
   silently dropped the CLI contract test. Fix: single block,
   option declared up-front, both unit and CLI tests under it.

6. **Plain `--watch` emits undocumented `error` token** —
   `tools/mos.c` emitted `error\n` to stdout on probe failure,
   contradicting the documented vocabulary
   `{open, empty, loading, ready, busy, unknown}` and breaking
   naive line-parsers. Fix: suppress in plain mode; emit a
   diagnostic to stderr instead. JSON mode unchanged (the
   typed envelope already carries `kind: "error"`).

7. **`current_profile_name: "no_current_profile"` for 0x0000** —
   the JSON emitter always asked `mos_profile_name()` for a name,
   but state-core only populates `current_profile` when state is
   READY; all other states had 0x0000 → "no_current_profile",
   diverging from the example fixtures. Fix: gate
   `current_profile_name` on `current_profile != 0x0000` in both
   `emit_json` (state mode) and the watch-event JSON path.

8. **`mos_internal_copy_bsd` return value discarded at open** —
   the open-by-service path silently produced handles with empty
   `bsd_name`, violating the public contract that `bsd_name`
   matches `^disk[0-9]+$`. The correct pattern (`!copy_bsd() ||
   empty`) was already applied at the bsd-name-lookup path; just
   wasn't applied at this site. Fix: return `MOS_ERR_NO_DEVICE`
   on failure.

9. **Homebrew floor stricter than CMake** — `homebrew/mos.rb:28`
   required `:sonoma` but `CMakeLists.txt:64` floors at 12.0
   (Monterey). Excluded Monterey/Ventura users for no reason.
   Fix: align Homebrew to `:monterey`; both files now point at
   the same floor.

10. **CI supply-chain partial pinning** — actions used mutable
    major-version tags (`@v4`) and `jsonschema` installed
    unpinned. Fix: SHA-pin both actions to authoritative release
    SHAs (`actions/checkout@11bd71... # v4.2.2`,
    `actions/setup-python@0b93645... # v5.3.0`); add
    `schemas/requirements-ci.txt` with `--require-hashes` covering
    `jsonschema` and every transitive dep.

Verified: 92/92 pure tests pass under
`-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration`
(was 89/89 before this pass; +3 watch regression tests). Schema
fixtures green (10 positive + 11 negative). C++ consumer
compiles clean across c++11/14/17/20. The original bug
reproducer now emits a snapshot immediately instead of a
55-year SLEEP_UNTIL.

Hardware integration matrix (BH16NS55, WH16NS60, A1379) unblocked.

### v0.3-dev: third external review pass landings

Date: 2026-05-14

Third review identified 9 findings, of which the most consequential
invalidated the prior pass's central fix. Landings:

1. **`--json=value` rejection path was dead code** —
   `long_options` declared `--json` as `no_argument`; getopt_long
   never sets `optarg` for `no_argument` options, so the
   `reject_legacy_json_version(optarg)` call always saw NULL and
   returned true. `--json=v2 --index 99` fell through to getopt's
   own '?' return rather than the typed `mos.state.v1` diagnostic
   the CLI test asserts on. Fix: declare as `optional_argument`.
   Now caught because finding 5 from the second pass made the CLI
   test actually run on default configure.

2. **Amalgamation test link missed `tests/test_render.c`** —
   `test_main.c` unconditionally calls `register_render_tests()`,
   so the link would fail with an undefined symbol. Fix: add the
   file to the amalgamation `cc` invocation in `.github/workflows/ci.yml`.

3. **IOReturn → mos_error mapping collapsed `kIOReturnNoDevice`
   to `MOS_ERR_IO` (critical)** — the second pass added a
   "probe → NO_DEVICE = terminal removal" path to the watch core
   and a pure regression test for it. But the Apple adapter's
   IOReturn translation table had no entry for `kIOReturnNoDevice`,
   so production `kIOReturnNoDevice` (what's actually returned
   when a drive is unplugged mid-probe) fell through to the
   default `MOS_ERR_IO`. The pure tests passed because they
   invoked the pump with `MOS_ERR_NO_DEVICE` directly — pure
   layer testing did not exercise the production wiring.

   Fix: move the mapping function to `mos_pure.c` under fixture
   coverage. `src/mos_scsi.c` keeps a thin shim that casts
   `IOReturn` to `int32_t` and calls the pure function. The pure
   layer encodes the IOReturn constants as numeric literals
   (`0xE00002C0` for `kIOReturnNoDevice`, etc.); the macOS shim
   wraps `_Static_assert((uint32_t)kIOReturnNoDevice == 0xE00002C0u, ...)`
   so any Darwin-SDK drift fails the build loudly rather than
   silently bypassing the test fixture. New `tests/test_ioreturn.c`
   pins all 11 mapped codes plus an unmapped-falls-through case.

   Also added: `kIOReturnNotAttached → MOS_ERR_NO_DEVICE`,
   `kIOReturnNoMemory → MOS_ERR_OOM`,
   `kIOReturnNoResources → MOS_ERR_OOM`.

   This is the exact failure mode the CLAUDE.md note warns about:
   testing the unit, not the wiring. The static_assert + pure
   fixture combination is the structural answer.

4. **`mos_raw_cdb(timeout_ms == 0)` was "wait forever"** —
   SCSITaskLib's `SetTimeoutDuration(0)` means infinite wait.
   A diagnostic call with the default-zero from a default-zeroed
   struct field would hang the caller's process indefinitely.
   Fix: reject `timeout_ms == 0` at the API boundary with
   `MOS_ERR_INVALID_ARG`; document the rationale in `include/mos.h`.

5. **Watch open had a hot-unplug TOCTOU window** —
   `watch_open_common` opened a handle to validate the drive,
   closed it, then re-resolved the io_service_t by BSD name and
   walked parents via `strstr(class_name, "BlockStorageDevice")`.
   Between close and rematch the BSD name could be reassigned;
   notifications could bind to a different drive. Fix: add
   `mos_internal_handle_get_service` accessor in `mos_internal.h`;
   `watch_open_common` retains the validated `io_service_t` before
   `mos_close` and uses that exact service for
   `IOServiceAddInterestNotification`. `find_service_by_bsd_name`
   removed entirely.

6. **`copy_bsd` could surface `rdiskN` names** — fallback path
   for USB bridges accepted `rdiskN` and `strlcpy`'d it directly
   to the public buffer. Public schema requires `^disk[0-9]+$`.
   `mos_internal_normalize_bsd_name` exists in `mos_pure.c` and
   already strips the `r` prefix; just wasn't called here. Fix:
   normalize before copying.

7. **STATUS.md was stale at "59/59 pure tests"** — counts
   refreshed to 103/103 (the suite has grown across the two
   prior review passes plus this one's 11 IOReturn tests).

8. **`GetTrayState` direct equality could miss future bits** —
   SDK exposes `kMMCDeviceTrayMask = 0x1`, indicating upper bits
   may carry future meaning. Fix: mask before comparing —
   `(state & kMMCDeviceTrayMask) == kMMCDeviceTrayOpen`.

9. **CMakeLists fatal-errored on non-Apple platforms** — the
   pure layer and unit tests build cleanly on Linux (the manual
   `gcc` invocation in the CI workflow proves this), but the
   project's own CMakeLists called `find_library(IOKit REQUIRED)`
   unconditionally, blocking non-macOS contributors from using
   the project's standard build path. Fix: wrap mos_core, mos,
   mos_probe, the CLI contract test, and the corresponding
   install rules in `if(APPLE) ... endif()`. mos_pure and
   mos_tests build on all platforms. Verified by running
   `cmake .. && cmake --build . && ctest` on Linux — 1 ctest
   entry, all 103 internal tests pass.

Verified: 103/103 pure tests pass under
`-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration`
(was 92/92 before this pass; +11 IOReturn tests). Schema fixtures
green. C++ consumer compiles clean c++11/14/17/20. CMake builds
and tests succeed on both Apple (via CI) and Linux paths.

Hardware integration matrix remains the next gate.

### v0.3-dev: stale test scaffolding cleanup

Date: 2026-05-14

Audit pass on the pure layer to remove tautological coverage —
functions that exist only to be tested with no production consumer.

- **`src/mos_cdb.c` + `tests/test_cdb.c` deleted.** The five
  `mos_internal_build_cdb_*` helpers (TEST UNIT READY, GET EVENT
  STATUS, GET CONFIGURATION, READ DISC INFO, INQUIRY) had zero
  production callers. The default state-query path uses Apple's
  MMC convenience methods, which build the CDBs inside the kext.
  `mos_raw_cdb()` takes the CDB from the caller. The AGENTS.md ADR
  defending these as "exported through the raw CDB surface" was
  factually wrong — the declarations lived in `src/mos_pure.h`
  (internal), not `include/mos.h` (public), so external callers
  could never reach them. v0.4 will drop `mos_raw_cdb()` entirely
  anyway. Removed: 93 lines of CDB builders, 95 lines of byte-layout
  tests, 1 stale ADR. SCSI byte-layout documentation stays in
  `ARCHITECTURE.md §4` where readers auditing the library can find
  it — the helper functions weren't where that documentation lived.

- **`EXPECT_BYTES` macro deleted from `tests/test_harness.h`.** Its
  sole consumer was `test_cdb.c`. Now unused.

- Pure test count: 96/96 (was 103/103, −7 CDB tests).

Note: `mos_internal_watch_notify_wake` kept as the external-trigger
hook on the pure watch state machine. Same audit pass also wired up
its first production caller (see v0.3-dev: Disk Arbitration wake
source below) — the function is no longer dead code.

### v0.3-dev: Disk Arbitration wake source

Date: 2026-05-14

Scaffolding for the planned shift from polling to event-driven media
detection. Before this change, `mos --watch` learned about media
inserts by polling — worst case ~2s latency between insert and
state_changed event (the default stable_poll_ms). After this change,
DiskArbitration callbacks wake the watch loop in <100ms when media
is inserted, ejected, or the disk description otherwise shifts.

Implementation in `src/mos_watch.c`:

- `setup_disk_arbitration_wake()` creates a DASession, registers
  `disk_description_changed_callback`, and schedules the session on
  the caller's run loop. Called from `watch_open_common` after the
  existing `kIOGeneralInterest` setup so `w->run_loop` is captured.
  Failure mode is symmetric with the IOKit notification setup: any
  failure leaves `w->da_session` NULL and the watch falls back to
  poll-only. Polling is the correctness floor; DA is the latency
  optimization.

- `disk_description_changed_callback()` calls
  `mos_internal_watch_notify_wake(&w->core)` (pulls
  `next_poll_at_mono_ms` to 0) and `CFRunLoopStop(w->run_loop)`
  (breaks the pump's current `CFRunLoopRunInMode` sleep). Filtering
  is in-callback by `DADiskGetBSDName` comparison: callback fires
  for all disks, returns early unless the disk name matches our
  drive's BSD name or starts with it followed by 's' (media-child
  entries like `disk4s1`). This avoids depending on specific DA
  description-key constants for the matching dictionary, which vary
  by SDK version and content type.

- `teardown_disk_arbitration_wake()` runs first in `mos_watch_close`,
  before the IOKit notification teardown — same reasoning as the
  existing ordering: stop callbacks before releasing memory they
  reference. Unschedule, unregister, CFRelease, in that order.

CMake links the DiskArbitration framework alongside IOKit and
CoreFoundation under the `if(APPLE)` gate. The pure-layer Linux
build is unaffected — `mos_watch.c` is in `mos_core`, not
`mos_pure`, so the new framework dependency doesn't reach Linux
contributors.

What this does NOT do: actually test the wake-up behavior on real
hardware. That requires the macOS integration matrix runs. The code
compiles cleanly under the macOS CI job and follows Apple's
documented patterns for DA usage; the v0.3-dev hardware pass will
verify the latency improvement empirically on tray-load (Apple A1379),
slot-load (where available), and USB-bridged drives.

What this also does: makes `mos_internal_watch_notify_wake` a real
production-coupled function rather than the architectural-readiness
hook it was in the prior pass. The wake-source contract is now load-
bearing on the macOS side: a regression in the pure-layer `notify_wake`
behavior (e.g. failing to clear the deadline) would surface as
unresponsive `mos --watch` on media inserts. The existing
`test_notify_wake_pulls_next_poll_forward` fixture is the regression
guard.

### v0.3-dev: no-CMake pure-tests target (reverted)

Date: 2026-05-14

Fourth external review flagged that the build system assumes
CMake, with no fallback for contributors who want to iterate on the
pure layer without installing it. A `make pure-tests` target was
added that invoked `cc` directly against the same source list and
warning flags as the CMake `mos_tests` target.

Reverted same day after the framing was challenged. The honest
audience for that target was Claude itself running verification on
Linux containers, not human contributors who would have CMake
available via `brew install` (macOS) or apt/dnf (Linux). Adding a
Makefile target to formalize Claude's workflow constraint as a
project feature was the same kind of "compile-without-IOKit as goal
instead of side effect" mistake the prior cleanup pass was meant to
correct. mac-optical-state is a macOS-native tool; the pure-layer
separation gives us testability without hardware as good engineering,
not because Linux is a deployment platform.

What stays: the CMake `if(APPLE)` gate around `mos_core` / `mos` /
`mos_probe` / CLI test (from the F9 finding). That has real
downstream-consumer value — `add_subdirectory(mac-optical-state)`
from a non-Apple build configures cleanly instead of hard-erroring
on `find_library(IOKit REQUIRED)`. The architectural separation
is the goal; the non-Apple build path is a downstream-library
courtesy, not a development target.

The same review also made several claims that didn't reproduce
against the tree:
- "`Makefile` calls `git describe` which fails in trees without
  `.git`" — the line has `|| echo dev` as a fallback. Verified:
  produces the literal "dev" in a non-git directory.
- "`make test` runs CLI shell tests on non-Apple hosts" — `make test`
  builds and runs `mos_tests` (the unit binary). The CLI shell test
  is a separate `ctest` entry that the G9 CMake gate only registers
  under `if(APPLE)`.
- "`MOS_WATCH_STABLE_MS` / `MOS_WATCH_TRANSITION_MS` env vars are
  read via `strtoul` in `mos_watch_core.c`" — no such code exists.
  No `getenv`, no `strtoul`, no env-var reads anywhere in the pure
  layer. The defaults (`MOS_WATCH_DEFAULT_STABLE_MS = 2000U`,
  `MOS_WATCH_DEFAULT_TRANSITION_MS = 200U`) are compile-time
  `#define` constants in `src/mos_watch_core.c`.

Documenting here so a future audit pass can cross-reference what
was actually true vs. claimed.

### v0.3-dev: fifth external review pass

Date: 2026-05-14

Real findings, fixed in priority order:

1. **DiskArbitration missing from amalgamation link command**
   (release-blocker). When the DA wake source landed, `CMakeLists.txt`
   got the `find_library(DISKARBITRATION_FRAMEWORK ...)` and
   `target_link_libraries(mos_core ... ${DISKARBITRATION_FRAMEWORK})`
   wiring, but the parallel update to `.github/workflows/ci.yml`'s
   amalgamation link command was missed. The amalgamated single-file
   build would have failed link on macOS CI. Fix: added
   `-framework DiskArbitration` to the link command.

2. **`ObtainExclusiveAccess` error mapping was F3 again.** The raw-CDB
   exclusive-access acquisition manually mapped `kIOReturnBusy` and
   `kIOReturnExclusiveAccess`, then collapsed every other code
   (including `kIOReturnNoDevice`, `kIOReturnNoMemory`,
   `kIOReturnTimeout`) to `MOS_ERR_IO`. Exactly the bug the F3 fix
   moved the mapping to `mos_pure.c` to prevent. Fix: replaced the
   manual mapping with `mos_internal_ioreturn_to_mos_error(rx)`,
   matching every other IOReturn callsite in `mos_scsi.c`. Lesson:
   the F3 audit ran across the rest of the file but missed this
   path; a comprehensive grep for `kIOReturn` would have caught it
   then.

3. **`IOObjectRetain` return value ignored.** `watch_open_common` did
   `IOObjectRetain(validated_svc); w->svc = validated_svc;` without
   checking the retain result. Adversarial hotplug timing or kernel
   resource exhaustion could give us a stored `w->svc` we don't
   actually own, which would corrupt subsequent notification
   registration or close-time release. Fix: check return; on failure
   leave `w->svc` NULL and fall back to poll-only (the existing
   failure-mode pattern for every other notification step).

4. **`IORegistryEntryGetRegistryEntryID` return value ignored.**
   `mos_internal_visit_collect` called the function without checking
   the return; on failure `id` stayed 0 and unrelated failures
   deduplicated against each other. More importantly, the public
   index contract (`mos_open_by_index()` reopens via
   `IORegistryEntryIDMatching` against the stored ID) requires a
   non-zero ID — entries with id==0 would enumerate but couldn't be
   reopened. Fix: skip entries when the lookup fails or returns 0.

5. **Callback registration before state machine init.**
   `watch_open_common` registered `kIOGeneralInterest` and DA
   callbacks before calling `mos_internal_watch_init`. In practice
   nothing dispatched because the run loop hadn't pumped yet, but
   the ordering was wrong by first principles: callbacks that mutate
   the state machine were registered against an un-initialized
   state machine. Fix: reordered so the pure init runs immediately
   after `mos_close(test)`, before any callback registration. The
   init has no failure path and no run-loop dependency, so doing it
   first costs nothing.

6. **Single-thread contract for `mos_watch_*` was in a comment, not
   the public header.** `mos_watch_t` stores a borrowed
   `CFRunLoopRef` from `CFRunLoopGetCurrent()` and reuses it at
   close. Under the documented threading contract this is safe, but
   the documented contract was in a source-file comment. Fix:
   tightened the threading note in `include/mos.h` to explicitly
   state that open/pump/close must run on the same thread, with the
   rationale (run loops are thread-owned, retain wouldn't help).

7. **Schema `stream_id` description said "monotonic timestamp"** but
   the code uses `start_wall_ms` (Unix epoch ms). Fix: updated
   `schemas/mos.event.v1.json` description to say "wall-clock (Unix
   epoch) timestamp" with rationale.

8. **README dependency claim stale.** Said "only IOKit and
   CoreFoundation"; updated to include DiskArbitration.

9. **README mentioned CDB builders** that were removed in the prior
   cleanup pass. Updated to describe the actual pure-test surface.

10. **CONTRIBUTING.md referenced "tests 59/59"** — stale count.
    Updated to 96/96.

11. **`mos_json_escape` / `mos_safe_ascii` NULL-buffer contract**
    not pinned. Added explicit "out may be NULL only when out_cap
    == 0" language to the docblock so the measure-only mode is
    documented and the dereference-NULL case is called out as a
    programming error.

Misread or outdated findings (kept here for the audit trail):

- ROADMAP test-count "inconsistency" (92/92, 103/103, 96/96 across
  entries): append-only history, each entry accurate at its date.
  Not drift. The chronological progression is correct.
- "No `make pure-tests` target in archive": correct as a fact but
  expected — that target was added then reverted last turn after
  the framing was challenged. The reviewer didn't have that
  context.
- Homebrew formula references to "v0.2.0": those are historical
  documentation of the HEAD-only → bottle migration plan, not
  stale version claims.

Deferred:

- CLI raw-stderr printing of user-controlled values (e.g.
  `fprintf(stderr, "mos: invalid --index value: %s\n", optarg)`).
  Hardening suggestion has merit but the threat model is local
  user, not drive-controlled, so lower priority than the IOKit
  lifetime fixes. Revisit if CLI is ever invoked from a context
  where stderr goes to an attacker-controlled terminal.

### v0.3-dev: cleanup-attribute pass for IOKit/CF refcount discipline

Date: 2026-05-14

Refactor pass to make refcount discipline automatic in the Apple
adapter where the cleanup-attr pattern has clear payoff. Motivated
by F5 (the TOCTOU bug that originated as a "forgot to release on
this error path" mistake) and the fifth review's `IOObjectRetain`
finding — both pointed to manual refcount audit as a known weak
link.

Infrastructure (`src/mos_internal.h`):
- `mos_internal_cleanup_cftype(CFTypeRef *p)` — CFRelease + clear on
  scope exit.
- `mos_internal_cleanup_io_object(io_object_t *p)` — IOObjectRelease
  + clear on scope exit.
- `MOS_CF_AUTO`, `MOS_IO_AUTO` macros for use at variable
  declarations.

Both cleanup callbacks check for the sentinel value before releasing
and clear after, so manual release-and-clear (e.g. the ownership
transfer pattern) and auto-cleanup coexist safely.

Refactored functions in `src/mos_scsi.c`:
- `mos_internal_copy_bsd` — 5 explicit releases → 0. Iterator loop
  with two-pass property lookup and multiple early-exit paths
  (continue when no BSD name, break on whole-found, normal end of
  loop). Five opportunities to forget a release, now zero. Restructured
  the `while ((child = IOIteratorNext(it)) != NULL)` form to
  `for (;;) { io_object_t child MOS_IO_AUTO = IOIteratorNext(it);
  if (child == NULL) break; ... }` so per-iteration scope owns its
  own cleanup.
- `mos_open_by_bsd_name` — 3 explicit releases → 0. Demonstrates the
  ownership-transfer pattern: when the match is found, the local
  `s` is consumed by `mos_internal_open_service` via assignment to
  a local `consumed`, then the cleanup-managed `s` is explicitly
  cleared to `IO_OBJECT_NULL` so the auto-cleanup on return is a
  no-op.
- `mos_enumerate_devices` — 2 explicit releases → 0. Pure iteration,
  no ownership transfer, same restructuring as `mos_internal_copy_bsd`.

NOT refactored (remaining 3 explicit releases in `mos_scsi.c` are
correct as-is):
- `mos_internal_is_authoring`'s `CFRelease(v)` — single property
  lookup with single release point; cleanup-attr would add noise
  without preventing any bug.
- `mos_internal_open_service`'s `IOObjectRelease(svc)` on calloc
  failure — single-line release-and-return that immediately precedes
  ownership transfer to `h->svc` on the success path. Inline release
  is clearer than a cleanup-managed local plus an explicit clear.
- `mos_close`'s `IOObjectRelease(h->svc)` — the canonical handle
  destructor.

Scope and portability:
- All changes confined to the Apple adapter (`mos_scsi.c` plus a
  helper block in `mos_internal.h` which the pure layer never
  includes).
- Cleanup attribute is a GNU extension supported identically by gcc
  and clang since the late 1990s, so the change does not introduce a
  new portability surface or compiler-specific compatibility block.
  Pure-layer Linux build continues to pass unchanged.

Bottom line: 15 IOObjectRelease + 4 CFRelease in `mos_scsi.c`
dropped to 2 IOObjectRelease + 1 CFRelease. The remaining three are
in contexts where cleanup-attr provides no value. Refcount discipline
is now a property of variable declarations, not of audit. 96/96 pure
tests pass; non-Apple CMake build unaffected.

### v0.3-dev: sixth review pass — run-loop integration contract

Date: 2026-05-14

The sixth external review opened with a staleness claim that
didn't reproduce against the current tree — the reviewer had
downloaded older snapshots from earlier in the session (sha
`9740104d...` lacked the −7 CDB cleanup; sha `6cc1b56f...` lacked
the fifth-review fixes). Current packaged sha is `599c8350f0...`
and contains every fix listed in prior ROADMAP entries.

The substantive macOS-specific findings did reproduce, however, and
the most consequential one was a real architectural defect for an
embeddable library — the watch's run-loop integration assumed the
host's `kCFRunLoopDefaultMode` was a safe place to schedule sources
and a safe target for `CFRunLoopStop`. That's true for the
standalone CLI; for a library embedded in a host app on the same
thread, it isn't. Six call sites in `src/mos_watch.c` needed
coordinated change.

Fixes:

1. **Private run-loop mode.** Added file-local constant
   `MOS_WATCH_RUN_LOOP_MODE = CFSTR("io.github.napieraj.mac-optical-state.watch")`.
   The reverse-DNS form ensures uniqueness against any other
   library that might also reach for a private mode. Replaced
   `kCFRunLoopDefaultMode` at every call site:
   - `CFRunLoopAddSource` for the IOKit interest-notification source
   - `CFRunLoopRemoveSource` in the open-time failure path
   - `CFRunLoopRemoveSource` in `mos_watch_close`
   - `DASessionScheduleWithRunLoop` for the DA session
   - `DASessionUnscheduleFromRunLoop` in DA teardown
   - `CFRunLoopRunInMode` in the pump's wait
   Net: our sources only dispatch while our pump is actively
   running our mode; host-app default-mode work runs without
   interference; our `CFRunLoopStop` cannot stop a run-loop
   invocation the host owns (the stop only fires when our mode is
   running, which only happens inside our pump).

2. **Decoupled DA setup from kIOGeneralInterest success.**
   Pre-fix, `w->run_loop = CFRunLoopGetCurrent()` was assigned
   only inside the successful kIOGeneralInterest branch. Any
   failure in the IOKit notification chain (port create, source
   acquisition, interest registration) left `w->run_loop` NULL,
   which silently disabled DA setup via the early `if (!w->run_loop)
   return;` check in `setup_disk_arbitration_wake`. The two
   mechanisms are independent best-effort wake sources; coupling
   them coupled failure modes that shouldn't be coupled.

   Fix: `w->run_loop` is now captured unconditionally before any
   setup attempt. Each mechanism fails soft on its own. The pump's
   wait gate changed from `if (w->run_loop)` to
   `if (w->run_loop && (w->notify_port || w->da_session))` — i.e.,
   "we have a run loop AND at least one source alive." Both paths
   independently dead → nanosleep fallback.

3. **`CFStringGetCString` return check.** Apple docs note the
   function may have written partial bytes to the buffer before
   returning false. The pre-fix code ignored the return and relied
   on the implicit `if (this_name[0] == 0) continue;` check, which
   a partial conversion could pass. Explicit clear-on-failure
   makes the empty-string check authoritative.

4. **`IOObjectConformsTo(child, "IOMedia")` on the authoritative
   Whole path.** Defense in depth. Pre-fix, any descendant node
   carrying a Whole-boolean property could win the primary path;
   restricting the primary path to entries that conform to the
   IOMedia class hardens against unusual storage-service topologies.
   The fallback path still accepts non-IOMedia entries whose BSD
   name has whole-disk shape — that's what makes USB bridges work.
   The string-name form of `IOObjectConformsTo` avoids pulling in
   `<IOKit/storage/IOMedia.h>` just for this check.

5. **Threading contract in `include/mos.h` updated.** Now explicit
   about the private run-loop mode: host default-mode work runs
   alongside the watch without interference, and the watch's
   `CFRunLoopStop` on notification cannot stop a run-loop
   invocation the host owns.

6. **Amalgamation deployment-target docs.** CMake pins
   `CMAKE_OSX_DEPLOYMENT_TARGET 12.0`; the amalgamation drop-in
   path had no parallel guidance, so amalgamation consumers
   silently floated to whatever default the host SDK provided.
   Updated `CONTRIBUTING.md` with the canonical build invocation
   (three frameworks + `-mmacosx-version-min=12.0`) so the
   amalgamation path and the CMake path target the same OS floor.

Deferred from this review (low value relative to cost):

- macOS-only "framework drift" CI compile job with stricter
  `-Werror=incompatible-function-pointer-types` etc. flags.
  The amalgamation job on macOS-latest catches most of this
  organically; the rest is real but second-order.
- App Sandbox documentation. Worth a brief note in README that
  sandboxed embedding is not validated, but no behavior change.
  Will batch with the integration matrix work.

Bottom line: the run-loop integration contract is now suitable
for an embeddable library. The two-source decoupling means DA
latency optimization survives any kIOGeneralInterest setup
failure independently. 96/96 pure tests pass; non-Apple CMake
build unaffected (none of this touches the pure layer).

### v0.3-dev: seventh review pass — strict-build + doc/script drift

Date: 2026-05-14

Seventh external review opened against archive sha `599c8350f0...`
— one revision behind the current tree (the sixth-review fixes were
not present in the artifact the reviewer downloaded). Current sha is
`df82b1e6af3339...`. The reviewer correctly noted what the previous
pass had landed (private run-loop mode, decoupled DA setup,
CFStringGetCString check, IOObjectConformsTo, threading-contract
update, schema fix, README dependency line). The new findings against
the actually-current tree are independent of the staleness issue:

1. **`-Werror=format-truncation` failure under GCC -O2 in
   `mos_watch_core.c`'s `format_rfc3339`.** Reviewer reproduced it
   with a standard strict-pure-build invocation. The pre-fix code
   used hand-rolled `snprintf("%04d-%02d-%02d...", tm.tm_year + 1900,
   ...)` with a comment explaining "locale-immune by construction" —
   GCC's value-range analysis saw `tm.tm_year` as unbounded `int`
   and computed worst-case output ~66 bytes into a 24-byte buffer,
   failing the warning. The first fix attempt bounded tm.tm_year
   but the compiler still warned about the unbounded tm_mon /
   tm_mday / tm_hour / tm_min / tm_sec fields. Final fix switched
   to `strftime("%Y-%m-%dT%H:%M:%SZ", &tm)` — POSIX guarantees those
   format specifiers are numeric and locale-independent (locale only
   affects textual specifiers like %A, %B, %p, %c/%x/%X which we
   deliberately don't use). The "locale-immune by construction" claim
   that motivated the original snprintf approach was over-cautious;
   the only specifiers in our format string are exactly the ones
   POSIX makes locale-independent. Also added `gmtime_r`/`gmtime_s`
   failure handling that was previously implicit.

2. **`scripts/amalgamate.sh` generated link instructions still
   stale.** CI's amalgamation link command was fixed in the fifth
   review, but the script that generates `dist/mos.c`'s header
   comment AND the `dist/MANIFEST.txt` footer both still said only
   `-framework IOKit -framework CoreFoundation`. Downstream
   consumers downloading the amalgamated artifact would see stale
   build instructions despite CI succeeding. Updated both blocks
   to include `-framework DiskArbitration` and
   `-mmacosx-version-min=12.0` (parallel to what CMake pins via
   `CMAKE_OSX_DEPLOYMENT_TARGET`). With the script fix, the contract
   is consistent across CI / dist / docs.

3. **`make build` on Linux failed with an opaque error.** The
   `build:` target depends on `cmake --build $(BUILD) --target $(PKG)`
   where `$(PKG) = mos`, but `mos` is only registered under
   `if(APPLE)` in CMakeLists.txt. The non-Apple CMake configure
   succeeds, then `make build` fails at `gmake[1]: No rule to make
   target 'mos'`. Added a uname check at the top of the target that
   prints a friendly hint pointing contributors to `make test`
   (which does work on Linux) and clarifying that pure-layer Linux
   compilability is an architectural side effect, not a deployment
   target. The hint message reinforces the framing the prior pass
   established.

4. **Stale "CDB builders" mentions in `CMakeLists.txt` and
   `src/mos_pure.h`.** Two comment-only references left over from
   the cleanup pass that deleted `mos_cdb.c`. Fixed both.

5. **`examples/wait-and-classify.sh` claimed `mos --watch` "holds
   the device handle open."** Wrong. The watch design opens a
   short-lived handle per probe — that's exactly what makes it
   cooperative with Finder, Disk Arbitration, and concurrent
   consuming applications. Updated the example comment to describe the
   actual design and call out the cooperation property.

6. **`INTEGRATION_HARNESS.md` referenced `mos --verbose --json`** but
   the CLI doesn't implement `--verbose`. Removed the `--verbose`
   flag from the harness instruction; the harness captures what
   `mos --json` actually prints.

7. **No CI step exercising the strict pure build.** Added a new
   `strict-pure-build` job on `ubuntu-latest` that runs the pure
   sources under `-O2 -Wall -Wextra -Wpedantic -Werror`. This is
   exactly the invocation that caught the format-truncation issue
   above; codifying it as CI prevents the same class of regression
   from sliding past the development-time `-Werror=implicit-function-declaration`
   warning level.

8. **macOS adapter SDK-drift CI compile.** Added a parallel
   `strict-adapter-build` job on `macos-latest` that compiles
   `src/mos_scsi.c` and `src/mos_watch.c` with stricter `-Werror`
   flags than the default CMake `mos_warnings` interface:
   `-Werror=incompatible-function-pointer-types` (catches Apple
   changing a callback signature in a future SDK — e.g. an extra
   parameter on `DARegisterDiskDescriptionChangedCallback`);
   `-Werror=int-conversion` (catches newer headers silently
   narrowing `kIO*` constants or `mach_port_t` variants);
   `-Werror=return-type` (defensive against conditional-compile
   shifts that drop a `return` under updated headers). Pinned to
   `-mmacosx-version-min=12.0` to match the CMake
   `CMAKE_OSX_DEPLOYMENT_TARGET` so the compile sees the same SDK
   shape the release build uses. This catches the class of bug
   that Linux CI fundamentally cannot see because it involves
   Apple SDK signatures.

What the reviewer flagged that didn't require new action:
- "Direct no-CMake pure-tests target is still absent" — correctly
  noted as previously reverted; not a release blocker.
- Various lifetime/error-mapping findings from earlier reviews —
  correctly noted as already fixed in the current tree.

96/96 pure tests pass under both the default warning level and the
new strict (-O2 -Werror) configuration. Non-Apple CMake build still
configures, builds, and tests cleanly. Schema fixtures green. The
amalgamation script and the integration harness docs now match the
shipped code.

### v0.3-dev: eighth review pass — run-loop bug + refactor sweep

Date: 2026-05-14

Eighth review caught a real Mac-side bug plus a list of cleanups
that had accumulated. Reviewer read sha `d6c364c6757a...` (current
tree as of the prior pass) and produced a 12-item patch list. All
twelve landed:

1. **`mos_watch_next_event` gate fixed.** The pump's run-loop wait
   was gated on `w->notify_port || w->da_session`, but the actually-
   scheduled IOKit source is `w->notify_source`. Partial-failure
   path: `IONotificationPortCreate` succeeds, then
   `IONotificationPortGetRunLoopSource` returns NULL, then DA setup
   also fails — `notify_port` stays non-NULL with no live source,
   the pump enters `CFRunLoopRunInMode` in a private mode with zero
   scheduled sources, the call returns immediately, and the watch
   tight-loops until timeout. Fix: gate on `notify_source ||
   da_session`. This is the canonical "thing that's actually
   scheduled" test.

2. **Setup-time invariant added.** `setup_iokit_interest_wake` now
   destroys `w->notify_port` immediately if
   `IONotificationPortGetRunLoopSource` returns NULL. The invariant
   "port non-NULL iff source live" is now enforced at the setup
   site, so the partial-failure case (1) defends against is also
   impossible by construction.

3. **README + CONTRIBUTING + CMake comments + examples/README**
   all updated to include DiskArbitration as a dependency
   alongside IOKit/CoreFoundation. README direct-link instructions
   and amalgamation manifest now include `-mmacosx-version-min=12.0`
   matching the CMake build's pinned deployment target.

4. **`--verbose` references retargeted.** README and ARCHITECTURE.md
   had three `--verbose` references for functionality that doesn't
   exist in v0.3. Repointed to "v0.4 typed APIs (a planned
   `mos_disc_info` accessor)" and noted that `mos_raw_cdb()` reaches
   the underlying MMC operations today.

5. **`tools/mos_probe.c` duplicate `print_safe` removed.** Now
   delegates to `mos_safe_ascii` via a 256-byte buffer (enough for
   SCSI INQUIRY vendor + product strings at worst-case 4× expansion).
   One escaping rule across the project.

6. **CLI user-controlled diagnostics sanitized.** Four `fprintf`
   sites in `tools/mos.c` that printed `optarg` / `argv[optind]` /
   subcommand strings raw to stderr now route through
   `print_safe_ascii`. Threat model is local user (not drive-
   controlled), so this is hardening uniformity rather than a fix
   for a specific attack — same "one helper, one escaping rule"
   principle.

7. **Raw-CDB setup cleanup collapsed.** Three near-identical
   `Release(t); ReleaseExclusiveAccess; have_exclusive=false; return
   mapper(sr)` blocks (after each of `SetCommandDescriptorBlock`,
   `SetScatterGatherEntries`, `SetTimeoutDuration`) converge on a
   single `setup_failed:` label. One cleanup body to audit, not
   three.

8. **Watch-event state-field filling extracted.**
   `fill_event_state_fields(e, r, latency)` copies the eight
   identical state fields from a probe result into an event;
   `poll_ms_for_state(w, state)` returns the transitional-vs-stable
   poll deadline. Three sites used the eight-line copy block,
   three sites used the poll-ms ternary. Both helpers are pure
   data transformations covered by existing tests.

9. **IOKit interest wake split into setup/teardown helpers.**
   `setup_iokit_interest_wake` and `teardown_iokit_interest_wake`
   now mirror the existing `setup_disk_arbitration_wake` /
   `teardown_disk_arbitration_wake` pair. `watch_open_common`
   becomes a clean sequence: validate handle, retain service,
   init pure core, capture run loop, set up IOKit wake, set up
   DA wake. `mos_watch_close` becomes the symmetric teardown
   pair plus the service release.

10. **"Pre-fix history" comments stripped from production code.**
    Seven historical narrative blocks across `mos_scsi.c`,
    `mos_watch.c`, `mos_watch_core.c`, `mos_pure.h`, and
    `tools/mos.c` rewritten as current-invariant rules. The "how
    we got here" narrative lives in this ROADMAP; source comments
    describe the rule a future maintainer must preserve.

11. **Strict adapter CI extended.** The `strict-adapter-build`
    job's compile loop now covers `src/mos_state.c`,
    `tools/mos.c`, and `tools/mos_probe.c` in addition to
    `mos_scsi.c` and `mos_watch.c`. All five are Apple-linked
    TUs with the same SDK-signature drift exposure.

12. **CMake `CMAKE_OSX_DEPLOYMENT_TARGET` block wrapped in
    `if(APPLE)`.** Non-Apple configures no longer populate a
    macOS-only cache variable. Pure cleanup; build behavior
    unchanged.

96/96 pure tests still pass under both default and strict
(-O2 -Werror) configurations. CMake non-Apple build: configure +
build + ctest all green. Schemas green. `make build` on Linux
gives the friendly hint instead of an opaque cmake error. The
tree is materially cleaner — every duplicated mechanical block
the reviewer flagged is now single-sourced.

Nothing was deferred from this review. Hardware integration matrix
on the BH16NS55, WH16NS60, and A1379 drives remains the only gate
to promote from v0.3-dev to v0.3.0.

### v0.3-dev: string-copy primitive normalization

Date: 2026-05-14

Audit pass triggered by "why are we mixing memcpy and strlcpy"
question. The codebase had three patterns for the same operation —
"copy a NUL-terminated string into a fixed-size buffer with bounded
length and guaranteed terminator":

1. `memcpy(dst, src, sizeof(dst))` — 3 sites in `mos_scsi.c`.
   Worked because the source buffers were always zero-initialized
   to the same width, but that's a fragile invariant to lean on
   and the call isn't self-documenting as a string copy.

2. `strlcpy(dst, src, sizeof(dst))` — 4 sites across `mos_scsi.c`
   and `tools/mos.c`.

3. Hand-rolled `for` loop with explicit NUL terminator — 4 sites
   in `mos_watch.c`, 1 site in `mos_watch_core.c`.

Normalized to a two-pattern split with explicit rationale:

- **Apple adapters (`mos_scsi.c`, `mos_watch.c`, `tools/mos.c`):
  `strlcpy` throughout.** Darwin provides `strlcpy` unconditionally
  since 10.0 (2001). It NUL-terminates within the destination
  buffer even when the source fills it exactly (unlike `strncpy`)
  and is self-documenting as a string copy. 11 sites total.

- **Pure layer (`mos_watch_core.c`): hand-rolled `for` loop.** This
  is the one place in the codebase that hand-rolls a bounded string
  copy. The reason is load-bearing: pure code must compile against
  any C11 libc, and glibc only added `strlcpy` in 2.38 (Aug 2023).
  The for-loop pattern is portable to every libc. Comment in
  `mos_watch_core.c` explains the Apple-vs-pure split inline. 1
  site total.

Remaining `memcpy` calls (3 sites) are genuine byte-buffer
operations — two copy an 18-byte `SCSITaskSenseData_Default`
struct into the caller's sense buffer; one is the raw-CDB
passthrough copying `len` bytes of arbitrary caller-supplied data.
None are string copies. Verified: 96/96 pure tests pass after
the normalization.

---

---

## 2026-05-11 — v0.3: event-driven --watch shipped

### Event-driven `--watch` mode

> **LANDED 2026-05-11.** This subsection is the original planning
> trail; the actual shipped shape is documented in the
> `2026-05-11 update: contract revision + --watch shipped` entry
> above. Notable departures: per-document `schema` (e.g.
> `mos.event.v1`) replaces `schema_version`; field names are
> `event` / `ts` / `seq` rather than `event_type` / `timestamp` /
> `sequence` / `event_id`; bounded polling supplements
> `kIOGeneralInterest` notifications rather than pure
> notification-only (the SuperDrive USB transport doesn't deliver
> media events reliably enough to skip polling). The planning text
> below is preserved as the design record.

DiskArbitration + IOKit interest notifications, no polling.

- `DARegisterDiskAppeared` / `DiskDisappeared` / `DescriptionChanged`
  callbacks for media events.
- `IOServiceAddInterestNotification` on `IOMedia` for sub-state changes.
- Reuse `mos_open` / `mos_state` for MMC profile classification (DA
  doesn't expose profile bytes directly).
- Sub-100ms latency, zero idle CPU, no drive polling.
- ~300 LOC, no third-party deps.
- Feasibility verified against Apple's open-sourced diskarbitrationd
  code — not just inferred from man pages.
- Possibly a `WATCH_DESIGN.md` document if the architecture justifies
  splitting from ARCHITECTURE.md.

#### NDJSON envelope and reserved field names

`--watch` will emit one JSON object per line (NDJSON / JSON Lines)
rather than the v2 single-record envelope. The per-event payload
should reuse the v2 field shape (`schema_version`, `state`,
`bsd_name`, etc.) augmented with stream-specific metadata at the top
level rather than wrapping the v2 record in an outer envelope —
preserving the v2 forward-compat rule that consumers ignore unknown
top-level keys means a v2-shaped consumer can still parse a v0.3
watch line and ignore the new metadata fields.

The following top-level field names are **reserved** by v2 for v0.3
streaming use — v2 must not introduce these as success-envelope or
failure-envelope fields:

- `event_type` — discriminator for the trigger source. Likely values
  drawn from the DA callback set (`disk_appeared`, `disk_disappeared`,
  `description_changed`) plus the IOKit interest notification set
  (`media_state_changed`, etc.). Final value list is a v0.3 design
  decision once the actual callback surface is exercised.
- `timestamp` — observation moment in some monotonic format (likely
  `clock_gettime(CLOCK_MONOTONIC)` formatted as decimal seconds with
  microsecond precision; ISO 8601 UTC also a candidate). Format is a
  v0.3 design decision.
- `sequence` — monotonic counter for ordering events within a stream.
  Lets consumers detect dropped events (sequence-skip) and reorder if
  the underlying notification source delivers out-of-order.
- `event_id` — opaque dedup token, allowing consumers to detect
  duplicates across reconnect/resume scenarios.

See `doc/research/2026-04-27-v2-contract-design.md` items 13 and 17
for the lock-in rationale. The reservation is what makes the v0.3
design space possible without a v2-bump for shape compatibility.

This subsumes the v0.2-era external-review proposal for an
asynchronous / cancellable / timeout-bearing `mos_query_state_async()`
API. Apple's `MMCDeviceInterface` convenience methods don't expose
timeouts at all — the blocking happens inside the user-client kernel-
IPC roundtrip — so a "timeout" parameter at mos's layer would have to
be implemented as "spawn a worker thread, signal back if it hasn't
returned, accept that we leaked a thread that may eventually
complete." That's the wrong shape for the GUI-doesn't-block use case
the proposal aimed at. Event notification through DiskArbitration +
IOKit interest is the right shape — the GUI subscribes to state
changes and doesn't block at all, rather than blocking with a
timeout. Documented here so the rejection survives in the design
record rather than reappearing as a recurring maintenance question.

---

## 2026-05-11 / 2026-05-12 — v0.3: contract revision, schemas, subcommands

### 2026-05-11 update: contract revision + --watch shipped

The schema family (`mos.state.v1`, `mos.error.v1`, `mos.list.v1`,
`mos.event.v1`), sysexits exit codes, the `is_rw` → `is_rewritable`
rename, the nested error envelope with `recoverable` hint, the
`current_profile_name` field, and `--watch` mode with NDJSON output
all landed in this iteration ahead of the typed-API work. Rationale:

- The v2 contract was internal-lock only (no public release, no
  customers, no backwards-compatibility burden). Architecture review
  identified renames worth doing before v2 went public: hex-string
  `current_profile`, flat `bsd_name`, and `--bsd` flag stay; field
  names and error structure clarify; exit codes adopt sysexits to
  give consuming applications real information instead of collapsing failures to
  exit 1.

- `--watch` lands now rather than as a separate v0.3 phase because
  the pure state machine (`src/mos_watch_core.c`) is testable
  without IOKit through the same `mos_mmc_ops_t`-style vtable
  pattern that `mos_state_core.c` uses. Twelve new pure unit tests
  exercise snapshot emission, state-change deltas, backoff
  scheduling (transition vs stable), wake/removed notifications,
  error events without state poisoning, sequence monotonicity,
  latency measurement, and RFC 3339 timestamp formatting.

- The Apple-side adapter (`src/mos_watch.c`) wires the vtable to
  real probes (per-poll handle open/close to tolerate driver
  detach/reattach) and registers `kIOGeneralInterest` notifications
  on the watched drive so device removal wakes the loop without
  waiting for the next scheduled poll. Hybrid: notifications when
  available, bounded polling always.

The typed-API work (five `mos_get_*` / `mos_*_control` surfaces
listed below) remains the v0.3 capability target. The contract
revision above is what makes those typed APIs land into a stable
shape — once the schema family is locked, typed APIs add new
schemas (e.g. `mos.capacity.v1`) without touching the existing
ones.

### 2026-05-12 update: schemas/ directory LANDED, subcommands shipped

Pre-hardware-integration hardening pass:

- **`schemas/` directory:** formal JSON Schema documents (draft
  2020-12) for the four v0.3 envelopes plus positive (10) and
  negative (11) fixtures, validated by `schemas/validate.py` in CI
  on every commit. The contract is now machine-checkable, not just
  prose-described. When the emit code in `tools/mos.c` changes,
  the schema and at least one fixture should change in the same
  commit. v0.4 typed-API schemas (`mos.capacity.v1`, etc.) will
  follow the same shape when those APIs land.

- **Subcommands shipped as additive aliases:** `mos status`,
  `mos list`, and `mos watch` work as aliases for the flag forms;
  the implicit-status form (`mos --bsd diskN` with no subcommand)
  remains permanent per ARCHITECTURE.md §10. Five reserved names
  for v0.4 typed APIs (`capacity`, `identity`, `tray`, `speed`,
  `features`) are recognized with a clear "not yet implemented"
  diagnostic so users who try them prematurely get useful feedback
  rather than a generic "unknown subcommand" error. Subcommand
  introduction now lets the v0.4 typed APIs land into a familiar
  surface rather than inventing the shape at the same time as the
  APIs.

- **Pre-existing CI + Homebrew formula drift corrected** to the
  v0.3 contract: `mos --index 99` now expected to exit 66
  (EX_NOINPUT) with empty plain-text stdout, or with a
  `mos.error.v1` envelope under `--json`. The `unknown + exit 1`
  v2-era assertions are gone.

- **`mos_json_escape` and `mos_safe_ascii`** promoted from CLI
  internals to public C API. Useful to any C embedder doing their
  own output rendering; tested against 30 hostile-input fixtures
  including realistic terminal-injection payloads (ANSI color, OSC
  52 clipboard, title-bar manipulation, cursor-position-report).

PVD parser STRUCK entirely (same session it was promoted): the public
wrapper had zero consumers and the internal parser zero call sites once
the pread died — dead code by the project's own deny-list standard.
The deciding argument: anyone doing filesystem-level reads owns their
own parsing (DVD/BD readers are in makemkvcon territory with mature
implementations); mos ships no parsers for bytes it refuses to read.
Removed: public API + prototype, internal parser + MOS_ISO9660_VOLID_CAP,
three tests (197 -> 194), fuzz phase 7 + MOS_FUZZ_PVD knob + CI env
(4.4M -> 4.2M iterations, both audit seeds re-run clean). Stage 2
re-scoped to SCSI-command surfaces only: CD-TEXT (ReadTOC fmt 5),
capacity, DI raw fields; UDF AVDP walk and PVD timestamp struck as
consumer territory. Session note: the container reset mid-session and
reverted the working tree to a stale state; restored from the output
zip (second time the zip-per-batch habit has been the recovery path).

CLI human-output redesign agreed (doc/research/2026-06-10-cli-design.md):
drutil-inspired aligned blocks and list table at field parity with
--json, one vocabulary (JSON enum strings verbatim), list gains State
by default (the zero-command-list idea was a cost entry mistaken for
doctrine) with per-entry error containment, bsd carries the full
/dev/diskN node everywhere with the field renamed to bare `bsd`
(flag/field/column identity; non-null round-trip property; null for
empty drives by V-1), `#` column header, watch becomes NDJSON-only,
multi-drive default is EX_USAGE not first-burner magic. Process
adopted: fuzz only on parser changes, amalgamate only at tag time.
Reversed in review: list column header is `Index` (not `#`) — header
matches the flag name, same one-vocabulary rule as the enum strings.
Column/row priority decided: State first after Index, identity
(Vendor/Product/Rev) last, in both the list table and the status
block — answer before nameplate.
Human views render `_or_` as `/` (`empty/open`) via a second column in
the same strings.c table — single-source, reversible; JSON unchanged.
Slash display mapping struck (same review): enum strings are verbatim
in every view — as canon the slash breaks identifiers and filenames;
as display it adds transform code for an edge-case string. Zero
mappings is the rule.
Full canonical CLI mock set written across all verbs incl. future
(drive/metadata/tray): tray verbs are silent-success + post-action
state.v1 under --json (zero new schemas); metadata human TOC is a
summary line (full table is JSON-only); usage-failure mini-list reuses
the list formatter; current_profile_name stays verbatim (bd_rom).
Priority pass across all CLI mocks: five-tier doctrine (answer,
evidence, media, addressing, identity; mos drive inverts by design),
Sense moves adjacent to State, status/drive/metadata gain
Index/BSD/Registry rows (registry decimal = JSON), Bus Enc dissolved
into the AACS line (it is a feature-0x010D flag — grouping over
labeling), Captured -> Timestamp with human format (JSON keeps
RFC3339 captured_at). Schema consequences: state.v1 += registry_id +
index (closes the state<->event join-key gap and the --registry-id
discoverability hole), list.v1 entries += registry_id, metadata.v1
context += registry_id (no index in archival docs).
Positional subject adopted for single-subject verbs (mos status 2 /
disk4 / /dev/disk4; metadata, drive, tray likewise) — diskutil/
smartctl/systemctl convention; syntactic disambiguation (all-digits =
index, else bsd); flags remain; --registry-id flag-only.
--brief killed: one machine interface (JSON); the bare-word mode is
fully retired. CLI design closed — no open items.

CLI redesign IMPLEMENTED (compile-gated by macOS CI for the tools/
adapter layer; everything pure is container-verified): layout engine
src/mos_human.{h,c} with six golden-string tests pinning the design
mocks byte-for-byte (200 tests total); bsd carries the full /dev node
in every surface (mos_bsd_path_from_unit, mos_cli_bsd_path); state.v1
gains registry_id + index (registry captured at open via
IORegistryEntryGetRegistryEntryID, propagated env->result through the
pure core; index resolved by registry match); list is two-phase
(enumerate, then one proven probe per drive) with per-entry error
containment, new mos.list.v1 entry shape, human table and EX_USAGE
mini-list sharing one formatter; positional drive subject with
syntactic disambiguation; watch NDJSON end to end including error
envelopes (plain emitter deleted); usage text rewritten; README
Output/Usage rewritten to the new surface. Schemas + 49 fixture files
swept (bsd rename, /dev pattern, new required fields), validator
green. CLI script: legacy bsd_name negative inverted, five new tests
(positional grammar, watch-sans-json envelope), early-exit bug in the
script's own summary placement fixed.

Pre-tag mutation ritual executed and VENDORED (scripts/mutation-pass.py,
adapted from the fourth review's harness; re-anchored, extended with 13
mutants over the audit-session and CLI-batch code, exits nonzero on
survivors). First run: 21/23 killed, two survivors, both real and both
in the highest-value code: (1) the eject-set membership — dropping 0x03
from the live state_core gate survived because the exhaustive nub
checker verifies its own vendored predicate COPY, so live/checker
divergence was invisible to the suite; pinned by a four-key table test.
(2) the registry_id env->result copy-through could be zeroed silently;
pinned on success and no-lock paths plus the NULL-accessor contract.
Re-run: 23/23 killed, 202/202 tests, ASan clean.

Naming standardized to the Apple-canonical BSD vocabulary, recorded as
an AGENTS standard: bsd_unit (kIOBSDUnitKey; the -1 sentinel comment is
now one verbatim phrase in all four sites), bsd_name (kIOBSDNameKey,
mos_bsd_name_format), dev node (diskutil's term: mos_bsd_path_from_unit
-> mos_bsd_dev_node, mos_cli_bsd_path -> mos_cli_bsd_dev_node, the
envelope param bsd_path -> dev_node). bsd_path/bsd_number/"device path"
are banned synonyms. CLI layer unified under the mos_cli_ prefix
(mos_human_* -> mos_cli_human_*, MOS_HUMAN_DASH -> MOS_CLI_HUMAN_DASH).
Tests and mutation anchors swept; 202/202, mutation 23/23, cli/
syntax-checks clean under -Werror.

Drift sweep: frozen v0.4 schema shapes in the media-info design doc
updated to the decided bsd vocabulary (they still showed bsd_name);
STATUS gains a path note (pre-restructure entries reference
tools/mos.c verbatim) and a 2026-06-10 shipped section; live cli/
comments and schemas/README emit-code pointers repointed at cli/;
fuzz TOC phase renumbered 8->7 with the header list rewrapped; dist
regenerated (public header had changed since the last amalgamation).
Orphan check 202=202.

Fifth (Linux) review processed — twelve findings, all verified against
the current tree and resolved (F4, dist staleness, had already been
fixed by the drift sweep; the reviewed snapshot predated it):
- F1/F2/F3 BLOCKING, all real: the cli/ restructure half-landed its
  build-graph wiring — cli/human.c was in neither the mos_tests CMake
  target nor the CI strict-pure list, and test_human.c lacked the
  in-file _POSIX_C_SOURCE guard (masked by manual gcc builds that
  passed the define and human.c on the command line). Fixed; the
  formerly-dark gates now RUN IN-CONTAINER: cmake build + ctest green,
  the CI strict line executed verbatim green. Root-cause accepted:
  manual compile lines are not the build graph — after any build-graph
  change, run the graph.
- F5: mos_bsd_dev_node lacked the UINT32_MAX domain guard its sibling
  has, three doc sites claimed otherwise, and the test pinned the
  domain via a tiny buffer (truncation masking). Guard added, mos.h cap
  contract corrected (in-domain max 20 bytes; recommend 24), io.h/io.c
  rewritten (also closing F8's "diskN" wire-shape drift), test split
  into a true domain pin (ample buffer) + separate truncation pin, and
  the mirror mutant added — first run of the fixed test FAILED against
  the 16-byte buffer, re-teaching the reviewer's own arithmetic;
  mutation now 24/24.
- F6: the mutation baseline wrote a 172KB ELF (mt_tests) into the live
  tree, ungitignored and preflight-invisible — it shipped in the
  reviewed zip. Baseline now builds in a tempdir like every mutant;
  mt_tests gitignored and added to preflight; artifact removed.
- F7: unreachable duplicate --list+selector guard removed.
- F9 CORRECTION, accepted on the reviewer's runtime evidence: the nub
  checker DOES drive the live core (instrumented ops + transcription
  cross-audit, exit 2); the eject-set mutant survived because the
  checker runs in neither the suite nor the mutation harness, not
  because it is blind. Test comment and STATUS wording corrected; the
  fixture stays as the fast suite-side pin.
- F10/F11/F12a-d: stale path in test_human.c header, watch env-range
  unified (0..3600000, 0 = library default), May-2026 timestamp prose,
  orphaned resolve_index_of comment re-homed, amalgamate "hoists"
  wording, da_session failure-cause comment corrected (registration
  returns void).
- F12e: the notification probe's teardown gains the full
  unschedule/unregister(x3, real symbols + &ctx)/release sequence
  matching mos_watch.c — the first insertion was blind and wrong
  (invented callback name, duplicate unschedule); corrected against
  the actual registration site.
All gates re-run: cmake+ctest 202/202, strict-verbatim 202/202, ASan
202/202, mutation 24/24, dist regenerated, preflight (now mt_tests-
aware) and staleness green.
