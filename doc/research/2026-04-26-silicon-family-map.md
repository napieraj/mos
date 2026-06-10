# Optical drive silicon families for mos v1.0 fixture coverage

Date: 2026-04-26

Procurement and architectural analysis for v1.0 fixture coverage
across vendors. The fixture-acquisition plan narrows to roughly
twelve drives across eight architectural rows, achievable from
Hamburg sources for €700–900 total.

## Falsified hypotheses worth surfacing first

Two foundational hypotheses from earlier planning failed under
research:

**Apple SuperDrive A1379 is NOT Panasonic UJ-8A8 OEM-relabeled.**
Late-production A1379 units universally report `HL-DT-ST DVDRW
GX50N` in macOS System Information — they are Hitachi-LG GX50N
mechanisms inside the Apple shell with a soldered USB bridge
running Apple firmware. Earlier A1379 production may have been
multi-sourced, but the post-2015 dominant configuration is HLDS.

For the SuperDrive A1379 fixture row, `mos` belongs to the HLDS/
MediaTek table, not the Panasonic table. The UJ-8A8 (Apple part
678-0611) is real and important, but it lives inside the *internal*
MacBook Pro Unibody SuperDrive (2009–2012), not the external A1379.
These are two distinct fixture rows that earlier framing conflated.

**The "Toshiba contributed silicon to TSST" marketing narrative is
fiction.** Every TSST drive teardown from SH-W162C onward (cdrinfo
PCB photos confirm) shows MediaTek silicon — MT1818-class for early
WriteMaster, MT1869L for SH-S203/223/224, MT1887 for SE-208 (USB
VID `0E8D` = MediaTek, dmesg `Product: MT1887`), MT19xx for SE-506
BD. TSST halted production in April 2016 and filed Chapter 15
bankruptcy mid-2016; the silicon platform was never sold or
licensed externally.

The interesting fixture quirk on SE-208/SE-506 is the **two-layer
vendor identity mismatch**: USB descriptor reports MediaTek (VID
`0E8D`), but the SCSI INQUIRY reports `TSSTcorp CDDVDW SE-208GB`.
`mos`'s USB-layer fixtures and SCSI-layer fixtures will see different
vendor strings on the same drive — important quirk to model.

## Underdocumented trap: post-2017 "Optiarc" is Vinpower-Lite-On

The original Sony Optiarc DVD generations (AD-7200S/A through
AD-7280S) carry genuine NEC/Renesas silicon and a tightly conformant
MMC dispatcher distinct from MediaTek. The AD-7220S is an early
trap — actually a Lite-On iHAP322/DH-22A8P rebadge in Optiarc
livery (community-confirmed crossflash to iHAS322 by user "Eddie24"
on club.myce).

The much bigger trap: **Vinpower acquired the Optiarc trademark
around 2017** and currently sells the AD-5290S series at
optiarcinc.com and via Amazon DE — these are **Lite-On iHAS124-class
drives with custom Vinpower firmware** for 8.7GB DVD+R DL XGD3
overburn, not Sony silicon.

club.myce post: *"58€ is too much money for a modified LiteOn iHAS"*

`mos` must detect this: vendor `Optiarc` + model `AD-5290S*` +
post-2017 manufacture date = treat as Lite-On family. Original
Sony Optiarc never used the 5290 SKU; the original range stopped
at 5280S-CB.

## Architectural picture beneath the marketing

Optical drive controller silicon in 2026 is dominated by three
families:

1. **MediaTek MT18xx (DVD)** and **MT19xx (Blu-ray)** — most of the
   market
2. **Renesas R8J32xxx (NEC-lineage)** — used by Pioneer and a thin
   slice of Lite-On
3. **Legacy Sanyo LC8974xx** — only Plextor's audiophile heritage

