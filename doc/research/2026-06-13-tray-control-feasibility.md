# Tray control (eject / close / prevent) in `mos`: feasibility report (June 2026)

Full-session research report. **Question (user):** is it feasible to modify
`mos`'s current contract and implement tray-control eject + close and the
prevent-eject bits — specifically for a *ripping-robot* workflow, where
preventing ejection keeps a tray from popping open while a robot arm moves
around the workspace? **Method:** verification of the already-drafted v0.4
tray design against the actual tree — the scope doctrine (`AGENTS.md` layer 1),
the contract stance (`ARCHITECTURE.md` §1), the masking-trap analysis
(§9.7/§9.9), the single-raw-CDB mechanism (`src/mos_scsi.c`), and the CLI's
reserved-verb wiring (`cli/main.c`). T10 CDB layouts cross-checked against the
in-repo spec citations. No `src/`, schema, or ADR changes; this file is the
deliverable.

Confidence per claim: HIGH = in-repo source or T10/MMC spec quoted; MEDIUM =
inferred from spec + Apple/Linux source, not yet validated on a Mac (`mos` has
never run on real hardware — `ROADMAP.md:8`).

## Verdict

**Feasible, and already the planned v0.4 surface — with one workflow-specific
crux that a single hardware test resolves.** Nothing here requires inventing a
new architecture: the contract change is pre-decided, the command mechanism
already exists (the GESN raw-CDB path), and the CLI verb is already reserved.
The eject/close/lock/unlock verbs are routine. The *robot* requirement —
"keep the tray locked while the arm moves" — turns entirely on **one open
question that is the #1 hardware gate in the tree already: does the prevent
bit survive `SCSITaskUserClient` close** (`ARCHITECTURE.md:906`, §9.9 item 1):

- **If it survives** (prevent state held in drive firmware until reset /
  power-cycle / explicit unlock): the robot workflow is a *perfect* fit for
  `mos`'s existing acquire-on-call / release-on-return model. `mos tray lock`
  fires one CDB, returns, and the tray stays locked with no process held open.
  This is the happy path and it costs almost nothing beyond the verbs.
- **If it clears on last close**: a persistent lock requires `mos` to **hold
  the exclusive handle open** for the whole locked window — a new lifecycle
  the architecture deliberately avoids, which also blocks Finder / MakeMKV /
  DiskArbitration from the drive for the duration and drops the lock on
  process death (exactly the "cleanup-on-process-death obligation" §1 names
  as the reason control verbs were deferred).

Both are buildable; they are different products. The test that decides which
one the user gets is `prevent=1; close handle; bare eject → does it refuse
with 5/53/02?` on the reference rig. **Recommendation: build the verbs as
designed, and gate the *persistent-lock UX promise* on that one capture.**

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

**The one genuinely open implementation axis — which API surface (§9.9
item 0).** There are two ways to issue these, and the choice is forced by the
masking trap, not preference:

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
  returns `MOS_ERR_BUSY` (§9.9 item 2 — contention with another holder is a
  hardware-gated unknown).
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

## Part 4 — The robot-specific crux: does the lock persist after `mos` returns?

This is the finding that matters most for *this* workflow and the one place
the existing design and the user's requirement meet a real fork.

`mos`'s entire command discipline is **acquire-on-call / release-on-return**:
`mos_raw_cdb` releases exclusive access before returning on every path
(`src/mos_scsi.c:911,930`), so "a diagnostic does not leave the drive blocked
against Finder / DiskArbitration." A `tray lock` built the obvious way fires
`PREVENT ALLOW(01b)` and then **closes the exclusive session on return.** The
robot needs the lock to *outlive* that return — to hold while the arm moves,
which could be seconds to minutes. Whether it does is `ARCHITECTURE.md:906`,
§9.9 item 1, verbatim: *"does the prevent bit survive `SCSITaskUserClient`
close — some stacks clear it on last close, which would force a held session
instead of the fast open-CDB-close model."*

Two outcomes, two products (MEDIUM — this is the central unverified axis):

| | Prevent **survives** close | Prevent **clears** on close |
|---|---|---|
| Lock model | open-CDB-close, fire-and-forget | held exclusive session for the lock window |
| Fits `mos` today | yes — same as every other verb | no — new lifecycle |
| Blocks Finder/MakeMKV/DA while locked | no | **yes**, for the whole window |
| Survives `mos` process death | yes (cleared only by reset / unlock) | no — lock drops instantly |
| Robot UX | `mos tray lock` then walk away | a long-lived `mos` process must babysit the lock |

T10 says the prevent state is maintained per I_T nexus; on macOS the
`SCSITaskUserClient` *is* the nexus, so whether closing it tears down the
nexus (and the prevent state with it) is precisely the stack-specific
behavior §9.9 flags. Linux's `sr`/`sg` keep the device open to hold the lock,
which is weak evidence (MEDIUM) for the "clears on close" branch — but it is
not dispositive for Apple's SAM stack, and persistent-prevent codes `10b/11b`
exist in the spec *specifically* to be more durable than `01b`, so the
survival question must be asked per prevent-level, not once.

