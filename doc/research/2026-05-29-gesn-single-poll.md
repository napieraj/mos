# GESN single-poll state read (research note)

**Date:** 2026-05-29
**Status:** Active design note. GESN *issuance* deferred and, given the
DiscRecording substrate, unlikely. Records why mos's value is the full-checking
state machine, not a signal source.
**Scope:** GET EVENT STATUS NOTIFICATION (GESN, opcode `0x4A`) as a *single-command*
source for several drive-state signals at once, weighed against mos's existing
media-change architecture (Disk Arbitration wake + Unit Attention sense +
`GetTrayState` + the F1 IOMedia registry-ID fingerprint).

## The observation

Reading the canonical libcdio MMC layer (`lib/driver/mmc/mmc.c`, GPLv3 —
reference only, see provenance below) surfaced a tidy pattern: it derives both
"media changed" and "tray open" from a *single* GESN poll, reading two bytes of
the event reply rather than issuing a command per signal. GESN is built for
exactly this — the request carries a Notification Class Request bitmask and the
drive answers with the highest-priority pending event across the classes you
asked for. The classes relevant to mos are Operational Change, Media, and Device
Busy; one poll can carry tray position, media-present / media-changed, and a
real "mid-operation" indication together.

## Response shape (confirm against MMC §6.1.5 at implementation)

The opcode is confirmed in the vendored SDK headers (`kMMCCmd_GET_EVENT_STATUS_
NOTIFICATION = 0x4A`, annotated "Sec. 6.1.5"). The descriptor layout below is
from MMC domain knowledge and libcdio's observed usage; it has **not** been
re-verified against a primary spec source this session, so treat the exact bit
offsets as to-confirm, not as load-bearing:

- 4-byte Event Header: bytes 0-1 = Event Descriptor Length (big-endian),
  byte 2 = NEA bit (`0x80`) + Notification Class (low 3 bits), byte 3 =
  Supported Event Classes bitmask.
- Then one class-specific Event Descriptor. For the Media class: an event-code
  nibble (NewMedia / MediaRemoval / MediaChanged...) and a Media Status byte
  whose low bits encode Media Present and Door/Tray Open. For the Device Busy
  class: a busy status plus a time field.

The **NEA ("No Event Available") bit is the validity gate** — if set, the
descriptor for the requested class is not meaningful and must not be trusted.
This is the same "check the valid bit before reading the payload" discipline
called out in the libcdio review; libcdio's consumers read the bytes without an
obvious NEA check, which is exactly the looseness to avoid.

## Why this is *not* adopted for the desktop watch / F1

mos already covers media change three ways, and they are collectively stronger
than GESN for the desktop use case:

1. **Disk Arbitration** push wake (`mos_watch.c`) — DA fires on insert/eject and
   pulls the next poll forward, so the desktop watch is event-driven, not a poll
   loop where GESN's "one command, many signals" economy would pay off.
2. **Unit Attention sense** (`mos_state_core.c`) — the media-change UA rides for
   free on any command's sense, with a one-shot clear.
3. **F1 media fingerprint** — the whole-disk **IOMedia registry entry ID**
   (`IORegistryEntryGetRegistryEntryID`), which mints a fresh identity on any
   physical swap *even at an unchanged profile*. This is a stronger signal than
   GESN's MediaChanged latch: GESN tells you "something changed," the registry ID
   gives you a stable new *identity* to compare. GESN would not replace it.

Add to that: tray already has a clean `GetTrayState` convenience wrapper, whereas
**GESN has no MMCDeviceInterface convenience method** (confirmed absent in the
15.5 `SCSITaskLib.h` this session) — issuance would be raw `SCSITaskInterface`
work (build the `0x4A` CDB, run the task), the most expensive integration path.
So for the desktop watch, GESN is a third overlapping source at the highest cost
and the weakest identity. Not a win.

## The deciding frame: DiscRecording feeds the signals, mos owns the state machine

This subsumes the rest. The DiscRecording substrate we are now building
enumeration on already exposes the coarse tray/media signals — DR polls GESN
internally and surfaces tray-open / media-present in its device status. So the
raw signals arrive *without mos issuing GESN at all*, and mos's own MMC path
(`GetTrayState` + `TestUnitReady` + sense + profile) already covers what the
state engine needs.

mos's differentiated value is therefore not being a signal source — it is the
**state machine that checks fully**: the pure, framework-free core (the part
implemented and exhaustively tested on Linux) that turns whatever raw inputs
arrive into a correct `OPEN / EMPTY / LOADING / READY / BUSY`. Per the standing
decision, the MMC state engine must **not** collapse into DR's status dict —
that dict is coarser and cannot make the `LOADING` vs `BUSY` and sense-driven
distinctions. DR enumerates and hands over cheap coarse status; mos interprets.

That reframes the question. It is never "should mos poll GESN," it is "does
mos's state machine have the inputs it needs to decide correctly" — and it does,
from MMC and DR, with no new `0x4A` command.

## The polling-supervisor case, re-read through that frame

A polling-supervisor consumer — one that loops without a GUI waiting on a DA
push — asks "is the drive done / safe to proceed?" on each iteration. The appeal
there is a coherent atomic snapshot of `OPEN / EMPTY / LOADING / READY / BUSY` —
but that snapshot is *mos's state-machine output*, not a raw GESN reply. The
engine already produces it from MMC (and can take DR's coarse status as a
corroborating input). The one thing a mos-issued GESN would add over that is the
finer **Device Busy** event class — a more direct "mid-operation" signal than the
TUR+sense inference. That is a marginal refinement, not a reason to add a raw
`0x4A` path. So GESN issuance stays deferred and, given the DR redundancy,
unlikely.

## If pursued (low likelihood)

Only worth it if a future need for the finer Device Busy / Operational Change
classes ever outweighs the TUR+sense inference — DR's coarse status plus mos's
state machine already cover the rest. If that need appears, follow the GET
CONFIGURATION precedent exactly:

1. **Pure side first.** A bounds-safe iterator over the event header + class
   descriptors, modeled on `mos_internal_config_next_feature` — never trust the
   device-reported Event Descriptor Length as anything but a shrink, gate the
   NEA bit, full-span reject. This is testable and fuzzable in the Linux CI with
   hostile-input fixtures, no hardware needed.
2. **Mac issuance as a stub.** Unlike tray/config there is no convenience
   wrapper, so the stub documents the raw-`SCSITaskInterface` `0x4A` issuance as
   the hardware-gated half, with the pure parser as its waiting caller (same
   shape as `mos_internal_mmc_get_features`).
3. **Do not collapse it into F1.** GESN supplements the supervisor's state read; the
   registry-ID fingerprint remains the media-identity authority.

## Provenance

The single-poll pattern was *observed* in libcdio (GPLv3). mos is 0BSD and is
meant to be embedded (MakeMKV/Kodi/HandBrake/VLC), so libcdio is strictly
reference-for-understanding: reimplement from the MMC spec, do not lift code.
