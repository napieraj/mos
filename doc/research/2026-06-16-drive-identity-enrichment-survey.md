# Drive-identity enrichment for `mos drive` — reachability survey

**Date:** 2026-06-16. **Status:** research / decision note. Companion to
`2026-06-16-serial-vpd-0x80-feasibility.md` (the serial read shipped; this
asks what *else* `mos drive` could surface now that the raw INQUIRY handle is
already open). Per the README append rule this is a new dated file. **No code
change here** — Process rule 2 / hardware-role ADR: this records the design
so a future session can pick specific items the maintainer green-lights.

## The question

Now that `mos drive` already (a) reads the DiscRecording directory
zero-command, (b) walks GET CONFIGURATION (for AACS), (c) reads MODE SENSE
page 0x2A (for `mechanical`), and (d) holds a raw INQUIRY handle (for the
VPD-0x80 serial), what additional **drive-static** identity/capability is
worth folding into `mos.drive.v1` — cheaply, on paths already open, within
scope doctrine? Maintainer prompt specifically: *"firmware name itself, not
just (rev = version)"*, plus anything else that enriches the verb.

Cross-checked against SPC-4/MMC-4, sg3_utils (`sg_inq`/`sg_vpd`/
`sg_get_config`), libcdio (`cd-drive`), cdrkit (`cdrecord -prcap`), the
Microsoft DDK `ntddmmc.h` feature structs, Apple QA1179, and the DR field
map (`doc/dr-field-mapping.md`).

## The firmware sub-question, resolved

Two true things that together answer it:

