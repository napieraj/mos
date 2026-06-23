# Feature survey — candidate features to implement in `mos`

**Date:** 2026-06-23. **Status:** research note (deep-research workflow output).
**Scope of the ask:** survey BROADLY across three angles — (1) new disc/drive
*facts* mos could report, (2) peer-tool feature gaps, (3) CLI/UX & output
features — *deliberately ignoring* mos's current scope doctrine, then triage.

This is a catalog, not a commitment. Many entries cross a stated ADR or
ROADMAP "out of scope" line; per the maintainer's "name what it crosses"
discipline each boundary-crosser says which line it crosses so the override
(if any) is explicit. Nothing here changes behavior — it's a menu.

## Method

Five parallel web-research agents (one per angle, MMC/SCSI facts split into
two), each returning falsifiable `CLAIM/SOURCE/EVIDENCE/CONFIDENCE` blocks.
Claims were then cross-checked against mos's **actual reported-field surface**
(the `schemas/mos.*.v1.json` field set) and against source
(`src/mos_discinfo.c`, `mos_perf.c`, `mos_modepage.c`, `mos_cdtoc.c`,
`include/mos.h`). That cross-check is the verification value-add: it
reclassifies a large fraction of agent "mos lacks X" claims to **ALREADY
SHIPPED** (the schema is authoritative ground truth, stronger than a web
claim) and confirms the genuinely-new ones against the decoder source.

**Status tags below:** `NEW` (not in any schema, decoder confirmed absent) ·
`PARTIAL` (a related field exists; this extends it) · `CROSSES` (would need an
ADR rebuttal — names the line).

---

## Excluded up front — already shipped (do NOT re-propose)

Verified present in `schemas/` and decoder source. Agents surfaced several of
these as "gaps"; they are not.

- **DVD/BD physical-format structure** — book type, layer type, track path
  (PTP/OTP), num layers, disc size, max rate, linear/track density
  (`mos.metadata` `disc_structure`/`physical`).
- **Per-disc copyright/region** (READ DISC STRUCTURE 01h) — `copyright`,
  `region` in `mos.metadata`.
- **BCA** — `bca` field in `mos.metadata`.
- **READ TRACK INFORMATION per-track facts** — NWA (`next_writable`), LRA
  (`last_recorded`), `blank`, `damage`, `track_mode`, `data_mode`,
  `free_blocks`, `track_size` (`mos.metadata` `track_info`).
- **CD-TEXT base** — album title/performer + per-track titles/performers
  (`cdtext`); extended fields deferred (see F11).
- **ATIP** — raw lead-in M:S:F (`atip`); MID→manufacturer-name table is
  deliberately consumer-side (ADR 2026-06-21).
- **session_layout** (cached full-TOC, `kIOCDMediaTOCKey`).
- **Background-format status** — `bg_format`/`bg_format_name` (the MRW/BG part
  of READ DISC INFORMATION; the disc-ID/barcode/OPC part is **not** — see F1).
- **Drive identity & capability** — vendor/product/revision/version
  descriptors, serial (feature 0108h), interconnect + location, content-
  protection capability block (CSS/CPRM/AACS/SecurDisc/VCPS), firmware date
  (010Ch), write-protect (0004h), nominal read/write *max* speed.
- **Capacity** — READ FORMAT CAPACITIES + kernel-cached size (`mos.capacity`).
- **Full feature enumeration** — `mos features` already dumps the raw GET
  CONFIGURATION feature list (codes/current/version/persistent).

---

## Tier 1 — new command-grounded facts (highest value, mostly in/near doctrine)

Ranked by value × feasibility.

### F1 — READ DISC INFORMATION extended fields `NEW`
**disc_id (32-bit, DID_V-gated), disc barcode (DBC_V-gated), OPC table
(`n_opc` + per-speed entries), application/disc-type byte, last-session
lead-in / last-possible lead-out.** mos's `mos_discinfo.c` decodes only
`sessions` + `erasable` from the 0x51 reply it *already issues* — so these are
more fields off a command already in hand, zero new command, no new lock.
`disc_id` is the standout: a **content-independent per-disc fingerprint** for
recordable media (the "is this the same blank/burned disc" question), reachable
purely via MMC — unlike the MusicBrainz/AccurateRip IDs (X1) it needs no
sector reads and crosses no consumer-side line.
- Source: Linux `include/uapi/linux/cdrom.h` `struct disc_information`
  (`disc_id`, `did_v`, `disc_bar_code[8]`, `dbc_v`, `n_opc`);
  https://www.spinics.net/lists/linux-scsi/msg161016.html (field semantics).
