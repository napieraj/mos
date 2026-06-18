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
  decoded inline.
- **Not decoded (bytes 12+):** Disc Identification, lead-in / lead-out
  addresses, bar code, OPC table — informational, not the status.

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
- **Spec:** MMC-6, opcode 0xAC, Performance Data TYPE 00h; the Nominal
  Performance Descriptor layout is built to spec.
- **Out of scope:** TYPE 03h write-speed descriptors. Apple's
  GetPerformance convenience method exposes TOLERANCE/WRITE/EXCEPT but not
  the TYPE field, so TYPE 03h is unreachable without a raw CDB.
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
  media-type string); for CDs it is now the primary TOC source (see Provenance
  below), the issued path its fallback. `mos_internal_cdtoc_parse` decodes it
  into the per-session layout (`mos.metadata.v1.disc.session_layout`), and
  `mos_internal_cdtoc_to_toc` into the per-track `mos_toc`.
- **Cross-check:** libcdio `lib/driver/osx.c` `read_toc_osx` — same
  `CDTOCGetDescriptorCount` walk, `adr==1` filter, POINT 0xA0/0xA1/0xA2
  handling, and `CDConvertMSFToLBA` (minus the 150-frame pregap). Struct layout:
  Apple `IOCDTypes.h` (`CDTOC` / `CDTOCDescriptor` / `CDMSF`).
- **Provenance:** the earlier "banked, not built" stance
  (`doc/research/2026-06-14-state-verb-rename.md`; analysis
  `doc/research/2026-06-13-disc-tools-state-survey.md` §6) was overridden
  2026-06-18 — see the AGENTS.md ADR. The cached full-TOC is now the **primary**
  CD TOC source: `mos_query_toc` decodes it via `mos_internal_cdtoc_to_toc`
  (fail-closed to this format-0000b standard — a duplicate track or a gap in
  first..last refuses the whole), with the issued `ReadTableOfContents` as the
  fallback (no `IOCDMedia` node yet) and the only path for DVD/BD.

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

