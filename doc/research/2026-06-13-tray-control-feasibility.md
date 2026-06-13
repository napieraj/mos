# Tray control (eject / close / prevent) in `mos`: feasibility report (June 2026)

Full-session research report. **Question (user):** is it feasible to modify
`mos`'s current contract and implement tray-control eject + close and the
prevent-eject bits — specifically for a *ripping-robot* workflow, where
preventing ejection keeps a tray from popping open while a robot arm moves
around the workspace? **Method:** verification of the already-drafted v0.4
tray design against the actual tree — the scope doctrine (`AGENTS.md` layer 1),
the contract stance (`ARCHITECTURE.md` §1), the masking-trap analysis
(§9.7/§9.9), the single-raw-CDB mechanism (`src/mos_scsi.c`), and the CLI's
reserved-verb wiring (`cli/main.c`) — and, for the load-bearing
lock-lifetime question, *derivation from canonical sources*: T10 SPC-4 (the
04-349 SPC-3↔MMC-5 PREVENT ALLOW merge) and Apple's
`IOSCSIMultimediaCommandsDevice` source, the same kext tier §5.5 and §9.7
already stand on. No `src/`, schema, or ADR changes; this file is the
deliverable.

**Method note (corrected after first draft).** Per the hardware-role ADR
(`AGENTS.md`, 2026-06-10), hardware *falsifies* spec-derived behavior and
supplies fixtures; it is never design input, and "it happened to work on my
drive" is not a conformance result. The first draft of this report wrongly
framed the central lock-persistence question as a coin-flip "a hardware
capture decides which product you get." It is a *research* question with a
spec-and-source-derived answer (Part 4); hardware can only break that
derivation, not establish it. This revision demotes every hardware item from
"open question we lack an answer to" to "spec-expected-good + what a rig could
falsify."

Confidence per claim: HIGH = in-repo source, T10/MMC spec, or Apple kext
source quoted; MEDIUM = inferred from spec + source but turning on a macOS
transport detail not separately confirmed (`mos` has never run on real
hardware — `ROADMAP.md:8`).

## Verdict

**Feasible, already the planned v0.4 surface, and the robot's specific
requirement is satisfied by spec + Apple source — no new lifecycle, no
hardware needed to decide the design.** The contract change is pre-decided
(Part 1), the command mechanism already exists (the GESN raw-CDB path, Part 2),
and the CLI verb is already reserved. The robot requirement — "keep the tray
locked while the arm moves" — turns on whether the prevent bit survives
`SCSITaskUserClient` close, and that is a **research question with a canonical
answer, not a hardware verification** (Part 4):

- **SPC-4** clears PREVENT ALLOW state only on I_T nexus loss, logical-unit
  reset, hard reset, power on, or PREEMPT AND ABORT — a per-I_T-nexus state.
  A handle close and a process exit are not on that list.
- A macOS `SCSITaskUserClient` close is **none** of those five events: the I_T
  nexus is owned by the kernel transport layer (`IOSCSIProtocolServices`) and
  persists across user-client open/close, and Apple's
  `IOSCSIMultimediaCommandsDevice` — unlike Linux's `cdrom_release` — does
  **not** voluntarily issue ALLOW on exclusive-access release
  (`HandleSetUserClientExclusivityState` suspends + resets cached media
  characteristics, with no `PREVENT_ALLOW_MEDIUM_REMOVAL`; the kext's only
  unlock is inside `EjectTheMedia`).

So the spec-derived expected-good is: **the lock survives, and `mos`'s existing
acquire-on-call / release-on-return model is the correct one** — `mos tray
lock` is fire-and-forget, no held session, nothing held against Finder /
MakeMKV / DiskArbitration for the lock window. The held-session alternative
the first draft floated is *not* required by spec. Hardware's role is to
*falsify* this (a non-conformant USB-SATA bridge that drops the nexus on
close; a firmware that ignores prevent and retracts anyway), never to
establish it. **Recommendation: build the four verbs to spec now, document the
lock as fire-and-forget with consumer-owned unlock, and treat the rig as a
falsification gate like every other — not a design input.**

---

