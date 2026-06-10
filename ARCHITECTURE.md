# ARCHITECTURE.md

## 1. Scope and non-goals

This document is the single source of truth for what the library does
and does not do. Three categories of claim appear here and they are
labeled throughout:

- **Guaranteed:** behavior documented in Apple headers, T10 specs, or
  the MMC-6 draft. Should hold on any conformant drive and any
  conformant macOS.
- **Inferred:** behavior we expect from reading protocol specs and
  Linux kernel source, but have not separately validated against
  Apple's specific implementation.
- **Hardware-observed:** behavior that requires empirical testing on
  actual drives. Some of this is recorded in third-party projects
  (libcdio, MakeMKV) and is reasonably trustworthy; the rest is still
  to-be-verified. See §9 Known Unknowns.

mos is a state *reporter*, not a state *controller*. The library
answers "what is the drive doing right now" and stops there. It does
not auto-close trays, auto-eject on close, lock doors on open, or
mount discs on insertion. The application consuming the library decides
what to do about reported state.

This mirrors the cdrom.c autoclose/autoeject debate that ran across
the 2.x kernel series: after roughly a decade of accumulated misery
where drivers each made their own opinionated choices about tray
management, the kernel settled on exposing those as user-controllable
options (`autoclose`, `autoeject`, `lockdoor` module parameters) with
all of them defaulting to "don't act unless asked." Restraint is a
feature. mos applies the same principle one layer up: even the
choice to expose tray control is deferred to v0.3, separately from
the query path, because the v0.2 reporter-only contract is
genuinely useful by itself and adding control verbs introduces a
different class of failure mode (locked drives, surprised users,
cleanup-on-process-death obligations). The decided surface, when it lands, is
`mos tray {eject, close, lock, unlock}` (`eject --force` = unlock-then-eject).
Whether each verb is issued through a no-exclusive-access MMC convenience
method or a raw CDB that needs exclusive access — and is therefore BUSY on a
mounted disc — is an open implementation question (§9.9), not settled here.
The contract states mechanism facts only — no application or safety
editorialising; the consumer derives suitability.

mos's vocabulary is *drive-handling state* — `OPEN / EMPTY /
LOADING / READY / BUSY`, plus the finer not-ready distinctions
`FORMATTING / MEDIA_UNREADABLE / DEVICE_FAULT / EMPTY_OR_OPEN` — which
answers "what is the
mechanical and exclusive-access situation of the drive right now."
This is deliberately a different vocabulary from systemd's
`cdrom_id`, which uses `BLANK / APPENDABLE / COMPLETE / OTHER` to
report *media-content state*: what's on the disc, not what the
drive is doing with it. Both are valid views of the same hardware
and answer different questions. A consuming application needs to know
whether the drive is ready to read (drive-state question, mos's answer); a
burning tool needs to know whether the disc has free space (media-content
question, cdrom_id's answer). Future contributors should not try
to merge the vocabularies — they're at different abstraction
levels by design. mos may eventually grow optional media-content
fields in `mos_state_result` (v0.3 ROADMAP item), but those will
be additive enrichment, not state-machine replacements. The first such
enrichment is a watch `media_changed` event, fingerprinted on the whole-disk
`IOMedia` registry entry ID (a physical swap mints a new ID even at an
unchanged profile, catching same-profile swaps), emitted additively in
`mos.event.v1` — never on the one-shot state path.

## 2. Why this project exists

`drutil status` on modern macOS reports mount state via DiskArbitration,
not drive state. A tray that has just been extended manually and a tray
that was closed on an empty slot both surface as "no media" — same
string, same exit code.

The drive itself knows the difference. MMC exposes it two independent
ways:

1. **Guaranteed.** `GET EVENT STATUS NOTIFICATION` (opcode `0x4A`) with
   the Media notification class returns a Media Status byte whose bit 0
   is `DoorOpen` and bit 1 is `MediaPresent`. T10 MMC-6 §6.8, mirrored
   in Linux `drivers/scsi/sr.c` as `media_event_desc`.
2. **Guaranteed.** `TEST UNIT READY` returning `CHECK CONDITION` with
   sense key `0x02 NOT READY` and ASC/ASCQ `3A/01` vs `3A/02` directly
   encodes *"MEDIUM NOT PRESENT - TRAY CLOSED"* vs *"MEDIUM NOT PRESENT -
   TRAY OPEN"*. T10 ASC/ASCQ list, entry `3A/01` and `3A/02`.

`drutil` throws both of those bits away. This tool surfaces them.

## 3. API choice: convenience for presence, raw GESN for the tray bit

The v0.1 skeleton made the opposite mistake in each direction; the v0.3
design splits the two surfaces by what each is trustworthy for.

**Guaranteed (from `SCSITaskLib.h`):** `MMCDeviceInterface` exposes
convenience methods — `GetTrayState`, `TestUnitReady`, `GetConfiguration`,
`ReadDiscInformation`, `Inquiry` — that do **not** require exclusive access.
Raw `SCSITaskDeviceInterface::CreateSCSITask` + `ExecuteTaskSync`, by
contrast, lives behind `ObtainExclusiveAccess`, which **returns
`kIOReturnBusy` if the media is still mounted**.

The design uses both, gated by what we need and when:

- **Presence — convenience `TestUnitReady`, non-exclusive, issued once.**
  This is the trusted primary: "do I have a disc, am I ready?" A `GOOD`
  status answers the whole question (closed + present + ready) and
  short-circuits to `READY` **without ever taking a lock** — so a query
  never disturbs a mounted, in-progress rip. Nothing on this path calls
  `ObtainExclusiveAccess`.

- **Tray open/closed — raw `GET EVENT STATUS NOTIFICATION` (0x4A), under
  exclusive access, only when TUR is not ready.** We deliberately do **not**
  use the `GetTrayState` convenience method: it is a wrapper around GESN that
  hard-codes `kMMCDeviceTrayClosed` + success when the underlying GESN fails
  (§4.2, §9.7), masking a failure as a confident "closed." Issuing the CDB
  ourselves keeps a failure honest, so the decision tree can fork on the TUR
  sense instead of trusting a fabricated verdict.

  Taking exclusive access here is safe for the very reason it would be fatal
  on a mounted disc: we reach this path **only when TUR reported not-ready**,
  which means no media is mounted, so `ObtainExclusiveAccess` does not hit the
  `kIOReturnBusy` mounted-volume case. `mos_raw_cdb()` acquires and releases
  the lock per call — never held for the handle's lifetime.

- **Diagnostic raw-CDB path:** `mos_raw_cdb()` (public C API; on the
  deprecation path for v0.4, see ROADMAP) gives embedders arbitrary CDB
  injection through the raw `SCSITaskDeviceInterface`; returns
  `MOS_ERR_BUSY` / `MOS_ERR_EXCLUSIVE_ACCESS` if something else holds the
  drive. There is no CLI flag for it — the CLI surface is derived state
  only (§10).

`mos_raw_cdb()` in `mos_scsi.c` is the *only* place `ObtainExclusiveAccess`
is ever called — both the tray-bit GESN and caller-issued diagnostics route
through it.

## 4. The MMC commands we issue (three on the state path)

Byte-exact CDB layouts. All fields are big-endian unless noted.

**Reading note:** The default query path builds only one of these CDBs
by hand — the GESN tray probe (§4.2), issued raw through `mos_raw_cdb()`
precisely because the `GetTrayState` convenience wrapper masks failure
(§3, §9.7). The rest go through `MMCDeviceInterface` convenience methods
(`TestUnitReady`, `GetConfiguration`) which are wired into the
kernel SCSI stack and issue equivalent CDBs on our behalf. (INQUIRY
retired 2026-06-10 with the DR pivot: identity strings come from the
DiscRecording directory — `DRDeviceCopyInfo`, the same INQUIRY bytes
pre-parsed by the framework — so mos no longer issues it; the layout
knowledge lives on in §6's identity-width notes.) We document
all the byte layouts here because:

1. They are part of the protocol contract. When the drive returns a
   specific sense key / ASC / ASCQ, those codes correspond to these
   commands regardless of whether we issued them directly or via a
   convenience wrapper.
2. Readers auditing the library need to see what wire format we expect
   in each direction, especially since `mos_raw_cdb()` lets callers
   issue these CDBs by hand for diagnostic purposes.

For the actual runtime call sites, see `mos_scsi.c` — the MMC wrappers
live there.

### 4.1 TEST UNIT READY (SPC-4 §6.47)

6-byte CDB, no data phase (issued via the convenience wrapper; timeout
is the kernel's, §8).

```
byte 0  0x00  opcode
byte 1  0x00  reserved
byte 2  0x00  reserved
byte 3  0x00  reserved
byte 4  0x00  reserved
byte 5  0x00  control
```

Success returns `GOOD`. `CHECK CONDITION` triggers sense parsing.

Issued **once** per query, like the macOS peers — we do not drain
`UNIT ATTENTION`. By the time mos holds a handle, the kernel's own device
initialization has already consumed the power-on / reset / media-change UA,
so a single TUR sees a settled drive. A stray UA reply is taken at face
value (it classifies to `unknown`, §5.4), not retried.

### 4.2 GET EVENT STATUS NOTIFICATION (MMC-6, opcode 0x4A)

10-byte CDB, data-in 8 bytes, timeout 2000 ms. IMMED=1 (Polled mode).
Notification class bitmap byte 4 = `0x10` (Media only).

```
byte 0  0x4A  opcode
byte 1  0x01  bit 0 IMMED = 1
byte 2  0x00  reserved
byte 3  0x00  reserved
byte 4  0x10  Notification Class Request (Media bit)
byte 5  0x00
byte 6  0x00
byte 7  0x00  Allocation Length MSB
byte 8  0x08  Allocation Length LSB
byte 9  0x00  control
```

Response (8 bytes):

```
Header (4 bytes)
  byte 0-1   Event Data Length (big-endian, excludes itself)
  byte 2     bit 7 NEA (No Event Available), bits 2:0 Notification Class
  byte 3     Supported Event Classes bitmap