### `src/mos_modepage.c` — MODE SENSE(10) optical pages
- **Spec:** page 0x2A is MMC-3 page-2A (CD/DVD Capabilities & Mechanical
  Status); page 0x01 is the SPC Read/Write Error Recovery page. Sub-page
  format (SPF=1) has a 4-byte header with a BE16 length; 0x2A/0x01 are
  page_0 format. Read-only — no MODE SELECT.
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
  - **Firmware Information (010Ch):** MMC-6 r02g §5.3.43, Table 197 — payload
    Century[2] Year[2] Month[2] Day[2] Hour[2] Minute[2] Second[2]
    Reserved[2], decimal ASCII, GMT; Additional Length 0x10. Emitted as an
    RFC 3339 UTC string (the format `mos.event.v1`'s `ts` uses). NB: 0x1FF
    (libcdio's `FIRMWARE_DATE`) is *Reserved* in MMC-6 — the feature is 010Ch.

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

### `src/mos_vpd80.c` — INQUIRY VPD page 0x80 (Unit Serial Number)
- **Spec:** SPC-4 §7.7.13, opcode 0x12 with EVPD=1, PAGE CODE 0x80. Byte 3
  is the (single-byte) PAGE LENGTH; the serial is the ASCII bytes 4..n.
  Byte 2 stays reserved for this page — it is NOT the high byte of a 2-byte
  length (that generalization is page 0x83's, not 0x80's).
- **Under-delivery refused / overflow marked:** the serial is a durable cache
  key, so completeness is enforced two ways. If PAGE LENGTH exceeds the bytes
  delivered (the transport under-filled the reply), the parser refuses rather
  than cache a silent prefix — complete or nothing. If the serial is complete
  on the wire but longer than the output buffer, it is truncated with a visible
  trailing `...` marker (never a silent prefix); real serials sit far below the
  64-byte sink, so this is a pathological/hostile-input guard.
- **Cross-check:** sg3_utils `sg_inq.c` (`fetch_unit_serial_num` →
  `vpd_fetch_page` with maxlen −1) takes the single-byte `b[3]` length and
  the serial at `b + 4`. Linux `drivers/scsi/scsi.c` reads
  `get_unaligned_be16(&buf[2])`, which equals `buf[3]` here because byte 2 is
  reserved (zero) on page 0x80 — same value, confirming the single-byte read.
- **Not decoded:** every other VPD page (0x00 supported-pages list, 0x83
  Device Identification, …) and the standard-INQUIRY data itself — this parser
  decodes only the serial page. (vendor/product/revision come zero-command
  from DiscRecording's directory on every path EXCEPT `mos drive`, which
  prefers a fresh raw standard INQUIRY via mos_inqdata.c and falls back to the
  DR cache only on BUSY.)

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
| `Inquiry` | INQUIRY (SPC-2) | standard INQUIRY (`mos_inqdata.c`, `mos drive`). **StandardData-only — no EVPD/PAGE_CODE**, so the VPD-0x80 serial is raw (`mos_serial.c`). |
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

### SCSITaskDeviceInterface — the raw-CDB path (`mos_raw_cdb`, the ONLY exclusive site)

mos authors a raw CDB only with a layer-1 showing; the **four** raw verbs (GESN
0x4A, START STOP UNIT 0x1B, PREVENT ALLOW MEDIUM REMOVAL 0x1E, INQUIRY EVPD
0x80) all go through `mos_scsi.c`'s `mos_raw_cdb` — the SINGLE
`ObtainExclusiveAccess` call site. Lifecycle: `ObtainExclusiveAccess` →
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
  Size + Preferred Block Size (capacity) and **`Writable`** (`mos_internal_read_writable`
  → `mos.state.v1`/`mos.event.v1` `writable`, tri-state -1/0/1, zero-command off the
  optical media node) — the mechanism bit, not a blank/appendable classification;
  `bsd_node` null ⇒ no whole-disk node ⇒ blank/unrecorded.
- **Optical media TYPE (`kIO{CD,DVD,BD}MediaTypeKey` = `"Type"`, OSString):**
  CD → `CD-ROM` / `CD-R` / `CD-RW`; DVD → `DVD-ROM` / `-R` / `-RW` / `+R` /
  `+RW` / `-RAM` / `HD DVD-{ROM,R,RW,RAM}`; BD → `BD-ROM` / `BD-R` / `BD-RE`.
  ROM-vs-recordable for free. mos reads these as `media_type`
  (`mos_internal_read_media_type` → `mos_internal_media_type_token`, a stable
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
  stack leaves it empty, so mos reads serial via raw VPD 0x80; see the
  2026-06-16 serial doc). Plus `kIOBSDNameKey` / `kIOBSDUnitKey` (the BSD
  vocabulary).

### Disc-state structs — via the `*MediaBSDClient` ioctls (command-gated, not free)

`IO{CD,DVD,BD}Types.h` define reply structs reached by `DKIOC*` ioctls on the
`IO{CD,DVD,BD}MediaBSDClient` — each is a **command**, not a registry read:
`CDTOC` / `CDMSF` (IOCDTypes); `DVDDiscInfo` (`discStatus`, `erasable`),
`DVDRZoneInfo` (`blank` / `damage` / `incremental`), `DVDPhysicalFormatInfo`,
`DVDCopyrightInfo`, and the `DVDKeyFormat` / CSS / CPRM / region enums
(IODVDTypes). mos reaches the same data through the MMCDeviceInterface
convenience methods above, not these ioctls.

### Decision order for a new verb

1. In the registry table → a zero-command read. Done.
2. Else a command-issuing convenience method above → non-exclusive, no lock.
   Prefer it.
3. Only if **neither** carries it — a documented absence (INQUIRY's missing
   EVPD) or a masking convenience (GetTrayState) — author a raw CDB through
   `mos_raw_cdb` with the AGENTS layer-1 showing. Never infer "no convenience
   method" from §9.7; that is a subset, this table is the inventory.
