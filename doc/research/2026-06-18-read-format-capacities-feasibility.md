# READ FORMAT CAPACITIES (0x23) — formattable capacity for blank rewritable media

**Date:** 2026-06-18. **Status:** research / decision note. New capability
candidate surfaced during a feature-exploration pass; no behavior change
here. Per the README append rule this is a new dated file; nothing prior is
edited.

## The question

`mos capacity` (`mos.capacity.v1`, cli/capacity.c) assembles a disc's size
from two no-raw-CDB sources: the whole-disk IOMedia node's kernel-cached
byte/block size (`kIOMediaSizeKey`, works on mounted media) and the
recordable/append view from READ TRACK INFORMATION. Both have a known blind
spot the accessor docs already name: on **blank or absent media the IOMedia
size is 0** (`mos.h`, `mos_capacity_have_media_size` contract), and a blank
disc has no readable track for the recordable view either. So for a *freshly
blank rewritable* — DVD±RW, DVD-RAM, BD-RE — `mos capacity` can report
little: the kernel never ran READ CAPACITY against unformatted/blank media,
and there is no track to interrogate.

The disc still has a well-defined *formattable* capacity, and the spec
command that reports it is **READ FORMAT CAPACITIES (0x23)**: a Current/
Maximum Capacity descriptor plus a Formattable Capacity Descriptor list (the
capacities the drive could format the medium to). The question:

> Is the formattable-capacity gap on blank rewritable media fillable, and at
> what cost — a convenience method (no lock), or a raw CDB under the
> AGENTS scope-doctrine layer-1 rule?

## Header finding: no convenience method — it is a raw verb

`ARCHITECTURE.md:870-873` enumerates the command-issuing `MMCDeviceInterface`
convenience methods: `Inquiry`, `TestUnitReady`, `GetConfiguration`,
`ModeSense10`, `ReadTableOfContents`, `ReadDiscInformation`,
`ReadTrackInformation`, `ReadDVDStructure`, `GetPerformance` (plus
`GetTrayState`/`SetTrayState` and `GetPerformanceV2`). The list is framed as
complete ("Every other command-issuing convenience method ... carries
`SCSITaskStatus *` + `SCSI_Sense_Data *`"). **There is no
`ReadFormatCapacities` / `GetFormatCapacities` wrapper.** This is the same
absence shape the read-capacity note (2026-06-13) and the serial note
(2026-06-16) established: a real MMC command with no convenience surface on
macOS.

Confirm-at-implementation discipline (as the serial note did): the
authoritative check is `SCSITaskLib.h` itself — the §11 public mirror, which
ARCHITECTURE records as signature-identical to the modern SDK for existing
methods. If a future SDK *adds* a format-capacities wrapper, this finding is
superseded and the command becomes a no-lock convenience read; until then,
the inventory shows none, so READ FORMAT CAPACITIES is a **raw CDB**.

No cheap no-command path carries it either: the IOMedia node only has what
the kernel's attach-time READ CAPACITY found (zero on blank media — the whole
gap), and DiscRecording's device/status dictionaries report media *type* and
status, not a formattable-capacity descriptor list.

## Layer-1 raw-verb showings

A raw CDB requires two showings (AGENTS scope doctrine, layer 1):

- **(a) No convenience / zero-command path carries it — SATISFIED** by the
  header inventory above. Same class of proof as the serial (missing VPD
  method) and read-capacity (missing READ CAPACITY wrapper) findings — an
  absence in the published interface, not a hardware observation.

- **(b) Nub-collision / exclusive-access — straightforward, benign
  degradation.** Raw means `mos_raw_cdb`, the single `ObtainExclusiveAccess`
  call site (ARCHITECTURE §3), which returns BUSY on mounted media. READ
  FORMAT CAPACITIES is a **non-destructive read** (it reports capacities; it
  is FORMAT UNIT, 0x04, that would write — explicitly out of scope, mos
  reports and never formats). It touches no IOMedia nub and creates no §5.5
  interleaving exposure. The consequence mirrors the serial: unreadable while
  a disc is mounted (BUSY → the field stays null) — and that is the *correct*
  degradation here, because the target case is a **blank** rewritable, which
  by definition is not mounted as a volume. The natural moment to ask "what
  can this blank disc be formatted to" is exactly when nothing holds the
  drive. No state change, so no lock-lifetime question (none of the tray-verb
  PREVENT machinery applies).

If built, the one-raw-CDB count goes **one-of-four → one-of-five** (GESN +
two tray opcodes + INQUIRY + READ FORMAT CAPACITIES), with `mos_raw_cdb`
still the sole exclusive-access site.

### Note on the v0.4 "drop raw_cdb" item