- Confidence: **high**. Crosses nothing.

### F2 — GET PERFORMANCE full speed-descriptor list + write-speed descriptors `PARTIAL`
mos returns only the *max* read/write + a count (`mos_perf.c`, TYPE 00h). Peers
enumerate every read/write **speed descriptor** row, and the write-speed list
(GET PERFORMANCE Type 03h, with WRC) is what burn tools key on.
- Source: `dvd+rw-mediainfo.cpp` ("Speed Descriptor#", "Write Speed #",
  "Current Write Speed"); `cdrecord -prcap`; cdrom.h `GPCMD_GET_PERFORMANCE`.
- Value: write-strategy / speed-selection for burn/rip consumers. Convenience
  method (`GetPerformance`), no raw CDB. Confidence: **high** (gap) /
  **medium** (Type 03h layout). Crosses nothing.

### F3 — MODE PAGE 0x2A capability bits mos omits `NEW`
mos reads page 2A for loading mechanism / lock / eject / buffer only
(`mos_modepage.c`). The page also carries: **C2-error-pointer support**,
**BURN-Free / buffer-underrun protection**, **Mount Rainier write**, per-format
write capability (DVD-R/+R/RAM), media-changed. The **C2 flag** is the single
most rip-relevant bit — EAC/AccurateRip select read strategy on it.
- Source: libcdio `cd-info` `print_drive_capabilities()` +
  `include/cdio/device.h` (`CDIO_DRIVE_CAP_READ_C2_ERRS`,
  `..._WRITE_MT_RAINIER`, `..._MISC_MEDIA_CHANGED`); `cdrecord -prcap` (BURNFREE).
- Caveat (from the EAC/Hydrogenaudio sources): C2-flag *presence* ≠
  reliability; some firmware reports C2 incorrectly. Report the bit, don't
  promise accuracy. Zero new command. Confidence: **high**. Crosses nothing.

### F4 — Blu-ray Spare Area Information / DDS — defect-management headroom `NEW`
Remaining vs used spare blocks for formatted BD-RE/BD-R (READ DISC STRUCTURE BD
format 0Ah SAI, 08h DDS). A disc-health proxy (defect headroom) *without* an
active surface scan — same command family mos already uses for disc structure.
- Source: T10 05-206r0 (BD format structure codes); Blu-ray spare-area /
  defect-management. Confidence: **medium** (reachability + exact layout
  unverified on hardware). Crosses nothing structural.

### F5 — DVD region / RPC drive state via REPORT KEY (read-only) `NEW` `CROSSES`
Current region code, RPC phase (I/II), **user resets remaining (max 5)**,
**vendor resets remaining (max 4)**. Distinct from the per-disc region *mask*
mos already reports — this is the *drive's* region-management state.
- Source: REPORT KEY (0xA4) / Regional Playback Control (Wikipedia RPC).
- **Crosses:** (a) the one-of-four raw-CDB count — REPORT KEY is a raw CDB
  (read-only); (b) the explicit line `include/mos.h:466` / `mos_pure.h:358`
  "region/key state lives behind REPORT KEY, which mos does not issue." A
  read-only REPORT KEY for region/resets-remaining (no SEND KEY, no key
  exchange) is the narrowest possible crossing. Confidence: **high** (the
  facts) — adoption is a doctrine decision.

### F6 — CD MCN + per-track ISRC via READ SUB-CHANNEL `NEW` `CROSSES`
Media Catalog Number (UPC-EAN, format 02h, MCVal-gated) and per-track ISRC
(format 03h, TCVal-gated). The authoritative source is the subchannel, not
CD-TEXT.
- Source: MMC-2 (T10 97-117r0) READ SUB-CHANNEL formats 02h/03h.
- **Crosses:** READ SUB-CHANNEL (0x42) has no `MMCDeviceInterface` convenience
  wrapper → a new raw CDB (needs the layer-1 showings). ROADMAP already lists
  ISRC/UPC (within CD-TEXT) as deferred stage-2; the subchannel is the better
  carrier. `mos_cdtoc.c:62` notes the catalogue/ISRC boundary explicitly.
  Confidence: **high**.

