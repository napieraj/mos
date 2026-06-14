# Media info: design and field→source matrix

2026-06-10. Motivating case: two identical drives are indistinguishable in
any `--list`-shaped output — mos reports drive state and `current_profile`
but nothing about the *disc*. Minimum viable target is `drutil status` /
`drutil discinfo` parity plus the one thing drutil doesn't show: the
volume name ("which movie is in which drive"). Known-good implementations
for cross-checking field semantics: `drutil` (the DiscRecording CLI),
`dvd+rw-mediainfo` (dvd+rw-tools), libcdio (`iso9660.h`,
`mmc.h`), MediaInfo. Per the AGENTS scope doctrine: **kernel-authored
convenience methods wherever one exists; raw CDBs only with GESN-grade
justification** — and it turns out the entire MVP needs zero raw CDBs.

## What ships in which stage

**Stage 0 — shipped with this note (pure, zero new wire traffic):**
`media_class` ("cd"/"dvd"/"bd"/"hd_dvd") derived from the
`current_profile` the state query already fetches (`mos_profile_class`,
totality-tested against the name table). Emitted in `mos.state.v1` and
`mos.event.v1` under the same 0x0000-suppression rule as
`current_profile_name`; negatives pin the closed enum and the
suppression. Also shipped: the ISO 9660 PVD volume-identifier parser
(`mos_internal_iso9660_volume_id`) — pure, hostile-input-hardened,
fuzzed — waiting for its adapter caller in stage 1.

**Stage 1 — REVISED (see the consolidated `mos.metadata.v1` design
below): the `media` object in `mos.state.v1` is WITHDRAWN** — it would
have put disc facts in two schemas. `mos.state.v1` gains only
`volume_name` (the human list-disambiguator) and `volume_path` (the
actionable form — `/Volumes/MY-ALBUM`, from
`kDADiskDescriptionVolumePathKey`, for piping straight into consumer
processes; both string|null, null when unmounted); everything else
moves to `mos.metadata.v1`. The original shape is kept below for the record of
what was considered:

```json
"media": {
  "class":         "bd",                  // = media_class, kept in both places? NO — see below
  "disc_status":   "complete",            // empty | appendable | complete | reserved
  "erasable":      false,
  "sessions":      1,
  "tracks":        1,
  "volume_name":   "ARRIVAL_4K_UHD",      // string | null
  "volume_name_source": "iso9660"         // "mount" | "iso9660" | null
}
```

Decision: `media_class` stays a top-level scalar (it ships now and is
cheap); the `media` object does NOT duplicate it. `volume_name: null`
with `volume_name_source: null` is the honest "media present, name not
determinable" answer (blank discs, pure-UDF discs until stage 2, audio
CDs until CD-TEXT).

**Stage 2 — deferred with named falsifiers:** UDF volume name (AVDP at
sector 256 → VDS → Logical Volume Descriptor; needed for most
DVD-Video/BD-Video, which often have no ISO9660 bridge PVD or a generic
one), CD-TEXT for audio (`ReadTableOfContents` format 5 — still a
convenience method), capacity/blocks (drutil's blocks used/free —
`ReadTrackInformation` or the DR status dict).

## Field → source matrix

