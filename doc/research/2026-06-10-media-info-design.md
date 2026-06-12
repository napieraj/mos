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