### F7 — Decode select GET CONFIGURATION features into named drive facts `PARTIAL`
`mos features` already enumerates the raw list; this *decodes* high-signal ones
into `mos drive`: Real-Time Streaming (0107h), Power Management (0100h),
Time-out (0105h), Defect Management, Microcode Upgrade, Mount Rainier, Layer
Jump, Disc Control Blocks.
- Source: `sg_get_config` feature table. Confidence: **medium** (some codes
  unpinned from free sources). Zero new command. Crosses nothing.

### F8 — MECHANISM STATUS (0xBD): fault / changer state / current slot `NEW`
Generic drive-mechanism facts; the **fault bit** is a drive-health signal, the
changer/slot fields matter only for changers (rare on Mac).
- Source: MMC-2 (T10 97-108r0); cdrom.h `GPCMD_MECHANISM_STATUS`. Confidence:
  **high** (facts) / value **low-medium** (tray already via GESN).

### F9 — Derived classifications from data mos already has `PARTIAL`
Cheap value off existing reads: **CD-Extra/Enhanced-CD** flag (audio session 1
+ data session 2, from `session_layout`); **disc mode** (CD-DA / CD-ROM /
CD-ROM XA) and finer audio-TOC columns (green/copy/channels) à la libcdio
`cd-info`. mos already has `session_layout`, `control`, `pre_emphasis`.
- Source: libcdio `cd-info`; Blue Book CD-Extra. Confidence: **high**.

### F10 — READ BUFFER CAPACITY (0x5C): live buffer free/total `NEW`
Marginal — a *transient* value, not stable state; only useful mid-write.
- Source: cdrom.h `GPCMD_READ_BUFFER_CAPACITY`; osdev. Confidence: **high**
  (fact) / value **low**.

### F11 — Extended CD-TEXT fields `PARTIAL`
Songwriter/composer/arranger/genre/message + disc UPC/ISRC packs +
multi-language blocks (ROADMAP stage-2, deferred). Same convenience
`ReadTableOfContents` mos already issues (format 0101b) — no new command.
- Source: libcdio cd-text-format. Confidence: **high**. Crosses nothing
  (already on the deferred list).

---

## Tier 2 — CLI / UX / output features (low-risk, high-utility, mostly new)

### U1 — Selectable columns for `list`/tables `NEW`
`-o field,...`, `-o +COL` (extend default), `--output-all`, `--list-columns`.
The single most-requested machine-table affordance.
- Source: `lsblk(8)`. Confidence: **high**.

### U2 — `--pairs` key="value" terse output (+ `-y/--shell`) `NEW`
Line-oriented `KEY="value"` (unsafe chars hex-escaped), with a shell-safe-name
mode so output is `eval`-able. The de-facto awk/shell companion to JSON.
- Source: `lsblk(8)` `--pairs`/`--shell`; `nmcli -t`. Confidence: **high**.

### U3 — `json-seq` (RFC 7464) framing option for `watch` `NEW`
RS-prefixed (0x1E) records → resync-safe stream framing, stronger than bare
NDJSON for long-lived consumers. Offer alongside current NDJSON.
- Source: `journalctl -o json-seq`. Confidence: **high**.

### U4 — Resumable watch: cursor / `--since` `PARTIAL`
mos events already carry `seq` + `ts`. Add `--since <ts|seq>` and a
`--show-cursor`/`--after-cursor` so a restarted consumer doesn't miss/replay.
- Source: `journalctl --cursor/--after-cursor/--since`. Confidence: **medium**.

### U5 — macOS-native `--plist` output `NEW`
XML property list is the macOS structured-output convention (`diskutil
-plist`); a mos `--plist` matches what mac users/automation expect.
- Source: diskutil `-plist`. Confidence: **medium**.

### U6 — `--plain` tabular mode `NEW`
Explicit machine-table mode (clig.dev) so human formatting can't break
grep/awk pipelines.
- Source: clig.dev. Confidence: **high**.