Media Event Descriptor (4 bytes, when class == 4)
  byte 0     bits 3:0 Media Event Code
               0 NoChange, 1 EjectRequested, 2 NewMedia,
               3 MediaRemoval, 4 MediaChanged,
               5 BGFormatCompleted, 6 BGFormatRestarted
  byte 1     Media Status byte
               bit 0 DoorOpen     (1 = open)
               bit 1 MediaPresent (1 = media present)
  byte 2-3   reserved (changer Start/End Slot)
```

**Authoritative citation for bit positions:** Linux
[`drivers/scsi/sr.c`](https://github.com/torvalds/linux/blob/master/drivers/scsi/sr.c)
field `media_present` / `door_open` in `media_event_desc`.

Note: this CDB **is** the default tray probe (since v0.3) — the
`get_tray_state` vtable op issues it through `mos_raw_cdb()` on the not-ready
path and decodes the door bit with the pure `mos_internal_gesn_media_door_open`
(NEA gate, Media-class check, full-span reject). We do **not** call the
`GetTrayState` MMC convenience method: it masks a GESN failure as a confident
"closed" (§3, §9.7). The decoder returning "no authoritative bit" makes the
decision tree fork on the TUR sense instead.

### 4.3 GET CONFIGURATION (MMC-6, opcode 0x46)

10-byte CDB, data-in 16 bytes (issued via the convenience wrapper;
timeout is the kernel's, §8). RT=10b (only the
feature specified), Starting Feature = 0x0000 (Profile List).

```
byte 0  0x46  opcode
byte 1  0x02  RT = 10b
byte 2-3 0x0000  Starting Feature Number
byte 4-6 reserved
byte 7-8 0x0010  Allocation Length
byte 9  0x00  control
```

Response header (8 bytes):

```
byte 0-3   Data Length
byte 4-5   reserved
byte 6-7   Current Profile  <-- what we want
```

Profile table (values a CD / DVD / BD drive can return):

| Value   | Media |
|---------|-------|
| 0x0000  | No current profile |
| 0x0008  | CD-ROM |
| 0x0009  | CD-R   |
| 0x000A  | CD-RW  |
| 0x0010  | DVD-ROM |
| 0x0011  | DVD-R Sequential |
| 0x0012  | DVD-RAM |
| 0x0013  | DVD-RW Restricted Overwrite |
| 0x0014  | DVD-RW Sequential |
| 0x0015  | DVD-R Dual-Layer Sequential |
| 0x0016  | DVD-R Dual-Layer Jump |
| 0x0017  | DVD-RW Dual-Layer |
| 0x001A  | DVD+RW |
| 0x001B  | DVD+R  |
| 0x002A  | DVD+RW Dual-Layer |
| 0x002B  | DVD+R Dual-Layer |
| 0x0040  | BD-ROM |
| 0x0041  | BD-R SRM |
| 0x0042  | BD-R RRM |
| 0x0043  | BD-RE |
| 0x0050  | HD DVD-ROM |
| 0x0051  | HD DVD-R   |
| 0x0052  | HD DVD-RAM |

**Inferred:** `0x0000` means no disc. In practice some drives
occasionally return stale values from the previous disc; we always
corroborate with GET EVENT STATUS NOTIFICATION or TEST UNIT READY
before believing a profile value means anything about current state.

### 4.4 READ DISC INFORMATION (MMC-6, opcode 0x51 — on-demand typed
### API since v0.4: mos_query_disc_info)

10-byte CDB, data-in 34 bytes. Not issued on the
default state path; reserved for the v0.4 typed APIs (a planned
`mos_disc_info` accessor) to surface session/track-count enrichment.
Does **not** affect state classification.

```
byte 0   0x51  opcode
byte 1   0x00  Data Type = 0 (Standard Disc Information)
byte 2-6 reserved
byte 7-8 0x0022  Allocation Length (34)
byte 9   0x00  control
```

Response byte 2 encodes:

```
bits 1:0  Disc Status
            00b Empty Disc
            01b Incomplete / appendable
            10b Finalized
            11b Random Access / other
