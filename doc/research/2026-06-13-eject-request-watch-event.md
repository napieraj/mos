# The `eject_requested` watch event: closing the robot loop, and why it
# is not free (June 2026)

Design note for the v0.4+ follow-on flagged in
`doc/research/2026-06-13-tray-control-feasibility.md` (A.6) and
`ROADMAP.md:188-194,205`. **Not implemented** — this is the design and,
more importantly, the record of *why it cannot ride the existing watch
machinery* and what a hardware rig has to settle before it ships. Written
the same day the tray verbs landed (commit: "Implement tray control verbs"),
prompted by the question: how does a `mos tray` action reach a `mos watch`
running in the background, so mos is both the **source** and the **event
bus** for optical state in an automated/robotic workflow?

## The framing, and the one case it breaks

The honest answer for almost everything is: **the drive is the bus.** mos
does not need a `mos`→`mos` IPC channel — a backgrounded `mos watch`
observes the *drive*, so any tray eject (from `mos tray`, `drutil`, Finder,
or the front-panel button) shows up on its next poll / `kIOGeneralInterest`
wake as a `state_changed` to `open`/`empty`. The actor writes its own
receipt (`mos.tray.v1`); the observer emits its event (`mos.event.v1`);
they meet at the drive, not over a socket. This is *more* robust than a
private channel (it catches every eject source) and it keeps mos's
no-daemon, no-ambient-state design intact (scope doctrine layer 3; the
watch opens its own short-lived handles and holds nothing).

There is exactly one case this does **not** cover, and it is precisely the
robot case the tray work exists for:

**Under Persistent Prevent (`mos tray lock --persistent`, PREVENT field
`11b`), an operator eject-button press produces NO state change.** That is
the entire point of the lock: the tray does not open, the medium stays
present, the drive stays `READY`. State-before == state-after. A
level-triggered, state-diff watch emits nothing. The drive instead raises a
GESN Media **EjectRequest** (event code 1) — and *that* is the signal the
orchestrator needs ("the human asked to eject; decide whether to honor it").
The lock converts a physical action into an event, and the event is invisible
to the watch as it exists today.

## Why it cannot piggyback on state detection: edge vs level

The current watch classifies state from TUR ⊕ the GESN Media **door-open
status bit** (`src/mos_scsi.c` `mos_internal_mmc_get_tray_state`,
`src/mos_pure.c` `mos_internal_gesn_media_door_open`) and emits on a
*transition* (`src/mos_watch_core.c`).

- The **door-open bit** is a *status* — a level. It is re-readable every
  poll, idempotent, and therefore diffable. This is why observation works
  for an actual eject.
- **EjectRequest** is an *event* — an edge in the drive's GESN event queue
  (Media class, event code 1). It does not persist as a state and does not
  move the classified state. You read it from the GESN response's
  event-code field, and reading it **consumes** it.

So surfacing it is not "add an enum value to the state machine." It needs a
*second path* in the watch probe: on each tick, issue a GESN Media-class
read and **drain the event queue**, emitting `MOS_EVENT_EJECT_REQUESTED` on
event presence, **independent of** the state classification. An edge-
triggered event pump alongside the level-triggered state poller.

There is a useful precedent for "emit while the classified state is
unchanged": `MOS_EVENT_MEDIA_CHANGED` already fires when the drive stayed
`READY` but the whole-disk `media_id` changed (`src/mos_watch_core.c`, the
F1 swap fingerprint). EjectRequest is the same shape — a while-`READY`
signal — but with a crucial difference: `media_id` is a re-readable
*identity* (level), while EjectRequest is a *consumed event* (edge). The
pump compares `media_id`; it would have to *drain* EjectRequest.

## The hard part — three constraints that make this hardware-gated

This is why EjectRequest is a follow-on with a rig gate, not a freebie:

1. **GESN Polled consumption is destructive and single-consumer.** Whoever
   reads the event clears it from the queue. There is no broadcast.

2. **The macOS optical kext already polls GESN for media events**
   (`IOSCSIMultimediaCommandsDevice`'s media-poll loop, ~1000 ms — the same
   poll floor the DR-pivot notes cite, `AGENTS.md` DA-retirement addendum).
   A userspace EjectRequest read therefore **races the kernel**: if the
   kernel drains the event first, mos never sees it. This is exactly why
   mos reads the door *status bit* (re-readable, survives the kernel poll)
   and never tried to drain the GESN event *queue*. **Whether an
   EjectRequest survives the kernel's own polling to reach a userspace
   client is a per-drive / per-OS-version falsification question** (the
   hardware-role ADR shape) — confidence here is MEDIUM at best from spec;
   it must be observed on the reference rig (BH16NS55 / WH16NS60), captured,
   and the result recorded as a dated fixture/finding before any code lands.