### U7 — Prometheus text-exposition mode `NEW`
`mos --metrics` → `# HELP`/`# TYPE` gauges (`mos_disc_present`,
`mos_drive_state`, capacity/free) for node_exporter-style scraping — a new
monitoring consumer class for the "is a disc in the drive" question.
- Source: Prometheus exposition format. Confidence: **high** (format) /
  medium (fit).

### U8 — Flat grep-JSON (`--json=g`) + YAML (`--json=y`) `NEW`
Dotted-key flat JSON for grep without a parser; YAML as a same-model alt.
- Source: smartctl `--json=giosu`; jc `-y`. Confidence: **high**.

### U9 — Larger-than-53-bit integers as number+string `NEW`
For LBA/capacity fields that can exceed JSON's safe-integer range, optionally
emit both forms (smartctl `--json=v`). Niche but defensive.
- Source: smartctl. Confidence: **medium**.

### U10 — Provenance/meta block `PARTIAL`
`mos.metadata` already has `captured_at`; generalize to a small meta envelope
(mos version + capture time) across documents, à la jc `_jc_meta`.
- Source: jc `--meta-out`. Confidence: **high**.

### U11 — Hygiene audit (likely mostly done) `PARTIAL`
Confirm/contract-test: `NO_COLOR`, isatty human/machine switch, `--quiet`,
`--version`, shell completion (a `completions/` dir exists). smartctl's
**exit-code bitmask** is noted as an *alternative* to mos's current sysexits
`EX_*` — a different model, not obviously better; recommend keeping `EX_*`.
- Source: clig.dev; smartctl exit bitmask. Confidence: **medium**.

> Corroboration (not a feature): smartctl's `json_format_version` and lsblk's
> versioned JSON validate mos's per-document `schema` field design.

---

## Tier 3 — boundary-crossers / out-of-scope (flagged, mostly decline)

### X1 — Computed CD disc-IDs (MusicBrainz / freedb-CDDB / AccurateRip) `CROSSES`
mos has every TOC input → these are **pure computations, zero commands**
(SHA-1/URL-safe-base64 for MusicBrainz; the XXYYYYZZ checksum for CDDB; the
TOC-offset id for AccurateRip). High consumer demand. **Crosses** ROADMAP's
"Third-party ids (MusicBrainz / AccurateRip / dvdid / BDMV) are *permanently
consumer-side*." Note: only the *disc ID* is pure-TOC; AccurateRip's per-track
content CRCs need sector reads (out). If any third-party id is ever admitted,
this is the cheapest. Source: musicbrainz.org Disc_ID_Calculation; libdiscid;
HydrogenAudio AccurateRip. Confidence: **high**.

