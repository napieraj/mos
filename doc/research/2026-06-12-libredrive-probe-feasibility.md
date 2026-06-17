# LibreDrive detection in `mos`: research report (June 2026)

Full-session research report. **Question (user):** how does `makemkvcon`
determine whether LibreDrive is enabled, and can `mos` use the same method to
surface a `libredrive` flag — specifically for the user's `HL-DT-ST BD-RE
WH16NS60` (MediaTek **MT1959** family, `ROADMAP.md:287`)? **Track:** the user
chose to explore a real probe, then scoped it to Phase A (research only).
**Method:** parallel codebase exploration of `mos`'s device-info and command
infrastructure, plus web research against MakeMKV's own documentation, the
public reverse-engineering corpus (coastermelt, mtk-odd-emulator, cyrozap's
notes), and the disc-dumping community. No `src/`, schema, or ADR changes;
this file is the deliverable.

Confidence per claim: HIGH = primary source (MakeMKV's own docs, a fetched
repo, or the `mos` tree) quoted; MEDIUM = secondary / community / user reports.

## Verdict

**Replicating makemkvcon's method is not feasible from public sources, and
would violate `mos` scope/safety doctrine even if it were. Track closed at
Phase A.** Three independent walls on recoverability, any one sufficient:

1. The status decision logic is proprietary and unreleased (MakeMKV's
   `LibDriveIO` + `SDF.bin`).
2. There is no clean read-only "status" command; the substrate is the
   vendor debug / code-injection interface, undocumented for MT1959.
3. "Enabled" is inseparable from the RAM-microcode upload that `mos`
   excludes on scope and safety grounds.

The only honest alternative is a **static identity heuristic** (a guess about
silicon family from the vendor/product strings `mos` already reads), which is
*not* makemkvcon's method and must not be labelled as detection. Adopting it
is a product decision, not a research conclusion.

---

## Part 1 — How makemkvcon actually determines LibreDrive status

LibreDrive status is **not** an MMC/T10 fact. It is produced by talking to the
optical drive's **controller silicon** over vendor-specific commands:

- LibreDrive is implemented in `LibDriveIO`, *"a simple interpreter that
  translates requested actions into a set of firmware-specific SCSI commands
  based on data blobs called SDF,"* with `SDF.bin` the master file of
  per-firmware blobs plus a master blob that *"exports an API to uniquely
  fingerprint a drive firmware version."* (HIGH — MakeMKV "What is LibreDrive")
- On open, the library fingerprints the firmware; if the version is supported
  it *uploads a small firmware extension into the drive's volatile RAM* that
  exposes the unrestricted read interface — *"the firmware extension stays
  only in drive RAM and drive firmware is not changed in any way."* (HIGH)
- The info-pane fields (drive platform e.g. `MT1959`, hardware support,
  firmware support) come from this probe and display without flashing.
  (HIGH for the mechanism; MEDIUM that the platform read is fully non-writing)
- Status strings: `Enabled`; `Possible (patched firmware available)` /
  "possible, not yet enabled"; unsupported. `Enabled` = RAM microcode loaded;
  `Possible` = silicon supported but stock firmware locks the
  microcode-access commands. (MEDIUM — forum/user reports)

So the "method" is: fingerprint the controller firmware via a proprietary
blob, attempt the RAM-only microcode upload, and report the outcome. The
transport is partly public; the *meaning* is closed.

## Part 2 — How `mos` models device info and commands today

What a `libredrive` flag would have to thread through, and what command
infrastructure exists (HIGH — `mos` tree).

**Device identity** comes from DiscRecording, not INQUIRY (the open-time
INQUIRY retired in the 2026-06-10 DR pivot): `src/mos_dr.c:103-120` pulls
vendor / product / firmware-revision from `DRDeviceCopyInfo` into fixed-width
slots on the handle (`vendor_str[9]`, `product_str[17]`, `revision_str[5]` in
`src/mos_internal.h`). These surface through `mos_state_result`
(`src/mos_pure.h`) via `mos_state_result_vendor/_product/_revision()`
(`include/mos.h`). This is how `mos` would know a drive is "an LG model" —
the vendor/product strings (e.g. `HL-DT-ST` / `BD-RE WH16NS60`), as raw bytes,
escaped only at the emit surface.

**No boolean device flags exist today.** The public surface is state enums,
numeric IDs (registry/bsd), MMC profile codes, and identity strings. A new
`bool libredrive` would be a first-of-kind: append to `mos_state_result`
(`src/mos_pure.h`), add a `mos_state_result_libredrive()` accessor
(`include/mos.h`), populate at query time, thread into the CLI row and
emitters (`cli/common.c` `query_row` + table/JSON emit, `cli/common.h`
`list_row`), and — because the published JSON schemas are closed
(`additionalProperties:false`, per the JSON-schema ADR) — land it under a new
schema name (`mos.state.v2`) with positive + negative fixtures and the
`schemas/validate.py` guard, not as an additive v1 field.

**Command infrastructure.** `mos` authors exactly **one** raw CDB in the whole
tree: GESN `0x4A`, built at `src/mos_scsi.c:465` and dispatched through
`mos_raw_cdb()` at `src/mos_scsi.c:677`, which is the only caller of
`ObtainExclusiveAccess` and releases the lock per call (acquire-on-call /
release-on-return). Everything else (TUR, GET CONFIGURATION, READ DISC
INFORMATION) goes through `MMCDeviceInterface` convenience methods that take
no lock. Sense is parsed in the pure layer (`src/mos_sense.c`); the nub gate
(`src/mos_state_core.c`, proven exhaustively in
`tests/audit/nub_invariant_check.c`) decides when the lock is safe to take.
A LibreDrive probe would be a **second raw CDB** — the heaviest possible
addition to this surface.

## Part 3 — Scope and safety constraints any probe must meet

From `AGENTS.md` scope doctrine and `ROADMAP.md` (HIGH — `mos` tree):

- **Out of scope, stated twice.** `ROADMAP.md:214` ("Out: firmware flashing,
  AACS/LibreDrive microcode…") and `ROADMAP.md:314` ("AACS handshake / BD+ /
  key extraction (MakeMKV territory)").
- **Layer 1 — one raw CDB.** A second raw verb requires (a) a documented
  showing no convenience method carries the info, and (b) a full
  nub-collision analysis (the GESN template), because raw means exclusive
  access.
- **Layer 2 — no SPC/vendor ambition.** The verification oracle is Apple's
  optical kext, never general SCSI; vendor commands are out.
- **Layer 3 — privilege footprint.** `SCSITaskUserClient` console grant only:
  no root, no entitlement, no block-device I/O.
- **Precedent.** A public PVD parser was shipped and struck the same day
  (2026-06-10) precisely because its only consumers were "tools already deep
  in makemkvcon territory."

A doctrine-overriding ADR was Phase B's job; it is moot given the Part 4
verdict.

## Part 4 — Phase A feasibility: the three walls

1. **The decision logic is proprietary and unreleased.** Whether a drive is
   `Enabled` / `Possible` / unsupported is computed by `LibDriveIO`
   interpreting `SDF.bin`, whose master blob *is* the firmware-fingerprint
   logic. `LibDriveIO` source has never been released (stated unreleased in
   MakeMKV's 2019 writeup; still unreleased through the November-2025
   `SDF.bin` update). No public drive/firmware→status table exists to
   reimplement. (HIGH)

2. **No clean read-only status command — the substrate is the injection
   interface.** Platform/firmware identity on these controllers is read via
   vendor debug opcodes (the MediaTek `0xF2`-series RAM peek/poke; `0xF1`
   cache read) — the *same* interface used to write RAM and inject microcode.
   The public RE corpus documents these only for older silicon:
   **mtk-odd-emulator** covers the **MT1329E** (Xbox-era Samsung SDG-605B) with
   `0xF202` bank-select / `0xF203` RAM-read / `0xF204` flash-write / `0xF205`
   RAM-retrieve (example CDB `F2 05 00 07 BF 00 00 00 00 60`);
   **coastermelt** covers the **MT1939** (ARM core, ~4 MB RAM, memory access +
   code execution over vendor SCSI). Neither documents an **MT1959** status
   command, and `0xF1` reads *disc data*, not platform/firmware status.
   cyrozap's `optical-disc-drive-re` is an index of these with no opcodes of
   its own. (HIGH that this is what the repos document; MEDIUM on the MT1959 gap)

3. **"Enabled" is inseparable from the excluded step.** Per MakeMKV's own
   words, `Enabled` reflects the RAM-microcode upload having succeeded — the
   exact microcode-access step `mos` excludes. A flag meaning "the upload
   would/did work" cannot be produced without performing or precisely
   modelling the upload. (HIGH)

Net: a read-only memory primitive against a MediaTek controller is publicly
*demonstrated to exist*, but it is undocumented for MT1959, it is the
debug/code-injection interface rather than a spec verb, and turning bytes into
`Enabled/Possible/No` requires the closed `SDF.bin` logic from wall 1. The
transport is partly public; the meaning is not.

## Part 5 — The only honest fallback (not detection)

If a flag is still wanted, the sole defensible option is a **static identity
heuristic**, clearly named as such — e.g. `libredrive_platform: likely`
derived from the vendor/product strings `mos` already reads (Part 2), matched
against the known MediaTek-MT19xx LG/rebadge family that `ROADMAP.md:287-289`
enumerates (BH14/16NS\*, WH16NS\*, BU/BP slim & USB, ASUS/Buffalo/Dell/HP/
Verbatim/Vinpower rebadges). This is a *guess about silicon family*, not
makemkvcon's runtime status: it cannot distinguish stock vs patched firmware,
cannot tell `Enabled` from `Possible`, and is exactly the device-string
special-casing the scope doctrine warns against (cf. the struck PVD parser).
It must not be labelled `libredrive` without that qualifier, and adopting it
is a product decision, not a research conclusion.

## Recommendation

Close the "explore the real probe" track here. The faithful method is not
recoverable from public sources and would violate scope/safety even if it
were; do not proceed to Phase B/C. If the user wants a flag anyway, scope it
as the labelled identity heuristic above and decide it on its own merits.

## Sources
- MakeMKV, "What is LibreDrive?" — https://forum.makemkv.com/forum/viewtopic.php?t=18856
- MakeMKV, "Direct access to SCSI devices on Mac OS X" — https://www.makemkv.com/osxmmc/
- LibreDrive overview — https://grokipedia.com/page/LibreDrive
- cyrozap, optical-disc-drive-re (Notes) — https://github.com/cyrozap/optical-disc-drive-re/blob/master/Notes.md
- JayFoxRox, mtk-odd-emulator — https://github.com/JayFoxRox/mtk-odd-emulator
- scanlime, coastermelt; PoC‖GTFO 7:3 — https://mcfp.felk.cvut.cz/publicDatasets/pocorgtfo/contents/articles/07-03.pdf
- LibDriveIO source-status / SDF.bin discussion — https://forum.makemkv.com/forum/viewtopic.php?t=24312
- `mos` tree: `src/mos_dr.c`, `src/mos_internal.h`, `src/mos_pure.h`, `include/mos.h`, `src/mos_scsi.c`, `src/mos_state_core.c`, `cli/common.c`, `AGENTS.md`, `ROADMAP.md`

---

## Addendum (2026-06-17): bus-encryption vs LibreDrive — why the flag stays unchanged

Revisited from a different angle than Parts 1–4 (which asked "detect LibreDrive
capability"): does LibreDrive make mos's `protection.aacs.bus_encryption` flag
*misleading*? Source: MakeMKV forum thread **t=19029** ("libredrive bypass bus
encryption?"), answered by MakeMKV's author (mike admin):

> "The moment you see 'Using LibreDrive mode', bus encryption is disabled **for
> the current disc only**. … your drive would still report that drive supports
> bus encryption as it is, **LibreDrive code does not meddle with AACS**, and
> some applications might think that sectors might need to be decrypted, while
> bus encryption was in fact not applied. The disc would be in this state until
> you eject it."

**Conclusion: no code change.** (HIGH — author's own words + the mos tree.)
- mos reports the GET CONFIGURATION AACS-feature **BEC bit** — a drive-static
  "supports bus encryption" *capability*. The author confirms LibreDrive does
  NOT change it ("does not meddle with AACS"), so the bit mos emits stays
  accurate and LibreDrive-invariant. This *vindicates* the protection ADR
  (report capability; ignore the media-dependent Current bit; never REPORT KEY).
- What LibreDrive changes is the **runtime-applied** state (encryption not
  applied for the loaded disc) — an enabled/enforced fact mos deliberately does
  not claim and structurally cannot observe (observing it = the LibreDrive
  runtime detection Parts 1–4 found infeasible / out of scope).
- Only real exposure is presentation: the human row `AACS (v68, bus encryption)`
  could be misread as "encryption active." **Maintainer decision (2026-06-17):
  leave it as-is** — anyone operating in LibreDrive territory owns that nuance;
  a generic spec-reporter does not annotate which of its honest readings a
  third-party tool might transiently invalidate, and cannot track that state.

**Two adjacent ideas reconsidered and declined the same day:**
- *Encryption-falsification probe* (issue an encryption-gated MMC command that
  fails on stock but succeeds under LibreDrive). It necessarily reduces to a
  content **sector read** or a **REPORT KEY / AACS-structure** read — both
  explicitly out of scope (`ROADMAP.md` raw-descrambled-sector-reads; `AGENTS.md`
  no-REPORT-KEY). And the clean fail-vs-succeed signal exists only for AACS2/UHD
  (CSS/AACS1 return ciphertext on a successful read — no signal) and only with
  such a disc loaded (media-dependent, not a drive flag).
- *Reverse-engineering MakeMKV's opcodes to surface the flag.* The MT1959
  opcodes are non-public (Part 4), so this means RE'ing MakeMKV's proprietary
  `LibDriveIO`/`SDF.bin` (its copyright + EULA), and a libredrive-status flag
  risks the DMCA §1201 "no commercially significant purpose other than to
  circumvent" prong even without shipping any microcode — additive legal gray
  area on top of the already-decided scope/safety "no." (Not legal advice;
  jurisdiction-dependent — counsel before any such code.)

## Sources (addendum)
- MakeMKV, "libredrive bypass bus encryption?" — https://forum.makemkv.com/forum/viewtopic.php?t=19029
