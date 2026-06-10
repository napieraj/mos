# HLDS optical drive reverse engineering — silicon and MMC implementation

Date: 2026-04-26

Research synthesis from the cdrinfo.pl reverse-engineering community
(Blackened2687, czary2mary, jadburner) and adjacent English-language
sources (forum.makemkv.com, club.myce.com), translating their findings
into implementation guidance for `mos` v0.3 typed APIs.

The most useful single deliverable from this community is a silicon
family tree, not an MMC behavior catalog. The 2016 *Hitachi/LG Optical
Drives Table V1.1* by Blackened2687 and czary2mary maps every HLDS SKU
from 2003–2016 to its controller chip, buffer, OPU, bootcode and
clones — and that mapping is what tells you why a BH16NS55 will accept
WH16NS60 1.00 firmware in the first place. Both drives use the same
MediaTek MT1959HWDN SoC and the same Hitachi HOP-B711 optical pickup;
the JB9 board added an external AACS2 cipher block but did not change
the MMC dispatcher.

That single fact has design consequences for `mos`: a typed API
written against MT1959HWDN should work unchanged across BH16NS50/55/58,
WH16NS58/60, BH16NS60, BU40N/BU50N (slim), the BP50NB40/BP55EB40/
BP60NB10 USB siblings, and the dozens of OEM relabels (ASUS BW-16D1HT,
Dell BH40N, HP BH50N, Buffalo BRUHD/BRXL, Verbatim 43888-90).

The community has produced almost no public command-level quirk
database — most CDB-behavior knowledge for `mos` should come from
libburn, libcdio, dvd+rw-tools and udev's `cdrom_id`, with cdrinfo.pl
providing the hardware substrate that justifies treating those
generic findings as transferable.

## Three-era silicon story

The *Encyklopedia HLDS* PDF reveals three clean architectural eras
that map almost perfectly onto observable MMC behavior:

**Era 1: Multi-chip set (2003–2005, GMA/GSA-40xx PATA)**
- NEC μPD85005S1112 read-channel ASIC
- Hitachi HD153720RTF (or 721/722/724) servo DSP
- NEC μPD703101AGJ-33 (V850 RISC core) host-side MCU
- Command dispatch traverses inter-chip bus
- Only era in which the servo loop is physically separate from the
  MMC interpreter

**Era 2: Renesas/Panasonic single-die (2005–2010)**
- Renesas R8J32xxx parts collapse the V850 + servo into a single die
  (paired with R2S/R2A analog companions)
- Parallel branch shipped Panasonic MN103S86GSG/SA6/SB4/SC*/SD* as a
  second supplier in DVD writers

**Era 3: MediaTek consolidation (2011–present)**
- MT1839LN, MT1882N/DN, MT1862N/AN for DVD
- MT1959 with three publicly observed die suffixes (USFN, UWDN, HWDN)
  for Blu-ray
- MT1959USFN ↔ JBC5 platform (2012-05-02), CH12NS30/38, UH12NS30
- MT1959UWDN ↔ JB7 platform (2011-11-17), BH14/16NS40/48, WH14/16NS40
  stamped "SVC NS40"
- MT1959HWDN ↔ JB8 platform (2015-05-08), BH16NS55 hardware lineage
- MT1959HWDN on JB9 (2015-09-03), BH16NS60/WH16NS60 with additional
  AACS2 silicon block

## Primary source: czary2mary on JB9

> "Hardware wersji HLDS NS60 pomimo zastosowania nowej płyty głównej
> JB9 i układu do deszyfracji AACS2 jest bardzo podobny jak w
> konstrukcjach NS50 \\ 51 \\ 55 \\ 58. Czyli mamy do czynienia z
> chipsetem Mediatek MT1959 HWDN oraz mechaniką oraz OPU Hitachi
> HOP-B711."

Translation: the JB9 platform's hardware, despite a new mainboard and
an AACS2 decryption circuit, is constructively very close to JB8 —
same MT1959HWDN SoC, same mechanics, same HOP-B711 OPU. This is the
single strongest primary source justifying a unified "MT1959HWDN
profile" inside `mos`.