Everything else has been absorbed (Toshiba and Samsung's TSST
silicon was MediaTek the whole time despite marketing claims),
discontinued (Panasonic MN103S, Yamaha YDS, Philips Nexperia,
Ricoh JustLink), or never publicly distinguished from MediaTek
(Lite-On's "in-house" silicon was always MediaTek-sourced).

The Linux kernel `drivers/scsi/sr_vendor.c` carries only **five**
vendor-quirk paths — `VENDOR_SCSI3`, `VENDOR_NEC`, `VENDOR_TOSHIBA`,
`VENDOR_WRITER`, and `VENDOR_CYGNAL_85ED` (the latter for a literal
Beurer glucose-meter "CD-on-a-chip"). The file's own header comment
declares: *"PIONEER, HITACHI, PLEXTOR, MATSHITA, TEAC, PHILIPS:
known to work with SONY (SCSI3 now) code."*

libcdio is deliberately vendor-neutral — its `mmc_get_hwinfo()`
issues a stock 0x12 INQUIRY and parses bytes 8–35 with no per-vendor
dispatch.

libburn's maintainer Thomas Schmitt declared in Debian bug #789260:
*"I never saw evidence that a particular DVD burner model is
incompatible. […] nowadays they all obey the command set and rules
of SCSI."*

The actionable per-vendor knowledge lives in **four places**:
1. cdrtools's pre-MMC dispatch table (irrelevant for any post-2002
   Mac fixture)
2. dvd+rw-tools's DVD+R DL bitsetting per-vendor mode-page knowledge
3. MakeMKV's closed-source `sdftool` and `libdriveio.so` with the
   private `sdf.bin` firmware-extension blobs (most operationally
   important — gates whether a drive can do "LibreDrive" raw reads)
4. **Firmware itself**, where the same silicon (e.g. Pioneer
   R8J32740FP42) behaves entirely differently between firmware
   revisions across a single December 2022 cutoff

The fixture-coverage implication: `mos` must distinguish drives by
silicon family AND by firmware policy generation, treating the
firmware date as a first-class fingerprint axis alongside the
INQUIRY string.

## Pioneer: Renesas-NEC silicon, locked by firmware policy

Pioneer's lineage:

- **R8J32720FPV** — BDR-205/S06 (2010-2011)
- **R8J32730FP44** — BDR-2207 (2012)
- **R8J32840** — BDR-209/211/S09/S11 (2013-2020), platforms RS8600/
  RS8800
- **R8J32740FP42** — BDR-212/213/S12/S13 (2019-2024), platforms
  RS8F00/RS9200
- Separate slim ASICs for BDR-XS06/XS07 (RS8E21) and BDR-XD07/XD08
  (RS9331)

Every internal Pioneer Blu-ray drive since 2010 runs Renesas
silicon — the "Pioneer never used MediaTek" claim from
forums.whirlpool.net.au is strong for BDR/BDC drives. The disputed
DVR-218 DVD-only outlier may have switched to MediaTek or Lite-On
for its final generation, but DVR-218/220 has low fixture value
regardless.

### Pioneer MMC dispatcher quirks

**PureRead** (audiophile feature flagship of the BDR-S series) is
exposed via a Pioneer proprietary MODE SENSE/SELECT page — not a
standard MMC mechanism. PureRead's philosophy is opposite to
MediaTek's: it silently retries on read errors and interpolates if
retries fail, deliberately suppressing C2 error reporting. dBpoweramp
forum:

> "Pioneer's toolset for mangling content only runs on Windows…
> [PureRead] mangles the data under even minor error conditions."

C2 error pointer support is reported as **No** on most Pioneer BD-RW
drives, breaking redumper-style C2-based extraction. INQUIRY
responses are abnormally padded with internal trim characters that
`mos` must normalize. The slot-load mechanisms (XS06, XS07) report
MECHANISM STATUS differently from tray drives — slot-load idle
returns `MECH_STATE_IDLE` even mid-load.

### The December 2022 firmware cutoff (operationally critical)

- Pre-December 2022 firmware exposes bus-encryption flag `0x1B`
  (LibreDrive-eligible)
- Post-December 2022 firmware flips this to `0x13` and enforces
  AACS2 host-key handshake before READ DISC STRUCTURE will return
  MKB data — yielding `ILLEGAL_REQUEST 0x05/24/00` on queries that
  succeed on LG MT1959

