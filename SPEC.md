# SPEC — external spec citation map

Single source of truth for the spec citations behind mos's pure response
parsers. Each parser keeps its **parsed byte-offset table and safety
contract inline** — those guard the parse and are verified against the
code beside them, so they stay local (AGENTS.md doctrine: spec byte
layouts live in the parsing `.c`). This file holds the parts that repeat
or are reference-only: the authoritative spec document per parser, the
external cross-checks used to validate the offsets, and the fields each
parser deliberately does **not** decode. A parser's header names its
command and points here rather than restating citations.

Opcodes, page codes, and field values quoted here are also operands in the
code; this table is the citation, not the parse.

## Parsers

### `src/mos_sense.c` — SCSI sense data
- **Spec:** SPC-4 §4.5.3 (fixed format, response code 0x70/0x71),
  §4.5.2 (descriptor format, 0x72/0x73).
- **Note:** optical drives return fixed format in practice; the
  descriptor path exists for correctness.

### `src/mos_discinfo.c` — READ DISC INFORMATION
- **Spec:** MMC-6, opcode 0x51, standard data type 000b. First 12 bytes
  decoded inline, plus the validity-gated identifiers below.
- **Decoded identification:** Disc Type (byte 8); 32-bit Disc Identification
  (bytes 12..15, gated on byte 7 DID_V bit 7); Disc Bar Code (bytes 24..31,
  gated on byte 7 DBC_V bit 6). Each gate also requires the reply's declared
  length to actually reach the field — a validity bit over uncarried bytes
  yields not-present, never an OOB read (dual-length rule O-4).
- **Not decoded:** lead-in / lead-out addresses (bytes 16..23) and the OPC
  table (bytes 32+, variable) — informational, not the status.

### `src/mos_discstruct.c` — READ DISC STRUCTURE (Blu-ray DI)
- **Spec:** MMC-5, opcode 0xAD, BD media type, format 0x00. The DI unit
  offsets (8 / 100 / 106 / 111) are MMC-5 / BDA-registered.
- **Not decoded (DI offsets 11..99):** the physical write-parameter
  region — no consumer value.

### `src/mos_trackinfo.c` — READ TRACK INFORMATION
- **Spec:** MMC, opcode 0x52, Track Information Block. The 32/33 Track/
  Session MSB tail is the MMC-6 longer reply (optional).
- **Cross-check:** offsets match the Linux kernel's packed
  `struct track_information`, `include/uapi/linux/cdrom.h`.

### `src/mos_formatcap.c` — READ FORMAT CAPACITIES
- **Spec:** MMC-6 §6.24, opcode 0x23. Capacity List Header (4 bytes; byte 3 is
  the single-byte CAPACITY LIST LENGTH = 8 + 8·N). Current/Maximum Capacity
  Descriptor at bytes 4-11 (Number of Blocks 4-7, Descriptor Type byte 8
  bits 1:0, Block Length 9-11). Formattable Capacity Descriptors are 8 bytes
  each from byte 12 (Number of Blocks 0-3, Format Type byte 4 bits 7:2, Type
  Dependent Parameter 5-7).
- **Dual-length (O-4):** CAPACITY LIST LENGTH is bounded by the reply buffer
  length the shell passes (the ReadFormatCapacities convenience method reports
  no realized count), then floored to whole 8-byte descriptors — an
  over-claimed length can never read past the buffer.
- **Cross-check:** descriptor-type codes (1 unformatted / 2 formatted / 3 no
  media) and the 8-byte descriptor stride match the MMC-6 tables; dvd+rw-tools
  (growisofs) drives DVD+RW/BD-RE off the same header + descriptor layout. No
  committed capture yet — a hardware reply is a falsifier per the hardware ADR,
  never a design input.
- **Not decoded:** the per-Format-Type meaning of the Type Dependent Parameter
  (surfaced raw as `param`). mos never issues FORMAT UNIT (0x04).

### `src/mos_physstruct.c` — READ DISC STRUCTURE (DVD/HD-DVD)
- **Spec:** MMC-5, opcode 0xAD, media type 0; Physical Format Information
  (format 0x00) and Copyright Management Information (format 0x01). The
  same media-type-0 reply carries HD-DVD book types (0x4..0x6).
- **Cross-check:** byte arithmetic matches Linux `drivers/cdrom/cdrom.c`
  (`dvd_read_physical`, `dvd_read_copyright`, `base = &buf[4]`).

### `src/mos_perf.c` — GET PERFORMANCE
- **Spec:** MMC-6, opcode 0xAC, Performance Data TYPE 00h (max read/write)
  and TYPE 03h (Write Speed descriptor list); both layouts built to spec.