Practical observation from the same community (Redump wiki, MakeMKV
t=18933): OpCode 0xF1 (a Plextor-style cache-read vendor opcode) is
*present* in early ASUS BW-16D1HT 3.02 firmware and *removed* in
3.10/3.11 on the same MT1959 silicon. Strong evidence that on this
platform the MMC command table is **firmware-defined, not silicon-
defined** — `mos` should expect identical typed-API behavior across
cross-flashed firmwares but should not assume vendor opcodes are
stable.

## Hardware-level facts that bound recovery and debug strategy

**No external SPI flash on MT1959.** Firmware lives on the SoC's
internal NOR. Recovery flashing only works via DosFlash with the SATA
controller in legacy IDE mode (not AHCI), on a 32-bit OS. forrest222
on elektroda.pl is explicit:

> "W BIOSie musisz zmienic SATA z AHCI na IDE… Bootujesz winpe x86
> (nie x64!), odpalasz DosFlash32_BH16NS40.exe, wskazujesz na
> firmware.bin."

**No JTAG/UART debug header is documented** on any HLDS PCB in any
source surveyed. If `mos` ever wants hardware-level state inspection,
that's an open research gap requiring physical teardown of a specific
PCB rev.

## MMC command behavior — mostly standard, three real watch-outs

The single most important calibration for `mos` is that the
cdrinfo.pl/MakeMKV/Myce/RedFox forums contain **no public CDB-level
quirk catalog** for the BH16/WH16 family. This is not a research
failure; it is a positive finding. HLDS BH16/WH16 drives behave as
MMC-6 / SPC-4 compliant devices for state-query purposes, with all
observed sense codes in user logs being standard values:

- 3A/01 medium-not-present tray closed
- 04/01 LU becoming ready
- 28/00 medium-may-have-changed
- 29/00 power-on/reset

The Linux `sr` driver carries no `BLIST_*` flag for HL-DT-ST in
`scsi_devinfo.c`. Over a decade of community Linux integration is
strong negative evidence: a non-spec sense behavior would have
generated a kernel quirk by now.

**`mos` should treat BH16NS55/WH16NS60 as standards-compliant for
state queries and not add HLDS-specific sense parsers without
empirical evidence.**

### Watch-out 1: BH16NS55 1.04/1.05 sleep bug