bits 3:2  State of Last Session (same encoding)
bit  4    Erasable
```

## 5. Sense parsing and decision tree

### 5.1 Sense formats

**Guaranteed (SPC-4 §4.5):** SCSI returns sense data in one of two
formats, distinguished by byte 0's response code:

| Byte 0 | Format      | Key at | ASC at | ASCQ at |
|--------|-------------|--------|--------|---------|
| 0x70   | fixed, current | [2]    | [12]   | [13]    |
| 0x71   | fixed, deferred | [2]    | [12]   | [13]    |
| 0x72   | descriptor, current | [1]    | [2]    | [3]     |
| 0x73   | descriptor, deferred | [1]    | [2]    | [3]     |

**Hardware-observed:** optical drives universally return fixed format.
Descriptor format appears on enterprise arrays and tape. We handle both
for correctness.

### 5.2 Sense keys (we only care about a few)

| Value | Name | Relevance |
|-------|------|-----------|
| 0x00  | NO SENSE | No error |
| 0x02  | NOT READY | Tray / media / spin-up situations |
| 0x03  | MEDIUM ERROR | Disc loaded but unreadable → media_unreadable |
| 0x04  | HARDWARE ERROR | Drive fault → device_fault |
| 0x05  | ILLEGAL REQUEST | **From the drive**, not from IOKit |
| 0x06  | UNIT ATTENTION | Media changed / reset — **not** drained; classifies to unknown |

**Correction to folklore:** sense key `0x05 ILLEGAL REQUEST` is issued
by the **drive firmware**, not synthesized by the OS. It appears in
MakeMKV logs going back to OS X 10.6 and is tied to region / AACS /
disc-condition issues. Claims that modern macOS IOKit hardening
generates `ILLEGAL REQUEST` by rejecting CDBs are unsupported by any
primary source; assume any 0x05 you see came from the drive.

### 5.3 ASC/ASCQ table — the closed-branch classifier

This table is consumed by `mos_internal_state_from_sense_closed()` **after**
the tray's open/closed verdict is already settled (by the GESN door bit, or
by the sense fork when GESN is silent — §5.4). It therefore never decides the
tray: it refines a known-closed, not-ready drive into the *reason*. A `3A/02`
("tray open") reaching it is treated as `empty`, because GESN already said
closed and the qualifier's tray hint is discarded — **enrich, don't
invalidate.**

| Sense | Meaning | Maps to state |
|----------|---------|---------------|
| key 0x04 (any) | Hardware error (outranks medium/not-ready detail) | device_fault |
| key 0x03, or 57/00 | Medium error / unable to recover TOC | media_unreadable |
| 3A/xx (any ASCQ) | Medium not present, tray closed | empty |
| 04/01    | Becoming ready                              | loading |
| 04/02    | Initialize command required (tray closed ⇒ present, stopped) | loading |
| 04/07    | Operation in progress                       | loading |
| 04/04    | Format in progress                          | formatting |
| 04/08    | Long write in progress                      | busy    |
| 28/00, 29/00 | Not-ready→ready / power-on UA (not drained) | unknown |
| anything else | Not decisively classifiable                | unknown |

Open/closed itself is **not** in this table — it is the GESN door bit (§4.2).
Only when GESN is silent does the sense supply the tray fork, and then just
the `3A` qualifier: `3A/02`→open, `3A/01`→closed, generic `3A/00`→the honest
union `empty_or_open`.

Source: T10 ASC/ASCQ public list
https://www.t10.org/lists/asc-num.htm; bit positions for the GESN media
descriptor from Linux `drivers/scsi/sr.c`.

### 5.4 Decision tree

Implemented in `mos_state_core.c`. Convenience TUR is trusted for presence
and runs first without a lock; the raw GESN tray bit is the secondary tool,
reached only when TUR is not ready.

```
1. (at open time) vendor / product / revision strings come from the
   DiscRecording directory (DR pivot 2026-06-10) — no INQUIRY issued.

2. TestUnitReady  (convenience, non-exclusive, ONE shot)
      transport EXCLUSIVE/BUSY      → BUSY  (another client holds the
                                             drive; that IS the answer)
      other transport error         → return negative mos_error (COMMS_FAIL)
      status == GOOD              → READY        (closed + present + ready;
                                                   no lock taken, no GESN)
      status is SAM-5 contended   → BUSY
      status == CHECK CONDITION   → parse sense (sk, asc, ascq); continue
      else / no usable sense      → UNKNOWN

3. Not ready ⇒ not mounted ⇒ the lock is free. get_tray_state:
      raw GESN (0x4A) under exclusive access (mos_raw_cdb)
      MOS_OK   → door bit is AUTHORITATIVE (open/closed)
      error    → no bit; fork on the TUR sense:
                    3A/02 → open
                    3A/01 → closed
                    3A/00 → EMPTY_OR_OPEN   (no medium, tray unobservable)
                    non-3A → closed         (disc engaged)

4. Fork on the tray verdict:
      open    → OPEN
      closed  → mos_internal_state_from_sense_closed(sk, asc, ascq)
                    → EMPTY / LOADING / FORMATTING / MEDIA_UNREADABLE
                      / DEVICE_FAULT / BUSY / UNKNOWN   (§5.3)

5. GetConfiguration  (enrichment only, READY only, never changes state)
      populate current_profile

6. (v0.4 typed APIs, not yet implemented) ReadDiscInformation
      populate disc_status enum