## Part 1 — "Modifying the contract" is a pre-decided override, not a fight

The contract clause in question is `ARCHITECTURE.md:20` — *"mos is a state
reporter, not a state controller."* That reads like a wall, but the same
section already carved the exception (HIGH — `mos` tree):

- §1 itself (`ARCHITECTURE.md:32-43`) states the *decided* surface:
  `mos tray {eject, close, lock, unlock}` with `eject --force` =
  unlock-then-eject, deferred "separately from the query path" precisely
  because the reporter-only contract is independently useful and control
  verbs "introduce a different class of failure mode (locked drives,
  surprised users, cleanup-on-process-death obligations)."
- The scope doctrine already names it in-bounds: *"Future tray verbs
  (eject/load, ROADMAP) follow the same rule — convenience first, raw only
  with the GESN-grade justification"* (`AGENTS.md`, scope doctrine layer 1).
- `ROADMAP.md:179-216` is a full design: the three-level MMC removal-gating
  model, the verb→CDB mapping, the `5/53/02` semantics, the EjectRequest
  soft-eject protocol, and the explicit rationale that *"a real caller now
  exists: an orchestrating consumer needs to lock idle drives so a stray
  eject can't disturb an unattended operation"* — which is **the user's robot
  workflow, already written into the roadmap as the motivating case.**

So the contract modification is auditable and small: append a dated ADR to
`AGENTS.md` ("controller verbs admitted, mechanism-facts-only") that names the
§1 reporter-only stance, states what changed (an orchestrating consumer now
exists), and explains why the new constraint dominates — the exact
append-don't-edit pattern `AGENTS.md` requires. The reporter contract for the
*query path* is untouched: `tray` is a distinct verb, the state machine never
issues a control command, and `mos_query_state` keeps its no-lock-on-ready
guarantee. The contract change is *additive*, not a reversal.

The one hard contract line that must be preserved verbatim: **mechanism facts
only.** `mos` reports what the command did and the gating level it observed;
it does *not* editorialise the lock as a safety interlock, and it does *not*
unmount for you (the deliberate contrast with `drutil tray eject`'s
unmount-then-eject *policy* — `ROADMAP.md:214`,
`doc/research/2026-06-10-drutil-contract.md`). For a robot orchestrator that
distinction is a feature: the orchestrator owns the bit lifecycle and the
unmount decision; `mos` exposes the primitive.

## Part 2 — eject / close: mechanism already exists, one API choice open

**The commands.** Both are 6-byte T10 CDBs `mos` does not yet author (HIGH —
SPC-4 / MMC-6, cross-checked against the §9.9 spec citations):

- **eject / close — START STOP UNIT (0x1B).** byte1 bit0 = IMMED; byte4 holds
  LoEj (bit1) + START (bit0). Eject = LoEj 1, START 0 → `byte4 = 0x02`. Close
  / load = LoEj 1, START 1 → `byte4 = 0x03`. This is byte-for-byte the
  sequence the macOS kernel's own `EjectTheMedia` issues (`ROADMAP.md:196` —
  unconditional `PREVENT_ALLOW(0)` then START STOP UNIT LoEj), which is why
  `eject --force` is honestly describable as "the OS's own eject, made
  explicit."
- **lock / unlock — PREVENT ALLOW MEDIUM REMOVAL (0x1E).** byte4 bits1:0 =
  Prevent field: `00b` allow, `01b` prevent, `10b`/`11b` persistent prevent.

**The mechanism is the GESN template, verbatim.** `mos` already authors one
raw CDB exactly this shape: `mos_internal_mmc_get_tray_state`
(`src/mos_scsi.c:465`) builds a fixed CDB array, calls `mos_raw_cdb()`
(`src/mos_scsi.c:814`), checks `task_status != kSCSITaskStatus_GOOD`, and
interprets the result. A `tray` verb is the same five lines with a 6-byte CDB
and a sense check instead of a payload decode. `mos_raw_cdb` already:
acquires `ObtainExclusiveAccess`, runs `ExecuteTaskSync`, copies out
`scsi_task_status` + 18-byte sense, and **releases the lock per call**
(`src/mos_scsi.c:911`). The `5/53/02` MEDIA REMOVAL PREVENTED refusal on a
locked eject arrives through that existing sense channel and is reportable —
*a reported fact, not an error to defeat* (`ROADMAP.md:195`).

