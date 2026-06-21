# `capacity` verb feasibility: READ CAPACITY, the missing convenience
# wrapper, and the cheaper paths (June 2026)

Research note for the next session. **Question:** how should the reserved
`capacity` verb (`cli/main.c`, the lone remaining reserved name after
`identity`/`features`/`tray`/`speed` retired) get disc capacity — is there an
Apple convenience wrapper for READ CAPACITY, what are the implementation
details, and what does the scope doctrine permit? **Method:** verified
against the actual `SCSITaskLib.h` header, the IOKit `IOMedia` properties, the
DiskArbitration description keys, and the T10 SBC/MMC command definitions;
cross-checked against what mos already plumbs. No `src/` or schema changes —
this file is the deliverable.

Confidence per claim: HIGH = header/spec/in-repo source quoted; MEDIUM =
inferred macOS behavior not separately confirmed on hardware — falsifiable
per the hardware-role ADR.

## Verdict

**Do not add a raw READ CAPACITY CDB. Capacity for present media is already
free from the kernel; the recordable/append-state view is already plumbed;
the one genuine gap (blank-media max formattable capacity) is the only thing
that could justify a raw verb, and it is deferrable behind a falsifier.**

1. **There is no READ CAPACITY convenience wrapper** (HIGH —
   `SCSITaskLib.h`). `MMCDeviceInterface`'s full method set is `Inquiry,
   TestUnitReady, GetPerformance, GetConfiguration, ModeSense10,
   SetWriteParametersModePage, GetTrayState, SetTrayState,
   ReadTableOfContents, ReadDiscInformation, ReadTrackInformation,
   ReadDVDStructure, GetSCSITaskDeviceInterface, GetPerformanceV2`. No
   `ReadCapacity`, no `ReadFormatCapacities`, no `GetMediaCapacity`. So a
   capacity *command* to the drive is raw-CDB-only — `ObtainExclusiveAccess`,
   the §5.5 lock — which matters below.

2. **The whole-disk byte capacity is a kernel-computed property, no command,
   no lock** (HIGH — `IOMedia.h`). The IOMedia node carries
   `kIOMediaSizeKey` (total media length in bytes, OSNumber) and
   `kIOMediaPreferredBlockSizeKey` (natural block size). The kernel's own
   block-storage stack already issued READ CAPACITY at attach and cached the
   result here. mos already resolves this exact node — `media_id` is the
   whole-disk IOMedia registry entry ID captured at open
   (`src/mos_internal.h`), and `bsd_unit` is read off it (`kIOBSDUnitKey`).
   Reading `kIOMediaSizeKey` from the same node is a registry property read in
   the same tier as the identity reads — **no SCSI command, no exclusive
   access, works on mounted media** (where a raw command cannot — point 4).

3. **The recordable / append-state capacity is already shipped** (HIGH — `mos`
   tree). `mos_query_track_info` (READ TRACK INFORMATION, a convenience
   method, no lock) exposes `track_size`, `free_blocks`, `next_writable`
   (NWA), `last_recorded` — for single-track pressed media `track_size` is
   effectively the disc capacity (`include/mos.h:562`), and `free_blocks`/NWA
   are the writable-space surface for recordable media.
   `mos_query_disc_info` (READ DISC INFORMATION) adds session/track structure.
   Neither needs a new command.

4. **A raw READ CAPACITY is the *wrong* tool, not just a redundant one**
   (HIGH — `ARCHITECTURE.md` §3). Raw means `ObtainExclusiveAccess`, which is
   `kIOReturnBusy` on a **mounted** volume — and capacity is most wanted for a
   mounted data disc ("how big is this disc I just inserted"). So a raw READ
   CAPACITY would be BUSY in exactly its primary case, while the IOMedia
   property (point 2) is *populated precisely because* the disc mounted. Raw
   READ CAPACITY also becomes the 4th authored raw CDB (GESN + the two tray
   opcodes + this), each of which the one-raw-CDB doctrine makes expensive.

**Recommendation:** build `capacity` as a *no-new-command* verb — whole-disk
bytes/blocks from `kIOMediaSizeKey`/`kIOMediaPreferredBlockSizeKey`, plus the
recordable view (`free_blocks`, NWA, `track_size`) from the
already-plumbed READ TRACK / DISC INFORMATION. Treat raw READ FORMAT
CAPACITIES as a separately-justified follow-on for the one fact the cheap
paths miss (Part 3 below), gated on a hardware falsifier.

---

## Part 1 — the command landscape (what each actually answers)

Three different T10 commands hide behind the word "capacity"; they are not
interchangeable (HIGH — SBC-3 / MMC-6, cross-checked osdev + the Toshiba
0x23 draft):

- **READ CAPACITY(10) — SBC `0x25`.** Returns the *last readable LBA* + block
  length (8-byte reply). Total bytes = (last_lba + 1) × block_len. This is the
  *readable* capacity of recorded media. On a **blank** recordable disc it is
  undefined / returns ~0 (there is no last readable block), so it answers
  "how much is recorded," not "how much fits." READ CAPACITY(16) (`0x9E`/SA
  `0x10`) is the 16-byte-LBA variant; optical never needs it (no optical disc
  exceeds 2^32 2 KB blocks).
- **READ FORMAT CAPACITIES — MMC `0x23`.** Returns formattable-capacity
  descriptors: current/maximum/formattable capacity by format type — the
  *writable* capacity, including for **blank** media where 0x25 is silent.
  This is the MMC-correct "how much fits on this blank disc" command.
- **READ DISC INFORMATION `0x51` / READ TRACK INFORMATION `0x52`** — already
  in mos. Track size, free blocks, NWA, last-recorded: the append-state /
  recordable view, per-track.

The kernel's `kIOMediaSizeKey` is the result of the kernel having run 0x25
(or the equivalent) on readable media; it is the byte total of point 2.

## Part 2 — capacity sources ranked against the scope doctrine

| source | command | lock | works mounted | blank media | doctrine |
|---|---|---|---|---|---|
| `kIOMediaSizeKey` / block size | none (registry) | none | **yes** | n/a (no node until formatted/recorded) | layer-3 clean: a property read, same tier as identity |
| READ TRACK INFORMATION | convenience `0x52` | none | yes | yes (NWA/free) | already shipped |
| READ DISC INFORMATION | convenience `0x51` | none | yes | yes (status) | already shipped |
| READ FORMAT CAPACITIES | **raw `0x23`** | **exclusive** | no (BUSY) | **yes (the gap)** | layer-1 raw verb — needs GESN-grade showing |
| READ CAPACITY(10) | **raw `0x25`** | **exclusive** | no (BUSY) | no | dominated by `kIOMediaSizeKey`; do not add |

The cheap three (IOMedia size + the two already-plumbed convenience reads)
cover every capacity question **except** the maximum formattable capacity of a
blank disc that has *not* been written at all (no IOMedia size, no track to
read). That one fact is what READ FORMAT CAPACITIES `0x23` uniquely answers.

## Part 3 — the one raw-CDB candidate, and why it waits

READ FORMAT CAPACITIES (`0x23`) is the *only* capacity command that earns
consideration as a raw verb, because it is the only one the cheap paths
cannot supply (blank-media max formattable capacity). If it is ever built it
must clear the same layer-1 bar GESN and the tray verbs cleared:

- **(a) no convenience method carries it** — HIGH, header-confirmed (Part 1):
  there is no `ReadFormatCapacities` wrapper, so it is genuinely raw-only.
- **(b) nub-collision analysis** — it is *not* gated on not-ready (unlike
  GESN), so it inherits the plain `ObtainExclusiveAccess` BUSY-on-mounted
  guard: `MOS_ERR_BUSY` on a mounted volume rather than disturbing a live
  IOMedia nub. Benign on a blank disc (nothing mounts a blank disc), which is
  exactly the case it is for.

But the motivating need is thin: "how much fits on this blank disc" is a
*burn-planning* question, and mos is a state reporter feeding rippers, not a
burn tool. The blank-vs-recordable *status* (the thing a ripper needs — "is
there anything to read") already ships via READ DISC INFORMATION's
disc-status byte (`mos.metadata.v1`). So READ FORMAT CAPACITIES is **deferred**
unless a consumer states a concrete need; if it lands, it lands as a raw verb
with the showing above + a dated fixture, never special-cased to a drive.

## Part 4 — scope & privilege (why the cheap path is doctrinally free)

- **Layer 3 (privilege footprint) is unchanged for the cheap path.** Reading
  `kIOMediaSizeKey` is an `IORegistryEntryCreateCFProperty` on a node mos
  already holds — same modality and privilege as the existing identity reads,
  no `SCSITaskUserClient`, no exclusive access, no entitlement. (The
  DiskArbitration alternative — `kDADiskDescriptionMediaSizeKey` /
  `kDADiskDescriptionMediaBlockSizeKey` — is the *same fact* via the DA
  description mos already reads for the mounted-volume lookup, `src/mos_da.c`;
  either source is doctrine-clean. Prefer IOMedia: it needs no media to be
  *mounted*, only *present*, whereas the DA description's media keys want a
  recognized disk. MEDIUM: confirm on hardware which keys DA populates for an
  unmounted-but-present optical disc.)
- **Layer 1 (command surface) is untouched by the cheap path** — it issues no
  CDB at all, so the one-raw-CDB count stays at one-of-three (GESN + two tray).

## Part 5 — what hardware can falsify (not establish)

Per the hardware-role ADR, the rig breaks the derivation; it never blesses it.
Falsifiable predictions, each landing as a fixture + dated note with a generic
defense if it fails:

1. **`kIOMediaSizeKey` is populated for optical media** (MEDIUM). Spec-expected
   yes for any disc the kernel mounted or recognized as a block device; a
   drive/bridge that presents an optical disc without a whole-disk IOMedia size
   would force the track-info fallback. (mos already treats "no whole-disk
   IOMedia node" as `bsd_unit = -1` / media absent, so the gate exists.)
2. **`kIOMediaPreferredBlockSizeKey` is 2048 for data optical** (MEDIUM) —
   needed to convert bytes↔blocks coherently; audio CD (2352) and mixed media
   are the edge cases to capture.
3. **READ CAPACITY(10) raw would BUSY on mounted optical** (HIGH by
   construction — `ObtainExclusiveAccess` on a mounted volume is
   `kIOReturnBusy`, §3) — this is the *reason* not to build it, not a risk to
   test.

## Part 6 — concrete surface sketch (next-session pickup)

A `capacity` verb / `mos.capacity.v1` that issues no new command:

- **Source:** add `mos_query_capacity(h, const mos_capacity **out)` filling a
  struct from (a) `kIOMediaSizeKey` + `kIOMediaPreferredBlockSizeKey` off the
  whole-disk IOMedia node mos already resolves (`media_id`), and (b) the
  already-decoded `mos_track_info` (`free_blocks`, `track_size`, NWA,
  `nwa_valid`). All non-fatal/nullable — a blank disc has no IOMedia size, a
  pressed disc has no free blocks.
- **Fields (sketch):** `media_bytes` (nullable), `block_bytes` (nullable),
  `media_blocks` (derived), plus the recordable view `free_blocks`,
  `next_writable_address` (gated on `nwa_valid`), `track_size`. Closed set,
  `additionalProperties:false`, positive + negative fixtures + the validate.py
  registration, same discipline as `mos.drive.v1`.
- **No exclusive access, no new raw CDB**, so no §5.5 / nub-collision work and
  no AGENTS controller/raw-verb ADR — it is a reporter read like `metadata`.
- **Headless-testable:** the IOMedia property read goes through the fake's
  registry (the adapter-fake harness already scripts IOMedia identity), and
  the track-info decode is already pure-tested; a `capacity` emit scenario
  joins the emit-fixtures harness like `drive`.
- **Pickup checklist:** (1) confirm the fake can script `kIOMediaSizeKey`;
  (2) `mos_query_capacity` + accessors (append-only); (3) `mos.capacity.v1`
  schema + fixtures; (4) `cli/capacity.c`, de-reserve in `main.c` (and drop
  `capacity` from the `test_cli.sh` reserved loop — it becomes the empty set,
  so retire the loop or leave a guard); (5) leave READ FORMAT CAPACITIES for a
  stated blank-media need, with the Part 3 showing.

## Sources

- **Apple `SCSITaskLib.h`** (MMCDeviceInterface method list, no ReadCapacity):
  phracker MacOSX-SDKs mirror, MacOSX10.2.8.sdk
  `IOKit.framework/.../scsi-commands/SCSITaskLib.h`. Cross-checked against
  `ARCHITECTURE.md:834` (the repo's convenience-method enumeration) and
  `:895` (GetPerformance/V2 only).
- **Apple `IOMedia.h`** — `kIOMediaSizeKey` (total media length, bytes),
  `kIOMediaPreferredBlockSizeKey` (natural block size): phracker
  `.../storage/IOMedia.h`; Apple "IOMedia.h User-Space" reference.
- **DiskArbitration `DADisk.h`** — `kDADiskDescriptionMediaSizeKey`,
  `kDADiskDescriptionMediaBlockSizeKey` (same fact via the DA description mos
  already reads, `src/mos_da.c`).
- **T10:** SBC-3 READ CAPACITY(10) `0x25` (last LBA + block length) and
  READ CAPACITY(16); MMC-6 READ FORMAT CAPACITIES `0x23` (formattable-capacity
  descriptors, blank-media capacity) — Toshiba `0x23` draft
  (mikrocontroller.net mirror), osdev.org Optical Drive.
- **`mos` tree:** `include/mos.h` (`mos_query_track_info` / capacity-adjacent
  accessors, `:550-586`); `src/mos_internal.h` (`media_id` = whole-disk
  IOMedia entry ID, `bsd_unit` off `kIOBSDUnitKey`); `src/mos_da.c` (the
  narrow DA re-admission); `AGENTS.md` (scope doctrine layers 1/3, one-raw-CDB
  rule, hardware-role ADR); `ROADMAP.md` (capacity = the lone reserved verb).