```

**The load-bearing invariant:** GESN owns open/closed; the sense only
*enriches* within that verdict — it never flips it. A `3A/02` in hand cannot
turn a GESN "closed" into `open` (stays `empty`), and a `3A/01` cannot turn a
GESN "open" into `closed` (stays `open`). The sense becomes the tray fork
*only* when GESN gave no authoritative bit. The old `04/02` ambiguity
(open-tray vs. present-but-stopped) dissolves naturally: GESN settles the
tray, so a closed `04/02` is unambiguously `loading`.

**Three independent axes, not one.** An earlier draft conflated *media
present* with *ready*. Tray position, unit-ready, and media-presence are
separate and can be in any combination. `READY` means the unit reports ready
— the strongest guarantee a non-destructive query yields. Presence is TUR's
(`GOOD`, or `3A` = no medium); the tray is GESN's; the not-ready reason is the
sense's.

### 5.5 The nub invariant — why the not-ready lock cannot collide with a mounted volume

§3's safety argument ("not-ready ⇒ not mounted ⇒ the lock is free") is
stronger than an inference — it is the kernel's own predicate, verified
against `IOSCSIMultimediaCommandsDevice::PollForMedia`
(apple-oss-distributions/IOSCSIArchitectureModelFamily):

**Kernel side — the flag and the decision are different predicates.**
While no media nub exists, the kext polls with TUR. The intermediate
flag `mediaFound` is set in two cases: TUR completes without CHECK
CONDITION, or CHECK CONDITION with ASC/ASCQ `00/00` (the `00/00` test
runs before, and independent of, the sense-key dispatch —
`IOSCSIMultimediaCommandsDevice.cpp:3890-3894`). But the flag is not
the nub decision: whenever the sense-key dispatch sets
`shouldEjectMedia`, the eject block **resets `mediaFound` to false**
(4012-4029) before the nub-creation bail (`require_quiet` at 4052). The
kernel's real nub predicate is therefore *flag minus eject*: at
`00/00`, keys `{NOT_READY, MEDIUM_ERROR, HARDWARE_ERROR, BLANK_CHECK}`
get ejected (the NOT_READY keep-list cannot match `00/00`; BLANK CHECK
keeps only `64/00`), and every **other** key — RECOVERED ERROR, ILLEGAL
REQUEST, UNIT ATTENTION, DATA PROTECT, the reserved keys — falls
through the dispatch's `default` arm with the nub intact. An earlier
revision of this section described the flag and called it the nub
predicate; the seam-fidelity audit's exhaustive checker found the
difference the hard way (11 inputs).

**mos side.** The gate in `mos_state_core.c` mirrors the kernel's
*full* predicate, eject reset included: a CHECK CONDITION with ASC/ASCQ
`00/00` routes to `UNKNOWN` without the lock for every
nub-preserving key, and proceeds to the lock only for the four keys the
kernel ejects at `00/00` — where no nub can survive and the GESN probe
is what distinguishes `device_fault` from `UNKNOWN`. GOOD
short-circuits to `READY`; contended statuses to `BUSY`; all before the
tray probe.

**Mechanically proven, not argued.** The equivalence is closed by
exhaustive enumeration: `tests/audit/nub_invariant_check.c` (authored
by the fourth external reviewer, vendored with its mos-side predicate
updated to the fixed gate) drives the real `mos_internal_query_state_core`
with an instrumented ops table and compares against an independent
kernel predicate transcribed from `PollForMedia`, over the full SCSI
status × sense key × ASC × ASCQ domain — 268,435,456 inputs, no
sampling, with a self-audit that fails loudly if the transcription
drifts from the real core. The dangerous quadrant (kernel keeps nub ∧
mos takes lock) is empty over the entire domain; CI runs the identical
path with a restricted status loop. Whenever mos takes the lock, the
kernel has not created the nub — so there is no mounted volume for
`ObtainExclusiveAccess` to collide with, on the loading / formatting /
TOC-error / medium-error / hardware-error branches just as on
empty-vs-open, and at the watch's poll rate just as on a one-shot.
Field corroboration of why the gate matters: dvd+rw-tools documents
that acquiring exclusive access while a nub exists temporarily removes
the BSD block device and its `/dev` entries — grabbing the lock under a
nub is destructive, which is precisely what this invariant prevents.

**Kernel auto-eject corollary (consumers should know this).** For every
not-ready sense outside the kernel's keep list — which is exactly
`{04/00, 04/01, 3A/xx, 57/00, 04/04}` — plus MEDIUM ERROR, HARDWARE
ERROR, and BLANK CHECK other than `64/00`, `PollForMedia` issues a
START STOP UNIT **eject** itself. On Apple's stack, therefore,
`media_unreadable` and `device_fault` are *transient in practice*
(expect a flicker into `open`/`empty` within about one kernel poll
period), and so are the `04/02` / `04/07` flavors of `loading` — the
drive state is stable, the *system* is not. mos reports the drive
truthfully; this note is about what the platform does next.

**Residual unverified sliver (hardware-gated).** The kernel's poll and
mos's TUR are separate commands up to one poll period apart. The only
dangerous interleaving is a backward flip — GOOD to the kernel (nub
created), then non-GOOD to mos within the same window — plus the
UNIT ATTENTION variant the single-TUR doctrine (§4.1) bets the kernel
consumes. One insert-under-watch hardware run retires both (STATUS,
hardware gate).

## 6. Discovery, addressing, and BSD-name resolution (DR directory)

**Since the DR pivot (2026-06-10), DiscRecording is the directory and
the doorbell; MMC stays the inspector** (doc/research/
2026-06-10-dr-pivot-implementation-plan.md). Discovery is
`DRCopyDeviceArray` — the same array drutil enumerates, so the 1-based
`--index` agrees with drutil's by provenance, not by a sort
approximation (order is a snapshot, stable only within an
enumerate→open window). DR exposes each device's IORegistry *path*;
mos resolves path → entry → uint64 entry ID as the one surviving
IOKit step of discovery, because the entry ID remains the identity
currency: reopen uses `IORegistryEntryIDMatching` (atomic; same drive
or `NO_DEVICE` if terminated), never by-name re-resolution, which
would land on whatever currently holds a recycled name after a
hot-unplug. Coverage is unchanged: `DRCopyDeviceArray` returns
writers only, which is the same set §9.1's SCSITaskUserClient attach
gate already limited mos to.

The pre-pivot class-walk enumerator (`IOBDBlockStorageDevice` /
`IODVDBlockStorageDevice` / `IOCDBlockStorageDevice` matching with
registry-ID dedupe and sort) is deleted; the kexts behind those
classes are of course still the substrate DR itself rides.

Two name operations remain, with one registry walk between them:

- **Naming a drive at open** still walks **down** from the
  BlockStorageDevice to its `IOMedia` child
  (`kIORegistryIterateRecursively` without parents) — the `BSD Name`
  property and the whole-disk IOMedia entry ID (`media_id`, the F1
  swap fingerprint, which DR has no equivalent for) live on the
  descendant media node. An empty/open-tray drive has no media child
  and therefore no name (unit -1); it still enumerates and opens —
  which is why discovery/index can never be replaced by a name lookup.
  Enumeration itself takes the media BSD name from DR's status
  media-info dictionary instead (same media-scoped semantics).
- **Resolving a named input** (`--bsd diskN`) goes through
  `DRDeviceCopyDeviceForBSDName` behind the unchanged
  `parse_bsd_unit` gate (malformed → `invalid_arg`, well-formed-but-
  absent → `no_device`). This dissolved the never-implemented v0.3
  walk-up plan (ROADMAP, "Architectural").

Per-poll reopen never re-resolves by name — it uses `IORegistryEntryIDMatching`
on the registry ID captured at open, which is what protects a watch from
BSD-name reassignment.

The same captured ID is emitted on every event as `registry_id`,
alongside `stream_open_ms` — session identity as two plain JSON
numbers, no composite token (the data layer stays normalized;
consumers wanting a single correlation key concatenate, which is the
right direction of derivation). `registry_id` is the watch target's
**attachment identity** — xnu mints it at first attach from a global,
never-reused monotone counter (`++gIORegistryLastID`,
`iokit/Kernel/IORegistryEntry.cpp`; real IDs start above 2^32+255), so
it exists for the whole plug session including media-less periods
(when no BSD name does), and a replug is a *new* ID by construction.
That non-reuse is the reopen path's TOCTOU defense: a
vanished-and-returned drive terminates the watch with `device_removed`
instead of being silently re-adopted. `stream_open_ms` is the
wall-clock open time, monotonicized per process by the adapter, so the
pair is unique per session even for same-millisecond reopens of the
same drive.

## 7. Error handling patterns

- Every `IOIteratorNext` result must be `IOObjectRelease`'d.
- `IOCreatePlugInInterfaceForService` requires `IODestroyPlugInInterface`.
- Every COM-style interface acquired via `QueryInterface` requires
  `(*iface)->Release(iface)`.
- `CFRelease` any `CFTypeRef` returned by
  `IORegistryEntryCreateCFProperty`.
- On sense key `0x06 UNIT ATTENTION`: mos does **not** drain or retry it.
  TEST UNIT READY is issued exactly once (§4.1, §5.4) — by the time mos holds
  a handle, the kernel's own device initialization has already consumed the
  power-on / reset / media-change UA, so a stray UA is taken at face value and
  classified to `unknown`, not retried. (The Linux first-toucher "retry the
  CDB once on UA" pattern does not apply to a user-space client behind the
  macOS storage stack.)

## 8. Timeouts

| Command                         | Timeout (ms) | Set by |
|---------------------------------|--------------|--------|
| TEST UNIT READY                 | kernel default | convenience wrapper |
| GET EVENT STATUS NOTIFICATION   | 2000         | **mos** (`mos_raw_cdb`) |
| GET CONFIGURATION               | kernel default | convenience wrapper |
| READ DISC INFORMATION           | kernel default | convenience wrapper (on-demand typed API only — never the state path) |
| INQUIRY                         | (retired — identity from the DR directory) | — |

Only the raw GESN carries a timeout mos chooses; the convenience methods
expose no timeout parameter, so the kernel's SCSI stack owns theirs. The
2000 ms GESN value bounds the only command mos issues under exclusive
access, which bounds how long the lock is ever held.

## 9. Known Unknowns — must be empirically validated

Items that the spec-and-header research does not settle and that
require real-hardware testing before v1.0.

### 9.1 Writer-vs-reader kext attach rule

**Documented by MakeMKV's developer; current applicability uncertain.**
The rule, in MakeMKV's own words on the canonical
https://www.makemkv.com/osxmmc/ page: *"if the drive is a read-only
device that can't write to any media (either CD-ROM, DVD-ROM or
BD-ROM) then access is blocked... if the drive is capable of writing
any media (for example Blu-ray reader that can write CD and DVD media)
then all access is allowed to any user."* The relevant IOKit property
is `kIOPropertySCSITaskAuthoringDevice` (`"SCSITaskAuthoringDevice"`).

The page editorializes the rule with: *"This policy is beyond our
comprehension - our only explanation is that weed is really easily
available to designers in Cupertino, CA."* That's not just developer
snark — it's signal. When a developer who's shipped on a platform
for fifteen years vents in their public technical docs, the rule
shaped product strategy enough to be worth a dig. Take it as evidence
that the rule was real, load-bearing, and consistent enough across
macOS releases for it to be worth ranting about rather than working
around per-version.

The consequence for mos's coverage: any drive mos can open is, by
definition, one that passed the SCSITaskUserClient attach gate.
Read-only drives that the gate filters out are invisible to mos —
they don't appear in `mos_enumerate_devices()` and `mos_open_*()`
returns `MOS_ERR_NO_DEVICE` against their BSD names. Empirically,
this means every drive mos sees is a writer. (This also matches
drutil's coverage: drutil and mos share the same DiscRecording/
SCSITaskUserClient substrate, so they see the same set of drives.)

Historical note: a v0.3.1-dev iteration of `mos_state_result` carried
an `is_rewritable` boolean derived from the kIOPropertySCSITaskAuthoringDevice
IOKit property. That field was removed before tag because (a) the
value was always true for any drive mos could see (the kernel filters
authoring=false drives out before mos can match them), (b) the name
was misleading — it conflated "drive is permitted to issue authoring
commands" (the kext flag's meaning) with "drive can write rewritable
media" (the MMC concept), and (c) exposing an Apple-internal kext
property as part of the public ABI was the kind of thing that bites
later when Apple changes the property's semantics across macOS
versions. v0.4's GET CONFIGURATION supported-profile work answers
the genuine "can this drive write rewritable media" question via MMC
rather than via a kext flag.

The reference rig (BH16NS55, WH16NS60, A1379 SuperDrive) are all
writers, so the matrix doesn't actually exercise the read-only-blocked
branch. To validate the rule in 2026 we'd need a pure BD-ROM reader,
which is increasingly hard to find. For now this is "we trust
MakeMKV's documentation, supported by the empirical observation that
read-only drives don't appear in mos enumeration."

### 9.2 Apple Silicon SuperDrive behavior

The Apple SuperDrive works via USB-A (with a dongle on Apple Silicon
Macs). IOKit matching is believed to work identically across Intel and
Apple Silicon, but the kext attach rules for USB vs built-in drives
have shifted across macOS versions. Validate against an actual
SuperDrive on arm64.

### 9.3 Pure BD-ROM readers

The LG WH14NS40 in read-only mode, for example, historically did not
get `SCSITaskUserClient` attached. The library returns
`MOS_ERR_DRIVER_REJECTED` cleanly in that case. Contribute a fixture if
you have one of these drives.

### 9.4 Timing of GES events after tray transitions

Some drives queue only one event per class; a fast successive probe
may see `NoChange` immediately after a tray transition. The decision
tree accepts `NoChange` + Media Status bits as authoritative — MMC-6
requires Media Status to be current at query time even when the Event
Code is 0. Some off-brand drives violate this. Verify on yours.

### 9.5 macOS 27+ DriverKit transition

**Current status (validated 2026-04-22 against macOS 26.5 beta
25F5042g):** `SCSITaskLib` and `MMCDeviceInterface` remain present
in the SDK with no deprecation annotations on any symbol mos
depends on. The kexts that expose mos's actual interface
dependencies are all loaded (versions in parentheses, all
unchanged from Sequoia 15.x):

- `IOSCSIArchitectureModelFamily` (545.100.10) — the SCSI
  command framework that mos's `MMCDeviceInterface` plumbs
  through.
- `IOSCSIMultimediaCommandsDevice` (545.100.10) — provides the
  MMC convenience methods (`GetTrayState`, `TestUnitReady`,
  `GetConfiguration`, `Inquiry`, `ReadDiscInformation`); the
  default query path uses `TestUnitReady` and `GetConfiguration`
  (INQUIRY retired to the DR directory, 2026-06-10).
- `IOStorageFamily` (2.1) — provides `IOMedia`, which mos still
  walks at open for the media_id fingerprint and bsd_unit (§6).
- `IOCDStorageFamily` / `IODVDStorageFamily` /
  `IOBDStorageFamily` (1.8) — provide the
  `IOCDBlockStorageDevice` / `IODVDBlockStorageDevice` /
  `IOBDBlockStorageDevice` classes mos matched against pre-pivot;
  discovery now rides `DRCopyDeviceArray` on the same kext
  substrate (DiscRecording sits above these families too).

`SCSITaskUserClient` (545.100.10) is also loaded. mos's raw-CDB
diagnostic path (`mos_raw_cdb()`, C API) opens this user-client
directly to acquire exclusive access for arbitrary CDB injection —
and since the 2026-05-30 redesign the default query path's not-ready
branch issues its GESN tray probe through the same function (§3). Whether `MMCDeviceInterface`'s convenience methods
route through the same kernel user-client internally is opaque
from user-space — Apple's plug-in implementation is closed —
but mos itself never opens the `SCSITaskUserClient` interface on
the default state-query path; that path goes through
`MMCDeviceInterface` only. mos is not a bypass tool and does
not write to disc, so the larger surface those kexts expose
(exclusive task delivery, write-mode parameter pages, AACS key
exchange) is not on mos's direct dependency path even though it
shares the same kext families.

**One concrete header-level change in macOS 26 that doesn't
affect us:** `<IOKit/storage/IOMedia.h>` was removed from the
userland SDK. mos doesn't reference anything from it, and the
defensive include line was dropped. This is actually evidence
*for* the rest of the analysis rather than against it: Apple is
in fact editing the SCSI/storage SDK surface in the 26 cycle,
just not in any way that hurts mos. "No deprecation annotations
on symbols we depend on" is a stronger statement when Apple has
demonstrably touched adjacent symbols and chosen to leave the
ones we use intact.

MakeMKV 1.18.x links against the same framework set mos targets
(`IOKit`, `CoreFoundation`, `DiskArbitration`, `Foundation`,
`Security`, `libobjc`, `libSystem`, `libc++`), with no DriverKit
linkage. Full investigation log including SDK header audit,
`kmutil showloaded` output, `otool -L` analysis, and macOS 26
release-notes review is preserved in
`doc/research/2026-04-22-driverkit-investigation.md`.

**The DriverKit replacement path** (`SCSIPeripheralsDriverKit`,
shipped since macOS 13 Ventura) is **not** a drop-in substitute
for an in-process library like mos. DriverKit drivers are `.dext`
bundles requiring Apple entitlement, System Extension activation
approval, and distribution inside a signed app bundle. The CLI /
library pattern of opening an `SCSITaskUserClient` from a user-
space process has no DriverKit analogue. Migrating mos to
DriverKit would mean rewriting as a system extension with a
completely different install story — that's not a port, it's a
new project.

**SCSITaskLib's survival is benign neglect, not a contract.**
Apple has precedent for terminating benign-neglect APIs without a
deprecation cycle (AppleHDA in macOS 26, FireWire in 26.1, neither
preceded by `API_DEPRECATED`). If Apple cuts `SCSITaskUserClient`
in a future release, mos has no in-process fallback — the project
ends or forks into a dext-based successor.

**Forward-looking signals to watch for** (any of these would
indicate the user-space SCSI path is actually being withdrawn):

1. `SCSITaskLib.h` disappears from a future SDK, or its copyright
   header gets touched.
2. New `API_DEPRECATED` annotations appear on any of the methods
   we call from `MMCDeviceInterface`.
3. `kmutil showloaded` on a future macOS release doesn't show
   `com.apple.iokit.SCSITaskUserClient`.
4. MakeMKV, libaacs, or another SCSITaskLib consumer ships a
   version that migrates to a system-extension model.
5. Release notes explicitly mention SCSITaskLib, SCSI user
   clients, or optical-drive access.

macOS 27 (Fall 2026, Apple-Silicon-only — the first post-Intel
release) is the most plausible checkpoint for any of those things
to happen. As of 26.5 beta, none have. Re-validate before any
release tag if a newer beta has dropped.

### 9.5.1 Universal builds and the Intel cliff

Apple's 26.4 release notes formalize what WWDC 2025 announced:

- *"macOS Tahoe 26 is the last release to support Intel based Macs."*
- *"Rosetta support for apps will end after macOS 27."*
- Starting in 26.4, the system notifies users when they launch apps
  that use Rosetta.

What this means for this project:

- **Through macOS 26.x:** universal builds remain useful. The x86_64
  slice runs **natively** on Intel Macs (not through Rosetta), so the
  Rosetta deprecation does not affect us. Our `make build-universal`
  target continues to do the right thing, and Intel users on Tahoe
  26.x get a native binary.
- **macOS 27 and later:** Apple Silicon only. The x86_64 slice has no
  remaining target audience. The `build-universal` target becomes
  dead weight and should be removed.
- **Plan:** keep universal builds on the main branch through 2026;
  drop the `build-universal` / `sign-universal` / `dist` universal
  targets when we cut a v1.0 release timed to macOS 27 compatibility.
  `make build` (native) remains the primary workflow either way.

### 9.6 Inquiry string trimming

SPC-4 requires INQUIRY vendor / product fields to be space-padded, but
some drives return junk bytes or fail to null-terminate. The
`mos_internal_mmc_inquiry` implementation must trim trailing spaces
and defensively null-terminate the 8-byte vendor and 16-byte product
slots on the handle. Anything that reads those slots later (CLI JSON
emission, mos_probe) copies them as ordinary C strings, so the
terminator is load-bearing.

### 9.7 `GetTrayState` masking trap (why we issue GESN directly)

**Guaranteed (header-provable), with one Hardware-observed detail.** The
MMC `GetTrayState` convenience method is a wrapper around GESN whose
signature is structurally unable to report a command-level failure:

```c
IOReturn ( *GetTrayState )( void * self, UInt8 * trayState );
```

Every other command-issuing convenience method in `MMCDeviceInterface`
(`Inquiry`, `TestUnitReady`, `GetConfiguration`, `ModeSense10`,
`ReadTableOfContents`, `ReadDiscInformation`, `ReadTrackInformation`,
`ReadDVDStructure`, `GetPerformance`) carries `SCSITaskStatus *` +
`SCSI_Sense_Data *` out-parameters. `GetTrayState` carries neither, its
documented IOReturns cover only transport (`kIOReturnNoDevice`,
`kIOReturnExclusiveAccess` — not even `kIOReturnNoMemory`, which
`TestUnitReady` documents), and its output domain is binary with
`kMMCDeviceTrayClosed = 0` — so a GESN that fails at the SCSI level has
no channel to surface, and a zeroed or unwritten out-parameter is
indistinguishable from a genuine "closed." This is provable from
`SCSITaskLib.h` (vendored under `docs/apple/` — dev-tree only, stripped
from release archives by preflight; the §11 public mirrors carry
identical declarations and are the verifiable path for artifact
consumers). And the *which value* half is
no longer Hardware-observed either — it is kernel-source-verified:
`IOSCSIMultimediaCommandsDevice::GetTrayState`
(apple-oss-distributions/IOSCSIArchitectureModelFamily,
`IOSCSIMultimediaCommands/IOSCSIMultimediaCommandsDevice.cpp`) handles
ANY GESN failure with, verbatim, the comment "Assume the tray is shut"
followed by `*trayState = 0; status = kIOReturnSuccess`. The success
path is equally instructive: it reads `statusBuffer[5] & 0x01` on GOOD
with no NEA gate, no notification-class check, and no Event Data Length
validation — mos's pure decoder is strictly stronger than Apple's own
consumer of the same reply. (Same source confirms the CDB equivalence:
Apple issues notification class `1 << 4`, allocation 8, polled — the
§4.2 layout byte-for-byte.)

Since v0.3 we therefore issue the raw GESN CDB ourselves (§4.2) and gate the
result in the pure decoder: a failed or invalid reply returns "no
authoritative bit," and the decision tree forks on the TUR sense instead of
inventing a verdict (§5.4). A generic-no-medium sense with no GESN bit
surfaces as `empty_or_open` rather than a false `empty`.

### 9.8 Sandboxed / App Store distribution

The library is designed for CLI + direct linking. In a sandboxed app
the SCSITaskUserClient interface is blocked at the `iokit-open` mach
lookup. This is out of scope — distribute via Homebrew or embed into a
non-sandboxed app.

### 9.9 Tray control-verb behaviour (for the deferred control surface, §1)

Spec-guaranteed (T10): START STOP UNIT eject, PREVENT ALLOW MEDIUM REMOVAL
prevent codes 0/1/2/3, the `0x53,02` "medium removal prevented" failure, and
that a persistent-prevent (3) holder can still eject through its own lock.

**Header-level inputs to question (0), established 2026-06-10 against the
public 10.2.8 `SCSITaskLib.h` copy (§11 link), which §11 records as
signature-identical to the modern SDK:**

- `SetTrayState(void *self, UInt8 trayState)` shares `GetTrayState`'s
  structural blindness (§9.7): no `SCSITaskStatus`/sense out-parameters.
  It does document `kIOReturnNotPermitted` for the media-inserted case,
  so one failure class surfaces via IOReturn — but the SCSI-level detail
  this section wants surfaced (`0x53/02` on a locked eject) is
  **structurally unreportable** through the convenience wrapper. The
  mechanism-facts-only contract ("mos reports what the command did")
  cannot be honored via `SetTrayState`, because the wrapper cannot
  report what the command did. This tilts the eject/close verbs toward
  the raw-CDB path before any hardware is touched.
- `SetMediaAccessPermission` and `SetCDSpeed` do **not exist** in the
  10.2.8 header (only `GetPerformance`/`GetPerformanceV2` cover the
  speed axis, read-only). Bounded claim: §11's signature-identity note
  covers existing methods, not additions, so confirm against the
  vendored modern header in `docs/apple/` — but if they are absent
  there too, the lock/unlock and speed-set verbs are raw-CDB-only and
  the exclusive-access question below partially answers itself.

To be validated on hardware before the verbs ship: (0) **which API surface**
each verb uses — an `MMCDeviceInterface` convenience method, which needs no
exclusive access but cannot surface sense (above), versus a raw
START STOP UNIT / PREVENT ALLOW CDB, which needs exclusive access and can
(§3); (1) **does the prevent bit survive
`SCSITaskUserClient` close** — some stacks clear it on last close, which would
force a held session instead of the fast open-CDB-close model; (2) whether the
exclusive access (if the raw path is used) contends with another application holding the
drive; (3) whether prevent=1/3 physically keeps a given drive model's tray
retracted (firmware-dependent); (4) whether exclusive access blocks
`DADiskEject`/`drutil` or there is a privileged override.

## 10. CLI shape: single binary, derived state, not opcode-enumerative

mos follows the sg3_utils internal decomposition model — reusable
library under `src/`, public header under `include/`, dedicated test
tree under `tests/`, JSON as the machine-readable layer, sysexits.h
exit codes for scripting, stdout for canonical output and stderr for
diagnostics — but deliberately follows the drutil-style single-binary
interface model. The two are separable design choices that the
sg3_utils tradition happens to combine; for mos's domain only the
first applies.

The load-bearing distinction is what the user-facing product *is*.
mos describes the derived optical-drive state. It does not expose
raw MMC operations as the user-facing product. The library's
contribution is the decision tree (see §5) that fuses TUR + GET
EVENT STATUS + GET CONFIGURATION + sense parsing into a single state
classification. Splitting the CLI into per-MMC-command binaries
(`mos_tur`, `mos_inq`, `mos_get_config`, ...) would demote mos into
a worse `sg_raw` equivalent and lose the classification layer
entirely. sg3_utils ships dozens of per-command executables because
its domain is all of SCSI — hundreds of opcodes across many device
classes, each operation genuinely distinct. mos's domain is one
classification operation plus a small set of derived auxiliary
queries (capacity, identity, tray, speed, features). Cardinality
governs the choice; the decomposition pattern alone does not.

The default invocation `mos --bsd disk4` is a permanent surface,
not a deprecation alias. Tools whose primary purpose is one obvious
operation keep that operation as the default invocation: `ps`, `df`,
`uptime` do not force a subcommand. Subcommand-mandatory CLIs
(`kubectl`, `git`, `docker`) make sense when the operations are
roughly equally likely; they are cargo-cult when applied to tools
with one obvious primary. mos belongs to the former category —
state classification is the operation; everything else is an
auxiliary.

The forward CLI shape:

```
mos                              # implicit: status of first drive
mos status                       # explicit form of the same
mos status --bsd disk4 --json
mos list                         # shipped (alias of --list)
mos list --json
mos watch --bsd disk4 --json     # shipped (alias of --watch)
mos capacity --bsd disk4 --json  # v0.4 typed API (reserved name today)
mos identity --bsd disk4 --json  # v0.4 typed API (reserved name today)
mos tray eject --bsd disk4
mos tray close --bsd disk4
mos speed get --bsd disk4 --json
mos speed set --bsd disk4 --target 8x
mos features --bsd disk4 --json
```

Each subcommand extends the schema family
(`mos.capacity.v1`, `mos.identity.v1`, `mos.tray.v1`, `mos.speed.v1`,
`mos.features.v1`) without adding executables to the Homebrew tap.
`--watch` remains a flag — it is a *mode of operation* applicable
across subcommands, not a distinct operation: `mos status --watch`
works today, and `mos features --watch` (future) describes over-time
observation of that query. The shipped `mos watch` subcommand is an
additive *alias* of the flag form (CLI contract test 15), kept because
"watch the default query" is common enough to deserve the short
spelling; the flag stays the composable primitive and is what future
subcommands combine with.

This positioning also clarifies a documentation register: mos is
not a SCSI utility and not a libcdio alternative. It is a state
classification tool — over time, over capability, over identity —
all derived from the same MMC substrate. The README's opening line,
the Homebrew formula description, and the v0.3+ typed-API names
should all sit in the classification register; departures into the
opcode-enumerative register would imply a different product.

The wire-level form of each schema family is machine-checkable. See
`schemas/` for JSON Schema documents covering `mos.state.v1`,
`mos.error.v1`, `mos.list.v1`, and `mos.event.v1`, plus positive
example fixtures and negative-case fixtures. CI runs
`schemas/validate.py` against both directions on every commit. When
the emit code in `tools/mos.c` changes, the schema and at least one
fixture should change in the same commit.

## 11. Prior art and references

Primary platform source (the seam's ground truth). The vendored
`docs/apple/` copies these cite are dev-tree-only (release-preflight
strips them; not ours to redistribute under 0BSD) — the public links
below are the verifiable evidence path for anyone holding a shipped
archive:

- **apple-oss-distributions/IOSCSIArchitectureModelFamily** — the
  kernel side of every claim in §5.5 and §9.7:
  `IOSCSIMultimediaCommands/IOSCSIMultimediaCommandsDevice.cpp`
  (`GetTrayState` masking, `PollForMedia` nub predicate and auto-eject
  table) and `UserClient/SCSITaskUserClient.cpp` (convenience-TUR
  exclusivity gate).
  https://github.com/apple-oss-distributions/IOSCSIArchitectureModelFamily

Gold-standard working C, study the structure:

- **libcdio** `lib/driver/osx.c` — canonical reference. GPLv3.
  https://github.com/libcdio/libcdio/blob/master/lib/driver/osx.c
- **libaacs** `src/file/mmc_device_darwin.c` — LGPL, coordinates with
  DiskArbitration around exclusive access.
  https://code.videolan.org/videolan/libaacs/-/blob/master/src/file/mmc_device_darwin.c
- **cdrtools** `libscg/scsi-mac-iokit.c` — Schilling's implementation,
  CDDL/GPL.

Headers (authoritative):

- `SCSITaskLib.h` (Apple, in the 15 / 26 SDK at
  `/System/Library/Frameworks/IOKit.framework/Headers/scsi/SCSITaskLib.h`).
  Historical copy (10.2, UUIDs and method signatures unchanged):
  https://github.com/phracker/MacOSX-SDKs/blob/master/MacOSX10.2.8.sdk/System/Library/Frameworks/IOKit.framework/Versions/A/Headers/scsi-commands/SCSITaskLib.h
- `DRCoreDevice.h` / `DRCoreNotifications.h` (Apple DiscRecording —
  the directory/doorbell substrate since the 2026-06-10 pivot).
  Vendored dev-tree-only at `docs/apple/DiscRecording/` from the
  macOS 15.5 SDK; public mirror (byte-stable since ~10.13 modulo
  include/import cosmetics):
  https://github.com/alexey-lysiuk/macos-sdk/blob/main/MacOSX15.5.sdk/System/Library/Frameworks/DiscRecording.framework/Versions/A/Headers/DRCoreDevice.h
  Feasibility evidence: doc/research/2026-06-10-dr-pivot-feasibility.md.

Specs:

- T10 MMC-6 project page: https://t10.org/members/w_mmc6.htm
- T10 MMC-2 draft (97-108r0): https://www.t10.org/ftp/t10/document.97/97-108r0.pdf
- T10 ASC/ASCQ list: https://www.t10.org/lists/asc-num.htm

Functional peer (closest userspace analogue to mos):

- systemd `src/udev/cdrom_id/cdrom_id.c` — userspace MMC prober.
  Same five commands, same kind of consumer (udev rather than
  a consuming application, structurally equivalent). Reference for the
  two-pass GET_CONFIGURATION pattern, sense-keyed pre-MMC2
  fallback, INQUIRY peripheral-type validation, DVD-RW
  restricted-overwrite firmware-quirk normalization, and
  bounded-retry open with jitter. Different *vocabulary* from
  mos (media-content state vs drive-handling state — see §1)
  but the same *shape*.
  https://github.com/systemd/systemd/blob/main/src/udev/cdrom_id/cdrom_id.c

Vendor primary sources (developer-authored, partially historical):

- MakeMKV "Direct access to SCSI devices on Mac OS X" —
  https://www.makemkv.com/osxmmc/. Now marked as historical
  (MakeMKV moved to OS-access mode by default), but the
  writer-vs-reader kext attach rule cited in §9.1 is canonically
  documented here, including the developer's editorial commentary
  on Apple's design choices that's worth reading just for the
  signal that the rule was load-bearing enough to vent about in
  public docs.
- MakeMKV "Mac OS X Blu-ray support" — https://www.makemkv.com/osxbd/.
  Documents that Apple stopped releasing source for
  IOSCSIArchitectureModelFamily after 10.4.4, which is why
  working at this layer requires reading SCSITaskLib.h and
  observing behavior rather than reading kernel source the way
  Linux work does. Also describes a specific Apple framework bug
  in the AACS key-exchange path that motivated the DASPI kext —
  the bug itself is circumvention scope and out of scope for mos,
  but the *fact that working around the framework was the right
  answer for years* shapes how to read Apple's IOKit MMC stack
  generally: it's opaque, occasionally buggy, and historically
  required workarounds.

Linux kernel cross-references (one layer below mos):

- `drivers/scsi/sr.c` — sense decode, GES/TUR mismatch heuristic
  (`tur_mismatch >= 8 → ignore_get_event`), TUR-on-repeat-hangs
  comment, media-presence-from-sense (ASC=0x3A) inversion.
  https://github.com/torvalds/linux/blob/master/drivers/scsi/sr.c
- `drivers/cdrom/cdrom.c` — tray-lock lifecycle (`cdrom_release`
  always pairs PREVENT with ALLOW), autoclose/autoeject as
  user-controlled rather than driver-imposed, two-pass shape for
  READ_DISC_INFO (`cdrom_get_disc_info`).
  https://github.com/torvalds/linux/blob/master/drivers/cdrom/cdrom.c
- Linux cdrom-standard design history (van Leeuwen 1996–1997):
  https://docs.kernel.org/cdrom/cdrom-standard.html