**Which API surface — resolved on paper, not a hardware question.** §9.9 lists
this as "item 0," but it is settled by the headers alone; hardware only
confirms the chosen path executes. There are two ways to issue these, and the
masking trap forces the choice:

- The `MMCDeviceInterface` **convenience** method `SetTrayState(self,
  trayState)` needs no exclusive access — but shares `GetTrayState`'s
  structural blindness (`ARCHITECTURE.md:884`): no `SCSITaskStatus`/sense
  out-parameters, so a `5/53/02` locked-eject refusal is **structurally
  unreportable**. It documents `kIOReturnNotPermitted` for the media-inserted
  case, so *one* failure class surfaces via IOReturn, but the
  mechanism-facts-only contract cannot be honored through it.
- The **raw** START STOP UNIT / PREVENT ALLOW CDB needs exclusive access (so
  it is `MOS_ERR_BUSY` on a mounted disc — §3) but *can* surface the sense.

`ROADMAP.md:208` already records the verdict: the verbs "tilt toward raw CDBs
via the §4 transport" because `SetTrayState` cannot honor the contract and
`SetMediaAccessPermission` is absent from the 10.2.8 header (§9.9, second
bullet — confirm against the vendored modern `SCSITaskLib.h` in `docs/apple/`,
but if absent there too, lock/unlock are raw-only). This is consistent with
scope doctrine layer 1's rule: a new raw verb is admissible *with* (a) a
documented showing no convenience method carries the information — the
masking-trap analysis is exactly that showing — and (b) a nub-collision
analysis (Part 3).

## Part 3 — The prevent bits and the nub / mount interaction

The robot use case is the prevent (`0x1E`) verb, so its interaction with
`mos`'s exclusive-access invariant is the load-bearing analysis.

**Locking needs exclusive access, which is BUSY on a *mounted* disc.** The raw
path calls `ObtainExclusiveAccess`, which "returns `kIOReturnBusy` if the
media is still mounted" (`ARCHITECTURE.md:97`). So `mos` can set the prevent
bit only on a drive macOS has **not** mounted as a volume. For the robot
workflow this is mostly the *natural* fit, not a blocker:

- **Idle empty drive** (the arm is repositioning, no disc, or a disc just
  dropped in but not yet ripped): no IOMedia nub exists, the lock is free,
  `mos tray lock` succeeds. This is the canonical "lock the tray so a stray
  eject can't fire while the arm moves" moment.
- **Drive holding an unmounted disc** (UDF video discs commonly do not
  auto-mount; a ripper like MakeMKV takes the disc itself): lock is free —
  *unless another app already holds exclusive access*, in which case `mos`
  returns `MOS_ERR_BUSY`, an honest reported fact (§9.9 item 2; a rig could
  falsify the clean-BUSY expectation by revealing a stack that blocks instead).
- **Drive with a mounted data volume**: `ObtainExclusiveAccess` is BUSY, so
  `mos` *cannot* set prevent via the raw path, and `SetMediaAccessPermission`
  (the no-lock route that could) is not in the SDK. For this case the honest
  answer is `MOS_ERR_BUSY` + "unmount first (the consumer's call — `mos` does
  not unmount)." A robot orchestrator unmounts between stages anyway.

**The nub invariant still holds and must be re-cited, not assumed.** Scope
doctrine layer 1 requires a fresh nub-collision analysis for every new raw
verb. For prevent/eject it is simpler than GESN's: the GESN probe is safe
because it is reached *only* on the not-ready (unmounted) path (§5.5,
`tests/audit/nub_invariant_check.c`, 268M-input exhaustive proof). The tray
verbs are *user-initiated*, not gated on not-ready, so they don't inherit
that proof — instead they inherit the plain `ObtainExclusiveAccess` BUSY-on-
mounted guard above, which is the same kernel predicate from the other side.
The dangerous case the §5.5 proof rules out (grabbing the lock under a live
nub, which "temporarily removes the BSD block device" — `ARCHITECTURE.md:493`)
is exactly what the BUSY return prevents here. No new invariant is needed; the
existing one covers it because the lock acquisition itself is the gate.

