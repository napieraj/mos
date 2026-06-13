# Optical-tool state survey: what the field surfaces, and where mos can go for more state enrichment

2026-06-13. Frozen snapshot, append-only per this directory's README rule.

**Motivating ask.** Cross-reference what the broader optical-tool
ecosystem — libcdio, MediaInfo, dvd+rw-mediainfo, ddrescue, redumper,
xorriso/libburn, dvdisaster, cdrdao, sg3_utils, cdrom_id, drutil — *does*
and *surfaces as state*, by what mechanism, and lay out the paths mos
could take to surface more disc/drive state. This extends the
2026-06-10 media-info design matrix (which already cross-checked
libcdio, dvd+rw-mediainfo, MediaInfo, and drutil for the disc-identity
fields) outward to the dumping/recovery tools and the SCSI-introspection
tools, and pulls the new candidate fields back through mos's scope
doctrine.

**Method.** Five parallel research passes, each fetching the tools' real
source/man pages (not memory) — the AGENTS R4 discipline. Sub-agent
reports are in the session transcript of record. Two claims were
load-bearing enough to verify directly against the tree rather than
trust a pass:

1. **CONFIRMED a project assertion** (MediaInfo is file-only — §1).
2. **OVERRODE a sub-agent assertion** (that `GetPerformance` /
   `ReadTrackInformation` lack `MMCDeviceInterface` convenience
   wrappers — they do not lack them; `ARCHITECTURE.md:834` is
   header-provable to the contrary — §4). This correction is the
   single most consequential finding for scope, because it moves the
   highest-value enrichment items from the raw-CDB column (GESN-grade
   justification) to the convenience-method column (no lock, no
   justification owed).

---

## 1. The MediaInfo correction (verify-before-repeating)

The motivating brief carried the claim "MediaInfo gets the TOC from the
kernel cache." It does not, and the project's prior assertion stands:
**MediaInfo is file-only.** GitHub code search over `MediaArea/MediaInfoLib`
and `MediaArea/ZenLib` returns zero hits for `ioctl`, `DeviceIoControl`,
`SCSI`, `CDROM_READ_TOC`, `kIOCDMediaTOCKey`, `sr0`, `/dev/cdrom`. The
optical-format readers (`File_Bdmv.cpp`, `File_DvdIfo.cpp`,
`File_Iso9660.cpp`) are pure byte-parsers over a mounted filesystem;
nothing opens a device, issues a TOC ioctl, or reads an IOKit property.
MediaInfo is a living example of the *consumer side* of the scope
boundary mos draws in the 06-10 matrix's volume-name row: it gets a
filesystem and parses files.

The "TOC from kernel cache" behavior is real but it belongs to
**libcdio**, not MediaInfo (the brief conflated them). See §7 — it is a
genuine candidate for mos, with sharp limits.

---

## 2. Cross-reference: what each tool surfaces, how, and its macOS posture

Mechanism legend: **CM** = an MMC convenience method exists (read-only,
no exclusive lock); **RAW** = raw SCSI/MMC CDB (needs exclusive lock on
macOS); **REG** = IOKit registry property (zero command); **FS** =
mounted-filesystem byte parse; **BLK** = block-device sector read.