| Field | Source (current architecture) | Authority for layout | DR-pivot equivalent (v0.4+) |
|---|---|---|---|
| media_class | `mos_profile_class(current_profile)` — pure, already-fetched | MMC-6 profile ranges | `kDRDeviceMediaClassKey` (CD/DVD/BD constants) |
| disc_status, erasable, sessions, tracks | **`ReadDiscInformation` convenience method** (`SCSITaskLib.h:802-825`); decoder ALREADY EXISTS (`mos_internal_disc_info_parse`, fuzz surface 5) — only the adapter call is missing (the `mos_state.c` TODO) | MMC-6 READ DISC INFORMATION (cross-check: `dvd+rw-mediainfo`'s disc-info block) | `kDRDeviceMediaSessionCountKey`, `kDRDeviceMediaTrackCountKey`, `kDRDeviceMediaIsBlankKey`, `kDRDeviceMediaIsErasableKey` |
| volume_name (mounted) | **DiskArbitration**: `DADiskCopyDescription` → `kDADiskDescriptionVolumeNameKey` on the IOMedia child. DA is ALREADY a link dependency (the watch's wake filter), so this adds no framework. | DA headers | same (DA is orthogonal to the DR pivot) |
| volume_name (present, unmounted) | **CONSUMER-SUPPLIED — structural boundary, not a fallback.** This is the COMMON case for the rip workload (UDF BD/UHD video often does not auto-mount; MakeMKV prefers unmounted). The volume name lives in disc SECTORS, and all three routes to those sectors on an unmounted disc are out of mos's scope: DA carries it only when mounted; DR's media-info dict is physical-facts-only (BSD name, blocks, counts, class — header-verified, NO volume/filesystem key); reading the sectors needs either a block-device pread (third I/O modality + root/TCC) or a raw READ(10) (the exclusive lock §5.5 forbids on nub-present media). So mos does NOT read it — and the consumer that needs it (gripperoni/MakeMKV) is ALREADY reading those sectors with the privilege mos refuses to take. And the consumer owns the PARSING too, not just the read (decided 2026-06-10, superseding the same-day public-parser idea): anyone doing filesystem-level reads implements their own PVD/UDF handling — for DVD/BD you are in makemkvcon territory already, with mature parsers in hand. mos ships no parser for bytes it refuses to read. | ECMA-119 §8.4 / UDF; libcdio cross-check | n/a — mos does no PVD/UDF sector I/O in any era |
| capacity blocks (stage 2) | `ReadTrackInformation` convenience, or defer to DR | MMC-6 | `kDRDeviceMediaBlocksUsedKey` / `kDRDeviceMediaBlocksFreeKey` / `kDRDeviceMediaBlocksOverwritableKey` |
| CD-TEXT (stage 2) | `ReadTableOfContents` convenience, format 0x05 | MMC-6 §6.26; cross-check libcdio `cdtext.c` | n/a (DR exposes burn-side CD-TEXT only) |
| book type / hybrid details (non-goal) | `ReadDVDStructure` convenience exists, but drutil-parity doesn't need it | — | — |

## Invariants the wiring must hold

1. **No raw CDBs.** Every row above is a convenience method, a DA call,
   or a block-device read. If stage 2 ever appears to need a raw CDB,
   that proposal goes through the AGENTS raw-verb rule (documented
   information-destruction showing + nub analysis).
2. **No lock.** Media info is fetched only on READY — the kernel nub
   exists, the volume may be mounted, and nothing here needs (or may
   take) exclusive access. The enrichment path and the §5.5 invariant
   never meet.
3. **Dual-length rule** (contract O-4) on every variable-size reply:
   `ReadDiscInformation` and the PVD pread both compute their parse
   bound via `mos_internal_trusted_len`.
4. **Failure is null, not error.** Media-info enrichment failing (drive
   won't answer disc info; PVD absent; volume unmounted and unreadable)
   degrades the `media` object fields to null/absent — it never changes
   `state` and never converts a successful state query into an error.
   Same doctrine as profile enrichment.
5. **Name precedence: mount wins.** A mounted volume's DA name reflects
   what the user sees in Finder (Joliet/UDF-aware); the PVD is the
   fallback for present-but-unmounted. `volume_name_source` records
   which path answered so fixtures can pin both.

## Disc identity: `mos.metadata.v1` (consolidated design)

Decisions taken 2026-06-10 after the fingerprinting research review:

**One document, not two.** The TOC lives ONLY inside `mos.metadata.v1`
(required, nullable); the standalone `mos.toc.v1` plan is withdrawn
before shipping. Rationale beyond convenience: the dedup key is the
canonical serialization of the identity material, and a key split
across two documents forces every consumer to define a concatenation
order — a consumer-side canonicalization spec, which inverts the
division of labour (mos owns serialization; consumers own hashing). One
document also removes the toc-shape drift axis entirely, and matches
house practice: schemas answer questions (state.v1 is already a TUR +
GESN + profile + INQUIRY composite), they do not mirror wire structures.

**No disc facts in two schemas.** The same razor kills the earlier
stage-1 plan of a `media` object inside `mos.state.v1`: it would have
duplicated `metadata.disc`. Division of labour now: `mos.state.v1` =
"what is the drive doing" plus two cheap labels (`media_class`,
shipped; `volume_name`, stage 1 — the original identical-drives list
ask). `mos.metadata.v1` = "what disc is this", the on-demand identity
record behind `mos metadata [--bsd diskN] --json`.

**Scope boundary (the mos-vs-makemkvcon line).** mos emits BYTES it
read and HASHES of bytes it read — nothing else. In scope: the faithful
TOC decode, PVD fields, READ DISC INFORMATION fields, profile/class,
and `mos-*-sha256-v1` content hashes over those (deterministic,
spec-grounded, reproducible Mac↔Linux, no vendored third-party crypto).
Out of scope, permanently: every NAMED third-party identity —
MusicBrainz disc id (libdiscid base64 + the +150 frame convention),
AccurateRip ids, dvdid (patent-specified degree-64 GF(2) CRC — note the
research doc's own warning that the polynomial is NOT CRC-64-ECMA),
BDMV structure hashes (reimplementing MakeMKV's proprietary disc hash
guarantees drift). All of those are pure functions of fields
`mos.metadata.v1` carries, so ripperoni computes them consumer-side
from the document; a MusicBrainz algorithm change never touches mos.

**LibreDrive is a MakeMKV property, not a drive property.** Forum
evidence settles it: a drive can report "Status: Enabled" on ORIGINAL
unpatched firmware, and enablement is gated by MakeMKV's per-drive
support database with no user switch — the status reflects MakeMKV's
RAM-loaded extension and its own database, none of it visible through
any SCSI command. mos therefore does NOT emit a libredrive field.
What mos CAN honestly surface, all spec-grounded GET CONFIGURATION /
disc-structure facts (the same bits MakeMKV's own drive dump shows as
"Bus encryption flags" / "Highest AACS version"): the AACS feature
(0x010D) presence and version, bus-encryption capability, and the
read-feature set. Those land in `drive.capabilities`; consumers
correlate with their own MakeMKV knowledge if they want regime
inference. Regime stamping of a fingerprint record is consumer-side
context, recorded outside mos.

### The schema taxonomy (revised: drive facts leave metadata entirely)

The governing rule, made explicit after review: **a schema's fields
belong to its SUBJECT plus addressing context, nothing else.**
`mos.state.v1` carries vendor/product legitimately — its subject is the
drive. `mos.metadata.v1`'s subject is the DISC, so drive facts are
foreign there; the earlier `drive` block was context-stamping smuggled
in as data, and it is withdrawn. The full taxonomy:

| Schema | Subject | Question | Cost (post-DR-pivot) |
|---|---|---|---|
| `mos.list.v1` | drive set | what drives exist | **zero commands** — `DRDeviceCopyInfo` is kernel-cached: vendor, product, firmware revision, physical interconnect (+ the write-capabilities dict) all come from the framework without an open or an MMC round-trip |
| `mos.drive.v1` (NEW, stage 1) | one drive, static | what IS this drive | one open — for exactly the TWO fields DR does not cache: the INQUIRY unit serial and the AACS/bus-encryption capabilities (GET CONFIGURATION feature walk). DR has no serial key and predates AACS. |
| `mos.state.v1` | one drive, dynamic | what is it DOING | one open (hot path — static capability facts deliberately excluded) |
| `mos.metadata.v1` | one disc | what disc is this | one open + media reads (all convenience) |
| `mos.event.v1` | one drive over time | state changes | watch |

Post-pivot, `mos list` therefore shows full drive identity for free, and
the command-issuance picture across the whole tool collapses to the
scope doctrine's cleanest form: enumeration issues NOTHING, everything
else is kernel-authored convenience, and the single mos-authored CDB in
existence remains the raw GESN — the expensive tray command, justified
in ARCHITECTURE §9.7, taken only under the §5.5-proven gate.

**Enumeration-completeness: moot, three times over.** `DRDevice`
historically does not surface non-burner drives (CocoaDev) — and it
does not matter, because the non-burner case is already excluded at
every layer above and below: (1) the market — pure optical readers are
unobtainable in 2026, writers themselves being a historical artifact;
(2) the kernel — the ARCHITECTURE §9.1 attach rule blocks
SCSITaskUserClient on read-only drives ("if the drive is a read-only
device... access is blocked"; the policy MakeMKV's developer famously
attributed to Cupertino's herbal supply), so mos could not OPEN a pure
reader even if it enumerated one; (3) DR itself — which is not a quirk but nomenclature: the framework
is named Disc RECORDING; a burn engine has no business enumerating
drives that cannot record, and reading its burner-only device array as
"historical blindness" was parsing the behavior while ignoring the
name. The consequence is an alignment, not a limitation: `DRCopyDeviceArray`'s burner filter
approximately EQUALS the SCSITaskUserClient-openable set, so the DR
pivot's enumeration boundary coincides with the capability boundary
mos already lives inside. No falsifier owed; burner-only is the entire
reachable universe.

Net schema count unchanged: `mos.toc.v1` died, `mos.drive.v1` is born.
The drive↔disc JOIN is consumer-side, per doctrine — the pipeline that
loaded the disc knows which drive it used; mos answers the two
questions separately. Durability note for that join: `registry_id` is
ATTACHMENT identity (replug = new ID by construction), the wrong
durable key for a drive inventory. The INQUIRY unit serial in
`mos.drive.v1` is the key that survives replug and machine moves —
that is how a Mac-probed record and a Hetzner-probed record agree on
"same physical drive."

### `mos.drive.v1` (lands WITH the `mos drive` verb, stage 1)

```json
{
  "schema":      "mos.drive.v1",
  "bsd":         "/dev/disk4",
  "registry_id": 4295021843,
  "vendor":      "HL-DT-ST",
  "product":     "BD-RE WH16NS60",
  "revision":    "1.00",
  "serial":      "M63IBOA5100",
  "capabilities": {
    "aacs":           true,
    "aacs_version":   68,
    "bus_encryption": true
  }
}
```

`serial` is nullable (INQUIRY unit-serial VPD page 0x80 — verify at
stage 1 whether the convenience InquiryDevice surfaces VPD pages or
this needs the standard-INQUIRY serial fallback; if a raw INQUIRY were
ever required it goes through the AGENTS raw-verb rule).
`capabilities` carries the spec-grounded GET CONFIGURATION facts only —
the AACS feature (0x010D) presence and version and the bus-encryption
capability bit: the same facts MakeMKV's drive dump displays, without
the MakeMKV-only LibreDrive status synthesis.

### `mos.metadata.v1` (lands WITH the `mos metadata` verb, stage 1)

Pure disc document. Everything outside `disc` is addressing/capture
context; `disc` is THE FINGERPRINT SUBTREE — consumers hash its
canonical serialization (or a class-appropriate subset such as
`disc.toc` alone for audio CDs; match policy is theirs). Identity
fields are REQUIRED AND NULLABLE, never optional — a fixed key set
keeps the canonical form trivial.

```json
{
  "schema":      "mos.metadata.v1",
  "bsd":         "/dev/disk4",
  "captured_at": "2026-06-10T14:32:00Z",
  "volume_path": "/Volumes/MY_MOVIE_UHD",

  "disc": {
    "profile":   "0x0040",
    "class":     "bd",
    "toc":       null,
    "disc_info":   { "status": "complete", "erasable": false,
                     "sessions": 1, "tracks": 1 },
    "volume_name": null
  }
}
```

`disc.toc` for a CD is the shape already implemented by
`mos_internal_toc_parse`:

```json
"toc": {
  "first_track": 1, "last_track": 12, "leadout_lba": 210895,
  "tracks": [
    { "track": 1, "adr": 1, "control": 0, "data": false, "start_lba": 0 }
  ]
}
```

`leadout_lba: null` (TOC without lead-out) means "no identity — do not
dedup on this"; a synthesized single-data-track DVD/BD TOC is emitted
faithfully but is documented as identity-useless (the research doc's
"never key on it"), which is why `disc.volume_name` (DA-sourced, mounted only) and stage-2
SCSI-derived fields (DI geometry) carry DVD/BD identity when present;
sector-derived identity is consumer-side.

### The v0.4 signal stack and probe ladder (design, decided 2026-06-10)

Four signal layers, each answering at a different level, ALL consumed
before any MMC command:

| Layer | Source | Delivery | Answers | Cost |
|---|---|---|---|---|
| Drive | DR `kDRDeviceStatusChangedNotification` (payload = the CopyStatus dict: `kDRDeviceMediaStateKey` present/in-transition/none, `kDRDeviceIsTrayOpenKey`) | push, payload included | tray + media presence + transitions, as the KERNEL's GESN-fed snapshot ("not guaranteed current") | zero |
| Filesystem | DA mount/unmount/appeared/disappeared callbacks | push | mounted ⇒ nub + readable; volume name | zero |
| Contention | `IsExclusiveAccessAvailable` (`GetUserClientExclusivityState`) | poll, no command | client-contention BUSY (another rip/burn holding the lock) — NOT device-internal busy | zero (ioctl) |
| Identity | DR/IOKit kernel cache | cached | vendor/product/revision/interconnect | zero |

Probe ladder built on the stack:

1. Mounted + exclusivity available → `ready`. Zero commands.
2. Exclusivity held → `busy` (contention kind). Zero commands.
3. Unmounted-but-present, or mounted with a transition signal →
   convenience TUR (unmounted ≠ not-GOOD: unmountable-FS data discs are
   GOOD; and DA/exclusivity cannot see device-internal busy — spin-up,
   seek, format). One kernel-authored command.
4. Not-ready AND the tray bit is the question (the 3A fork) → the raw
   GESN under the lock. The ONLY mos-authored command, on the rarest
   branch.

Trust edges, to land as seam-contract clauses with Mac checks when this
is implemented: (a) exclusivity state and DA mount state are SNAPSHOTS
— a one-shot `ready`/`busy` from them is "as of the last platform
event"; the watch self-corrects next tick; (b) tier 1 trades a rare
false-`ready` (mounted disc momentarily device-busy) for the
command-free path — acceptable for state reporting, and the metadata
verb's reads will surface a busy drive naturally; (c) DR's tray bit is
a stale-able cache with the GetTrayState masking lineage — it may
PRE-ANSWER, it never OVERRULES a fresh mos GESN (the existing
"GESN open/closed is never overturned" contract, extended to the DR
snapshot).

**The niche, stated exactly:** post-pivot, mos's common path is honest
synthesis of platform signals with their staleness stated; mos's
irreplaceable path is the empty-vs-open disambiguation — the
authoritative, failure-honest, current-this-instant tray bit via the
one CDB mos authors, under the exhaustively-proven §5.5 gate. drutil
does not offer it; DR explicitly disclaims currency; the kernel's own
convenience fabricates on failure. Rare, load-bearing, and the lock is
now held only on that rarest branch — which shrinks §5.5's residual
timing sliver from every-poll exposure to the occasional
disambiguation moment.

### Staging

- Stage 1 (`mos metadata` + `mos drive` verbs): ReadDiscInformation
  (decoder shipped), ReadTableOfContents → toc (parser shipped), DA/PVD
  volume fields (parser shipped) for metadata; GET CONFIGURATION RT=0
  capabilities walk (config walker shipped; O-4 bound) + INQUIRY serial
  for drive. Both schema files + fixtures land here.
- Stage 2 (SCSI-command surfaces only — sector-derived items struck
  with the parser): CD-TEXT via ReadTableOfContents format 5
  (convenience), capacity, DI raw fields for BD/UHD discrimination
  (UHD reports profile 0x0040 identically to BD — tool-observed;
  detect defensively).
- Never: named third-party ids, AACS-tier reads, BDMV filesystem
  hashing, libredrive status synthesis.

## v1 shippable gate impact

`media_class` is in the v1 schemas now (closed enum, suppression guard,
two negatives) and costs nothing on the wire — it ships with v0.3. The
`media` object is a stage-1 schema addition that lands ONLY with its
emitter (schemas-win doctrine: no speculative fields); since schemas are
mutable until first tag, the gate question is whether v0.3 tags before
stage 1 lands. Recommendation: tag v0.3 with `media_class` only — the
identical-drives case gets its first-order answer (class), and the
volume name follows in v0.3.x/v0.4 with the adapter wiring, Mac-smoke
items (DA name on a mounted disc; PVD name on an unmounted data disc;
null on a blank), and fixtures from real media via `mos_probe`.

## Addendum 2026-06-12 — stage-1 reconciliation + canonical-implementation research

The 06-10 design above predates two tree changes and is now
reconciled against them plus a research pass over the named
known-good implementations (session of record: the stage-1 kickoff).

**Decisions taken (maintainer + session):**

1. **Volume name/path source: DiskArbitration, re-admitted narrowly.**
   The 06-11 DA retirement was about the watch's wake callbacks and
   the probe's control arm — neither returns. The metadata path gets
   a ONE-SHOT, callback-free `DADiskCopyDescription` lookup
   (VolumeName/VolumePath keys), gated on the media nub existing
   (bsd_unit present; no IOMedia → fields null, DA never called).
   Requires the dated AGENTS.md append when the wiring lands.
2. **Verbs are `mos metadata` and `mos drive`** (per the taxonomy
   above); the reserved-subcommand list's `identity` is their fossil
   and retires when they land. `capacity/tray/speed/features` stay
   reserved.
3. **Serial ships null in stage 1** with the VPD-0x80-vs-convenience
   question as a recorded Mac falsifier. A raw INQUIRY would need the
   AGENTS raw-verb showing; stage 1 does not attempt it.
4. **TOC route stays convenience `ReadTableOfContents`** (format
   0000b, LBA), parsed by the shipped `mos_internal_toc_parse`.
   Falsifier noted: libcdio's macOS driver reads the kernel's
   full-TOC blob from `kIOCDMediaTOCKey` (zero commands, CD-only,
   different wire shape — POINT descriptors, A0/A1/A2, MSF); if the
   convenience RTOC misbehaves on real hardware, that property is
   the kernel-authored fallback to evaluate.

**Research findings (full agent reports in the session transcript):**

- **READ DISC INFORMATION byte map confirmed five ways** (dvd+rw-
  mediainfo, Linux kernel uapi, QEMU scsi-disk, libburn, systemd
  cdrom_id): status `b2&3`, last-session `(b2>>2)&3`, erasable
  `b2&0x10`, sessions `9<<8|4`, tracks `10<<8|5` / `11<<8|6`,
  declared length excludes itself (+2). Zero disagreement;
  `mos_discinfo.c` matches and is stricter than every reference
  (dvd+rw trusts fixed offsets in a 32-byte alloc; libcdio decodes
  only the erasable bit and never reads the reply length).
- **Schema-prose obligations:** disc_status `11b` ("other") is the
  NORMAL regime for DVD-RAM/BD-RE random-access media (libburn
  pivots on it; dvd+rw-mediainfo suppresses DVD-RAM per-track info
  as meaningless). Document, don't special-case.
- **TOC defenses to pin in fixtures** (libcdio osx/gnu_linux
  drivers): skip `adr != 1` descriptors; bound tracks to [1,99] and
  clamp counts; first/last by min/max scan, not header trust;
  missing lead-out = identity loss (the existing `leadout_lba:
  null` rule); reject `next_lba <= start_lba`.
- **Validation posture confirmed** (dvd+rw-mediainfo + libcdio
  converge): two-phase fetch at the drive's declared length, clamp
  to local buffer, zero-init reply buffers, bail on arithmetic
  impossibilities, whitelist-and-default enums, NO per-vendor
  branching (sole exception in 25 years of dvd+rw-tools: a Sony TOC
  control-byte bit). This is the dual-length/GESN doctrine already
  in force; no posture change.
- **MediaInfo is file-only** (no SCSI/ioctl/device code in either
  MediaArea repo; optical = parsing IFO/BDMV/ISO bytes off a mounted
  filesystem) — a living example of the consumer-side boundary the
  matrix's volume-name row draws.
- **Stage-2 notes banked:** BG format status (RDI byte 7 & 3, with
  the REQUEST SENSE progress-percent read when "in progress");
  CD-TEXT structural gates (18-byte packs, multiple-of-18 length,
  block count ≤ 8, charset codes lie — libcdio treats declared-ASCII
  as ISO-8859-1; CRC bytes present but unverified in libcdio);
  book-type discrimination via READ DVD STRUCTURE format 00h when
  profile alone is ambiguous.

**Correction (2026-06-12, same session):** the original stage-1 sketch
above illustrates `volume_path` with `/Volumes/MY_MOVIE_UHD` — a
mounted UHD title. Per this doc's own matrix row ("UDF BD/UHD video
often does not auto-mount; MakeMKV prefers unmounted"), that is not
the typical reading: BD/UHD video normally presents as
media-present-unmounted on macOS, so the volume fields read null and
identity rides on profile/TOC/disc-info. The shipped fixtures and
README examples reflect this — the flagship `mos.metadata.v1` example
is an unmounted UHD BD; the mounted examples are a DVD-video volume,
an audio CD (macOS's generic "Audio CD" label), and data discs, which
genuinely mount. The historical sketch above is left as written.

**M-DISC (2026-06-12, same session):** added as the fixture-pinned
archival example (`mos.metadata.v1.mdisc_archive`,
`mos.state.v1.mdisc_archive_mounted`) — deliberately: the volume
surface is not exclusively ripping-adjacent, and archival burns are
the canonical mounted-data case. Handling claim: at the MMC level
M-DISC is profile-transparent — the BD flavor reports standard BD-R
SRM (0x0041, already in the profile/class tables), and the
M-DISC-ness lives in disc-structure manufacturer/media-ID fields mos
does not read (READ DISC STRUCTURE is stage-2 banked, book-type
note above). So M-DISC is handled by construction today: state,
profile, class, TOC, disc_info, and the mounted volume name all
behave as for any BD-R. A hardware falsification pass should include
one finalized M-DISC alongside the pressed/UHD/audio matrix rows.

**TOC-on-DVD/BD verified (2026-06-12, same session):** challenged in
review ("does TOC even look like that on anything non-CD?") and
checked against canonical sources rather than this doc's own claims.
Confirmed: the single-synthesized-track shape is NORMATIVE, not a
drive courtesy. MMC-5 table 483 ("Fabrication of TOC Form 0 for
Single Session DVD") mandates track 1 @ LBA 0 plus an AAh lead-out
for DVD; the Mt. Fuji companion (INF-TA-1010, the spec vendors
actually implement) carries an explicit "Detail of BD media
fabricated READ TOC response" section, so BD fabrication is normative
too (section title verified; the table itself is member-gated).
Corroborated: QEMU's cdrom_read_toc synthesizes identically for
CD/DVD; udev's cdrom_id issues READ TOC unconditionally on every
medium; libcdio-devel (T. Schmitt) treats the lead-out as a size
query on DVD and BD alike. Two wrinkles for consumers, now in the
schema prose: (1) on overwritable media (DVD+RW/BD-RE) some
firmwares report formatted capacity rather than written extent in
the fabricated lead-out — not a precise data-size source (libburn
prefers READ DISC/TRACK INFORMATION for sizes); (2) blank CD-R/RW
rejects format 0 by spec, blank DVD/BD recordables are
drive-dependent — toc:null on blanks is expected output, not a bug.
No UHD-specific READ TOC failures found (MakeMKV-forum failures are
AACS sector reads, not TOC). The identity rule is unchanged and
exactly right: never key on a synthesized TOC; mos_toc_have_leadout
gates identity use.

## Log-derived fixture batch 2 + provenance correction (2026-06-13)

Second Phase-A0 pass (forum/list captures the first agent couldn't
reach; user supplied the page text). Three outcomes — one correction,
one new fixture, one piece of by-construction evidence.

**Correction — "Highest AACS version" is not the descriptor byte.**
The batch-1 fixture `getconfig_aacs_wh16ns40.bin` listed payload
byte 3 = 78 as ATTESTED, sourced from MakeMKV's "Highest AACS
version: 78" line. That is wrong provenance: forum t=6685 demonstrates
the number is a MakeMKV-local statistic (the highest saved MKB-dump
file — delete one and the displayed value drops), and moderator
Woodstock states it is "rather than being read from the drive"
(t=14372, t=17356). The two identical-BU40N dumps showing 77 vs 81
corroborate (MKB history differs, hardware does not). The DESIGN is
unaffected — descriptor byte 3 IS the AACS Version per libaacs/
UDFclient, and `mos_query_drive_caps` decodes that byte correctly —
but no MakeMKV dump attests it, so the fixture's byte 3 is now marked
illustrative scaffold and the load-bearing assertion is bus_encryption
(byte 0 = 0x17, the one attested descriptor byte). Fixtures README and
the mirroring test corrected the same day. The raw-descriptor-vs-dump
capture stays on the falsification matrix; its job is now to pin
byte 3, which nothing in the wild does.

**New fixture — first erasable=true RDI.** `readdiscinfo_complete_bdre.bin`
from a verbatim BDR-209D dump (status complete, 1 session, 1 track;
erasable inferred from the 43h BD-RE rewritable profile, since
dvd+rw-mediainfo never prints the bit). Its `FABRICATED TOC` block is
the drive's real READ TOC reply (confirmed against
dvd+rw-mediainfo.cpp L1048) and shows the lead-out at the disc's
formatted capacity, not a written extent — live corroboration of the
overwritable-media lead-out wrinkle already in the leadout_lba prose.

**M-DISC by-construction claim — now evidenced.** A verified xorriso
capture (cdwrite list, msg14517) of an M-DISC BD-R in an HL-DT-ST
WH16NS40: `Media product: MILLEN/MR1/0 , Millenniata Inc.`,
`Media current: BD-R sequential recording`, `Media status: is blank`,
12219392 writable blocks. This confirms what the design doc asserted
from spec: M-DISC BD presents as ordinary BD-R SRM (profile 41h) at
the MMC command layer — state, profile, class, disc-info, and the
fabricated TOC all behave as for any BD-R. The M-DISC-ness lives in
the `MILLEN/MR1` manufacturer/media ID (disc-structure data), not in
the profile or disc-info mos already reads. Whether that ID — or a
drive-side M-DISC write capability — is reachable through an MMC read
mos could honestly surface is under active research (see the
stage-2 M-DISC question); xorriso prints the ID from a real read, so
SOMETHING carries it. Still-open gaps from this batch: the M-DISC
**DVD** profile (11h DVD-R vs 1Bh DVD+R — no captured Mounted Media
line found; Schmitt asserts "DVD+R or BD-R" but unverified), and an
**appendable** BD-R RDI (no verbatim capture surfaced — the only
candidate was a fetch-blocked Launchpad snippet).

## M-DISC: surfaceable, as a registered manufacturer ID (2026-06-13)

Closes the open M-DISC-reachability question from the batch-2 entry.
Three converging sources (dvd+rw-mediainfo.cpp verified locally,
dvdisaster scsi-layer.c, and the Blu-ray Disc Association licensee
registry) settle it.

**Where M-DISC lives.** Not in any read mos does today, and not as a
flag bit. It is the disc's **Disc Manufacturer ID + Media Type ID**,
read via **READ DISC STRUCTURE (0xAD)**, BD media type, format 0x00
(Disc Information / DI): manufacturer ID = 6 bytes at DI offset +100,
media type ID = 3 bytes at +106 (verified at dvd+rw-mediainfo.cpp
L352, `Media ID: %6.6s/%-3.3s` from `di+4+100`/`di+4+106`). For
Millenniata M-DISC BD-R these are the REGISTERED values **`MILLEN`** /
**`MR1`** (BDA licensee list; full media code `MILLEN-MR1-000`) —
exactly the `MILLEN/MR1/0` xorriso printed from the verified capture.

**It is the disc that self-identifies, not the drive.** There is NO
drive-side "M-DISC write capable" advertisement in any standard MMC
read — no GET CONFIGURATION feature, no mode page (verified: dvd+rw-
tools, cdrtools, k3b all treat M-DISC as ordinary recordable media and
none contain an mdisc/millenniata string). The drive selects its
higher write strategy FROM the media ID it reads, not from a
capability bit it exposes. This resolves the "the capability must
present itself somewhere" intuition: it presents as DISC identity
(manufacturer ID), never as drive capability.

**Detection is a string match, not a bit.** `manufacturer_id ==
"MILLEN" && media_type_id == "MR1"`. Even dvdisaster does not
special-case it — M-DISC awareness is a layer the consumer adds.

**Doctrine fit — surface the ID, not an `mdisc` boolean.** The
correct shape under the scope doctrine is for mos to emit the
**registered manufacturer/media-type ID fields** (a faithful read of
a spec-defined structure) and leave the `MILLEN -> M-DISC`
interpretation consumer-side — the same division as MusicBrainz ids
and the LibreDrive status (mos emits bytes it read; consumers
interpret). A hardcoded `mdisc: true` that string-matches MILLEN
inside mos is the vendor-string special-casing the doctrine forbids.
Bonus: the manufacturer ID is useful well beyond M-DISC — it names
the actual disc maker (CMCMAG, RITEK, VERBAT, MILLEN...), a real
quality/dedup signal, which is why surfacing the general field beats a
single-purpose M-DISC flag.

**Cost / gating for a stage-2 increment.**
- Command: 0xAD READ DISC STRUCTURE. This entry's design doc table
  already notes a `ReadDVDStructure` convenience method exists, so the
  likely cost is a layer-1 convenience call, NOT a raw CDB — SDK-verify
  the exact MMCDeviceInterface selector before building; if absent, it
  needs the AGENTS raw-verb showing.
- BD path is clean (registered MILLEN/MR1 at fixed DI offsets).
- DVD M-DISC is NOT clean: no BD-style DI unit, the manufacturer ID is
  scattered across format-specific offsets (DVD-dash media-ID format
  0x0E at +17/+25; DVD-plus ADIP format 0x11 at +23/+31), and M-DISC
  DVD lacks the tidy MILLEN/MR1 pair. BD-first; DVD deferred until a
  real M-DISC DVD capture pins it.
- Fixture path: a real READ DISC STRUCTURE DI capture (or a reversed
  verbatim log) becomes the committed fixture; the BD DI offsets are
  the parse target.

## READ DISC STRUCTURE shipped — disc_structure identity (2026-06-13)

Implemented the M-DISC resolution above as a general field, per the
"surface everything the command returns" steer (not tunnel-visioned on
the M-DISC angle). `mos_query_disc_id` issues READ DISC STRUCTURE via
the Apple `ReadDiscStructure` convenience method (SDK-verified
signature: MEDIA_TYPE/ADDRESS/LAYER_NUMBER/FORMAT + buffer; the generic
MMC-5 wrapper, not the DVD-only `ReadDVDStructure`), BD media type,
format 0x00 (Disc Information). The pure decoder (mos_discstruct.c)
extracts the four confirmed ASCII/enumerable DI fields — Disc Type
Identifier (offset 8: BDR/BDW/BDO), Disc Manufacturer ID (100), Media
Type ID (106), Product Revision (111) — and deliberately skips the
physical write-parameter region (11..99), whose sub-field packing was
not multiply-confirmed.

Security posture (the device controls both length and bytes): the
identity fields are read at CONSTANT offsets inside the FIRST DI unit
only — no walking a device-controlled chain, so no payload value ever
becomes a read offset; the one device length (Disc Structure Data
Length) can only SHRINK the trusted region via the dual-length clamp;
no payload byte is dereferenced as a pointer; and the adapter uses a
single fixed zero-init buffer (no two-phase read-the-length-then-
reallocate). Gated by 3M-iteration exact-allocation ASan/UBSan fuzzing
(fuzz_pure phase 8) plus inline hostile-buffer tests.

Surfaced in mos.metadata.v1 as a nullable `disc_structure` sub-object
(mirrors disc_info), gated on a BD profile in the verb. Classification
stays consumer-side (MILLEN => M-DISC), per the doctrine. DVD-side
manufacturer ID (formats 0x0E/0x11, scattered offsets, no clean
MILLEN/MR1 pair) remains deferred until a real M-DISC DVD capture.

## CD-TEXT shipped — album-level disc.cdtext (2026-06-14)

Implemented the stage-2 CD-TEXT item from the staging list above as the
first deliberately-bounded slice: the disc-level (album) **Title** and
**Performer**. Motivating gap: macOS labels every audio disc the generic
`Audio CD` (the audio_cd fixture's volume_name), so before this an audio
CD had no human identity at all — only a synthesized TOC, which is the
fail-closed dedup primitive but not a name a person reads. CD-TEXT is
that name.

**What ships.** `mos_query_cdtext` issues READ TOC/PMA/ATIP **format
0101b** via the same non-exclusive `ReadTableOfContents` convenience
method the format-0000b TOC uses (layer-1 preferred form — kernel-
authored, no raw CDB, the one-raw-CDB count stays at GESN + the two tray
opcodes). The pure decoder (`mos_cdtext.c`) reconstructs the track-0
(album) Title (pack 0x80) and Performer (pack 0x81) of the **first
language block** (block 0). Surfaced in `mos.metadata.v1` as a nullable
`disc.cdtext` sub-object (`{title, performer}`, each required-and-
nullable), gated on a `cd` profile class in the verb. Byte layout cross-
verified against libcdio `lib/driver/cdtext.c`: 18-byte packs after a
4-byte header, byte3 = `DBCC<<7 | block<<4 | char_pos`, text bytes
[4..15] with NUL separating per-track strings, 8 language blocks.

**Scope boundary — the load-bearing half.** This is deliberately NOT a
full CD-TEXT decode, and the omissions are the decision future instances
will want to "complete":
- **Album-level only.** Per-track titles are not decoded. The album
  Title/Performer answer "which disc"; per-track text is a 99-entry
  array this slice does not carry.
- **Two field types only** (Title, Performer). Songwriter / composer /
  arranger / message / genre / ISRC / UPC / disc-id packs are skipped.
- **Block 0 only.** Additional language blocks (1..7) are ignored — the
  first block is the default/primary language.
- **Single-byte charset only.** A double-byte (DBCC) album field reads
  as absent (null), never mis-decoded as Latin-1. Transcoding MS-JIS /
  16-bit is not attempted; the CLI escapes single-byte bytes at emit
  (mos_safe_ascii), same as the volume name and INQUIRY identity.

**Why best-effort, not fail-closed.** Unlike the TOC (whose parser
rejects any incoherent table because a half-parsed fingerprint would be
falsely stable), CD-TEXT here is **display text**, not a dedup key. A
malformed or truncated reply degrades to a bounded, escaped, possibly-
empty string — `have=false` → the verb reports null, same convention as
the other media reads. Audio-CD identity/dedup rides on `disc.toc`, the
fail-closed primitive; CD-TEXT only puts a human name beside it. This is
why the decoder trusts pack order and does not reject on disorder.

**What hardware can falsify (not establish), per the hardware-role ADR.**
There is no in-repo CD-TEXT capture yet — the unit tests build spec-
derived packs (`tests/test_cdtext.c`), and the no-OOB/termination
property is fuzz-gated (`fuzz_pure` CD-TEXT phase, 3M iters ASan/UBSan
clean). A real audio-CD-with-CD-TEXT reply is a fixture-acquisition
target: it lands as a committed `.bin` with a dated fixtures README
entry, and the decoder is built to it — a surprise never becomes a per-
drive special-case. Specific falsifiers a rig run could surface: a drive
that returns CD-TEXT with the album string starting at a non-zero
char_pos in the first pack; a block-0 charcode the per-pack DBCC bit
disagrees with (the 0x8F Block Size pack, not parsed here, is the
authority and would become the gate if observed); firmwares that reject
format 0101b on a CD-TEXT-bearing disc (toc-style format support varies).

**Pre-first-tag schema mutability used.** `disc.cdtext` landed directly
in `mos.metadata.v1` (closed key set extended, additionalProperties
false), with schema + 7 examples + a new `cdtext_missing_field` negative
+ emitter + README updated in one commit — the JSON-schema ADR's mutable-
in-place clause (no external consumers before the first tag). The freeze
applies at the first tag like every other field.

**Remaining stage-2 after this:** BG format status (RDI byte 7 & 3 +
REQUEST SENSE progress), and — within CD-TEXT itself — per-track titles,
the other field types, and multi-language blocks, each a fresh argument
to make here against an observed need, not a speculative expansion.

## BG Format Status shipped — disc_info.bg_format (2026-06-14)

Shipped the byte-7 half of the banked "BG format status (RDI byte 7 &
3 + REQUEST SENSE progress)" item. The background-format state is
already in the READ DISC INFORMATION reply `mos_query_disc_info`
fetches, so this is a pure-decoder extension of `mos_discinfo.c` — **no
new command, no new wire traffic**. Byte 7 sits inside the
through-byte-11 region the decode already proves present, so no extra
bound.

**What ships.** `mos_disc_info.bg_format_status` = `buf[7] & 0x03`, the
2-bit BG Format Status, surfaced in `mos.metadata.v1.disc.disc_info` as
`bg_format` (0..3) + `bg_format_name` (token, drift-guarded). Values and
tokens track the Linux `CDM_MRW_*` macros (verified against
torvalds/linux `include/uapi/linux/cdrom.h`): 0 `none` (NOTMRW), 1
`inactive` (BGFORMAT_INACTIVE — started, not running), 2 `active`
(BGFORMAT_ACTIVE — in progress), 3 `complete` (BGFORMAT_COMPLETE). The
human `Disc` row appends only the in-flight states (`inactive`/`active`)
— the "is this disc still formatting" signal that bears on readability;
`none`/`complete` are the unremarkable common cases.

**What stays deferred.** The REQUEST SENSE **progress-percent** read
(the `SKSV` progress indication available while a format is `active`)
needs a new command path and only refines the `active` state with a
percentage — banked, not built, pending an observed need. The 2-bit
state is the load-bearing fact; the percentage is gravy.

**Falsifier (per the hardware-role ADR).** No in-repo capture sets a
non-zero BG Format Status yet — the existing RDI fixtures are CD/BD-R
(write-once, always `none`). A real DVD+RW or BD-RE mid-background-format
capture is the fixture-acquisition target; it lands as a `.bin` with a
dated fixtures-README entry, and the decode is already built to the spec
byte. The decode itself is exercised over the full byte-7 space in
`tests/test_discinfo.c` and stays in-bounds under the discinfo fuzz
phase.