**Persistent prevent (codes 2/3) is the genuinely robot-grade primitive.**
`ROADMAP.md:186-194`: at level 2/3 the tray is locked *and* a front-panel
button press is converted into a host-visible GESN **EjectRequest (Media
event code 1)** — the soft-eject protocol, "the drive asks; the orchestrator
decides." `mos` already parses the GESN Media class (the same decoder behind
the tray bit, `src/mos_scsi.c:505-511`), so surfacing `eject_requested`
through `--watch` is a natural extension once the verbs exist (event code
verified against Linux `sr.c` → `DISK_EVENT_EJECT_REQUEST`, `ROADMAP.md:191`).
For a ripping robot this is the ideal interlock: an operator pressing eject
during an unattended run doesn't fire the tray into the arm — it raises an
event the orchestrator can honor or defer.

## Part 4 — Does the lock survive `mos` returning? Spec and Apple source say yes.

This is the load-bearing question for the robot workflow, and the first draft
got its *epistemics* wrong: it presented a 50/50 fork for hardware to settle.
The hardware-role ADR forbids exactly that — hardware falsifies, it does not
decide. The question is answerable now from T10 + Apple's kext source, the
same canonical tier §5.5 (`PollForMedia`) and §9.7 (`GetTrayState`) already
stand on.

`mos`'s command discipline is **acquire-on-call / release-on-return**:
`mos_raw_cdb` releases exclusive access before returning on every path
(`src/mos_scsi.c:911,930`). A `tray lock` built the obvious way fires
`PREVENT ALLOW(01b)` and closes the exclusive session on return. The robot
needs the lock to *outlive* that return — to hold while the arm moves, seconds
to minutes. Three canonical facts establish that it does:

1. **SPC-4 clearing conditions (HIGH — T10, 04-349 SPC-3↔MMC-5 merge).**
   PREVENT ALLOW MEDIUM REMOVAL state is maintained **per I_T nexus**, and the
   device server clears it to its vendor-default ("allow") on **exactly five
   events: I_T nexus loss, logical-unit reset, hard reset, power on, and a
   persistent-reservation PREEMPT AND ABORT.** The LU keeps *prevent* and
   *persistent prevent* as two independent states, both on that same clear
   list. A command completing, a handle closing, and a process exiting are not
   on it.

2. **A `SCSITaskUserClient` close is none of those five (HIGH — IOKit
   architecture).** The I_T nexus is the kernel HBA-initiator-port ↔ drive
   target/LU relationship, owned by `IOSCSIProtocolServices` / the transport
   layer — *not* by the user client. It persists across user-client open and
   close. (The first draft's claim that "the `SCSITaskUserClient` *is* the
   nexus" was simply wrong: the user client is a userspace path onto the
   kernel's single initiator, not an initiator of its own.) Closing the client
   is neither an I_T nexus loss nor a logical-unit/hard reset, and nothing in
   `mos_raw_cdb`'s release path issues one.

3. **Apple's stack does not voluntarily unlock on release (HIGH — kext
   source).** This is the one place macOS could differ from spec by *choosing*
   to unlock, the way Linux does — `cdrom_release` calls `lock_door(cdi, 0)`
   on every close (`INTEGRATION_HARNESS.md:407`). It does not.
   `IOSCSIMultimediaCommandsDevice::HandleSetUserClientExclusivityState` — the
   function that runs when a user client takes or drops exclusive access —
   issues `kSCSIServicesNotification_Suspend` + `ResetMediaCharacteristics()`
   on release and **no `PREVENT_ALLOW_MEDIUM_REMOVAL`**. The kext's *only*
   unlock is inside `EjectTheMedia()`, which does
   `PREVENT_ALLOW_MEDIUM_REMOVAL(request, kMediaStateUnlocked, 0)` immediately
   before the eject — separately confirming the kernel's own eject is the
   unlock-then-eject sequence `ROADMAP.md:196` describes. Releasing exclusive
   access is not an eject and carries no unlock; `ResetMediaCharacteristics`
   resets the kext's *cached software* media facts, not the drive's prevent
   register.