| Tool | Primary purpose | State it surfaces (beyond mos today) | Mechanism | macOS reality |
|---|---|---|---|---|
| **cdrom_id** (systemd/udev) | pure state reporter — closest analog to mos | profile flags, `MEDIA_STATE` blank/appendable/complete/other, session/track counts | GET CONFIGURATION 0x46 (profile-list feature only), READ DISC INFORMATION 0x51, READ TOC 0x43; drive caps via Linux ioctls | Linux-only; **surfaces essentially the same set mos already has** |
| **libcdio** | portable CD/DVD access lib | cached TOC (CD), CD-TEXT, ISRC/MCN, disc-mode, drive caps | macOS: `kIOCDMediaTOCKey` (REG, CD-only), registry feature flags; CD-TEXT/ISRC/MCN via subchannel (BLK) | works via IOKit registry reads; MMC-command path goes through the Darwin driver |
| **MediaInfo** | container/codec metadata | IFO/BDMV/ISO structure (titles, codecs) | FS only | file-only; **no device access** (§1) |
| **dvd+rw-mediainfo** | disc-info dumper | media product/ID, disc status, fabricated TOC | READ DISC INFO 0x51, READ DISC STRUCTURE 0xAD, READ TOC 0x43 (all CM-equivalent) | Linux-centric; already mapped in the 06-10 matrix |
| **drutil** | macOS DiscRecording CLI | capacity (blocks/free/used), write speeds (media-present), buffer size (`Cache`), serial | DiscRecording / GET CONFIGURATION; speeds & buffer = the 0x2A / GET PERFORMANCE class | **the native-macOS reference** for the capacity/speed/serial surface mos lacks |
| **redumper** | byte-perfect dumper | per-sector C2/error map, subchannel Q (ISRC/MCN/CD-TEXT), DVD physical structure + region/CSS, lead-out-via-cache | RAW throughout (READ CD 0xBE+C2, READ DISC STRUCTURE 0xAD, vendor 0xF1 cache); **macOS: SCSITaskDeviceInterface + ObtainExclusiveAccess** | **works on macOS via the raw path mos deliberately avoids** — proves the path, takes the lock |
| **xorriso / libburn** | ISO mastering + burn engine | speeds, capacity/free/NWA, BG-format progress, BD spare-area, buffer | RAW MMC (GET PERFORMANCE 0xAC, READ TRACK INFO 0x52, READ FORMAT CAPACITIES 0x23, READ BUFFER CAPACITY 0x5C) | **macOS MMC path is effectively dead** — routed through libcdio's Darwin driver, gated behind an undefined `GET_SCSI_FIXED` macro → degrades to ISO-file-only |
| **dvdisaster** | ECC / damaged-disc recovery | per-sector good/bad/slow map, manufacturer ID, error-recovery tuning | RAW (READ CD 0xBE+C2, READ(10), MODE SENSE/SELECT 0x01, READ DISC STRUCTURE 0xAD); macOS via SCSITaskDeviceInterface + ObtainExclusiveAccess | macOS port exists; takes the lock (raw CDBs by construction) |
| **GNU ddrescue** | generic data salvage | recovery bookkeeping (`? * / - +` mapfile) — *not drive state* | BLK only (kernel `read()` + EIO) | works on `/dev/rdiskN`; sees only pass/fail per block, no C2/raw/mode-page |
| **cdrdao** | disc-at-once copy | raw subchannel, pre-gaps, index marks, ISRC/MCN, CD-TEXT | READ TOC 0x43 (CM) + READ CD 0xBE w/ subchannel (BLK) | CD-structure-by-reading; outside a state library's remit |
| **sg3_utils** | raw SCSI introspection | MODE pages (0x2A caps, 0x01 error-recovery), LOG SENSE counters, VPD serial, GET PERFORMANCE, READ CAPACITY | RAW SG_IO | Linux; the canonical enumeration of *what is readable as state* |

**Two architectural facts fall out of this table and are worth banking
as standing context:**

- **redumper proves the raw path works on macOS on console-user
  privilege** (SCSITaskDeviceInterface → `ObtainExclusiveAccess` →
  arbitrary CDBs, drive selected by BSD name). mos's one-raw-CDB rule is
  therefore a **self-imposed scope choice, not a platform limit** — which
  is exactly how AGENTS scope-doctrine layer 1 frames it. Every raw tool
  in the table (redumper, dvdisaster) takes the exclusive lock by
  construction; mos's convenience-method posture is the thing that lets
  it read disc state *without* the lock and *without* fighting a mount.
  That is the differentiator, restated by the whole field.
