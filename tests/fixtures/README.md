# Fixtures

SCSI/MMC command responses used as test inputs. Two kinds live here, and
the distinction matters:

1. **Spec-derived fixtures (authoritative).** Byte buffers built to the
   T10/MMC layout. These are the correctness inputs: mos is a spec-defined-MMC
   library, so a response that is valid *per the spec* is a valid test input
   regardless of which drive would emit it. A single drive can neither validate
   nor define conformance (see `../../ROADMAP.md`, "Gate — spec-conformance").
   Each is verified by being parsed through the production code path.

2. **Device captures (optional realism).** Raw `.bin` pulled from a real drive.
   These corroborate the spec-derived set; they do not gate it. None are
   required to ship. The contributing path below applies if you have a drive
   and want realism or to catch a non-conformant quirk.

## Present fixtures

| File | Command | Models | Consumed by |
|------|---------|--------|-------------|
| `getconfig_dvdrom_current.bin` | GET CONFIGURATION 0x46 (RT=0) | DVD-ROM drive, current profile 0x0010; Profile List + Core + Removable Medium | `test_config.c :: config_walks_real_dvdrom_profile_list` |
| `readdiscinfo_blank_cdr.bin` | READ DISC INFORMATION 0x51 (type 000b) | blank CD-R — disc status 00 (Blank) | `test_discinfo.c :: discinfo_blank_cdr_decodes_blank` |
| `readdiscinfo_complete_cdrom.bin` | READ DISC INFORMATION 0x51 (type 000b) | finalized CD-ROM — disc status 10 (Complete) | `test_discinfo.c :: discinfo_complete_cdrom_decodes_complete` |

All three are walked/decoded through the production parser under ASan in CI.

### `getconfig_dvdrom_current.bin` (40 bytes)
Header Data Length `0x24` (= 40 − 4), current profile `0x0010`. Three features:
`0x0000` Profile List (cur+persist, payload = DVD-ROM current + CD-ROM not),
`0x0001` Core (cur+persist v2, Physical Interface = ATAPI 0x02), `0x0003`
Removable Medium (cur+persist v2, byte `0x29` = tray-load / eject / lock).

### `readdiscinfo_blank_cdr.bin` / `readdiscinfo_complete_cdrom.bin` (34 bytes each)
A matched pair differing only in the disc-completion signal, so a test that asserts
Blank-vs-Complete is isolated from the rest of the response:

```
byte 2    : 00 (Blank, last-session empty)  →  0E (Complete, last-session complete)
byte 16-19: 00 61 18 01 (ATIP lead-in MSF)  →  FF FF FF FF (FFh-filled, finalized)
byte 20-23: 00 4F 3B 4A (lead-out MSF)      →  FF FF FF FF
```

The address fields encode the real spec behaviour: a **blank recordable** disc
carries valid ATIP-derived lead-in (97:24:01) and last-possible-lead-out
(79:59:74, an 80-min disc); a **finalized** disc has no further-recording
target, so those fields are FFh-filled. (The parser does not decode the address
fields — status and the session/track counts are the signal — but the fixtures
carry spec-faithful values so they stay usable when address decode lands.)

Caveats baked in: MSF values in the blank fixture are illustrative (per-disc,
from ATIP). The FFh fill on the complete fixture spans the nominally-reserved
bytes 16/20 (observed convention; a strict reading leaves those `00`). OPC table
entries = 0; real blank media often returns OPC entries, extending the response
8 bytes each (the length field then grows past `0x20`). DID_V/DBC_V/DAC_V are
clear, so the Disc ID / bar code / app-code fields are zeroed (not valid).

## Disc-status ground truth (READ DISC INFO byte 2, bits 1:0)

`blank=00  appendable=01  complete/finalized=10  other=11`. From real
`dvd+rw-mediainfo` output, the expected decode:

| Media | Tool string | byte-2 status |
|-------|-------------|---------------|
| blank DVD+RW (RICOHJPN/W11) | `Disc status: blank` | 00 |
| complete DVD+R (CMC MAG/M01) | `Disc status: complete` | 10 |
| multi-session DVD-RW, last session reserved/damaged | `Disc status: complete` | 10 |

## READ CAPACITY reference

Origin-zero: `number_of_blocks = last_LBA + 1`. The decoded label shifts between
`sg_readcap` builds (`Last logical block address` / `Number of blocks` vs
`Last LBA` / `Number of logical blocks`) — irrelevant to mos, which parses the
raw 8-byte (10) / 32-byte (16) response, not the tool's text. Cross-check from
`dvd+rw-mediainfo`: complete DVD+R reports `1409376*2048` → raw READ CAPACITY(10)
last_LBA = `00 15 81 9F` (1409375), block length = `00 00 08 00` (2048). A raw
READ CAPACITY fixture lands with the v0.4 `capacity` parser.

## Format

Raw bytes, no wrapper. Filename encodes semantics:
`<command>_<state>_<drive-model>.bin` for captures; spec-derived buffers drop
the model. When the planned `mos_capture` tool ships (v0.3, see
`../../INTEGRATION_HARNESS.md`), each `.bin` gains a `.json` manifest with command
metadata, transfer length, task status, parsed sense, and a SHA-256.

## Contributing device captures

If you have an optical drive and want to add realism:

1. Build `mos` (default configure; the diagnostic `probe` subcommand
   rides it under `MOS_CLI_PROBE`, default ON).