3. **Two `mos watch` instances would steal events from each other**, for
   the same single-consumer reason — and would each also race the kernel.
   The watch-all multiplexer is one process, so the in-tree case is fine,
   but the contract has to state that EjectRequest delivery is
   at-most-one-consumer.

### Fallbacks if the kernel eats the events

If the rig shows the kernel consumes EjectRequest before userspace can read
it, the likely options, worst to best, are:

- **No clean userspace path.** This would explain why `drutil` and the
  other macOS peers don't expose a soft-eject signal either. The honest
  consequence: the persistent-lock soft-button is *cooperative at the drive
  but not observable by mos*, and the orchestrator must lean on the actual
  eject path (it ejects on its own schedule / timeout; the human's button
  press is absorbed silently). The `lock --persistent` mechanism still
  works as a *lock* — it just can't surface the request.
- **A kernel-forwarded IOKit notification.** Worth a rig check: does the
  optical kext re-emit the consumed EjectRequest as a `kIOMessage…` on the
  service the watch already holds a `kIOGeneralInterest` notifier on? If so,
  mos consumes the *notification* (free, no GESN race) rather than the GESN
  event. This is the cleanest outcome and the first thing the rig should
  test. (Speculative — no evidence yet that the kext forwards it.)

## What it would touch (when the rig blesses it)

- **Watch probe** (`src/mos_watch.c` / `src/mos_watch_core.c`): the probe
  returns not just a `mos_state_result` but "events drained this tick"; the
  pump emits each as its own `EMIT_EVENT` with stream-global seq, the
  watch-all interleave rules unchanged.
- **GESN read in a new state.** Today mos issues GESN only on the *not-ready*
  branch (§5.5: safe because unmounted). EjectRequest fires when media is
  present and the drive is `READY` — a state where mos currently issues no
  GESN at all. This is still opcode `0x4A` (the one raw CDB; the one-raw-CDB
  count does not change), but issuing it in `READY` is a new command-surface
  position that needs its own nub-collision note: a GESN read takes the
  exclusive lock, and on a `READY` *mounted* drive `ObtainExclusiveAccess`
  is BUSY — so the event-drain can run only on a `READY` drive that is not
  mounted as a volume (the persistent-lock robot case: a disc loaded for
  ripping, not auto-mounted). This constraint must be stated, not papered
  over: EjectRequest polling is viable exactly in the unmounted-but-loaded
  window, which is where the robot operates.
- **Public API** (`include/mos.h`): `MOS_EVENT_EJECT_REQUESTED` — an
  additive `mos_event_kind` value, forward-compatible per the JSON-schema
  ADR (consumers treat unknown kinds as ignorable). `mos.event.v1` `event`
  enum gains the token; the `validate.py` `event_kind_string` drift guard
  keeps it in lockstep.
- **Arming** is explicit and consumer-owned: EjectRequest only fires after
  `mos tray lock --persistent`. The watch does not arm it; the orchestrator
  locks, then watches. (Event code cross-checked against Linux `sr.c`
  `DISK_EVENT_EJECT_REQUEST`, `ROADMAP.md:191`.)

## Verdict

State-observation closes the loop for anything that *moves* the tray; the
Persistent-Prevent soft-eject is the one case it structurally cannot, and
closing *that* needs an edge-triggered GESN event-drain in the watch — gated
on a rig answering whether the event survives the kernel's own GESN polling
(constraint 2). Build order: (a) rig check — does EjectRequest reach
userspace at all, via GESN drain or a forwarded IOKit notification; (b) only
if yes, the probe/pump event-drain + the additive enum. No IPC daemon: the
mechanism stays observation through the drive, consistent with mos's
no-ambient-state design. Until the rig check exists, this stays
documentation.

## Sources

- `doc/research/2026-06-13-tray-control-feasibility.md` (A.6 follow-on,
  Part 3 persistent-prevent semantics, Part 6 item 4 per-drive button
  matrix as falsifier); `ROADMAP.md:179-216` (three-level gating model,
  `eject_requested` extension, `sr.c` event-code cross-check).
- `src/mos_scsi.c` (`mos_internal_mmc_get_tray_state` — GESN issued on the
  not-ready branch only); `src/mos_pure.c`
  (`mos_internal_gesn_media_door_open` — reads the door *status* bit, not
  the event queue); `src/mos_watch_core.c` (state-diff pump; `media_changed`
  as the while-`READY` precedent); `AGENTS.md` (DA-retirement addendum —
  the kernel's ~1000 ms media poll; scope doctrine layer 3 — no daemon).
- T10 04-349r1 §6.18.3.2 (Persistent Prevent: button press generates a GESN
  EjectRequest, the drive does not eject; an initiator eject still
  succeeds); MMC-6 GET EVENT STATUS NOTIFICATION Media event class, Polled
  mode, event code 1 (EjectRequest), single-consumer queue semantics.