- **libburn's macOS MMC path is dead** (the libcdio Darwin
  `run_mmc_cmd_osx` is disabled behind `GET_SCSI_FIXED`). So the most
  capable open-source optical engine degrades to ISO-file-only on the
  Mac. mos talking `MMCDeviceInterface` natively is doing the thing
  libburn was *designed for but never shipped working* on macOS. The
  niche the 06-10 doc named ("authoritative, failure-honest,
  current-this-instant tray bit") has a companion: **native, lock-free
  MMC disc-state reads on macOS at all**, which the portable stack does
  not deliver.

---

## 3. cdrom_id is the analog, and the comparison validates mos's scope

cdrom_id is the only tool here built for the same job as mos — a pure
state reporter, not a ripper or burner. Its exported surface
(`ID_CDROM_MEDIA_STATE` ∈ {blank, appendable, complete, other},
session/track counts, the per-profile capability flags) is **essentially
the set mos already has**: `mos_disc_status` already emits exactly the
blank/appendable/complete/other vocabulary, and the GET CONFIGURATION
feature walk already covers the profile/capability flags. There is
nothing in cdrom_id to borrow except the confirmation that mos's
field selection is the right one for a state reporter. The enrichment
opportunities all come from the *dumper/burner/introspection* tools,
which read more because they do more.

---

## 4. The convenience-method correction (the load-bearing scope finding)

A sub-agent asserted that `READ TRACK INFORMATION` and `GET PERFORMANCE`
have no `MMCDeviceInterface` convenience wrapper, which would have put
the capacity and speed surfaces behind the raw-CDB gate (GESN-grade
justification + nub-collision analysis). **That is wrong**, and the tree
is header-provable to the contrary. `ARCHITECTURE.md:834` enumerates the
`MMCDeviceInterface` convenience methods, every one read-only and free of
`ObtainExclusiveAccess` (§9.7), from the vendored `SCSITaskLib.h` (§11
public mirror: the phracker MacOSX10.2.8.sdk copy, recorded as
signature-identical to the modern SDK for existing methods):

> `Inquiry`, `TestUnitReady`, `GetConfiguration`, **`ModeSense10`**,
> `ReadTableOfContents`, `ReadDiscInformation`, **`ReadTrackInformation`**,
> **`ReadDVDStructure`**, **`GetPerformance`**

— plus `ReadDiscStructure` (the generic MMC-5 wrapper mos already calls
for BD DI) and `GetTrayState`/`SetTrayState` (the information-destroying
pair §9.7 documents).

The consequence: **the four highest-value enrichment surfaces are all
convenience methods** — capacity/NWA (`ReadTrackInformation`), speeds
(`GetPerformance`), mechanical/lock/buffer state (`ModeSense10` page
0x2A), and DVD/BD physical/copyright structure (`ReadDVDStructure` /
`ReadDiscStructure`). None requires the exclusive lock; none owes the
raw-verb showing. They are the cleanest form of the scope doctrine
(layer 1: kernel-authored convenience), not exceptions to it.

One thing this correction does **not** settle: whether the convenience
`Inquiry` surfaces VPD pages (page 0x80 unit serial) or only standard
INQUIRY. The 06-10 doc already records this as the open stage-1
falsifier; it needs the modern vendored header (dev-tree-only, stripped
from this checkout) or hardware. Carried forward, not resolved here. If
the convenience `Inquiry` is standard-page-only, the serial needs a raw
INQUIRY and the AGENTS raw-verb showing.

`SetCDSpeed` / `SetMediaAccessPermission` are confirmed **absent** from
the header (`ARCHITECTURE.md:894`); only `GetPerformance`/`GetPerformanceV2`
touch the speed axis, and they are read-only — which is the correct shape
for a state library anyway (we report speeds, we do not set them).

---

## 5. Candidate new state, tiered by scope fit

The governing test (AGENTS scope doctrine): layer 1 prefers a
convenience method over a raw CDB; layer 2 forbids SPC ambition (no mode
pages "in the abstract," only what the optical decision/identity tree
needs); layer 3 caps the privilege footprint (no sector/block reads, no
filesystem parsing); layer 4 treats all input as adversarial. A
candidate is *in scope* when it is a single read-only command (ideally
CM), needs no lock, needs no elevation, and answers a real disc/drive
question — and failure degrades to null, never to error (the standing
enrichment doctrine).

### Tier A — convenience method, read-only, no lock (in-scope; the preferred form)

| # | State | Convenience method | What it adds | Notes / cross-ref |
|---|---|---|---|---|
| A1 | **DVD physical structure** — book type, layer break / layer count, track path (parallel vs opposite), disc size, BCA presence | `ReadDVDStructure` / `ReadDiscStructure` format 0x00, DVD media | the DVD analog of the BD DI mos already reads; disambiguates DVD-Video pressings, dual-layer geometry | redumper `print_physical_structure`; dvdisaster; same opcode family mos already issues |
| A2 | **DVD copyright / protection class** — region management byte, CSS vs CPRM presence | `ReadDiscStructure` COPYRIGHT format (0x01) | "is this disc CSS-protected, which region" as a faithful spec read (no key handling) | redumper COPYRIGHT structure. Surface the bytes, never the keys (layer 3) |
| A3 | **Capacity / free / used / next-writable-address** | `ReadTrackInformation` (0x52) | total/used/free blocks, NWA — the drutil `status` capacity surface | the 06-10 matrix already earmarked `ReadTrackInformation` for stage-2 capacity. This is the correction's biggest unlock |
| A4 | **DVD manufacturer / media ID** — the disc maker (CMCMAG, RITEK, VERBAT, MILLEN…) for DVD media | `ReadDVDStructure` lead-in (0x0E) / ADIP (0x11) | extends the BD MILLEN/MR1 M-DISC identity (already shipped) to DVD; a real quality/dedup signal | 06-10 doc deferred DVD-side mfr ID pending a real capture — scattered format-specific offsets, no clean MILLEN/MR1 pair. Still the gating blocker |
| A5 | **Drive speeds (read/write descriptors, current/max)** | `GetPerformance` (0xAC) | the MMC-sanctioned modern speed source (supersedes mode-page 0x2A speed fields) | xorriso `-list_speeds`; drutil `status` Writable section |
| A6 | **Mechanical / loading state** — loading-mechanism type (tray/slot/caddy/changer), eject support, lock support, **live media-locked bit**, drive buffer size | `ModeSense10` page 0x2A | the *unique 0x2A residue* — the only state in the table GET CONFIGURATION / GESN genuinely cannot supply | sg3_utils / cdrecord `-prcap`. Note 0x2A is MMC-5/6 *Legacy annex*; read only the non-deprecated residue (mechanism/lock/buffer), take speeds from A5 |

### Tier B — readable state, but new modality or low real-world payoff

| # | State | Command | Why not Tier A |
|---|---|---|---|
| B1 | **Error-recovery configuration** — AWRE/ARRE/PER/DCR + read-retry count (changeable/default/saved) | `ModeSense10` page 0x01 (CM) | genuinely readable state and lock-free, but niche: it reports how the drive *would* behave on a bad read — interesting to a recovery consumer, marginal to a state reporter. Read-only only; the MODE SELECT *write* dvdisaster does is out (mutation, layer 2) |
| B2 | **Formatting state + BACKGROUND FORMAT progress** | READ FORMAT CAPACITIES (0x23) — **no CM** | RAW → GESN-grade showing. *But* BG-format progress is partly in READ DISC INFORMATION byte 7 (already CM, already read) + the REQUEST SENSE progress field; prefer extending the existing CM read before authoring a raw verb |
| B3 | **Total formatted capacity (bytes)** | READ CAPACITY(10) (0x25) — **no CM** | RAW; and largely derivable from A3's block counts. Low marginal value |
| B4 | **Drive buffer size/free** | READ BUFFER CAPACITY (0x5C) — **no CM** | RAW; *but buffer size is already in A6's mode-page 0x2A* (CM). Take it there, skip the raw verb |
| B5 | **BD spare-area / defect-management usage** | `ReadDiscStructure` BD spare-area format (CM) | actually Tier-A-eligible (CM, lock-free); parked in B only because it is BD-write-workflow state with a narrow audience. Cheap to add alongside A1/A2 if a consumer wants it |
| B6 | **LOG SENSE counters** — read/write error counters (0x03/0x02), informational-exceptions/SMART (0x2F), start-stop cycles (0x0E) | LOG SENSE (0x4D) — **no CM** | RAW, and **optical hardware rarely populates these** (the sg_logs man page warns of broad non-support). Park as hardware-capture-first per the AGENTS "hardware → fixture, never design input" ADR — do not design to it |

### Tier C — out of scope by doctrine (named so the next session does not re-litigate)

| State | Why out |
|---|---|
| Per-sector C2 error pointers / read-quality maps (redumper, dvdisaster, xorriso `-check_media`) | sector reads — layer 3. The defining capability of a *dumper*, antithetical to a state reporter |
| Subchannel Q / ISRC / MCN | sector reads (subchannel rides the data stream) — layer 3. *(CD-TEXT is the exception: `ReadTableOfContents` format 0x05 is a CM — already staged in the 06-10 doc, stays in scope)* |
| Lead-out-via-cache exploit (redumper vendor `0xF1`) | raw vendor CDB + exclusive lock — fails the one-raw-CDB rule outright |
| Drive read-offset detection (redumper / AccurateRip DB) | a static data table, not a command; and a *ripping* concern — consumer-side, like the MusicBrainz/AccurateRip IDs the 06-10 doc already places outside mos |
| Descrambling, CSS/CPPM key handling, copy-protection fingerprinting | raw key commands, sector reads, and the makemkvcon territory the scope doctrine names explicitly |
| ddrescue mapfile model | block-device salvage bookkeeping — not drive state at all |
| MODE SELECT (writing any mode page) | mutation — a state library reports, it does not tune |

---

## 6. The kIOCDMediaTOCKey kernel-cache TOC (the brief's real "TOC from cache")

libcdio's macOS driver (`lib/driver/osx.c`, `read_toc_osx`) reads the
TOC from Apple's cached registry property `kIOCDMediaTOCKey` — a
`CDTOC` blob — with **zero SCSI commands and no exclusive access**
(`IORegistryEntryCreateCFProperties` on the media service). This is the
zero-command fallback the 06-10 doc flagged ("if the convenience RTOC
misbehaves on real hardware, that property is the kernel-authored
fallback to evaluate"). The survey confirms it is real and sharpens the
limits:

- **CD-only.** The blob lives on the CD media class. There is **no
  cached-TOC equivalent for DVD/BD** — macOS exposes only
  `kIODVDMediaTypeKey` (a media-type *string*) for those. So this cannot
  cover non-CD media; mos's `ReadTableOfContents` convenience path stays
  the universal route.
- **Richer but different shape.** It is the *full-TOC* form — POINT
  descriptors (A0/A1/A2), ADR/control, and **MSF** addresses — not the
  format-0 LBA shape `mos_internal_toc_parse` consumes. Adopting it means
  a separate CDTOC parser + MSF→LBA conversion, not a reuse of the
  format-0 path.
- **Pros:** no command, no `ObtainExclusiveAccess`, works while the disc
  is mounted, survives the kext owning the device. **Cons:** CD-only,
  stale-able (cached at media-detection time, not re-read per query),
  and the shape divergence above.

**Recommendation:** treat it as a CD-only, zero-command *corroboration /
fallback* source, not a primary path — a hardware-contingent option to
evaluate if the convenience RTOC disappoints, exactly as the 06-10 doc
framed it. Its existence is also a clean illustration for the docs that
"kernel-cached" disc state on macOS is a CD-era artifact, not a general
mechanism — which is why mos's DR-substrate + convenience-MMC design is
the right general answer.

---

## 7. Recommended paths, ranked

Scored against the four scope layers, highest payoff-per-risk first:

1. **A1+A2+A4 — extend the READ DISC STRUCTURE surface (one opcode mos
   already issues).** DVD physical structure (book type / layer break /
   track path), DVD copyright class (region + CSS/CPRM presence), and
   the DVD manufacturer/media ID. Same `ReadDiscStructure`/`ReadDVDStructure`
   convenience family, same fail-closed constant-offset parse discipline
   the BD DI decoder (`mos_discstruct.c`) already proves, no new lock, no
   new privilege. This is the natural next increment after the BD DI work
   that shipped 2026-06-13. **Blocker for A4 specifically:** the DVD-side
   manufacturer ID still needs a real capture to pin the scattered
   format-0x0E/0x11 offsets (the 06-10 doc's standing deferral) — BD-first,
   DVD when a fixture lands.

2. **A3 — capacity / NWA via `ReadTrackInformation`.** The drutil
   `status` capacity surface, already earmarked in the 06-10 matrix and
   now confirmed to be a convenience method. Highest-value *new field
   family* (blocks total/used/free, next-writable-address) for a list/
   metadata consumer distinguishing a near-full appendable disc from a
   fresh one. Fits cleanly into `mos.metadata.v1`'s `disc` subtree.

3. **A5 — drive speeds via `GetPerformance`.** A `mos.drive.v1` field
   (static-ish drive capability) — the read/write speed descriptors.
   Convenience, read-only. Lower urgency than capacity but cheap and it
   is what drutil shows.

4. **A6 — mechanical/lock/buffer via `ModeSense10` page 0x2A.** The one
   surface with *no* GET-CONFIGURATION overlap: loading-mechanism type,
   the live media-locked bit, buffer size. Introduces the MODE SENSE
   modality (mos does none today), so it is the largest *new-surface*
   step — but it is still a convenience method, read-only, lock-free.
   Read only the non-deprecated 0x2A residue; do **not** re-derive
   speeds (A5) or media-support (GET CONFIGURATION) from it.

5. **B5 — BD spare-area** if a BD-write consumer asks; it is CM and
   lock-free, so it rides along with path 1 at near-zero marginal cost.

Everything in Tier C stays out, permanently, and §5's Tier-C table is
the place to point when the question recurs.

---

## 8. Open items to SDK-verify before building (standing discipline)

Per the repo rule that the convenience-method *existence* is
header-grounded but each *selector signature* must be confirmed against
the vendored modern `SCSITaskLib.h` before code is written:

- **Does the convenience `Inquiry` carry VPD pages?** (page 0x80 serial
  for `mos.drive.v1`). Open since 06-10; still open. If no, the serial is
  a raw-INQUIRY decision under the AGENTS raw-verb rule.
- **`ReadTrackInformation` / `GetPerformance` / `ModeSense10` exact
  selector signatures** — confirmed to *exist* (`ARCHITECTURE.md:834`);
  confirm the argument shapes against `docs/apple/` before wiring.
- **DVD `ReadDVDStructure` format coverage** — which DVD structure
  formats (0x00 physical, 0x01 copyright, 0x0E lead-in, 0x11 ADIP) the
  Apple convenience accepts, vs needing the generic `ReadDiscStructure`
  with a format argument (which mos already calls for BD).
- **Mode-page 0x2A on real hardware** — what a USB-bridged consumer
  burner actually populates in the non-deprecated residue, and whether
  the live media-locked bit tracks PREVENT/ALLOW state. Hardware-capture
  item, not a design input (AGENTS hardware ADR).

All four are falsifiers/fixtures, not blockers on the design above:
paths 1–4 are spec- and convenience-grounded; the hardware pass refutes
or feeds them, it does not steer them.