**Derived conclusion (HIGH, falsifiable):** the prevent bit survives
`mos_raw_cdb`'s release-on-return and the handle close. `mos`'s existing
acquire-on-call / release-on-return model is therefore the *correct* model for
the robot lock — no held session, nothing blocked against Finder / MakeMKV /
DA for the lock window. `mos tray lock` is fire-and-forget; the tray stays
locked until an explicit `ALLOW`, a bus reset, or power-cycle. The
held-session lifecycle the first draft floated is not required.

**The persistence cuts both ways, and the contract already resolves it.**
Because the lock outlives the process, a `mos` that exits or is `kill -9`'d
mid-lock leaves the drive locked — the stale-lock hazard the lock-lifetime
note flags (`INTEGRATION_HARNESS.md:407-416`; `cdrom_id` opts out of the
kernel's lock policy with `CDO_LOCK` before taking over). For a *CLI* that is a
footgun; for the *robot* it is the entire point. Mechanism-facts-only resolves
it without `mos` picking a policy: `lock`/`unlock` are primitives, persistence
is a reported fact, and the **orchestrator owns the unlock**. `mos` must not
`atexit`-unlock a deliberately-persistent lock — that would defeat the robot
case. A plain CLI `lock` may carry an `atexit` ALLOW convenience; `lock
--persistent` does not, and the caller accepts the lifecycle (§1, consumer
derives suitability).

**Recovery is derivable too (HIGH).** A Mac presents a *single* initiator to
the drive (one HBA / USB-bridge port → one I_T nexus to the target), so a
stale lock left by a dead `mos` is clearable by *any* later `mos` on the same
host issuing `ALLOW` — same I_T nexus, and SPC-4 says that nexus's own ALLOW
clears its prevent state. The robot operator is never stuck waiting for a
power-cycle (unlike a multi-initiator SAN target): orchestrator restart →
`mos tray unlock` always recovers.

**What hardware can still falsify (not establish).** Per the hardware-role
ADR, the rig exists to *break* this derivation. Falsifiable predictions, each
spec-expected-good, each landing as a fixture + dated note with a *generic*
defense if it fails (the §5.5 / GESN pattern), never a per-device special-case:
(a) a non-conformant USB-SATA bridge that drops or resets the nexus on
user-client close would clear the lock early — `prevent=1; close; re-open;
eject succeeds` instead of `5/53/02`; (b) a drive whose firmware ignores
prevent and retracts the tray anyway (§9.9 item 3, firmware-dependent physical
behavior — note this is about the *mechanism*, the prevent *state* still reads
back); (c) `ObtainExclusiveAccess` contending with another holder (§9.9 item
2). None of these is the design *decision*; the design is spec-derived above.

## Part 5 — Concrete surface changes (scoped, no behavior shipped here)

What landing the verbs touches (HIGH — `mos` tree). Nothing below is large;
the list is for sizing, and the schema/ADR pieces are the disciplined parts:

1. **Public API** (`include/mos.h`): four functions, e.g.
   `mos_tray_eject(h, bool force)`, `mos_tray_close(h)`,
   `mos_tray_lock(h, bool persistent)`, `mos_tray_unlock(h)` — each returning
   `mos_error` plus an out-struct/accessors carrying the observed gating level
   and the raw sense (mechanism facts). No held-handle variant is needed — Part
   4 derives that the lock survives release. Append-only; ABI rules in the
   header preface apply.
2. **Implementation** (`src/mos_scsi.c`): two new internal CDB builders on the
   `mos_internal_mmc_get_tray_state` template, both routed through the
   existing `mos_raw_cdb`. No new lock primitive.
3. **CLI** (`cli/main.c`): the verb is **already reserved** — `"tray"` is in
   `reserved_subcommands[]` (`cli/main.c:22`) and currently returns the
   "reserved for v0.4, not yet implemented" diagnostic (`cli/main.c:232-240`).
   Wiring is a new `cli/tray.c` dispatch on `{eject, close, lock, unlock}` +
   `--force`/`--persistent` flags, mirroring `cli/metadata.c` / `cli/drive.c`.
4. **Schema** (`schemas/`): a new `mos.tray.v1` document type (the verbs
   report an *action outcome*, which no existing schema models). Per the
   JSON-schema ADR it is `additionalProperties:false` with positive +
   negative fixtures and the `validate.py` guard in the same commit. Because
   `mos` is pre-first-tag, `mos.tray.v1` is mutable-in-place until the
   freeze — no `v2` churn during development.
5. **ADR** (`AGENTS.md`): the dated append from Part 1 admitting controller
   verbs, mechanism-facts-only, naming the §1 stance it narrows.
6. **`raw_cdb` removal** (`ROADMAP.md:218`) is the *paired* v0.4 item — once
   the typed tray/capacity/speed verbs cover the diagnostic cases, the raw
   passthrough is legacy and its removal is the major-version justification.
   Not a prerequisite for the tray verbs; a consequence of them.

## Part 6 — Falsification targets (not "open questions"): spec-expected-good, what a rig could break

The design above is derived from spec, headers, and Apple source — it is not
waiting on hardware to be *decided*. `mos` has never run on a real drive
(`ROADMAP.md:8`), so each derived behavior is a *falsifiable prediction*; the
reference rig (BH16NS55 / WH16NS60) earns its keep only by *breaking* one. Per
the hardware-role ADR a break lands as a fixture + dated note with a generic
defense, never a per-device special-case. Reframing §9.9 + ROADMAP "Open
empirical questions" (`ROADMAP.md:239`) from questions-we-lack-answers-to into
predictions-with-a-derived-answer:

0. **API surface** (convenience vs raw) — *resolved on paper* (Part 2):
   `SetTrayState` is header-provably sense-blind, `SetMediaAccessPermission` is
   absent from the SDK header, so the verbs are raw. Hardware confirms the raw
   path executes; it cannot change the choice.
1. **Prevent bit survives `SCSITaskUserClient` close** — *spec-expected YES*
   (Part 4: SPC-4 five-event clear list + no voluntary kext unlock).
   Falsified only by a non-conformant bridge that drops the nexus on close.
2. **Exclusive-access contention** with another holder (a ripper mid-job) —
   spec-expected `kIOReturnBusy` → `MOS_ERR_BUSY`, an honest reported fact.
   Hardware could reveal a stack that blocks or deadlocks instead.
3. **prevent=1/3 physically retracts the tray** — firmware-dependent *physical*
   behavior; the prevent *state* still reads back per spec, but whether a
   given mechanism honors it mechanically is the one genuinely
   hardware-resident fact here (§9.9 item 3). This is the weakest derivation
   and the most worth a capture.
4. **Can something privileged still force the tray** during the lock window
   (`DADiskEject` / `drutil` / a host `PREVENT ALLOW(00b)` from another
   client) — directly relevant to "keep *anything* from ejecting." Spec-
   expected: a host `ALLOW(00b)` on the same single-initiator nexus *does*
   clear the lock (Part 4 recovery corollary), so the guarantee is "no stray
   eject without an explicit host unlock," not "physically un-ejectable." That
   is the honest contract to document; the persistent (`10b/11b`) levels add
   the cooperative-button (EjectRequest) behavior on top, not host-override
   resistance. A rig clarifies the per-drive button matrix; it does not change
   this derived guarantee.

**Privilege footprint is unchanged** (scope doctrine layer 3): the raw tray
verbs need exactly the `SCSITaskUserClient` exclusive-access grant `mos`
already uses for GESN — no root, no entitlement, no TCC, no block-device I/O.
The command surface stays MMC/T10; no SPC ambition; the one-raw-CDB rule
becomes one-of-three raw CDBs, each with its layer-1 justification recorded.

**Privilege footprint is unchanged** (scope doctrine layer 3): the raw tray
verbs need exactly the `SCSITaskUserClient` exclusive-access grant `mos`
already uses for GESN — no root, no entitlement, no TCC, no block-device I/O.
The command surface stays MMC/T10; no SPC ambition; the one-raw-CDB rule
becomes one-of-three raw CDBs, each with its layer-1 justification recorded.

## Recommendation

Proceed, and build to the spec-derived design — do not hold any of it hostage
to hardware. The contract modification is a pre-decided, additive ADR append,
not a reversal; the eject/close/lock/unlock verbs are the GESN raw-CDB template
with 6-byte CDBs and a sense check; the CLI verb is already reserved and the
schema/ADR work is routine pre-tag discipline. Build the four verbs
mechanism-facts-only on the raw-CDB path (the masking trap forecloses the
convenience route for honest sense).

The robot's "lock it and walk away" requirement is **satisfied by the
spec-derived design, not pending a capture**: SPC-4's five-event clear list
plus Apple's no-voluntary-unlock-on-release path (Part 4) put the prevent bit's
survival across handle close on the same canonical footing as the §5.5 nub
invariant — derived from T10 + the kext, falsifiable by a non-conformant
bridge, not established by "it worked on my drive." Document the lock as
fire-and-forget with consumer-owned unlock and the single-initiator recovery
path. The reference rig's job is the genuinely hardware-resident residue:
whether a given *mechanism* physically honors prevent (Part 6 item 3) and the
per-drive cooperative-button matrix — both fixtures-and-falsification, neither
a design input.

## Sources
- `mos` tree: `ARCHITECTURE.md` §1, §3, §4.2, §5.5, §9.7, §9.9; `ROADMAP.md`
  ("Now — v0.4", "Open empirical questions"); `AGENTS.md` (scope doctrine
  layer 1, hardware-role ADR); `INTEGRATION_HARNESS.md` (lock-lifetime
  discipline :407-416); `doc/research/2026-04-26-hlds-silicon-and-mmc.md:212`
  (persistent-prevent initiator semantics); `src/mos_scsi.c`
  (`mos_internal_mmc_get_tray_state` :465, `mos_raw_cdb` :814, lock release
  :911/:930); `cli/main.c` (reserved-verb wiring :18-32, :232-240);
  `include/mos.h` (raw-CDB + ABI contract); `schemas/` (JSON-schema ADR,
  `validate.py`).
- **T10 SPC-4 / 04-349r1** (SPC-3↔MMC-5 PREVENT ALLOW merge): prevent +
  persistent-prevent are per-I_T-nexus states cleared only on I_T nexus loss,
  logical-unit reset, hard reset, power on, or PREEMPT AND ABORT.
  https://www.t10.org/ftp/t10/document.04/04-349r1.pdf — and the I_T-nexus-loss
  task-management definition, 04-008r0 / 04-372r0
  (https://www.t10.org/ftp/t10/document.04/04-008r0.pdf).
- **T10 SPC-4 / MMC-6 CDBs:** START STOP UNIT (0x1B) LoEj/START; PREVENT ALLOW
  MEDIUM REMOVAL (0x1E) Prevent field 00/01/10/11; sense 5/53/02 MEDIA REMOVAL
  PREVENTED; GET EVENT STATUS NOTIFICATION Media event code 1 (EjectRequest).
- **Apple `IOSCSIArchitectureModelFamily`** (`IOSCSIMultimediaCommandsDevice.cpp`):
  `EjectTheMedia` issues `PREVENT_ALLOW_MEDIUM_REMOVAL(…, kMediaStateUnlocked, 0)`
  before eject; `HandleSetUserClientExclusivityState` issues
  `kSCSIServicesNotification_Suspend` + `ResetMediaCharacteristics()` on
  exclusive-access release with no `PREVENT_ALLOW_MEDIUM_REMOVAL` — i.e. no
  voluntary unlock on close (contrast Linux `cdrom.c` `cdrom_release` →
  `lock_door(0)`). Mirror: https://github.com/aosm/IOSCSIArchitectureModelFamily
- Cross-references already in-tree: Apple `IOSCSIMultimediaCommandsDevice`
  (`EjectTheMedia`, `GetTrayState` "Assume the tray is shut"); Linux `sr.c`
  (`DISK_EVENT_EJECT_REQUEST`); `doc/research/2026-06-10-drutil-contract.md`
  (the unmount-then-eject *policy* contrast).