### X2 — Disc QUALITY / error scanning (C1/C2, PIE/PIF, jitter, LDC/BIS) `CROSSES`
The flagship consumer feature (Nero DiscSpeed, dvdisaster). **Crosses**
reporter-not-controller *and* the no-retry ethos — it's an active multi-pass
scan, not a state read. Also drive-dependent (most drives can't report
PIE/PIF) and the sources warn quality scans are **not** lifetime predictors.
Recommend decline as a `mos` feature (it's a different tool). Source: Nero
DiscSpeed ScanDisc; dvdisaster manual. Confidence: **high**.

### X3 — Read transfer-rate benchmark `CROSSES`
Speed-vs-position curve (Nero). Same active-operation category as X2. Distinct
from F2's *nominal* descriptors. Decline. Source: Nero CD-DVD Speed.

### X4 — Read-offset / drive-cache surfacing for accurate ripping `PARTIAL`
The read-offset value itself is an external DB keyed on INQUIRY identity (which
mos already surfaces well) — not a drive-queryable fact. The *in-scope* slice
is F3 (expose the C2 + cache-behavior capability bits); the offset number
stays consumer-side. Source: EAC extraction tech; AccurateRip offset DB.

### X5 — M-DISC capability detection (feature 0x0028) `CROSSES`
ROADMAP already marks "speculative v1.x." Independently, the research found
detection is **unreliable** — newer M-DISC BD-Rs share media IDs with standard
inorganic BD-R. Recommend **do not pursue** (the bit doesn't mean what it
claims). Source: Wikipedia M-DISC. Confidence: **medium**.

### X6 — AACS Volume ID / PMSN / MKB version `CROSSES`
Requires AACS authentication (host certificate, REPORT/SEND KEY crypto) —
explicit ROADMAP out-of-scope (MakeMKV/LibreDrive territory). Only the AACS
*capability bit* (already surfaced) is in-scope. PMSN lives in the BCA but
whether it's readable without auth is hardware-uncertain. Decline. Source:
AACS BD spec; MakeMKV forum. Confidence: high (the boundary).

### X7 — Filesystem-level identity (ISO9660/UDF labels & IDs, dvdid, DVD-Video titles/chapters) `CROSSES`
Volume label / SYSTEM_ID / PUBLISHER_ID / dvdid CRC64 / VIDEO_TS title-chapter
structure all need **sector/file reads** — scope layer 3 (no block-device
I/O). mos's DiskArbitration `volume_name`/`volume_path` is the in-scope
equivalent for the mounted label. Consumer-side (consistent with the PVD-parser
removal). Source: libblkid; libdvdread/lsdvd; pydvdid. Confidence: **high**.

### X8 — LibreDrive / UHD-friendly flash state `CROSSES`
A proprietary firmware probe (MakeMKV's own), not standard MMC. Out. Source:
makemkvcon LibreDrive Information panel.

---

## Recommended shortlist (if picking a handful)

1. **F1 disc_id + barcode + OPC** — pure win: a real per-disc fingerprint off a
   command already issued, crosses nothing.
2. **F3 C2 / BURN-Free / write-capability bits** — highest rip-consumer value,
   zero new command (more bits off page 2A).
3. **U1 + U2 selectable columns + `--pairs`** — the biggest machine-consumer
   ergonomics jump, no protocol risk.
4. **F2 full speed-descriptor list** — burn consumers; convenience method.
5. **F9 CD-Extra / disc-mode derivation** — cheap classification off existing
   data.
6. **U3 json-seq for watch** — robust streaming, small change.

**Decline / park:** X2/X3 (scanning & benchmarking — different tool), X5
(M-DISC unreliable), X6/X7/X8 (auth/filesystem/firmware — out of scope).
**Explicit doctrine calls (worth a yes/no):** F5 (read-only REPORT KEY region
state — one raw CDB), F6 (READ SUB-CHANNEL MCN/ISRC — one raw CDB), X1
(pure-TOC MusicBrainz/CDDB ids — crosses "permanently consumer-side").

## Addendum (2026-06-23) — low-hanging fruit vs raw MMC, against the real SDK

Classified against the actual `MMCDeviceInterface` vtable in the macOS **26.4**
SDK (`IOKit.framework/.../scsi/SCSITaskLib.h`, maintainer-supplied). A feature
is **low-hanging** if a convenience wrapper carries it (non-exclusive, no
`ObtainExclusiveAccess`, no raw CDB authored by mos); it **requires raw MMC**
if no wrapper exists, so it must go through `GetSCSITaskDeviceInterface` →
`SCSITaskInterface` with exclusive access (a new entry on `mos_internal_raw_cdb`,
raising the one-of-N raw count and re-raising the §5.5 invariant).

**Full convenience inventory (the only wrappers that exist):** `Inquiry`
(standard data only — EVPD/PAGE_CODE structurally absent), `TestUnitReady`,
`GetPerformance`/`GetPerformanceV2` (DATA_TYPE + TYPE), `GetConfiguration`,
`ModeSense10`, `SetWriteParametersModePage` (MODE SELECT — write),
`GetTrayState` (sense-blind), `SetTrayState`, `ReadTableOfContents`,
`ReadDiscInformation`/`V2` (DATA_TYPE), `ReadTrackInformation`/`V2`,
`ReadDVDStructure`, `ReadDiscStructure` (MMC-5, MEDIA_TYPE → DVD **and** BD),
`ReadFormatCapacities`, `SetCDSpeed`, `SetStreaming`, and the escape hatch
`GetSCSITaskDeviceInterface`.

### Low-hanging — convenience wrapper exists (no raw CDB, no new lock)

| Item | Wrapper (SCSITaskLib.h) | Even cheaper? |
|------|------------------------|----------------|
| **F1** disc_id / barcode / OPC / app-code | `ReadDiscInformation` :846 | **Pure-parse** — mos *already issues* 0x51; decode more bytes of a reply already in hand. The lowest fruit on the list. |
| **F3** page 0x2A extra bits (C2, BURN-Free, Mt-Rainier, write caps) | `ModeSense10` :731 | **Pure-parse** — mos already reads page 0x2A; decode more bits. |
| **F7** decode select GET CONFIG features (0107h/0100h/0105h/…) | `GetConfiguration` :700 | **Pure-parse** — mos already walks RT=0. |
| **F11** extended CD-TEXT fields | `ReadTableOfContents` :819 (FORMAT 0101b) | **Pure-parse** — mos already issues this format. |
| **F9** CD-Extra / disc-mode derivation | `ReadTableOfContents` :819 | **Pure-compute** — from data already fetched. |
| **F2** full GET PERFORMANCE descriptor list + write speeds | `GetPerformanceV2` :949 (DATA_TYPE+TYPE) | New *call(s)* (Type 03h etc.), but convenience — no lock. |
| **F4** BD Spare Area Info / DDS | `ReadDiscStructure` :1035 (MEDIA_TYPE=BD) | New FORMAT codes, convenience — no lock. |
| **X1** MusicBrainz / CDDB disc IDs | (none needed) | **Pure-compute** from TOC mos already has — but crosses the "third-party ids consumer-side" line. |

### Requires raw MMC — no wrapper (→ `GetSCSITaskDeviceInterface`, exclusive access)

| Item | Opcode | Why raw |
|------|--------|---------|
| **F5** REPORT KEY (region code, RPC phase, resets-remaining) | 0xA4 | No wrapper. Also crosses the explicit "mos does not issue REPORT KEY" line. |
| **F6** READ SUB-CHANNEL (MCN / ISRC) | 0x42 | No wrapper. |
| **F8** MECHANISM STATUS (fault / changer / slot) | 0xBD | No wrapper. |
| **F10** READ BUFFER CAPACITY | 0x5C | No wrapper. |

**Confirms existing doctrine:** the `Inquiry` wrapper (:609) takes only
`SCSICmd_INQUIRY_StandardData` with `inqBufferSize` capped below its size — no
EVPD, no PAGE_CODE — so VPD pages and the >36-byte version descriptors are
unreachable via convenience (why mos's standard-INQUIRY path is already raw, and
why the retired VPD-0x80 serial was raw). `GetTrayState` (:766) wraps GESN but
is sense-blind, so the GESN tray probe and any `eject_requested` event remain
raw (as today). **Net:** every Tier-1 *pure-parse/compute* item (F1, F3, F7,
F9, F11) and the convenience-call items (F2, F4) are low-hanging; only F5, F6,
F8, F10 require a new raw verb.

## Source index (selected)

- Linux `include/uapi/linux/cdrom.h` (disc_information / track_information /
  GPCMD opcodes) — F1, F6(opcode), F8, F10.
- https://www.spinics.net/lists/linux-scsi/msg161016.html — READ DISC INFO
  field semantics (F1).
- libcdio `cd-info.c` + `include/cdio/device.h` — drive-capability bitmap (F3, F9).
- `dvd+rw-mediainfo.cpp`; `cdrecord(1)` `-prcap`/`-atip` — F2, F3.
- T10 MMC-2 (97-117r0 READ SUB-CHANNEL, 97-108r0 MECHANISM STATUS),
  05-206r0 (BD structures) — F4, F6, F8.
- musicbrainz.org Disc_ID_Calculation; metabrainz libdiscid; HydrogenAudio
  AccurateRip; liquisearch CDDB — X1.
- Wikipedia: Regional_Playback_Control (F5), Burst_cutting_area (F1/BCA),
  M-DISC (X5), Media_Identification_Code (ATIP/MID).
- smartctl(8); lsblk(8); journalctl(1); findmnt(8); clig.dev; jc;
  diskutil; Prometheus exposition format — U1–U11.
- EAC extraction technology; dvdisaster(1)/manual; Nero DiscSpeed — X2/X3/X4.
- AACS BD-Prerecorded spec; MakeMKV LibreDrive — X6/X8.
