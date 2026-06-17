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
- **Cross-check:** Linux `include/scsi/scsi.h` (the VERSION value table, as
  `resp[2]+1`); sg3_utils `src/sg_inq_data.c` `sg_version_descriptor_arr`
  (the descriptor code→name table; mos maps the "no version claimed" family
  codes, unknown → hex).

### `src/mos_vpd80.c` — INQUIRY VPD page 0x80 (Unit Serial Number)
- **Spec:** SPC-4 §7.7.13, opcode 0x12 with EVPD=1, PAGE CODE 0x80. Byte 3
  is the (single-byte) PAGE LENGTH; the serial is the ASCII bytes 4..n.
  Byte 2 stays reserved for this page — it is NOT the high byte of a 2-byte
  length (that generalization is page 0x83's, not 0x80's).
- **Cross-check:** sg3_utils `sg_inq.c` (`fetch_unit_serial_num` →
  `vpd_fetch_page` with maxlen −1) takes the single-byte `b[3]` length and
  the serial at `b + 4`. Linux `drivers/scsi/scsi.c` reads
  `get_unaligned_be16(&buf[2])`, which equals `buf[3]` here because byte 2 is
  reserved (zero) on page 0x80 — same value, confirming the single-byte read.
- **Not decoded:** every other VPD page (0x00 supported-pages list, 0x83
  Device Identification, …) and the standard-INQUIRY data itself
  (vendor/product/revision come from DiscRecording's directory, zero
  commands) — this parser decodes only the serial page.
