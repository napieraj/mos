# Optical Drive Serial Retrieval: VPD 0x80 vs GET CONFIGURATION 0x0108

A falsification of the "use SCSI VPD page 0x80 to read an optical drive's unit
serial" hypothesis, and the case for MMC `GET CONFIGURATION` Feature `0108h`
(Logical Unit Serial Number) as the correct, in-spec carrier. Grounded in the
MMC-4 standard, the SCSI generic utilities (`sg3_utils`), and a cross-family
survey of real drive captures.

This is the decision basis for the AGENTS.md serial-source ADR
("serial source is feature 0108h; raw VPD 0x80 is RETIRED", 2026-06-21).

> [!IMPORTANT]
> **Bottom line.** For optical drives, VPD page 0x80 is the wrong abstraction —
> wrong command family *and* empirically unpopulated. The serial is exposed via
> MMC `GET CONFIGURATION` Feature `0108h`. The mechanism is present across every
> current OEM firmware lineage, but **population is firmware-dependent, not
> vendor-dependent**: the same OEM ships some firmware that fills it and some
> that leaves it blank. The only correct posture is to *probe* the feature list
> and *validate* the bytes, never to assume a fixed location or even presence.

---

## 1. The hypothesis under test

**H0:** *An optical drive surfaces its unit serial via the SCSI VPD "Unit Serial
Number" page (0x80), the way disks and SSDs do.*

If H0 were true we'd expect optical drives to advertise 0x80 in their Supported
VPD Pages (0x00) and return a unique serial from an `INQUIRY` with `EVPD=1`,
`page=0x80`. They don't. The serial demonstrably exists and is reachable — just
not there. That gap is the falsification.

## 2. Two command worlds: SPC vs MMC

This is the root cause and the reason the VPD instinct misfires.

- **VPD 0x80 / 0x83 live in SPC** (SCSI Primary Commands). They were designed for
  block storage — tracking a specific spindle inside an array. Both are
  *optional*. `sg_ident`/`sg_inq` treat the INQUIRY vendor/product/revision
  strings plus the Device Identification VPD page (0x83) as the SCSI-layer
  identity set.
- **Optical drives are MMC peripherals** (peripheral device type `05h`). MMC
  (SCSI Multimedia Commands) defines its **own** identity surface, including a
  dedicated serial feature.

The tell is structural: MMC would not define a Logical Unit Serial Number feature
if the SPC VPD path were the intended carrier for this device class. The serial
mechanism for optical lives in MMC, not SPC.

## 3. Falsifying VPD 0x80 for optical

### 3.1 Spec argument

VPD 0x80 is optional in SPC and aimed at SBC-class devices. Nothing in MMC
requires an optical drive to implement it, and the MMC identity model routes
serial reporting through `GET CONFIGURATION` instead. So at spec level there is
no expectation that 0x80 carries an optical serial.

### 3.2 Empirical argument

Across optical firmware, an `EVPD` `INQUIRY` for 0x80 typically yields one of:

1. `0x80` absent from Supported VPD Pages (0x00); the request returns
   `CHECK CONDITION` / `ILLEGAL REQUEST` (sense `05`, ASC/ASCQ `24h/00h`,
   invalid field in CDB).
2. The page returns but space- or zero-filled.
3. A constant shared across every unit of a model — a firmware literal, not a
   serial.

The ATA layer is no better: `IDENTIFY PACKET DEVICE` words 10–19 (the serial
field that is reliable on HDD/SSD) are almost always blank for ATAPI optical.

**Conclusion:** the serial is not in VPD 0x80, yet it is exposed elsewhere (see
§4–§5). H0 is falsified. VPD 0x80 is not the optical serial carrier.

## 4. The in-spec carrier: GET CONFIGURATION Feature 0108h

MMC-4 defines Feature `0108h`, "Logical Unit Serial Number" (Microsoft's MMC
driver headers call it the "Device Serial Number" feature). Per the standard it
furnishes the initiator with a serial that uniquely identifies the *device*
(distinct from media). It is retrieved with `GET CONFIGURATION` (opcode `46h`)
and decoded by `sg_get_config`.

### Descriptor layout

A Feature Descriptor is a 4-byte header followed by the serial as ASCII graphic
characters (`0x20`–`0x7E`):

| Offset | Field                | Notes                                         |
|-------:|----------------------|-----------------------------------------------|
| 0–1    | Feature Code         | `0108h`                                       |
| 2      | Flags                | bits: `Version<<2` \| `Persistent<<1` \| `Current` |
| 3      | Additional Length    | `N` = length of the serial in bytes           |
| 4..4+N | Serial Number        | ASCII `0x20`–`0x7E`                            |