ROADMAP v0.4 removes the *public* `mos_raw_cdb()` passthrough. That is a
different axis from the *internal* raw-CDB mechanism every typed verb already
rides: GESN, the tray opcodes, and INQUIRY all author raw CDBs through the
internal path and would survive the passthrough removal. A fifth internal raw
verb does not conflict with dropping the public passthrough — but it does add
raw surface right as the project is trying to shrink it, which is part of the
scope cost weighed below.

## Implementation shape (if pursued — not built here)

Per Process rule 2 and the hardware-role ADR this note records the design; it
changes nothing.

- **CDB (10-byte):** `23 00 00 00 00 00 00 AA AA 00` — opcode 0x23, bytes 7-8
  ALLOCATION LENGTH (big-endian; the Capacity List header is 4 bytes + 8
  bytes per descriptor, so 0xFF is ample), byte 9 CONTROL=0. Issued through
  `mos_raw_cdb(... MOS_XFER_FROM_TARGET ...)`.

- **Pure parser** (`src/mos_formatcap.c`, matching the bounded/hostile-input
  decoder pattern, fuzzed against the full octet domain):
  - Byte 3 is the Capacity List Length; **bound it by the actual transfer
    count** (the repo's dual-length rule O-4 — trust the reply's own length
    only up to bytes returned).
  - Bytes 4-11 are the Current/Maximum Capacity Descriptor: bytes 4-7 Number
    of Blocks (big-endian), byte 8 bits 1:0 Descriptor Type (1 unformatted,
    2 formatted, 3 no media), bytes 9-11 Block Length.
  - Bytes 12.. are the Formattable Capacity Descriptors (8 bytes each); decode
    the count from the list length, surface each descriptor's block count and
    block length. Reject a list length that is not 8 + 8·k.
  - Confirm exact offsets against MMC-6 §6.24 (READ FORMAT CAPACITIES) at
    implementation and add the citation to `SPEC.md` under the new
    `src/mos_formatcap.c` entry; parsed offsets stay inline per the SPEC.md
    ADR.

- **Surface decision (open):** fold into `mos.capacity.v1` as a nullable
  `formattable` block (the current/max descriptor + the formattable list),
  null when the command returns BUSY/IO or the medium is not formattable.
  Pre-first-tag this is a mutable-in-place schema edit (JSON-schema ADR):
  schema + example + negatives + emitter + README + SPEC.md in one commit.
  Alternative — a distinct `mos format-capacities` verb — is heavier and
  probably unwarranted for one descriptor list; capacity is its natural home.

- **Tests** (`tests/`, pure, no IOKit): a positive fixture (a blank BD-RE /
  DVD+RW format-capacities reply) plus negatives — list length not a multiple
  of 8 after the header, list length overrunning the transfer count, "no
  media" descriptor type (3), zero descriptors. Link `mos_pure` only.

## What hardware can falsify, never establish

Per the hardware-role ADR, a run can refute the design, never steer it. Each
candidate lands as a fixture + dated note with a generic defense:

- A read-only drive or a non-rewritable medium answers 5/20/00 (invalid
  opcode) or returns only a "no media"/formatted descriptor — classified to a
  null/empty formattable list, expected not a defect.
- A USB-SATA bridge that synthesizes a truncated or mis-sized capacity list —
  caught by the dual-length bound and the 8-byte-multiple gate, no special
  case.
- A drive reporting a Maximum Capacity that disagrees with the IOMedia size on
  formatted media — surfaced faithfully; mos reports both, reconciliation is
  the consumer's call (same division as everywhere else).

## Decision / recommendation

Feasibility is settled: **READ FORMAT CAPACITIES is a raw CDB (no convenience
path), with both layer-1 showings satisfiable from the header and a benign
BUSY-on-mounted degradation.** Whether to *build* it is a scope call:

- **For:** it closes a named, real gap — `mos capacity` is near-silent on
  blank rewritable media, and formattable capacity is the spec answer.
  Blank-RW is squarely in the rip/archival workflow mos serves (BD-RE
  reuse, DVD-RAM).
- **Against / scope:** it is the **fifth** raw verb, added while v0.4 is
  shrinking raw surface; its only consumer is "how big can I format this
  blank rewritable," a narrower ask than the existing capacity fields; and the
  graceful-null on mounted media means it contributes nothing on the common
  (mounted, readable) case.

Recommendation: **note-only; build when a consumer needs formattable capacity
for blank rewritable media.** The expensive unknown (convenience vs raw, and
the nub analysis) is now answered, so the build is a bounded, spec-grounded
task whenever wanted. Pickup checklist: CDB above → `src/mos_formatcap.c`
pure parser + SPEC.md entry → negative fixtures → `mos_query_*` accessor →
`mos.capacity.v1` `formattable` block (schema + examples + negatives +
emitter) → README/capacity note on the null-on-mounted behavior.