- **TYPE 03h (Write Speed) — `mos.state.v1.speeds.descriptors` (F2):** each
  16-byte Write Speed Descriptor carries Read Speed at [8..11] and Write
  Speed at [12..15] (BE, kB/s). Reached via the `GetPerformanceV2`
  convenience method (DATA_TYPE + TYPE fields — confirmed in SCSITaskLib.h,
  added in Mac OS X 10.2), so NO raw CDB. (Supersedes the earlier note that
  TYPE 03h was unreachable: that was true of the obsolete `GetPerformance`
  wrapper, which lacks the TYPE field; `GetPerformanceV2` has it.) Gated by
  `mos_query_drive_perf`'s `want_descriptors` arg — issued only on
  `mos state --json` (where the list is emitted); the human view and the polled
  `mos watch` pass false and pay only the two Type 00h reads (speeds ADR
  hot-path budget).
- **Human 1× multiple (cli/human.c `mos_cli_human_rate_x`):** the reported
  kB/s is scaled to the loaded medium's nominal 1× data rate for the human
  view only (JSON keeps raw kbps). Bases, by media class
  (`mos_profile_class`): CD 1× = 75 sectors/s × 2048 B = 153.6 kB/s; DVD 1×
  = 11.08 Mbit/s = 1385 kB/s; BD 1× = 36 Mbit/s = 4500 kB/s; HD DVD 1× =
  36.55 Mbit/s = 4568 kB/s. Nominal (hence the "~"); a class with no defined
  base falls back to the absolute rate.

### `src/mos_pure.c` (`mos_internal_toc_parse`) — READ TOC/PMA/ATIP format 0000b
- **Spec:** MMC-6 §6.27.2.3; the formatted TOC — per-track ADR/control + start
  LBA plus the lead-out (track 0xAA). Issued via the non-exclusive
  `ReadTableOfContents` convenience method (`mos_query.c` `mos_query_toc`,
  `format=0x00`).
- **Richer kernel-cached source — `src/mos_cdtoc.c`.** macOS caches a *full-TOC*
  as `kIOCDMediaTOCKey` — a `CDTOC` blob on the `IOCDMedia` node, read with
  **zero SCSI commands and no exclusive access**, fresh off the current media
  node each query (so the "stale cache" worry does not apply — the kernel
  refreshes it on every media change). It is a *superset* of this format-0000b
  read: POINT descriptors A0/A1/A2 carry the per-session first/last track and
  lead-out — the session structure this read omits and `disc_info` gives only
  for the last session. **CD-only** (no DVD/BD equivalent — those expose only a
  media-type string); for CDs it is now the primary TOC source (see Source
  order below), the issued path its fallback. `mos_internal_cdtoc_parse` decodes it
  into the per-session layout (`mos.metadata.v1.disc.session_layout`), and
  `mos_internal_cdtoc_to_toc` into the per-track `mos_toc`.
- **Cross-check:** libcdio `lib/driver/osx.c` `read_toc_osx` — same
  `CDTOCGetDescriptorCount` walk, `adr==1` filter, POINT 0xA0/0xA1/0xA2
  handling, and `CDConvertMSFToLBA` (minus the 150-frame pregap). Struct layout:
  Apple `IOCDTypes.h` (`CDTOC` / `CDTOCDescriptor` / `CDMSF`).
- **Source order:** the cached full-TOC is the **primary** CD TOC source —
  `mos_query_toc` decodes it via `mos_internal_cdtoc_to_toc` (fail-closed to
  this format-0000b standard — a duplicate track or a gap in first..last
  refuses the whole) — with the issued `ReadTableOfContents` as the fallback
  (no `IOCDMedia` node yet) and the only path for DVD/BD. The decision to
  prefer the cache is recorded in the AGENTS.md ADR.

### `src/mos_cdtext.c` — READ TOC/PMA/ATIP format 0101b (CD-TEXT)
- **Spec:** MMC-3 §6.27 / Red Book CD-TEXT; 18-byte packs.
- **Cross-check:** libcdio `lib/driver/cdtext.c` (CRC present, not
  verified, as in libcdio).
- **Decoded:** album + per-track Title (0x80) and Performer (0x81), from
  the first language block (block 0), single-byte charset.
- **Not decoded:** other field types (songwriter / composer / arranger /
  message / genre / ISRC / UPC / disc-id); language blocks 1..7;
  double-byte (DBCC / MS-JIS) text — a DBCC field reads as absent rather
  than mis-decoded as Latin-1.

