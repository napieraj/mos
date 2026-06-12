# LibreDrive status probe: Phase A feasibility (June 2026)

Phase A of the "explore the real probe" track (plan: explore a real
LibreDrive-status probe). Question: can `mos` faithfully replicate the way
`makemkvcon` determines LibreDrive status — for the user's `HL-DT-ST BD-RE
WH16NS60` (MediaTek **MT1959** family, `ROADMAP.md:287`) — using a
**read-only** command recoverable from public sources? Method: web research
against MakeMKV's own documentation, the public reverse-engineering corpus
(coastermelt, mtk-odd-emulator, cyrozap's notes), and the disc-dumping
community. No `src/` work; this file is the Phase A deliverable and gate.

Confidence per claim: HIGH = primary source (MakeMKV's own docs / a fetched
repo) quoted; MEDIUM = secondary / community / user reports.

## Verdict

**Not publicly recoverable as a faithful read-only status probe. Phase A
gate → STOP.** Three independent walls, any one sufficient:

1. **The decision logic is proprietary and unreleased.** Whether a drive is
   `Enabled` / `Possible` / unsupported is computed by MakeMKV's `LibDriveIO`
   interpreting `SDF.bin`. `SDF.bin` carries per-firmware blobs *and a master
   blob that "exports an API to uniquely fingerprint a drive firmware
   version"* — i.e. the identification logic itself is a closed data blob.
   `LibDriveIO` source has never been released (stated unreleased in MakeMKV's
   own 2019 write-up; still unreleased through the November-2025 `SDF.bin`
   update). There is no public table mapping drive/firmware to status to
   reimplement. (HIGH)

2. **No clean read-only "status" command exists; the substrate is the
   code-injection interface.** Platform/firmware identity on these controllers
   is read through vendor-specific debug opcodes (the MediaTek `0xF2`-series
   RAM peek/poke; `0xF1` cache read) — the *same* interface used to write RAM
   and inject microcode. Reading the platform is not a benign INQUIRY-style
   verb; it means driving the controller's debug command set. (HIGH for the
   opcode family's existence; MEDIUM that MT1959 status specifically is read
   this way)

3. **"Enabled" is a runtime fact inseparable from the excluded step.**
   MakeMKV's own words: when the firmware version is supported, `LibDriveIO`
   *"uploads a small firmware extension into drive volatile memory… the
   firmware extension stays only in drive RAM and drive firmware is not
   changed."* `Enabled` reflects that RAM upload having succeeded — which is
   exactly the microcode-access step `mos` excludes on scope and safety
   grounds. A status flag that means "the upload would/did work" cannot be
   produced without performing (or precisely modelling) the upload. (HIGH)

## Evidence

### How makemkvcon actually determines status
- LibreDrive is implemented in `LibDriveIO`, *"a simple interpreter that
  translates requested actions into a set of firmware-specific SCSI commands
  based on data blobs called SDF,"* with `SDF.bin` as the master file of
  per-firmware blobs plus the fingerprinting master blob. (HIGH — MakeMKV
  "What is LibreDrive")
- On open, the library fingerprints the firmware, and if supported uploads a
  RAM-only firmware extension exposing the unrestricted read interface; flash
  is untouched. The info-pane fields (drive platform e.g. `MT1959`, hardware
  support, firmware support) come from this probe and are shown without
  flashing. (HIGH for the mechanism; MEDIUM that the platform read is fully
  non-writing)
- Status strings observed: `Enabled`; `Possible (patched firmware
  available)` / "possible, not yet enabled"; unsupported. `Enabled` = RAM
  microcode loaded; `Possible` = silicon supported but stock firmware locks
  the microcode-access commands. (MEDIUM — forum/user reports)

### The public RE substrate (what *is* available, and its limits)
- **coastermelt** (scanlime) reverse-engineered the Samsung SE-506CB /
  **MT1939**: ARM core, ~4 MB RAM, memory access and code execution via
  vendor SCSI commands. (MEDIUM — PoC‖GTFO 7:3 + repo)
- **mtk-odd-emulator** (JayFoxRox) targets the older **MT1329E** (Xbox-era
  Samsung SDG-605B) and documents `0xF2`-series debug opcodes —
  `0xF202` bank select, `0xF203` RAM read, `0xF204` flash *write*, `0xF205`
  RAM retrieval (example CDB `F2 05 00 07 BF 00 00 00 00 60`). These are
  **generation-specific** and are the MT1329E set, **not** a documented
  MT1959 status command. (HIGH that this is what the repo documents; the gap
  to MT1959 is the point)
- `0xF1` "read cache" is a known custom opcode on MT1939/MT1959 used by
  disc-dumping tools (DIC/Redump) for lead-out reads — read-only, but it
  reads *disc data*, not platform/firmware status. (MEDIUM)
- cyrozap's `optical-disc-drive-re` is an index of the above; it contains no
  opcodes or status-detection sequence of its own. (HIGH — fetched)

So a read-only memory primitive against a MediaTek controller is publicly
*demonstrated to exist*, but (a) it is undocumented for the MT1959
specifically, (b) it is the debug/code-injection interface rather than a
spec or convenience verb, and (c) turning bytes read at some address into
`Enabled/Possible/No` requires the closed `SDF.bin` fingerprint logic from
wall 1. The transport is partially public; the meaning is not.

## Why this also fails the scope and safety tests (independent of recoverability)
- Even a perfect public spec would not clear `ROADMAP.md:214,314` (AACS /
  LibreDrive microcode explicitly out) or `AGENTS.md` layer 1 (one raw CDB)
  / layer 2 (no SPC/vendor ambition). The override ADR was Phase B's job and
  is moot now.
- The read primitive *is* the entry to the microcode-access interface; there
  is no clean severance between "ask the status" and "drive the injection
  substrate." Reproducing it would put `mos` in the business of operating a
  drive's vendor debug interface — outside both its command-surface doctrine
  and its read-only/defensive posture.

## The only honest fallback (not detection)
If a flag is still wanted, the sole defensible option is a **static identity
heuristic**, clearly named as such (e.g. a `libredrive_platform: likely`
derived from the vendor/product strings `mos` already reads, matched against
the known MediaTek-MT19xx LG/rebadge family that `ROADMAP.md:287-289`
enumerates). This is a *guess about silicon family*, not makemkvcon's
runtime status: it cannot distinguish stock vs patched firmware, cannot tell
`Enabled` from `Possible`, and is exactly the device-string special-casing
the scope doctrine warns against (cf. the struck PVD parser, 2026-06-10). It
should not be labelled `libredrive` without that qualifier, and adopting it
is a product decision, not a research conclusion.

## Recommendation
Close the "explore the real probe" track here: the faithful method is not
recoverable from public sources and would violate scope/safety even if it
were. Carry the `Verdict` and the fallback note to the user; do not proceed
to Phase B/C. If the user wants a flag anyway, scope it as the labelled
identity heuristic above, decided on its own merits.

## Sources
- MakeMKV, "What is LibreDrive?" — https://forum.makemkv.com/forum/viewtopic.php?t=18856
- MakeMKV, "Direct access to SCSI devices on Mac OS X" — https://www.makemkv.com/osxmmc/
- LibreDrive overview — https://grokipedia.com/page/LibreDrive
- cyrozap, optical-disc-drive-re (Notes) — https://github.com/cyrozap/optical-disc-drive-re/blob/master/Notes.md
- JayFoxRox, mtk-odd-emulator — https://github.com/JayFoxRox/mtk-odd-emulator
- scanlime, coastermelt; PoC‖GTFO 7:3 — https://mcfp.felk.cvut.cz/publicDatasets/pocorgtfo/contents/articles/07-03.pdf
- LibDriveIO source-status / SDF.bin discussion — https://forum.makemkv.com/forum/viewtopic.php?t=24312