Stock LG firmware on the half-height JB8 SKU silently puts the drive
into a degenerate state after roughly two minutes idle with media in
the tray. Subsequent CDBs grind, time out, and only a tray-open/close
cycle recovers. WH16NS60 1.00 (the user's running firmware) does not
exhibit this bug — the most-cited firmware-level reason the community
recommends the cross-flash.

For `mos`: a typed API targeting *stock* BH16NS55 firmware needs an
idle keepalive (a periodic TUR every ~90 s); on WH16NS60 1.00, no
keepalive is needed.

### Watch-out 2: Media-becoming-ready ~30 second window

Well-documented in MakeMKV's "spins disc for about 30 seconds making
the same repeated three noises" descriptions across BH16NS55 and
WH16NS60 threads. `mos`'s tray/ready API should poll TUR at 250–500
ms with a soft timeout near 30 s, treat ASC 04/01 as "becoming ready,
continue", and only after TUR returns READY confirm with a GET
CONFIGURATION whose Current Profile is non-zero before declaring
media valid.

### Watch-out 3: Post-bricked-flash recovery mode

When a drive is in post-bricked-flash recovery mode, BH16NS55 reports
as `BD-RE WH16NS40 1.00/1.01` with revision string "BOOT" or starting
with `0.` — and crucially it answers INQUIRY and TUR=READY but
*cannot service READ*. `mos` should treat any INQUIRY revision
matching `^(BOOT|0\.)` as degenerate-mode, exposing it through a
typed state rather than letting the rest of the API assume a usable
drive.

For UNIT ATTENTION handling, GET EVENT STATUS NOTIFICATION timing,
and TUR state-machine transitions: no public HLDS-specific dataset
exists. Standard "TUR, on UA retry once" approach has no documented
failure. GESN works in polling mode (Linux uses 2 Hz; no MakeMKV
thread reports HLDS misbehaving at higher rates). Defensible default
is GESN at 1 Hz, never above 4 Hz, requesting Class 4 (Media) plus
Class 1 (Operational Change). Treat this as POSSIBLE-by-absence-of-
complaints, with empirical capture against the user's own BH16NS55
as canonical augmentation.

## Implementation-ready guidance for v0.3 typed APIs

### `mos_get_disc_capacity()`

Canonical reference is `dvd+rw-mediainfo`, which on a blank DVD+R
prints `READ CAPACITY: 02048=0` while simultaneously reporting
`Free Blocks: 22951042KB` from READ DISC INFORMATION + READ TRACK
INFORMATION. **READ CAPACITY(10) returns LBA=0 on blank/sequentially-
recorded media — by spec, not by HLDS quirk.**

Therefore:
- Treat READ DISC INFORMATION (0x51) plus READ TRACK INFORMATION
  (0x52) as the primary capacity source
- READ CAPACITY only as the "last readable LBA" check on already-
  finalized media
- For BD-R DL/TL/QL where 32-bit LBA might overflow, prefer
  READ CAPACITY(16) / SAI(16) with READ CAPACITY(10) as fallback
- READ FORMAT CAPACITIES (0x23) is the authoritative source for
  *maximum formattable* capacity on write-once and formattable media
- READ DISC STRUCTURE format 0x00 gives the legacy lead-out byte
  count for DVD/BD and is what dvd+rw-tools actually reports as
  canonical disc size
- Sentinel rule: if READ CAPACITY returns LBA<4096 or =0, switch to
  the DISC INFO / TRACK INFO / FORMAT CAPACITIES path

### `mos_get_disc_identity()`

READ DISC STRUCTURE (0xAD) format 0x00 is the practical primary on
DVD+R/+RW, even though the manufacturer ID is technically encoded in
ADIP — most consumer drives surface the MID via the format-0x00
"media specific" bytes, and **format 0x11 (ADIP proper) commonly
returns CHECK CONDITION with ASC 0x24 (Invalid field in CDB)**.

DVD Identifier's two-method approach (Method 1 = format 0x11,
Method 2 = format 0x00) exists precisely because format 0x11 is
unreliable across drive families.

- DVD-R/-RW: use format 0x00 only (MID is in pre-pits, not ADIP)
- BD: use format 0x00 (DI); BD format 0x09 (BCA), 0x0A (POW),
  0x12 (Track Info) typically open on BD-R/RE and gate behind host-
  bus-key handshake on AACS-protected BD-ROM (this gating is the
  bypass concern; `mos` should not depend on it)
- CD-R/CD-RW: READ TOC/PMA/ATIP (0x43) format 0x04 (ATIP) with
  libburn's canonical CDB `{0x43, 2, 4, 0,0,0, 0, 16, 0, 0}`

### `mos_tray_control()`

START STOP UNIT (0x1B) and PREVENT/ALLOW MEDIUM REMOVAL (0x1E), both
with well-defined MMC semantics that HLDS appears to honor without
deviation.

Non-obvious correctness rule: **persistent-prevent state**. PREVENT
codes 0–3 with code 3 being persistent prevent that survives until
the *same initiator* issues code 2, or a power cycle, or LU reset.
udev acquires a prevent-lock on every disc insertion via `cdrom_id
--lock-media` to enable kernel eject-button notifications.

`mos` must release any prevent-lock its process acquired before
exiting, because a stale lock persists on the drive across process
death and only clears on power cycle or another initiator unlocking.

Slim drives (BU40N, BP50NB40) respond to standard 0x1B with LoEj=1;
the LG BU20N service manual confirms the front-panel eject button is
software-gated through firmware that internally issues the same
LoEj — there is no slim-specific protocol.

### `mos_speed_control()`

SET STREAMING (0xB6) as canonical for BD/DVD/CD on modern HLDS, with
SET CD SPEED (0xBB) as a CD-only legacy fallback that drives still
honor for back-compat. GET PERFORMANCE (0xAC) enumerates supported
(read, write) speeds per profile.

cdrdao output on an HLDS-class drive shows coherent CD/DVD/BD
descriptors:
`Maximum read speed: 17982 kB/s (CD 102x, DVD 12x, BD 4x)`

Three behaviors deserve to be encoded in the API contract:

1. **RipLock**: stock LG firmware silently caps BD-Video read speed
   to ~5–6× regardless of SET STREAMING; community-patched MK
   firmwares remove the cap. Whether the user's WH16NS60 1.00 has
   RipLock active depends on whether the cross-flash landed on stock
   1.00 or the MK 1.00 build.
2. **Speed does not survive media changes** — MakeMKV's per-drive-
   per-media-type speed model is exactly because drives don't
   reliably retain SET STREAMING across disc swaps. `mos` should
   re-issue on every media-change event.
3. **Quiet/acoustic-management mode is not exposed via standard MMC
   pages** on this HLDS family. There is no Mode Page 0x1C entry, no
   vendor-specific MMC page, and no documented vendor CDB; RipLock-
   style acoustic gating is patched into firmware images, not
   exposed. Document quiet mode as unimplementable in v0.3 and offer
   a lower SET STREAMING target as the workaround.

### `mos_enumerate_features()`

Spec-compliant **two-pass pattern**:

1. GET CONFIGURATION (0x46) with RT=00, Starting Feature=0,
   Allocation Length=8
2. Parse the BE32 Data Length at bytes 0–3 (which counts bytes
   *after* the length field)
3. Allocate `data_length + 4`, then reissue

libcdio's mmc.h header definitions and xorriso/libburn's cdrskin
walk this pattern verbatim.

Two practical observations:
1. HLDS feature lists are typically <40 descriptors and well under
   2 KiB total — a 4 KiB single-shot is a safe fast path that skips
   the header pass entirely
2. udev cdrom_id captures against HL-DT-ST GT40N/GSA-T20N show
   `size of features buffer 0x015c` and `0x0160` respectively, with
   no observed buffer-overflow vector and no documented case where
   HLDS reports a Data Length larger than what it actually fills

Defensive fallback: if Data Length comes back implausibly small
(<8) or the firmware lies, walk iteratively with Starting Feature
Number incremented past the highest feature code returned. Use RT=00
for capability discovery and RT=10 for "what does the inserted media
support". The Allocation Length field is 16-bit, so clamp to 65535.

## Audio CD detection — the spec is the design

The `drutil`-can't-distinguish use case has a clean answer that
doesn't depend on any HLDS-specific behavior.

**Pure CD-DA discs are profile 0x0008 (CD-ROM) per MMC spec**;
CD-DA detection requires inspecting **READ TOC (0x43) format 0x00**
track-control bytes (bit 2 of the CONTROL nibble distinguishes data
from audio tracks) — not the GET CONFIGURATION current profile. This
is generic MMC, confirmed by OSDev's MMC reference and by libcdio's
`cd-info`.

The libcdio-devel mailing list (2010-02) contains a positive
transcript on an HLDS drive (HL-DT-ST DVDRAM GUD0N) correctly
distinguishing CD-DA tracks from a CD-ROM track on an Enhanced CD
with `Disc mode: CD-DA, CD-ROM` and the data session reachable via
READ TOC format 0x01 (Multi-session).

The closed-vs-open session distinction is solved without parsing the
full TOC. **READ DISC INFORMATION (0x51)** returns:
- Disc Status field (Empty / Incomplete / Finalized / Others) in
  byte 2 bits 0–1
- State-of-last-session in bits 2–3
- Erasable flag in bit 4
- Number of Sessions across bytes 4 and 9

A closed-session audio CD: Disc Status=Finalized, last-session-
state=Complete. An open-session audio CD: Disc Status=Incomplete,
last-session-state=Empty or Incomplete.

For Hybrid SACD, **standalone PC drives cannot read the HD/DSD
layer** — they read the underlying Red Book CD layer and report it
as profile 0x0008 with audio tracks in TOC. This is universal, not
an HLDS quirk.

CD-Text via READ TOC format 0x05: cdrdao carries a `0x00200000`
"do not try to read CD-Text" flag because some drives lock up or
send junk. No source identifies HLDS as such a drive class. Treat
as presumed-working, verify empirically, fall back gracefully.

## What the Polish community is and isn't

The cdrinfo.pl reverse-engineering scene around Blackened2687 and
czary2mary is, with high confidence, **a hardware-identification
community first, a flashing/UHD community second, and a behavioral-
MMC community essentially zero**.