**This is a one-capture question**, and it is already on the hardware gate
(INTEGRATION_HARNESS.md). Concretely: `prevent(01b)` → release handle →
re-open → bare `eject` → observe `5/53/02` or a tray that opens. Repeat for
`10b`/`11b`. Per the hardware-role ADR (`AGENTS.md`), the capture *falsifies
or feeds* the design; it does not steer it — the verbs ship built-to-spec, and
the UX promise ("walk away, the tray stays locked") is what waits on the
result. If the answer is "clears on close," the held-session model is a
documented `mos_tray_lock`/`mos_tray_unlock` *handle pair* whose lifetime is
the lock — a clean addition, but one whose contention and process-death
semantics must be documented up front.

## Part 5 — Concrete surface changes (scoped, no behavior shipped here)

What landing the verbs touches (HIGH — `mos` tree). Nothing below is large;
the list is for sizing, and the schema/ADR pieces are the disciplined parts:

1. **Public API** (`include/mos.h`): four functions, e.g.
   `mos_tray_eject(h, bool force)`, `mos_tray_close(h)`,
   `mos_tray_lock(h, bool persistent)`, `mos_tray_unlock(h)` — each returning
   `mos_error` plus an out-struct/accessors carrying the observed gating level
   and the raw sense (mechanism facts). If Part 4 lands on "clears on close,"
   add the held-handle variant. Append-only; ABI rules in the header preface
   apply.
2. **Implementation** (`src/mos_scsi.c`): two new internal CDB builders on the
   `mos_internal_mmc_get_tray_state` template, both routed through the
   existing `mos_raw_cdb`. No new lock primitive (unless held-session).
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

## Part 6 — What blocks *shipping* (not design): the hardware gates

`mos` has never run on a real drive (`ROADMAP.md:8`); every line below is
off-Mac supposition until the reference rig (BH16NS55 / WH16NS60) confirms it.
Per the hardware-role ADR these gate the *ship*, not the *build* — the verbs
are written to spec and the rig falsifies or feeds them. From §9.9 + ROADMAP
"Open empirical questions" (`ROADMAP.md:239`):

0. **Which API surface** each verb actually needs (convenience vs raw) — Part
   2; the masking-trap analysis answers it on paper, hardware confirms.
1. **Does the prevent bit survive `SCSITaskUserClient` close** — Part 4, the
   workflow-determining one.
2. **Does exclusive access contend** with another app holding the drive
   (a ripper mid-job).
3. **Does prevent=1/3 physically keep a given model's tray retracted** —
   firmware-dependent; the persistent-prevent behavior matrix per drive.
4. **Does exclusive access block `DADiskEject` / `drutil`**, or is there a
   privileged override (can something *else* still force the tray during the
   lock window — directly relevant to "keep anything from ejecting").

Item 4 is worth flagging for the robot specifically: a lock that the host OS
or a privileged tool can override is a weaker guarantee than one held in drive
firmware. The persistent-prevent (`10b/11b`) levels exist to make the
*physical button* cooperative; whether they also resist a host-issued
`PREVENT ALLOW(00b)` from another client is item 4's job to establish.

**Privilege footprint is unchanged** (scope doctrine layer 3): the raw tray
verbs need exactly the `SCSITaskUserClient` exclusive-access grant `mos`
already uses for GESN — no root, no entitlement, no TCC, no block-device I/O.
The command surface stays MMC/T10; no SPC ambition; the one-raw-CDB rule
becomes one-of-three raw CDBs, each with its layer-1 justification recorded.

## Recommendation

Proceed. The contract modification is a pre-decided, additive ADR append, not
a reversal; the eject/close/lock/unlock verbs are the GESN raw-CDB template
with 6-byte CDBs and a sense check; the CLI verb is already reserved and the
schema/ADR work is routine pre-tag discipline. Build the four verbs to spec
now, mechanism-facts-only, raw-CDB path (the masking trap forecloses the
convenience route for honest sense).

Hold exactly one promise hostage to hardware: **the persistent-lock UX for
the robot** — "lock the tray and walk away while the arm moves" — depends on
the prevent bit surviving handle close (§9.9 item 1). Run that single capture
on the reference rig before documenting the lock as fire-and-forget; if it
clears on close, ship the held-session handle pair instead and document its
contention + process-death semantics. Either way the robot is served; the test
only decides which lifecycle the docs promise.

## Sources
- `mos` tree: `ARCHITECTURE.md` §1, §3, §4.2, §5.5, §9.7, §9.9; `ROADMAP.md`
  ("Now — v0.4", "Open empirical questions"); `AGENTS.md` (scope doctrine
  layer 1, hardware-role ADR); `src/mos_scsi.c` (`mos_internal_mmc_get_tray_state`
  :465, `mos_raw_cdb` :814, lock release :911/:930); `cli/main.c`
  (reserved-verb wiring :18-32, :232-240); `include/mos.h` (raw-CDB + ABI
  contract); `schemas/` (JSON-schema ADR, `validate.py`).
- T10 SPC-4 / MMC-6: START STOP UNIT (0x1B) LoEj/START; PREVENT ALLOW MEDIUM
  REMOVAL (0x1E) Prevent field 00/01/10/11; sense 5/53/02 MEDIA REMOVAL
  PREVENTED; GET EVENT STATUS NOTIFICATION Media event code 1 (EjectRequest).
- Cross-references already in-tree: Apple `IOSCSIMultimediaCommandsDevice`
  (`EjectTheMedia`, `GetTrayState` "Assume the tray is shut"); Linux `sr.c`
  (`DISK_EVENT_EJECT_REQUEST`); `doc/research/2026-06-10-drutil-contract.md`
  (the unmount-then-eject *policy* contrast).