### Decoding the LG WH16NS60 capture

From the `mos probe --capture` output, grepping the serial substring
`47 47 33 55 43` = `"GG3UC"`:

- **`GET CONFIGURATION` (opcode `46h`)** — descriptor begins
  `01 08 03 0C ...`:
  - `0108h` → Logical Unit Serial Number feature
  - `03h` → Persistent=1, Current=1
  - `0Ch` → 12-byte serial follows
  - the 12 ASCII bytes contain `GG3UC`
  This is the in-spec, portable path. ✅

- **`INQUIRY` standard (opcode `12h`, `EVPD=0`)** — after the standard
  vendor/product/revision fields (`...WH16NS60` + `1.00`), the **vendor-specific
  tail** also contains the same `GG3UC` serial. This is HL-DT-ST / LG convention
  — the bytes past offset 36 are manufacturer-defined and carry no portability
  guarantee. ⚠️ Useful as a same-vendor shortcut, never as a cross-vendor rule.

So on this LG the serial exists in the drive and is reachable **in-spec without
touching VPD at all** — exactly the claim.

## 5. Cross-family findings

### 5.1 "Brands" collapse to OEM firmware platforms

Surveying other "families" is mostly an illusion: the consumer BD market is a
handful of OEM firmware platforms wearing different badges. Most external "ASUS"
drives are rebadged Pioneer (or, more recently, MediaTek) mechanisms. Checking
ASUS-vs-Pioneer is frequently the *same silicon and firmware*.

| Badge / product        | Actual platform          | Lineage          |
|------------------------|--------------------------|------------------|
| ASUS BW-16D1H-U        | Pioneer BDR-209 (RS8601) | Pioneer          |
| ASUS BW-16D1X-U        | Pioneer (RS8F00)         | Pioneer          |
| ASUS SBC-06D2X-U       | Pioneer BDR-US03 (RS8591)| Pioneer          |
| ASUS SBW-06D5H-U       | Pioneer BDR-US03 (RS8511)| Pioneer          |
| ASUS BW-16D1H-U **PRO**| MediaTek (MT1959)        | MediaTek         |
| LG WH16NS60 / BH16NS55 | Hitachi-LG               | Hitachi-LG       |

So the real axis is **OEM firmware platform** (Hitachi-LG; Pioneer `RSxxxx`;
MediaTek `MTxxxx`; plus the older TSST/Lite-On/Optiarc lineages), not the brand
on the bezel.

### 5.2 Serial presence is firmware-dependent

Real captures show the serial is *exposed and queryable* across all these
lineages — but whether it's populated tracks **firmware**, not vendor. Same OEM,
different result:

| Drive (as reported)         | Platform / firmware        | Serial field          |
|-----------------------------|----------------------------|-----------------------|
| ASUS BW-16D1H-U             | Pioneer BDR-209, E114      | `RFDL020701WL` ✅     |
| ASUS BW-16D1X-U             | Pioneer, E111/ID76         | `RDDL007335WL` ✅     |
| PIONEER BDR-212U            | native Pioneer, 1.01       | `AKDL002525WL` ✅     |
| **PIONEER BDR-213M**        | native Pioneer, 1.02       | **blank** ❌          |
| ASUS SBC-06D2X-U            | Pioneer BDR-US03, RS8591   | blank in some units ❌|
| ASUS BW-16D1H-U PRO         | MediaTek MT1959            | populated ✅          |
| LG WH16NS60                 | Hitachi-LG, 1.00           | `...GG3UC...` ✅      |

The Pioneer rows are the key result: `BDR-209` and `BDR-212U` fill the serial,
the newer `BDR-213M` firmware leaves it blank. This is the spec's optionality
showing up as **per-firmware** behaviour, and it kills any heuristic based on
"this vendor always provides a serial."

### 5.3 Caveat on the evidence

The cross-family data above is read from third-party tool drive logs that surface
a single "Serial number" field. That proves two things rigorously — the serial is
*exposed/queryable* across families, and its *presence is firmware-dependent* —
but it does **not** prove each family populates `0108h` specifically rather than
the vendor INQUIRY tail. The clean confirmation is a direct
`sg_get_config` / `mos probe --capture` dump per platform. Treat §5.2 as strong
circumstantial support; close the gap with first-party captures on at least one
Pioneer and one MediaTek unit.

## 6. Don't confuse 0108h with 0109h

Adjacent in the feature table, opposite in meaning:

- **`0108h` Logical Unit Serial Number** — the **drive's** serial. Persistent
  across media. This is what you want for drive identity / fingerprinting.
- **`0109h` Media Serial Number** — a **per-disc** serial, read via the
  `READ MEDIA SERIAL NUMBER` command (`sg_rmsn`). Changes with the loaded disc;
  frequently unimplemented. Not drive identity.

## 7. Implementation in `mos`

mos decodes `0108h` from the RT=0 GET CONFIGURATION walk it *already* issues for
protection / profiles / firmware_date (`mos_internal_serial_from_config`,
`src/mos_config.c`) — so the serial costs no extra command. A targeted RT=10b
probe (request only `0108h`) is also valid for a tool that wants only the serial;
the targeted CDB is:

```c
/* GET CONFIGURATION, RT=10b, Starting Feature = 0x0108 */
uint8_t cdb[10] = {
    0x46,        /* opcode: GET CONFIGURATION            */
    0x02,        /* RT = 10b: only the named feature     */
    0x01, 0x08,  /* Starting Feature Number = 0x0108     */
    0x00, 0x00, 0x00,
    0x00, 0xFF,  /* Allocation Length (big-endian)       */
    0x00         /* Control                              */
};
```

### Parse + validate (mos posture)

1. Find the `0108h` descriptor in the feature walk; read byte 3 (Additional
   Length `N`) and extract the next `N` bytes.
2. Right-trim trailing spaces / NULs; refuse an interior NUL or an over-length
   serial (complete-or-unavailable — a partial identity key is worse than none).
3. Empty after trimming ⇒ null (the firmware-blank case, e.g. Pioneer BDR-213M).

**Uniqueness is the consumer's concern, not mos's.** `Persistent=1` in the flags
byte is a claim, not a guarantee; the only real uniqueness test is "differs
across two same-model units." mos *reports* the serial when present; a consumer
that needs a dependable fingerprint keys on the product+revision+read-offset
triple (as AccurateRip / whipper do) because no optical serial is dependable
enough across the install base to key on alone.

**No vendor INQUIRY-tail fallback.** The same serial appears in the standard
INQUIRY vendor-specific region (offset ≈36+) on LG, but that offset is
manufacturer-defined — decoding it would be the per-device special-casing the
hardware-role ADR forbids. mos leaves it as corroboration only.

**No VPD 0x80.** Retired entirely (§3) — wrong command family, empirically empty.

## 8. Conclusion

VPD 0x80 is falsified as the optical serial carrier on two independent grounds —
architectural (optical is MMC; the serial feature lives in MMC, not SPC/VPD) and
empirical (firmware leaves 0x80 empty while the serial is reachable elsewhere).
The in-spec carrier is `GET CONFIGURATION` Feature `0108h`, present across the LG,
Pioneer, and MediaTek lineages. Its weakness is the same optionality as VPD —
expressed here as per-firmware population — so the engineering rule is **probe,
extract, validate**, fail closed on absent/blank, and never assume a fixed
location or presence.

---

## References

- **T10 / INCITS, *SCSI Multimedia Commands – 4 (MMC-4)*** — Feature `0108h`
  Logical Unit Serial Number; Feature `0109h` Media Serial Number; feature
  descriptor format.
- **Microsoft Windows Driver docs**, `FEATURE_DATA_LOGICAL_UNIT_SERIAL_NUMBER`
  (`ntddmmc.h`) — descriptor = `FEATURE_HEADER` + ASCII serial (`0x20`–`0x7E`);
  "furnishes the initiator with a serial number that uniquely identifies the
  device."
- **`sg3_utils`** — `sg_get_config` (`case 0x108: "Drive serial number"`),
  `sg_rmsn` (`READ MEDIA SERIAL NUMBER`, feature `0x109`), `sg_ident`/`sg_inq`
  (SCSI-layer identity: INQUIRY strings + Device Identification VPD 0x83).
- **`libcdio`** — `CDIO_MMC_FEATURE_LU_SN = 0x108` ("The Logical Unit has a
  unique identifier").
- **smartmontools** — `scsicmds.cpp`: "CD or DVD-ROM devices often do not
  support VPD pages 0x80, 0x83 or 0x85."
- **Cross-family drive captures** — public MakeMKV drive-information logs (LG,
  Pioneer, ASUS-rebadged-Pioneer, MediaTek), showing firmware-dependent serial
  population (third-party; see §5.3 caveat).
- **`mos probe --capture`** — first-party LG WH16NS60 capture decoded in §4.