Concrete deliverable: the *Hitachi/LG Optical Drives Table V1.1* — a
60 KB PDF mapping every HLDS SKU 2003–2016 to chipset/buffer/OPU/
clones — plus the parallel LiteOn and Toshiba/Samsung tables, plus
tooling integration (DosFlash patched for BH16NS40/BH16NS55, hosted
at forum.cdrinfo.pl/f29/).

Acknowledgements line in V1.1:
> "Special thanks to Devilsclaw for his Flasher tool, cvs, jadburner,
> vroom, zevia, MegaDETH, ~KIPPER~, Sunfish, jcroy and many other
> members and reviewers from CDFreaks/MyCE, CDRinfo.pl and iXBT."

Tri-national, English-working-language, forum-hosted: cdrinfo.pl
(Polish anchor), club.myce.com (English anchor, formerly CDFreaks),
iXBT (Russian anchor).

What this community has **not** produced (strong negative evidence):
- No Google Scholar–indexed papers
- No CCC, DEF CON, HOPE, RECON, OffensiveCon, Confidence (Kraków),
  PWNing, or SECURE talks attributable to the named regulars
- No T10/INCITS contributions
- No Linux kernel commits crediting cdrinfo.pl or the named handles
- No libcdio, cdrtools, libdvdread, or libbluray CHANGELOG credits

The single relevant CCC talk in the optical-drive-RE space is 36C3
(2019) "Hacking Sony PlayStation Blu-ray Drives" by Boris Larin
(oct0xor), who is Russian (Kaspersky GReAT), not Polish, and targets
PS3/PS4 BD drives, not LG/HLDS retail.

For `mos` documentation, the natural and only tier-1 citation is:

> Blackened2687 & czary2mary, *Hitachi/LG Optical Drives Table V1.1*,
> 2016, hosted at forum.cdrinfo.pl/attachments/f15/

There is no paper, no talk, no patch series to cite alongside it.
If `mos` needs deeper MMC-quirk data later, the better wells are
libcdio-devel mailing list archives, redumper's GitHub issues, a
drive read-offset database, cdrdao's driver-flag tables, and
MakeMKV forum threads from user `mike` — though the latter's deepest
work is largely bypass-related and out of scope.

## Conclusion

`mos` should not chase HLDS-specific quirks for the v0.3 typed APIs.
The community's silicon-mapping work establishes that the
BH16NS55/WH16NS60 sit on a single, stable, well-trodden MT1959HWDN
platform. The absence of kernel quirk-list entries, libcdio quirks,
cdrtools workarounds and forum sense-code threads is itself a signal
that this platform behaves to spec on the commands `mos` cares about.

The implementation-ready bottom line for v0.3: write a single typed
API that targets MMC-6 / SPC-4 conformance, with three explicit
profile flags carved out:
1. BH16NS55-stock idle-keepalive flag (irrelevant for cross-flashed
   users but necessary for users on stock)
2. Degenerate-mode detector keyed off INQUIRY revision strings
   starting with `BOOT` or `0.`
3. RipLock-aware speed-control contract that documents silent
   capping on protected BD-ROM under stock LG firmware

Empirical verification the user should still run — `sg_get_config
-H`, `sg_logs -p sense`, `sg_inq -e` against the actual BH16NS55
once — is the bridge from these public findings to the typed API
that ships.

## Sources

- forum.cdrinfo.pl/attachments/f15/77905d1461420808-encyklopedia-
  hitachi-lg-hlds-hlds_table_v1.1.pdf (Blackened2687 + czary2mary,
  *Hitachi/LG Optical Drives Table V1.1*, 2016)
- forum.cdrinfo.pl/f29/ (Pioneer DVRTool/BDRFlash threads)
- forum.cdrinfo.pl/f15/ (Encyklopedia HLDS subforum)
- club.myce.com (English-language successor to CDFreaks)
- forum.makemkv.com (MakeMKV community drive databases)
- forum.redfox.bz (RedFox/AnyDVD community)
- redump.org/wiki (Optical drive compatibility documentation)
- elektroda.pl (Polish electronics community, hardware recovery
  threads)