The cdrinfo.pl BDRFlash tool (suspended Feb 2024 after Pioneer
engagement, reopened May 2024) is the canonical primary source:

> "If the date of the firmware on your drive is December 2022 or
> later, your drive cannot be crossflashed or downgraded at present."

### Pioneer fixture recommendations

**Buy:**
- BDR-209EBK pre-flashed to BDR-211 (€70–110, the de-facto Hamburg
  crossflash listing) — covers RS8600/RS8800
- BDR-S12J-X or BDR-212EBK with pre-2023 firmware (€120–220) —
  covers RS8F00 and the audiophile vendor-command path
- BDR-XS06 (€40–60) — covers the slim slot-load RS8E21 ASIC

**Total Pioneer budget: €230–390**, three drives across three
architecturally distinct silicon dies.

**Skip:**
- DVR-218/220 (disputed silicon, low fixture distinctiveness)
- BDR-S13U-X with 2023+ firmware (LibreDrive blocked, you cannot
  exercise the full capability matrix)
- ASUS/Verbatim-branded Pioneer rebadges (silicon underneath is
  Pioneer but the relabel obscures vendor fingerprint)

## Panasonic: SuperDrive misidentification + genuine fixture value

The user's earlier hypothesis that the Apple SuperDrive A1379 is a
Panasonic UJ-8A8 OEM relabel is **falsified** (see "Falsified
hypotheses" above).

The Panasonic story underneath is still architecturally rich. The
**MN103S86GSG** (and the DVD-RAM-capable variant **MN103SA6GSJ**)
are the canonical Panasonic AM33-architecture controllers used both
in Panasonic-branded UJ drives *and* in pre-2011 LG/HLDS GWA/GSA
drives — proving via the firmware feature-bit toggle that the same
silicon supports DVD-RAM but LG-branded firmware can disable it.

Panasonic transferred its semiconductor business to Nuvoton (Taiwan,
Winbond subsidiary) on **September 1, 2020**, and removed MN103S
product pages on September 30, 2022. The Linux `arch/mn10300` was
deprecated and removed from the kernel in 4.17 (2018). The MN103/
AM33 line is dead-ended.

### The Apple SuperDrive 0xEA wake command

The MMC dispatcher quirk that justifies a Panasonic-style fixture
is the **Apple SuperDrive 0xEA wake command** — `EA 00 00 00 00 00
01`, a 7-byte vendor-specific CDB with the unusual control byte
`0x01` instead of the standard `0x00`. Documented across Linux
(`sg_raw`), FreeBSD (`camcontrol cmd`), and udev rules; required
because Apple-firmware Panasonic-and-HLDS-bridges remain in tray-
disabled state until the 0xEA enable is received.

luz' 2011 hex-disassembly research (hardturm.ch/luz blog) is the
canonical primary source: the macOS kernel boot-arg `mbasd=1`
(MacBook Air SuperDrive) both overrides the host-Mac whitelist *and*
tweaks USB power management.

For `mos`, this is the textbook MMC dispatcher quirk that no other
vendor reproduces — and it belongs to the Apple bridge, not the
underlying optical mechanism. The DVD-RAM heritage of internal UJ
drives adds a second distinct quirk surface (READ DISC STRUCTURE
format codes 0x80–0x8F unique to DVD-RAM, plus GET CONFIGURATION
feature 0x0022/0x002A profiles that pure-DVD±RW drives lack).

### Panasonic fixture recommendations

**Buy:**
- Apple A1379 SuperDrive (€25–35) for the 0xEA quirk fixture —
  required regardless of underlying mechanism (see "0xEA gate
  acquisition urgency" below)
- UJ-8A8 with Apple PN 678-0611 sticker (€15–25) for the genuine
  internal Panasonic SATA path
- UJ-8E2 or UJ-8FB (€10–15) for the modern slim DVD MN103-era
  fixture
- UJ-265 BDXL slot drive (€60–120) for the locked-firmware MakeMKV-
  hostile BD path

**Total Panasonic budget: €110–195**.

The UJ-265 specifically captures the **negative-test case** of a
drive that refuses to go faster than 2x in MakeMKV — a different
failure mode from Pioneer's firmware-policy lockouts.

### 0xEA gate acquisition urgency

Apple discontinued the SuperDrive in August 2024 (US/Canada/
Australia first), with global delisting completed in 2025. With the
SuperDrive discontinued, no one new is going to do this research —
Apple won't publish IOKit traces for a discontinued product, the
MakeMKV community has no commercial incentive, and existing open-
source implementations operate at a different layer than `mos` does.
Whatever `mos` discovers becomes the canonical reference for the
IOKit-layer SuperDrive 0xEA flow.

Secondary-market pricing for legacy Apple peripherals follows a
predictable curve — cheap-and-plentiful for 2-3 years post-
discontinuation while warehouse inventory clears, then climbing as
collectors and pro users with workflow dependencies compete for
remaining stock. Currently in the cheap-and-plentiful window. Buy
the fixture sooner rather than later.

## Sony Optiarc and Samsung/TSST

The Sony Optiarc lineage runs:

- **NEC MC-10043 → MC-10044 → MC-10045** (DVD)
- **Renesas MC-10131** (post-Renesas/NEC merger of April 2010)
- **MediaTek MT1939MWDU** for Blu-ray (BWU-500S; the BD-writer
  sibling of LG's MT1959 — same MediaTek MT19xx family showing up
  in three vendors)

### Sony Optiarc fixture recommendations

**Buy:**
- AD-7240S or AD-7260S (€8–12) for NEC MC-10045 coverage (~15
  Optiarc DVD SKUs)
- AD-7280S (€10–15) for Renesas MC-10131
- BD-5300S or BWU-500S (€30–50, when findable) for the MT1939 Sony-
  flavor BD path

**Skip:**
- AD-5290S (Vinpower-Lite-On rebrand trap)
- AD-7220S (already-a-Lite-On rebrand trap from the Sony era)

### Samsung/TSST

TSST drives are MediaTek-based throughout (see "Falsified hypotheses").

**Buy:**
- Samsung SE-208 (€20) — canonical USB-MediaTek dispatcher (covers
  ~30 SE-2xx/SN-2xx variants and the USB-vs-SCSI vendor-string
  quirk)
- Samsung SH-S223F or SH-224DB (€7) — internal MT1869L

**Skip:**
- SE-218 (architecturally identical to SE-208)

**Total Sony+Samsung budget: €75–105**.

## Plextor: the only living Sanyo silicon

The genuine Plextor era (Sanyo silicon, Plextor-engineered firmware)
ends in 2007–2008 for DVD writers:

- **PX-W4824/5224** (Sanyo LC89xxx CD-RW)
- **PX-712A** (Sanyo LC897491)
- **PX-716A** (Sanyo LC897492FL)
- **PX-755A/760A** (Sanyo LC897496K)

Everything from PX-810SA onward is OEM:
- PX-810SA wraps Pioneer DVR-212 internals
- Every subsequent PX-87x/88x/89x and PX-LB950SA is a Lite-On rebrand
  identifiable from the Blackened2687 LiteOn Optical Drives Table v1.7

Notably, **PX-891SAF is Lite-On iHAS124 F (MediaTek MT1862N)** — not
a "Plextor SATA Blu-ray-DVD drive" as sometimes claimed. The Blu-ray
flagship was PX-LB950SA (also a Lite-On iHBS212-2 rebrand on
MT1939MWDU silicon).

### Plextor's D8 opcode

Plextor's **D8 opcode** (the proprietary "read CD-DA/CD-XA as raw
audio" command, predating standard MMC `READ CD (BE)`) is the gold
standard for raw CD-audio reads and present on every Sanyo-silicon Plextor —
PX-W4824, PlexWriter Premium, Premium 2, PX-712A, PX-716A, PX-755A,
PX-760A.

Post-2008 Lite-On-rebadge "Plextors" do **not** expose D8, instead
using the Lite-On `BE_CDDA` opcode family. redumper's source code
(github.com/superg/redumper) explicitly groups PLEXTOR (D8) versus
LG/ASUS/LITE-ON (BE_CDDA) as the two compatible-drive families.

PoweRec, GigaRec (writes 70%–130% capacity), VariRec, AutoStrategy,
Q-Check (PI/PIF/Jitter/Beta) all live in Plextor proprietary mode-
page bits accessed via PlexTools/PlexUtilities.

### Plextor fixture recommendations

**The PlexWriter Premium 2 still ships new from Plextor's Japanese
factory** (community-confirmed at gearspace.com), making it the
**only currently-acquirable Sanyo silicon**. Pricing is firmly
collector-tier: €150–280 used on Kleinanzeigen, ~€280 NOS on
Bimedis/Reverb. The PX-712A €350 outlier on eBay DE is a single
seller; market median for PX-712A/PX-716A is €30–80.

**Buy:**
- PlexWriter Premium 2 (€150–250, mandatory — only Sanyo CD-DA-
  extraction reference left in production)
- PX-716SA (€40–70, Sanyo LC897492FL DVD writer with full PoweRec/
  AutoStrategy/GigaRec MMC vendor surface)
- PX-760SA (€50–80, the most evolved Sanyo dispatcher, covers DL
  writing and DVD-RAM read)

**Total Plextor budget: €240–400**.

**Skip** every post-2008 "Plextor" — they are silicon-equivalent to
Lite-On drives already in the fixture set, and buying them is
double-billing the same row.

## Lite-On: MediaTek workhorse with one Renesas outlier

The Blackened2687 LiteOn Optical Drives Table v1.7 maps every iHAS
suffix letter to a distinct MediaTek chip:

- iHAS124 A = MT1879
- iHAS124 B = MT1839LN
- iHAS124 C = MT1809LN
- iHAS124 E = MT1882N
- iHAS124 F = MT1862N

With one critical non-MediaTek outlier:
- **iHAS124 W = Renesas R8J32091NP** (same NEC-lineage silicon that
  powers Pioneer drives)

Within a generation letter, silicon and PCB are identical and only
the EEPROM differs (LabelTag/SmartErase/LightScribe enable bits) —
the C0deKing/Liggy crossflash recipe. Across generation letters the
silicon really does differ.

The Blu-ray writer line (iHBS112/212/312) runs **MT1939MWDU** —
MediaTek's BD-writer sibling chip to the LG MT1959HWDN. MediaTek
MT1939 (Lite-On + Sony BWU-500S) and MT1959 (LG/HLDS) are direct
siblings in the same family. The MMC dispatcher logic is
architecturally similar enough that one fixture row covers both,
but firmware surfaces (LibreDrive eligibility especially) differ.

Lite-On's PLDS (Philips-LiteOn Digital Storage) effectively ceased
PC ODD development around 2017; the SSD business went to Kioxia in
2019; optical inventory is run-out via Vinpower channels.

### Lite-On fixture recommendations

**Buy:**
- iHAS124 B or 324 B (€15–25) — MediaTek MT1839LN anchor
- iHAS124 F (€15–25, sometimes still NOS) — latest MediaTek MT1862N
  dispatcher (identical to PX-891SAF)
- iHBS112 or iHBS212 (€40–80) — MT1939MWDU BD-writer sibling that
  MediaTek MT1959 alone does not fully exercise
- **iHAS124 W (€15–30) is the highest-leverage Lite-On purchase**
  because it's the only Lite-On drive with non-MediaTek silicon
  (Renesas R8J32091NP), giving `mos` a second NEC-lineage dispatcher
  fixture beyond Pioneer

**Total Lite-On budget: €85–160**, four fixtures across three
silicon families.

## ASUS distinguished from rebrands

ASUS does not manufacture optical mechanisms; the lineup splits
across three OEM partners:

1. **BW-16D1HT and BC-12D2HT are LG/HLDS-OEM** with MediaTek MT1959
   (or pre-2016 MT1939) silicon — confirmed identical MMC behavior
   to LG BH16NS40, with cosmetic firmware differences only (vendor
   strings, AACS bus-encryption flag tweaks). Warrant a fixture row
   only as **vendor-string-variant coverage** (INQUIRY reports
   `ASUS` not `HL-DT-ST`, with 3-digit `1.00`/`3.00` firmware
   revisions versus LG's 2-digit `1.02`/`1.03`).
2. **SBW-06D2X-U is genuinely Pioneer-OEM** running the Pioneer
   RS8511 platform — cross-flashable to Pioneer BDR-UD04 (forum.
   makemkv.com confirms).
3. **DRW-24D5MT and DRW-24F1MT are MediaTek-based ASUS-original**
   (the "MT" suffix is the tell).

**DRW-24B1ST shipped from 2010 to roughly 2022 with multiple internal
silicon revisions** (Lite-On in some batches, Optiarc in others);
fingerprinting by firmware date string year prefix is more reliable
than relying on the model number.

SDRW-08D2S-U/U7M-U/U9M-U slim USB drives are Lite-On DS-8A mechanisms
where the USB-bridge dominates MMC behavior, not the optical silicon
— low fixture value.

### ASUS fixture recommendations

**Buy:**
- SBW-06D2X-U (€40–60) — unique Pioneer slim-BD silicon RS8511
- BW-16D1HT (€75–85) — mandatory ASUS-vendor-string row over LG
  MT1959
- DRW-24D5MT new (€22) — modern MediaTek SATA DVD

**Total ASUS budget: €140–170**.

**Skip:**
- BC-12D2HT (silicon-duplicate of BW-16D1HT)
- SDRW-08* slim USB (USB-bridge fixture, not optical-silicon fixture)
- DRW-24B1ST (revision opacity makes it a poor canonical reference)

## Exotic silicon: Yamaha is worth it, the rest are skip

The single high-value exotic fixture is the **Yamaha CRW-F1**,
running the Yamaha **YDC132-V** in-house ASIC — the swan-song silicon
Yamaha shipped before exiting optical drives in 2003. The CRW-F1
exposes architecturally unique MMC features:

- AudioMaster Quality Recording (vendor-specific SET CD SPEED +
  WRITE PARAMETERS quirks at 1x/4x/8x with longer pit/land lengths)
- DiscT@2 laser labeling (vendor-specific WRITE command after lead-
  out with a custom CDB)
- 8MB buffer
- SafeBurn buffer-underrun protection

Read-offset databases track the CRW-F1 with a specific read offset
(+686 samples IDE, +691 SCSI), confirming community treats it as a
distinct entity. Realistic Hamburg pricing is €80–150 for working
units; skip if not findable under €80.

**Skip every other exotic.** Kenwood TrueX 72X has high fixture
value architecturally (7-beam diffraction-grating multibeam) but
practical risk of dead unit is too high — drives ran hot per
O'Reilly *PC Hardware in a Nutshell* and most surviving units are
vibration-damaged. Ricoh JustLink (MP9120/MP7125) is invisible to
the OS — handled silently in firmware, generic ATAPI MMC otherwise.
BenQ DW1655 (Philips Nexperia PNX7862) is optionally interesting at
sub-€20 for SolidBurn/WOPC vendor mode-page coverage but lower
priority than Pioneer. HP and Verbatim are pure rebrands of LG/
Lite-On/Pioneer mechanisms.

Console drives are out of scope: PS5 uses Sony in-house custom
optical silicon cryptographically paired to motherboard, Xbox Series
X uses Lite-On DG-6M5S-03B also cryptographically paired. Both
present non-standard MMC subsets with extensive proprietary auth
commands and are not USB-attachable. Mention only as silicon-family
context.

## Recommended v1.0 fixture set

Eight architectural rows below cover every distinct MMC dispatcher
behavior surfaced in primary-source documentation. Total: twelve
drives at €700–900 from Hamburg sources, achievable in a 4–8 week
sourcing window on Kleinanzeigen plus targeted eBay-DE/UK auctions.

| Architectural row | Canonical drive | Hamburg cost | What it covers |
|---|---|---|---|
| MediaTek MT1959 (LG/HLDS) | (already owned) | — | BH16NS40-NS60, WH16NS40, BU40N, BP50NB40 — ~30 SKUs |
| Renesas R8J32xxx (Pioneer) | BDR-209EBK + BDR-S12J-X (or BDR-212EBK) + BDR-XS06 | €230–390 | RS8600/RS8800/RS8F00/RS8E21 — ~40 BDR/Sx SKUs across three dies |
| Apple-firmware bridge + 0xEA gate | Apple A1379 SuperDrive | €25–35 | The unique macOS-native MMC quirk |
| Panasonic MN103S internal SATA | UJ-8A8 (PN 678-0611) + UJ-265 BDXL | €75–145 | Internal MacBook Pro Unibody 2009-2012; BDXL with locked firmware |
| Sanyo LC8974xx (Plextor genuine) | PlexWriter Premium 2 + PX-716SA + PX-760SA | €240–400 | D8 opcode + PoweRec/GigaRec/AutoStrategy vendor surface |
| MediaTek MT18xx (Lite-On DVD) | iHAS124 B + iHAS124 F | €30–50 | MT1839LN and MT1862N dispatchers |
| Renesas R8J32091NP (Lite-On outlier) | iHAS124 W | €15–30 | Second NEC-lineage dispatcher beyond Pioneer |
| MediaTek MT19xx BD sibling | iHBS212 (or Sony BWU-500S) | €40–80 | MT1939MWDU as the LG MT1959 sibling |
| NEC MC-10045 / Renesas MC-10131 (Sony Optiarc) | AD-7260S + AD-7280S | €18–30 | True NEC-Optiarc silicon before Vinpower trap |
| MediaTek MT1887 USB-DVD (Samsung TSST) | Samsung SE-208 | €20 | USB-vs-SCSI vendor-string mismatch quirk |
| ASUS Pioneer-OEM slim | SBW-06D2X-U | €40–60 | Pioneer RS8511 + ASUS vendor-string row |
| Yamaha YDC132-V (exotic) | Yamaha CRW-F1 IDE | €80–150 (if patient) | AudioMaster + DiscT@2 architectural diversity |

## Conclusion

The narrative arc that emerges from this research is consolidation
followed by abandonment:

- Pioneer exited optical drives in May 2025
- TSST shut down in April 2016
- Panasonic semiconductors sold to Nuvoton in September 2020
- Yamaha left in 2003
- BenQ around 2008
- Samsung wound down

**The optical drive silicon map for `mos` v1.0 is essentially the
final map.** What's bought this year is what the test fixture covers
indefinitely; the Sanyo, MediaTek MT19xx, and Renesas R8J families
that anchor the architectural diversity are not being refreshed.

The Plextor PlexWriter Premium 2, still shipping new from Plextor's
Japanese factory, is the canary — when that production line ends,
every architecturally distinct silicon family except MediaTek will
be available only on the secondary market.

The most operationally important finding is the **firmware-as-
fingerprint axis**. The Pioneer December 2022 cutoff demonstrates
that identical silicon (R8J32740FP42 across BDR-212/213) presents
two completely different MMC dispatcher behaviors depending on
firmware date — bus-encryption flag flips from `0x1B` to `0x13`,
AACS host-key handshake becomes mandatory, READ DISC STRUCTURE
returns `ILLEGAL_REQUEST` on queries that succeeded before. `mos`'s
fixture schema should therefore index by silicon family AND firmware
policy generation, not by silicon family alone.

The honest fixture set, twelve drives across eight architectural
rows for €700–900, gives `mos` defensible coverage of every
documented MMC dispatcher quirk family that an external macOS
application can encounter — including the one fixture that no other
library appears to test (the Yamaha YDC132-V AudioMaster vendor-
mode-page surface) and the one that `mos` uniquely needs (the Apple
SuperDrive 0xEA gate).