### `src/mos_atip.c` — READ TOC/PMA/ATIP format 0100b (ATIP)
- **Spec:** MMC-6 r02g §6.25, Table 488 ("READ TOC/PMA/ATIP response data,
  Format = 0100b"). Issued via the convenience `ReadTableOfContents`
  (FORMAT=0x04), CD-R/RW only — pressed CD / DVD / BD answer CHECK CONDITION.
- **Decoded (raw spec fields only):** URU (byte 5 bit 6), Disc Type (byte 6
  bit 6), Disc Sub-Type (byte 6 bits 5-3), Reference Speed (byte 4 bits 2-0),
  ATIP Start Time of Lead-in M:S:F (bytes 8-10 — the manufacturer/MID
  identity) and Last Possible Start Time of Lead-out M:S:F (bytes 12-14 —
  nominal capacity). Surfaced as `mos.metadata.v1.disc.atip`.
- **Not decoded / deliberately consumer-side:** the MID-to-MANUFACTURER NAME
  table (Orange Book [CD-Ref6-9], a curated per-device map the hardware-role
  ADR keeps out of mos); the A1/A2/A3/S4 additional-information values; the
  Indicative Target Writing Power. mos ships the raw M:S:F; a consumer keys
  the Orange Book table off it.
- **Bounds:** the device-reported ATIP Data Length (bytes 0-1, BE) may only
  SHRINK the trusted region (dual-length rule); a reply shorter than 15 bytes
  fails closed (no descriptor through the lead-out). No payload byte is an
  offset. Spec-built — no in-repo capture yet; a real `mos probe --capture`
  ATIP reply is the standing falsifier.

### `src/mos_modepage.c` — MODE SENSE(10) optical pages
- **Spec:** page 0x2A is MMC-3 page-2A (CD/DVD Capabilities & Mechanical
  Status); page 0x01 is the SPC Read/Write Error Recovery page. Sub-page
  format (SPF=1) has a 4-byte header with a BE16 length; 0x2A/0x01 are
  page_0 format. Read-only — no MODE SELECT.
- **Page 0x2A read/rip caps:** BUF/BURN-Free page[4] bit7, Multisession
  page[4] bit6, CD-DA stream accurate page[5] bit1, C2 error pointers
  page[5] bit4 (all within the ≥12-byte page floor already required for the
  buffer size). C2-pointer reliability is firmware-dependent — mos reports
  the claim, not the accuracy.
- **Cross-check:** page 0x2A offsets against Linux `sr.c`
  `get_capabilities`.

### `src/mos_config.c` — GET CONFIGURATION feature walk + payload decodes
- **Spec:** MMC-6 §5.2 feature descriptors. The walker's bounds rules are
  the inline safety contract. Typed payload decodes off the same walk:
  - **Profile List (0x0000):** MMC-6 §5.3.1 — 4-byte Profile Descriptors
    (Profile Number BE16, byte 2 bit0 CurrentP); the drive-static code set.
  - **Content protection:** drive-static capability bits (feature PRESENCE =
    the drive can authenticate the scheme; the per-feature Current bit is
    media state, ignored). Version-carrying schemes put their version at
    payload byte 3 (Additional Length 4): **CSS 0106h** (§5.3.38, CSS
    Version), **CPRM 010Bh** (§5.3.42, CPRM version), **AACS 010Dh** (§5.3.44
    Table 198 — byte 0 BEC bit 1 / WBE bit 2, AACS Version byte 3).
    Presence-only (Additional Length 0): **SecurDisc 0113h** (§5.3.46),
    **VCPS 0110h** (legacy, MMC-5 — designated Legacy in MMC-6 Annex E.6).
    Enforcement/region/key state behind REPORT KEY is out of scope.
  - **Write Protect Feature (0004h):** MMC-6 r02g §5.3.5, Table 101 — payload
    byte 0 carries the CAPABILITY bits SSWPP (bit 0), SPWP (bit 1), WDCB
    (bit 2), DWP (bit 3). These say what the drive can REPORT/CHANGE about
    write protection, not whether the loaded disc IS write-protected (the
    Timeout & Protect mode page 1Dh / MECHANISM STATUS carry per-disc state,
    which mos does not read). Decoded into `mos.drive.v1.write_protect`,
    null when absent. NB: 0026h is the *Restricted Overwrite* feature, NOT
    write protect — a libcdio/MS-DDK naming trap the spec corrects.
  - **Firmware Information (010Ch):** MMC-6 r02g §5.3.43, Table 197 — payload
    Century[2] Year[2] Month[2] Day[2] Hour[2] Minute[2] Second[2]
    Reserved[2], decimal ASCII, GMT; Additional Length 0x10. Emitted as an
    RFC 3339 UTC string (the format `mos.event.v1`'s `ts` uses). NB: 0x1FF
    (libcdio's `FIRMWARE_DATE`) is *Reserved* in MMC-6 — the feature is 010Ch.
  - **Logical Unit Serial Number (0108h):** MMC — feature payload is the
    drive serial as ASCII (trailing space/NUL padding trimmed; interior NUL or
    over-length refused, complete-or-unavailable — a durable identity key is
    whole or nothing). The serial source (`mos_drive_caps_serial`): non-exclusive, read
    from this same RT=0 walk, populated where VPD 0x80 is empty (the WH16NS60
    finding — AGENTS.md serial-source ADR). Neighbours in the walk: 0109h Media
    Serial Number, 010Ah Disc Control Blocks.
  - **Curated capability presence (`mos.drive.v1.capabilities`):** PRESENCE of
    optional features in the same walk — **Real-Time Streaming 0107h**
    (host-paced read/write performance), **Power Management 0100h**, **Time-out
    0105h**. Presence only (no payload decode); the curated, named subset
    `mos drive` surfaces from the walk `mos features` dumps raw.

### `src/mos_inqdata.c` — standard INQUIRY data (identity + version + descriptors)
- **Spec:** SPC-4 §6.4.2, opcode 0x12 EVPD=0. VENDOR bytes 8-15, PRODUCT
  16-31, PRODUCT REVISION 32-35 (ASCII, space-padded — the drive's
  self-reported identity, which `mos drive` prefers over the DR cache);
  VERSION byte 2; VERSION DESCRIPTORS bytes 58-73 (eight BE16 codes, 0x0000 =
  empty slot). Needs a raw read with allocation length ≥74 — the convenience
  Inquiry returns only the 36-byte header.
- **Under-delivery refused:** a conformant standard INQUIRY always returns at
  least the 36-byte standard header (Additional Length ≥31), so a trusted
  region shorter than 36 bytes means the transport cut the transfer mid-
  identity (declared > delivered). The parser refuses (returns false) rather
  than emit a partial vendor/product/revision — `mos drive` prefers this read
  over the DR cache, so an incomplete read must defer to the complete cached
  identity. The canonical-data corollary of the dual-length rule (O-4).
- **Cross-check:** Linux `include/scsi/scsi.h` (the VERSION value table, as
  `resp[2]+1`); sg3_utils `src/sg_inq_data.c` `sg_version_descriptor_arr`
  (the descriptor code→name table; mos maps the "no version claimed" family
  codes, unknown → hex).

### Drive serial — NOT a VPD page
The drive serial is read from GET CONFIGURATION feature 0108h (`mos_config.c`,
above), not INQUIRY VPD page 0x80. VPD 0x80 is the SPC/block-storage serial
carrier and is the wrong abstraction for an MMC optical drive — architecturally
(the serial feature lives in MMC, not SPC) and empirically (it read empty on the
drives surveyed while 0108h carried the serial). Decision + cross-family survey:
`doc/research/2026-06-21-optical-serial-vpd80-vs-0108h.md`; AGENTS.md
serial-source ADR. (vendor/product/revision come zero-command from
DiscRecording's directory on every path EXCEPT `mos drive`, which prefers a
fresh raw standard INQUIRY via mos_inqdata.c and falls back to the DR cache only
on BUSY.)

## macOS IOKit platform surface (the command/wrapper/registry inventory)

**Why this exists.** Before admitting a raw CDB, a feasibility analysis must
show no convenience method carries the data (AGENTS scope-doctrine layer 1).
`ARCHITECTURE.md` §9.7 lists only the ~9 methods its GetTrayState masking-trap
argument needs — an **illustrative subset, not the inventory**. Reading it as
the inventory is what wrongly admitted READ FORMAT CAPACITIES as a raw CDB (the
`ReadFormatCapacities` wrapper existed all along; corrected 2026-06-18). This
section is the **complete** inventory — consult it, never §9.7's subset, when
deciding raw-vs-convenience. Verified verbatim against the **macOS 26.4
(Tahoe)** SDK (`IOKit.framework/Headers/scsi/SCSITaskLib.h`, `…/storage/IO*Media.h`,
`IOStorageDeviceCharacteristics.h`, `IOSCSIMultimediaCommandsDevice.h`). The
`MMCDeviceInterface` UUID `1F651106-23CC-11D5-BBDB-003065704866` is stable /
append-only, so 10.5 / 11.3 are subsets of this list.

### MMCDeviceInterface — command-issuing convenience methods (NON-exclusive)

Each issues one command and returns `SCSITaskStatus *` + `SCSI_Sense_Data *`
out-params; none takes `ObtainExclusiveAccess` (fails `kIOReturnExclusiveAccess`
only if ANOTHER client holds the device). Signature: `self`, the listed params,
then `(taskStatus, senseDataBuffer)`.

| Method | Issues (spec) | mos use |
|--------|---------------|---------|
| `Inquiry` | INQUIRY (SPC-2) | standard INQUIRY (`mos_inqdata.c`, `mos drive`), StandardData-only — no EVPD/PAGE_CODE. The drive serial is NOT read via INQUIRY; it is GET CONFIGURATION feature 0108h (`mos_config.c`). |
| `TestUnitReady` | TEST_UNIT_READY (SPC-2) | the state core's readiness probe (`mos_state_core.c`). |
| `GetConfiguration` | GET_CONFIGURATION (MMC-2) | current profile + feature walk (`mos_config.c`, `mos_scsi.c`). RT / STARTING_FEATURE_NUMBER. |
| `ModeSense10` | MODE_SENSE_10 (SPC-2) | pages 0x2A + 0x01 (`mos_modepage.c`, `mos drive`). LLBAA / DBD / PC / PAGE_CODE. |
| `SetWriteParametersModePage` | MODE_SELECT (SPC-2) | **unused** — mos never tunes the drive. |
| `GetTrayState` | GET_EVENT_STATUS_NOTIFICATION (MMC-2) | **NOT used — masking trap**: no sense out-params, hard-codes closed+success on failure (§9.7). mos issues raw GESN 0x4A. |
| `SetTrayState` | START_STOP_UNIT (SBC-3) | **NOT used for tray** — sense-blind, eject/load only; mos issues raw 0x1B + PREVENT ALLOW 0x1E for honest outcomes + lock. |
| `ReadTableOfContents` | READ_TOC_PMA_ATIP (MMC-2/SFF-8020i) | fallback CD TOC (`mos_scsi.c`); primary is the cached `kIOCDMediaTOCKey`. MSF / FORMAT / TRACK_SESSION_NUMBER. |
| `ReadDiscInformation` | READ_DISC_INFORMATION (MMC-2) | disc completion / erasable / sessions (`mos_discinfo.c`). |
| `ReadTrackInformation` | READ_TRACK_INFORMATION (MMC-2) | append point / track size (`mos_query.c`). ADDRESS_NUMBER_TYPE / LBA. |
| `ReadDVDStructure` | READ_DVD_STRUCTURE (MMC-2) | **unused** — mos uses the generic `ReadDiscStructure` (below). |
| `GetPerformance` | GET_PERFORMANCE (MMC-2) | max read/write speeds (`mos_perf.c`). TOLERANCE / WRITE / EXCEPT. |
| `GetSCSITaskDeviceInterface` | — (handle accessor) | → the raw-task interface below. |
| `GetPerformanceV2` | GET_PERFORMANCE (Mt. Fuji 5) | **unused** — `GetPerformance` suffices. |
| `SetCDSpeed` | SET_CD_SPEED (MMC-2) | **unused** — mos reports speed, never sets it. |
| `ReadFormatCapacities` | READ_FORMAT_CAPACITIES (MMC-2; "Added in 10.3") | formattable view (`mos_query.c` → `mos_formatcap.c`). **Non-exclusive** — the convenience method that corrected the raw-CDB error. |
| `ReadDiscStructure` | READ_DISC_STRUCTURE (MMC-5) | BD disc-id (DI) + DVD physical/copyright (`mos_discstruct.c` / `mos_physstruct.c`). MEDIA_TYPE / FORMAT. |
| `ReadDiscInformationV2` | READ_DISC_INFORMATION (MMC-5) | **unused** — DATA_TYPE variant. |
| `ReadTrackInformationV2` | READ_TRACK_INFORMATION (Mt. Fuji 5) | **unused** — OPEN-bit variant. |
| `SetStreaming` | SET_STREAMING (MMC-5) | **unused** — mos reports, never sets. |

Non-command members: `AddCallbackDispatcherToRunLoop` /
`RemoveCallbackDispatcherFromRunLoop` (async run-loop wiring — unused; mos is
synchronous).

### SCSITaskDeviceInterface — the raw-CDB path (`mos_internal_raw_cdb`, the ONLY exclusive site)

mos authors a raw CDB only with a layer-1 showing; the **four** raw verbs (GESN
0x4A, START STOP UNIT 0x1B, PREVENT ALLOW MEDIUM REMOVAL 0x1E, INQUIRY standard
EVPD=0) all go through `mos_scsi.c`'s `mos_internal_raw_cdb` — the SINGLE
`ObtainExclusiveAccess` call site (internal mechanism only; the public
passthrough was retired — AGENTS.md. The diagnostic `mos probe --capture`
fixed menu issues its read-only command set through the same path). Lifecycle: `ObtainExclusiveAccess` →
`CreateSCSITask` → `SetCommandDescriptorBlock` (+ `SetScatterGatherEntries`,
`SetTimeoutDuration`) → `ExecuteTaskSync` → `GetTaskStatus` /
`GetRealizedDataTransferCount` / `GetAutoSenseData` → `ReleaseExclusiveAccess`.
Also available: `IsExclusiveAccessAvailable`, `AbortTask`, the async
`ExecuteTaskAsync` / `SetTaskCompletionCallback` (unused — synchronous only).

### IOKit registry properties — ZERO commands (`IORegistryEntryCreateCFProperty`)

Read off the media / device nodes: no MMC, no exclusive access, work on mounted
media. The cheap-enrichment surface (disc-ingest gaps note,
`doc/research/2026-06-18-disc-ingest-surfaced-gaps.md`).

- **IOMedia (generic, every media class):** `Content` / `Content Hint` /
  `Content Mask`, `Ejectable`, `Leaf`, `Open`, `Preferred Block Size`
  (`kIOMediaPreferredBlockSizeKey`), `Removable`, `Size` (`kIOMediaSizeKey`),
  `UUID`, `Whole`, **`Writable`** (`kIOMediaWritableKey`, OSBoolean). mos reads
  Size + Preferred Block Size (capacity) and **`Writable`** (read off
  `kIOMediaWritableKey` in the whole-disk walk, `src/mos_scsi.c`
  → `mos.state.v1`/`mos.event.v1` `writable`, tri-state -1/0/1, zero-command off the
  optical media node) — the mechanism bit, not a blank/appendable classification;
  `bsd_node` null ⇒ no whole-disk node ⇒ blank/unrecorded.
- **Optical media TYPE (`kIO{CD,DVD,BD}MediaTypeKey` = `"Type"`, OSString):**
  CD → `CD-ROM` / `CD-R` / `CD-RW`; DVD → `DVD-ROM` / `-R` / `-RW` / `+R` /
  `+RW` / `-RAM` / `HD DVD-{ROM,R,RW,RAM}`; BD → `BD-ROM` / `BD-R` / `BD-RE`.
  ROM-vs-recordable for free. mos reads these as `media_type` (off the `"Type"`
  key in the whole-disk walk, `src/mos_scsi.c`, mapped by
  `mos_internal_media_type_token` to a stable
  `cd_rom`/`dvd_minus_r`/`bd_re`/… token), zero-command off the media node, on
  both `mos.state.v1` and `mos.event.v1` — present even off the not-ready branch
  where `current_profile`/`media_class` are suppressed, and finer than
  `media_class` (the not-ready fallback this row was flagged as a candidate for).
- **`kIOCDMediaTOCKey` (`"TOC"`):** the cached full-TOC `CDTOC` blob. **CD only**
  (no DVD/BD equivalent). mos's primary CD TOC source (`mos_cdtoc.c`).
- **Device Characteristics (drive node):** `kIOPropertySupportedCDFeaturesKey` /
  `…DVDFeaturesKey` / `…BDFeaturesKey` (`"CD/DVD/BD Features"` capability
  bitfields — bit meanings in `IOSCSIMultimediaCommandsDevice.h`),
  `kIOPropertyProductSerialNumberKey` (`"Serial Number"` — the optical SCSI
  stack leaves it empty, so mos reads the serial from GET CONFIGURATION feature
  0108h instead; see `doc/research/2026-06-21-optical-serial-vpd80-vs-0108h.md`).
  Plus `kIOBSDNameKey` / `kIOBSDUnitKey` (the BSD vocabulary).

### Disc-state structs — via the `*MediaBSDClient` ioctls (command-gated, not free)

`IO{CD,DVD,BD}Types.h` define reply structs reached by `DKIOC*` ioctls on the
`IO{CD,DVD,BD}MediaBSDClient` — each is a **command**, not a registry read:
`CDTOC` / `CDMSF` (IOCDTypes); `DVDDiscInfo` (`discStatus`, `erasable`),
`DVDRZoneInfo` (`blank` / `damage` / `incremental`), `DVDPhysicalFormatInfo`,
`DVDCopyrightInfo`, and the `DVDKeyFormat` / CSS / CPRM / region enums
(IODVDTypes). mos reaches the same data through the MMCDeviceInterface
convenience methods above, not these ioctls.

### DiskArbitration — mount-layer reads + the one DA action (optional link dep)

Not an IOKit/MMC surface — the **mount layer** (`mos_da.c`). Linked only when
`MOS_USE_DISKARBITRATION` (default on); the opt-out build stubs all of it to
"unmounted" / "no unmount" with no shape change. Every function returns an
**owned** reference (release as noted). **Provenance** (the framework splits its
headers): the disk-object calls (`DADiskCreate*` / `DADiskCopy*` / `DADiskGetTypeID`)
and the `kDADiskDescription*` keys are verified verbatim against
`DiskArbitration.framework/Headers/DADisk.h` (canonical); `DASessionCreate` /
`DASessionSetDispatchQueue` live in `DASession.h`; `DADiskUnmount` (with its
`DADiskUnmountOptions` / `DADiskUnmountCallback`) in the umbrella
`DiskArbitration.h`. Every function mos uses is `macos(10.4)` EXCEPT
`DASessionSetDispatchQueue` (`macos(10.7)`) — both well under mos's 12.0
deployment floor.

| Function | Header | Signature → result (release) | mos use |
|----------|--------|------------------------------|---------|
| `DASessionCreate` | DASession.h | `(allocator)` → `DASessionRef` (CFRelease) | both DA paths' session. The read path schedules no queue (synchronous); the unmount sets `DASessionSetDispatchQueue` (`macos(10.7)`; a global queue, `NULL` to unschedule before release) + a semaphore. The run-loop scheduling alternatives (`DASessionScheduleWithRunLoop`, the `DAApprovalSession*` family) are unused — mos uses the dispatch-queue path. |
| `DADiskCreateFromIOMedia` | DADisk.h | `(allocator, session, io_service_t)` → `DADiskRef` (CFRelease) | volume lookup. **NAME-BACKED**: reads `kIOBSDNameKey`, delegates to `DADiskCreateFromBSDName` — the ref stores only the `diskN` string, so what it later resolves to is re-checked, not pinned. |
| `DADiskCreateFromBSDName` | DADisk.h | `(allocator, session, const char *)` → `DADiskRef` (CFRelease) | the force-unmount target. |
| `DADiskCopyDescription` | DADisk.h | `(DADiskRef)` → `CFDictionaryRef` (CFRelease) | volume name/path read (keys below). Header note: contacts the daemon for the LATEST description (resolved by the ref's name), unless called inside a registered DA callback. |
| `DADiskCopyIOMedia` | DADisk.h | `(DADiskRef)` → `io_service_t` (**IOObjectRelease**) | volume lookup's **endpoint identity guard** (A2): the IOMedia the ref currently resolves to; mos compares its `IORegistryEntryGetRegistryEntryID` to `media_id` and commits the name/path only on a match. Valid for a READ (no later daemon re-resolution); the unmount ACTION cannot use it (the daemon re-resolves the name AFTER any check — AGENTS TOCTOU addendum). |
| `DADiskUnmount` | DiskArbitration.h | `(disk, options, callback, context)` → `void` (async; callback `(disk, dissenter, context)`, NULL dissenter = success, non-NULL = busy) | the **SINGLE DA action**: `tray eject`'s GRACEFUL unmount with `kDADiskUnmountOptionWhole` (`0x1`) — **never** `Force`, so a busy filesystem dissents and mos surfaces `MOS_ERR_BUSY` (no data loss; both default and --force). Made synchronous via the queue + semaphore (the unbounded-wait KNOWN ISSUE if `DASessionSetDispatchQueue` silently fails — post-tag, ROADMAP). |

Description keys (DADisk.h, both `macos(10.4)`): `kDADiskDescriptionVolumeNameKey`
(CFString → `volume_name`), `kDADiskDescriptionVolumePathKey` (CFURL →
`volume_path`, the mount proof — absent ⇒ not mounted). DADisk.h carries many more
media/device keys (`…Media{Writable,Whole,Size,BSDName}Key`, …) that mos reads
zero-command off the IOKit registry instead, not via DA. Unused DADisk.h surface:
`DADiskCreateFromVolumePath` (`macos(10.7)`), `DADiskGetBSDName`,
`DADiskCopyWholeDisk`, `DADiskGetTypeID`. Privilege: every DA read takes no
entitlement / TCC / exclusive access (scope-doctrine layer 3); `DADiskUnmount` is
the only DA action, and it is GRACEFUL (no `Force`) — mos has no data-loss path.

### DiscRecording device directory — enumeration + identity (`mos_dr.c`)

The zero-command device directory mos enumerates and reads drive identity from —
no MMC, no exclusive access (a framework over the same kext the command path
uses). Verified against `DiscRecording.framework/Headers/DRCoreDevice.h`; all
`10.2+`, every `Copy*` returns OWNED (`CFRelease`).

| Function | Result (release) | mos use |
|----------|------------------|---------|
| `DRCopyDeviceArray(void)` | `CFArrayRef` (CFRelease) | the enumeration source (`mos_enumerate_devices`; `mos_dr.c` snapshot). Returns ALL writable optical devices — coincides with mos's openable set because the §9.1 attach rule blocks `SCSITaskUserClient` on read-only drives (W1 disposition). |
| `DRDeviceCopyDeviceForBSDName(CFStringRef)` | `DRDeviceRef` (CFRelease) | resolve a `diskN` selector → DR device. |
| `DRDeviceCopyDeviceForIORegistryEntryPath(CFStringRef)` | `DRDeviceRef` (CFRelease) | resolve a registry path (a notification's `kDRDeviceIORegistryEntryPathKey`) → DR device → registry id. |
| `DRDeviceCopyInfo(DRDeviceRef)` | `CFDictionaryRef` (CFRelease) | the identity directory (keys below). |
| `DRDeviceCopyStatus(DRDeviceRef)` | `CFDictionaryRef` (CFRelease) | coarse passive status ("not guaranteed current" — the division-of-labour floor; mos owns the synchronous state machine). |
| `DRDeviceIsValid(DRDeviceRef)` | `Boolean` | liveness gate before trusting a DR ref under a hostile/stale directory (R3 hardening). |

`DRDeviceCopyInfo` keys (DRCoreDevice.h, CFString): `kDRDeviceVendorNameKey` /
`kDRDeviceProductNameKey` / `kDRDeviceFirmwareRevisionKey` (the pre-parsed INQUIRY
identity mos caches at open), `kDRDeviceIORegistryEntryPathKey` (→ registry id, the
probe authority), and `kDRDeviceMediaInfoKey` (a SUBDICTIONARY; mos reads its
`kDRDeviceMediaBSDNameKey`). `mos drive` additionally reads
`kDRDevicePhysicalInterconnectKey` (→ `interconnect`: ATAPI/FibreChannel/FireWire/
USB/SCSI) and `kDRDevicePhysicalInterconnectLocationKey` (→ `interconnect_location`:
Internal/External/Unknown) off the same dict, mapped to stable tokens by CFEqual
against the SDK value constants (`mos_dr.c` → `mos_internal_interconnect_token`).
No commands, no exclusive access, no entitlement.

### Watch-wake surface — IOKit interest + DiscRecording doorbell (`mos_watch.c`)

The notification sources the watch schedules on its private run-loop mode — never
command-issuing, all wake-only (the poll floor is the correctness floor; the
doorbell is latency only). Verified against `IOKit.framework/Headers/IOKitLib.h`
+ `IOMessage.h` and `DiscRecording.framework/Headers/DRCoreNotifications.h`.
**Ownership rule: `Create*` returns OWNED (release it), `Get*` returns BORROWED
(do NOT release) — `mos_watch.c` teardown honors it (audited: the IOKit source is
freed via `IONotificationPortDestroy`, the DR source/center are `CFRelease`d).**

IOKit interest (single-target termination / property wake):
- `IONotificationPortCreate(mach_port_t)` → `IONotificationPortRef` — OWNED,
  `IONotificationPortDestroy`.
- `IONotificationPortGetRunLoopSource(port)` → `CFRunLoopSourceRef` — **BORROWED**
  (header: "caller should not release"; freed by `IONotificationPortDestroy`).
- `IOServiceAddInterestNotification(port, service, kIOGeneralInterest, cb, refcon,
  &notif)` — message types from `IOMessage.h`: `kIOMessageServiceIsTerminated`
  (drive removed → terminal for a single-target watch),
  `kIOMessageServicePropertyChange` (re-poll wake).

DiscRecording doorbell (media/tray-change + all-mode discovery):
- `DRNotificationCenterCreate(void)` → `DRNotificationCenterRef` — OWNED.
  **Run-loop affinity: delivers on the run loop it was CREATED on** (the W2
  single-thread-contract basis — to receive on another loop you must create the
  center from it).
- `DRNotificationCenterCreateRunLoopSource(center)` → `CFRunLoopSourceRef` —
  **OWNED** (`Create` — released; contrast the IOKit `Get` above).
- `DRNotificationCenterAddObserver(center, observer, cb, name, object)` /
  `…RemoveObserver(center, observer, name, object)`; callback
  `DRNotificationCallback = void(*)(center, observer, CFStringRef name,
  DRTypeRef object, CFDictionaryRef info)`.
- Notification names: `kDRDeviceAppearedNotification` (all-mode join),
  `kDRDeviceDisappearedNotification`, `kDRDeviceStatusChangedNotification`
  (device-scoped state wake); `kDRDeviceIORegistryEntryPathKey` resolves the
  changed device → registry id. (Apple-availability: the DR notification API is
  `10.2+`, well under mos's 12.0 floor.)

No commands, no exclusive access, no entitlement. Single-target creation failure
falls back to poll-only; all-mode has no poll floor, so `mos_watch_open_all` fails
instead (discovery rides the doorbell).

### Decision order for a new verb

1. In the registry table → a zero-command read. Done.
2. Else a command-issuing convenience method above → non-exclusive, no lock.
   Prefer it.
3. Only if **neither** carries it — a documented absence (the convenience
   `Inquiry` returns only the 36-byte StandardData, so the version descriptors
   at bytes 58-73 are unreachable) or a masking convenience (GetTrayState) —
   author a raw CDB through
   `mos_internal_raw_cdb` with the AGENTS layer-1 showing. Never infer "no convenience
   method" from §9.7; that is a subset, this table is the inventory.