2. Run `mos status --json` in each tray/media state (plus one
   `mos probe --dump` per drive for the DiscRecording dictionaries)
   and submit the output with the integration-harness PR.
3. For raw byte capture, write a small C program that calls
   `mos_open_by_bsd_name()` then `mos_raw_cdb()` with the CDB of interest
   (e.g. 0x46 GET CONFIGURATION, 0x51 READ DISC INFO) and dump the buffer.
   `mos_raw_cdb` is public and works today; the `mos_capture` convenience
   wrapper does not exist yet.

## Log-derived captures (2026-06-12, media-info stage 1)

Three fixtures reversed from PUBLISHED tool output through the tools'
source code — the methodology the disc-status ground-truth table above
established, applied to the new v0.4 commands. Tier: device capture
(realism), with the attested-vs-scaffold split recorded per fixture.

| File | Command | Models | Consumed by |
|------|---------|--------|-------------|
| `readtoc_f0_audio_cd_single.bin` | READ TOC/PMA/ATIP 0x43 fmt 0000b | real 4-track audio CD single | `test_config.c :: toc_parses_real_pony_cd_single` |
| `readdiscinfo_blank_bdr.bin` | READ DISC INFORMATION 0x51 | blank BD-R (BH16NS40 / JVC-AM/S6L) | `test_discinfo.c :: discinfo_blank_bdr_decodes_blank` |
| `getconfig_aacs_wh16ns40.bin` | GET CONFIGURATION 0x46 (RT=0) | WH16NS40 1.05 AACS feature | `test_config.c :: aacs_caps_from_real_wh16ns40_capture` |

### `readtoc_f0_audio_cd_single.bin` (44 bytes)
Ginuwine "Pony" CD single (1996). Provenance: whipper rip log in
whipper-team/whipper PR #382 — MusicBrainz disc
`TX6lKZ481BHv1ZW6pd6007j6OY4-` (release 84e4ddc3-92fd-49d5-b15d-3be7f85c85ab),
CDDB 2d047f04, AccurateRip-confirmed (confidence 24-34). LBAs derived
from the log's attested `toc=1+4+86497+150+24687+47627+68002`
(MB offset = TOC LBA + 150): tracks at 0 / 24537 / 47477 / 67852,
lead-out 86347. Byte map per cdrecord `scsi_cdr.c struct trackdesc`
(byte 1 = ADR<<4|control, bytes 4-7 BE32 LBA) and libcdio sector.c
(LSN = LBA, pregap 150). ATTESTED: track count, every LBA, audio (the
log records stereo, no pre-emphasis). ASSUMED (standard commercial
pressing): ADR=1, copy-permit bit 0. Derivation note: a transcription
error in the research pass (track 2 as 0x5FE9) was caught by
recomputing from the toc string — 24537 = 0x5FD9; the toc string is
the authority.

### `readdiscinfo_blank_bdr.bin` (34 bytes)
Blank BD-R, HL-DT-ST BD-RE BH16NS40 1.00, media ID JVC-AM/S6L.
Provenance: complete dvd+rw-mediainfo log,
github.com/kneutron/ansitest `burnUDFsystem2DVD+R` lines 58-89
("Mounted Media: 41h, BD-R SRM / Disc status: blank / Number of
Sessions: 1 / Next Track: 1 / Number of Tracks: 1 / State of Last
Session: empty"). Byte map per dvd+rw-tools 7.1 `dvd+rw-mediainfo.cpp`
lines 863-887: status = b2&3, last-session = (b2>>2)&3, sessions =
b9<<8|b4, next track = b10<<8|b5, tracks = b11<<8|b6. Erasable clear
(BD-R is write-once; note dvd+rw-mediainfo prints no "Erasable:" line
— it consumes b2&0x10 only as a READ FORMAT CAPACITIES gate, so an
"Erasable:" line in a found log means cdrecord -minfo, not this tool).
Bytes 7-33 zero: DID_V/DBC_V/DAC_V clear, and the CD ATIP lead-in/out
MSF fields (16-23) are not applicable to BD — zeroed, unattested.
Corroborates `schemas/examples/mos.metadata.v1.blank_bdr.json`, whose
disc_info values match this capture field-for-field.

### `getconfig_aacs_wh16ns40.bin` (20 bytes)
AACS feature (0x010D) for HL-DT-ST BD-RE WH16NS40, revision 1.05.
Provenance: MakeMKV drive-info dump in blog.ssanj.net 2023-10-02
(verified via its source mirror raw.githubusercontent.com/ssanj/
babyloncandle-docker): "Bus encryption flags: 17", "Highest AACS
version: 78". ATTESTED: payload byte 0 = 0x17 (per the libaacs/
UDFclient bit map: RDC|WBE|BEC|BNG — the dump prints no 0x prefix;
hex is the reading consistent with every observed LG value) and
payload byte 3 = 78. UNATTESTED scaffold: header current profile
(0x0000 — dump context had no disc proven), feature-header byte 2
(version/persistent/current bits), nonce block count, AGID count
(all zeroed). Mapping caveat: MakeMKV's "Highest AACS version" line
plausibly reads the 0x010D byte but could be MKB-derived — two
same-model/same-firmware BU40N dumps show 77 vs 81
(automatic-ripping-machine#1558, jlesage/docker-makemkv#248), so the
value is drive/disc STATE, not a firmware constant, whichever source
MakeMKV prints. A hardware capture of the raw descriptor next to a
MakeMKV dump from the same drive settles it (falsification matrix).