1. **A fuller firmware *version string* than the 4-char `revision` is not
   reachable on macOS.** The 4-char SCSI PRODUCT REVISION LEVEL is literally
   a SATL *truncation* of the ATA IDENTIFY firmware revision ("select 4 of
   the 8 ASCII characters", T10 SAT 05-137r1), so the genuinely fuller string
   is the ATA 8-char field — but Apple provides **no ATA pass-through API**
   for these devices (QA1179: SCSITaskUserClient is MMC-only), and the
   ATA PASS-THROUGH opcode `0xA1` collides with MMC **BLANK** on a type-5
   device. `hdparm -I` / `smartctl -i` show 8 chars only via the ATA/SAT path
   macOS doesn't expose for optical drives. The only *reachable* fuller bytes
   are the vendor-specific INQUIRY tail (36+), where LG/Pioneer stash a
   firmware date — but unstandardized per-vendor, exactly the device-quirk
   special-casing the hardware-role ADR bars from `src/`. **Verdict: keep
   `revision`; do not chase a longer version string.**

2. **A firmware *creation date* IS reachable and MMC-standard** — GET
   CONFIGURATION **feature 010Ch** (Firmware Information). Drive-static, free
   on the GET CONFIGURATION walk mos already issues, decoded against MMC like
   every other feature. Optional, so best-effort/null-by-default — never an
   error when missing. **This is the firmware enrichment the ATA path couldn't
   give:** not a name, a build timestamp, the more useful "which firmware is
   this" signal.

   **Correction / resolution (2026-06-16).** Initial sources (libcdio
   `CDIO_MMC_FEATURE_FIRMWARE_DATE`, MS DDK `FEATURE_DATA_FIRMWARE_DATE`) put
   this at "0x1FF" with a Year[4] payload whose MS-DDK description
   self-contradicted, so it was first DEFERRED pending a primary spec. The
   maintainer then supplied MMC-6 r02g (§5.3.43, Table 197), which corrected
   both: the feature is **010Ch** (0x1FF is *Reserved* in MMC-6) and the year
   is **Century[2] + Year[2]** (bytes 4-7), not Year[4]. Built to that
   verified layout — payload Century/Year/Month/Day/Hour/Minute/Second decimal
   ASCII (GMT) → `firmware_date` as an RFC 3339 UTC string (same format as
   `mos.event.v1`'s `ts`). **Shipped, not deferred.**

## Reachability tiers (cheapest first)

mos already pays for four data sources. The value of a candidate is mostly
"which already-open source carries it":

- **Tier 0 — zero-command DR directory** (`DRDeviceCopyInfo`): no command at
  all, works with media mounted. The cheapest and most doctrine-friendly.
- **Tier 1 — existing GET CONFIGURATION walk** (`mos_query_drive_caps`,
  convenience `GetConfiguration`, non-exclusive): mos already issues it and
  extracts only AACS (0x010D); other feature payloads are free to decode.
- **Tier 1 — existing raw INQUIRY handle** (`mos_serial.c`, opcode 0x12):
  a second INQUIRY (EVPD=0, larger allocation length) is the *same opcode,
  same exclusive-access site, same BUSY-on-mounted backoff* — no new raw
  verb, the one-of-four CDB count is unchanged.
- **Tier 1 — existing MODE SENSE page 0x2A** (`mos_query_mode_caps`): mos
  already reads it for `mechanical`; the same reply carries more bits.

## Ranked candidates

| Candidate | Source | Tier | Optical-populated | Verdict |
|---|---|---|---|---|
| Physical interconnect (bus type) | DR `kDRDevicePhysicalInterconnect{,Location}Key` | 0 | always | **skip — always USB-bridge today (no signal); bridge identity** |
| Write-capability (boolean) | implied by openability | — | always | skip — tautology (mos only opens burners) |
| **Supported-profile list** | GET CONFIG feature 0x0000 | 1 (existing walk) | always | **ship — richest cap fact; carries per-format write** |
| **Firmware creation date** | GET CONFIG feature 010Ch | 1 (existing walk) | sometimes | **shipped — RFC 3339, best-effort** |
| **INQUIRY version + version descriptors** | raw INQUIRY EVPD=0 bytes 2, 58-73 | 1 (existing handle) | partial | **ship — standards the drive claims** |
| Page-0x2A extra cap bits | MODE SENSE 0x2A | 1 (existing read) | always | maybe — mostly legacy CD bits |
| GET CONFIG serial (0x0108) | feature 0x0108 | 1 | sometimes | skip — redundant with VPD 0x80 |
| VPD 0x83 (device ID / WWN) | raw INQUIRY EVPD=1 0x83 | 1 | **rarely** | skip — ~always null on optical |
| VPD 0x00 (supported pages) | raw INQUIRY EVPD=1 0x00 | 1 | always | skip — redundant with echo gate |
| Vendor INQUIRY tail (36+) | raw INQUIRY EVPD=0 | 1 | varies | skip — unstandardized quirk |
| ATA firmware (8-char) / serial | ATA PASS-THROUGH | — | n/a | **impossible on macOS** (QA1179, 0xA1=BLANK) |
| Region code / RPC | REPORT KEY (raw) | new verb | always | out of scope (new raw verb + not identity) |

### The strong cheap wins

(Tier 0 is empty: its one candidate — physical interconnect — is rejected
below as a near-constant "USB" with no signal. Every cheap win is Tier 1.)

1. **Supported-profile list (feature 0x0000).** The single richest
   capability fact and the *modern, BD-aware* answer to "what disc types can
   this drive read/write" — superior to libcdio's page-2A read/write flags,
   whose vocabulary stops at DVD (no Blu-ray). Always present. mos already
   walks the feature list; it currently extracts only AACS, so the profile
   list is a payload decode on a reply already in hand. (Note: the *current*
   profile in the GET CONFIG header is media-dependent and deliberately stays
   out of `mos drive` — the supported-profile *list* is the drive-static
   part.) The per-format **write** capability is the presence of the writable
   profiles in this same list (DVD+R, BD-R, …) — so it needs no separate DR
   `kDRDeviceCanWrite*Key` matrix. And a *boolean* "can this drive write" is
   not worth a field at all: see the tautology below.

   **The write-capable boolean is a platform tautology — do not surface it.**
   Any drive mos can open is a burner by construction, gated at *both* layers:
   the IOKit attach rule blocks `SCSITaskUserClient` on read-only drives
   (`ARCHITECTURE.md` §9.1, on `kIOPropertySCSITaskAuthoringDevice` — "if the
   drive is a read-only device… access is blocked… if the drive is capable of
   writing any media… all access is allowed"), and DR enumerates only
   recorders (the framework is named Disc
   **Recording**; `DRCopyDeviceArray`'s burner filter ≈ the
   SCSITaskUserClient-openable set — `2026-06-10-media-info-design.md` §"the
   non-burner case is already excluded at every layer"). So "can write: true"
   is always true for any drive `mos drive` reaches — a field that can never
   say anything. Only the *granular which-formats* question carries signal,
   and that is the profile list above.

2. **INQUIRY version byte + version descriptors.** A second raw INQUIRY
   (EVPD=0, allocation length ≥ 74) on the existing handle yields byte 2
   (SPC compliance level — "SPC-3"/"SPC-4") and bytes 58-73 (up to eight
   codes naming the standards the drive claims: SPC-4, MMC-6, SBC, SAM-4…).
   A durable, human-meaningful conformance fingerprint nothing in `mos drive`
   carries. The convenience `Inquiry` returns only the 36-byte header, so the
   descriptors need the raw read — the *same* structural-unreachability
   showing already accepted for VPD 0x80, on the same handle. The code→name
   table is a small static lookup (like mos's other string tables); unknown
   codes emit as hex and never fail (the unknown-enum rule).

3. **Firmware creation date (feature 010Ch).** As above — best-effort,
   the reachable firmware enrichment, free on the existing walk. Shipped to
   the MMC-6 r02g Table 197 layout (RFC 3339 UTC, matching the event `ts`).

### The rejections (and why, briefly)

- **Physical interconnect / bus type (DR `kDRDevicePhysicalInterconnectKey`).**
  Looks like the cheapest win (zero-command DR field, "what bus is this
  drive"), but it carries no signal on mos's actual hardware: in 2026 every
  optical drive is a bare mechanism in a **USB-bridge** shell (the Apple USB
  SuperDrive; bare drives in USB-SATA enclosures — `README.md`,
  `2026-04-26-silicon-family-map.md`; the serial doc's "pure optical readers
  are unobtainable, writers themselves a historical artifact"). So
  `interconnect` is a near-constant `"USB"` — the same no-information shape as
  the can-write tautology — and what it reports is the **bridge/enclosure's**
  bus, not the drive's, exactly the "unreliable through USB bridges, out of
  scope" identity the drutil-contract note (`2026-06-10-drutil-contract.md`)
  already set aside. Skip. (The GET CONFIG Core 0x0001 bus field is the same
  near-constant value from a command path — skip for the same reason.)
- **VPD 0x83 (Device Identification / WWN).** Looks like a better durable ID
  than the vendor-formatted serial, but optical/ATAPI drives commonly don't
  implement 0x80/0x83/0x85 (return 5/24/00) — on mos's target hardware it is
  near-always null. If ever wanted, treat it like the serial: best-effort,
  null-by-default, absence ≠ error.
- **VPD 0x00 (supported pages).** Issuing it to pre-check 0x80/0x83 is an
  extra exclusive-access round-trip to learn what the page-code-echo gate
  (`mos_serial.c`) already discovers for free.
- **GET CONFIG serial (0x0108).** Same datum as VPD 0x80; redundant. Useful
  only as a cross-check/fallback if a drive implements one but not the other.
- **Vendor INQUIRY tail (36+).** Where a firmware date / second serial
  sometimes lives, but unstandardized per-vendor — device-quirk parsing the
  hardware-role ADR forbids.
- **Page-0x2A extra bits** (test-write, BURN-Proof, C2, multi-session,
  Mode-2, ISRC/MCN read, R-W subchannel, prevent-jumper). All drive-static
  and free on the 0x2A read mos already does, but mostly **legacy CD-era**
  capability flags better expressed by the GET CONFIG profile/feature list.
  Surface only if a consumer wants an exhaustive capability matrix. Confirmed
  non-starters in 0x2A: **no region/RPC byte** and **no defect-management
  bit** (cdrecord/cdrkit source).
- **Region code / RPC state.** Needs REPORT KEY — a *new* raw verb requiring
  its own layer-1 showing, and region is arguably media-policy, not drive
  identity. Out of scope here.
- **ATA 8-char firmware / ATA serial.** Unreachable on macOS (no ATA
  pass-through; opcode collision). Dead end, not a scope choice.

## Scope-doctrine compliance

- **Command surface (layer 1):** every shippable candidate rides a path mos
  already issues — DR (no command), the existing GET CONFIGURATION
  convenience walk, the existing raw INQUIRY handle, or the existing MODE
  SENSE 0x2A. **No new raw verb; the one-of-four raw-CDB count is unchanged.**
  (A second EVPD=0 INQUIRY is the same 0x12 opcode/site as the serial read.)
- **No SPC ambition (layer 2):** version descriptors are standard
  INQUIRY-identity, read-only — not the mode/log-page introspection the
  clause forecloses. Profile/feature payloads are decoded against MMC, the
  optical-kext oracle, exactly as AACS already is.
- **Privilege (layer 3):** unchanged — DR reads and the convenience
  GetConfiguration/ModeSense10 take no entitlement; the raw INQUIRY is the
  same SCSITaskUserClient console grant already used. No root, no TCC.
- **Reporter, not controller:** all read-only facts.

## Recommendation — cheap wins, tiered by cost

**Tier 0 — zero command (DR directory):** *empty.* Its only candidate,
physical interconnect, is rejected — in 2026 every optical drive is a
USB-bridge shell, so the field is a near-constant `"USB"` (no signal) and
reports the bridge's bus, not the drive's (see rejections).

**Tier 1 — rides a command mos already issues (no new raw verb; the
one-of-four raw-CDB count is unchanged):**

1. **Supported-profile list** — GET CONFIG feature 0x0000, on the walk mos
   already does for AACS. The richest, BD-aware capability fact; it also
   carries the per-format **write** capability (writable-profile presence) —
   so no separate write matrix, and no boolean "can write" (that is a
   platform tautology, mos only opens burners). **Highest value; best first.**
2. **INQUIRY version byte + version descriptors** — a second EVPD=0 read on
   the raw INQUIRY handle already opened for the serial. The "standards this
   drive claims" (SPC-4/MMC-6…) fingerprint. Lowest-risk of the raw paths
   (the handle and showing already exist).
3. **Firmware creation date** — GET CONFIG feature 010Ch, same walk as #1.
   Best-effort, null when absent; the firmware answer beyond `revision`.
4. *(lower value)* **Page-0x2A extra capability bits** — the MODE SENSE 0x2A
   read mos already does for `mechanical` carries more bits (test-write,
   BURN-Proof, C2, multi-session…), but mostly legacy CD-era; surface only if
   an exhaustive capability matrix is wanted.

Suggested build order: 1 → 2 → 3 (profile list highest-value; descriptors and
firmware-date additive best-effort; #1 and #3 are one GET-CONFIG payload-
decode change, #2 is one INQUIRY-EVPD=0 change). All are additive,
drive-static, and within the closed-field-set
schema policy (each a new key/block → a `mos.drive.v1` field-set addition,
pre-tag mutable-in-place per the JSON-schema ADR: schema + example + negative
fixtures + emitter + docs in one commit). Page-0x2A extras, VPD 0x83, and the
GET-CONFIG serial are documented above as deferred/declined with reasons so a
future session doesn't re-derive them. The firmware *version string* and the
ATA path are closed as impossible, not deferred.

## What hardware can falsify, never establish

Per the hardware-role ADR, a run can refute these but never steered them:
a drive that omits feature 010Ch (firmware date) or 0x0000 detail, a USB-SATA
bridge that synthesizes bogus version descriptors, an optical drive that
*does* populate VPD 0x83 (re-opening that field's value), or — the one that
would revive the interconnect field — a genuine non-USB optical drive
reappearing on a shipping Mac. Each lands as a committed fixture + dated note with a generic
validity gate (length-keyed, page-code-echoed), never a per-device special
case.

## Sources

- Apple QA1179 — no ATA pass-through; SCSITaskUserClient is MMC-only:
  https://developer.apple.com/library/archive/qa/qa1179/_index.html
- T10 SAT 05-137r1 — SCSI rev is 4-of-8 of the ATA firmware revision:
  https://www.t10.org/ftp/t10/document.05/05-137r1.pdf
- MS DDK `ntddmmc.h` feature structs (0x0001 Core, 0x0003 Removable,
  0x0106 CSS, 0x0107 RTS, 0x0108 Serial; firmware date is 010Ch per MMC-6 r02g, not 0x1FF):
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddmmc/ne-ntddmmc-_feature_number
- libcdio feature constants + `cd-drive` capability model (page 0x2A):
  https://raw.githubusercontent.com/Distrotech/libcdio/master/include/cdio/mmc.h
- cdrkit `wodim/scsi_cdr.c` `print_capabilities()` + `struct cd_mode_page_2A`
  (full `-prcap` page-0x2A decode):
  https://raw.githubusercontent.com/Distrotech/cdrkit/7b4bb72389ea5ea3ecc94545036dcff4728ec38a/wodim/scsi_cdr.c
- sg3_utils `sg_inq.c` (standard INQUIRY fields, version descriptors),
  `sg_get_config` (GET CONFIGURATION), `sg_vpd` (VPD pages).
- VPD support on optical drives (often absent): ESX VPD note —
  https://elatov.github.io/2012/08/determine-disk-vpd-information-from-esx-classic/
- MakeMKV `osxmmc` — the reverse-engineering writeup of macOS's optical-MMC
  access and the upstream source of the read-only-drive-blocking policy
  behind the write-cap tautology: https://www.makemkv.com/osxmmc/. The
  verbatim rule and its developer's quip — *"our only explanation is that
  weed is really easily available to designers in Cupertino, CA"* — are
  already quoted in-repo at `ARCHITECTURE.md` §9.1, with the gating property
  `kIOPropertySCSITaskAuthoringDevice` (`2026-06-10-media-info-design.md`
  paraphrases it "Cupertino's herbal supply").
- In-repo: `doc/dr-field-mapping.md` (DR zero-command keys, interconnect),
  `AGENTS.md` scope doctrine, `ARCHITECTURE.md` §9.1 (writer-vs-reader attach
  rule), `2026-06-10-media-info-design.md` §"non-burner case excluded",
  `2026-06-16-serial-vpd-0x80-feasibility.md`.
