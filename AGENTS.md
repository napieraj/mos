# AGENTS.md

`mos` (mac-optical-state) — pure-C macOS optical drive state library. 0BSD.
Public API in `include/mos.h`, opaque handle. Repo:
`github.com/napieraj/mos`. Binary and repo `mos`, project name
`mac-optical-state`. Both names are correct.

For build, layout, and contribution flow, see `README.md` and
`CONTRIBUTING.md`. For Claude-specific failure modes and process rules
documented during v0.1–v0.2 development, see `CLAUDE.md`.

## Decisions that look wrong but aren't

ADR-style entries. These were correct under the constraints in force
when written; constraints drift. If you believe one no longer applies,
**don't edit the historical entry** — append a new dated entry that
argues against the original on the merits. Naming the prior decision,
stating what changed, and explaining why the new constraint dominates
forces the override to be auditable rather than silent. An entry from
2026-04-22 read in 2027 is not load-bearing on its own; the chain of
appends is.

A bare timestamp without the argument is just a date on a folk
ritual. The append must rebut.

### Opaque `mos_device_info_t` + callback enumeration
Date: 2026-04-22

Your first instinct will be `drives[16]; mos_enumerate(...)`. That
doesn't compile. Intentional — ABI stability across versions. The
callback pattern is shown in the `mos.h` docstring.

### Binary `mos`, repo `mac-optical-state`
Date: 2026-04-22

You will want to rename one to match the other for "consistency" —
don't. Both correct. Binary gets the ergonomic name; repo keeps the
descriptive one. CMake target name and Homebrew formula match the
binary. README title and `project()` id keep the repo name.

### Single-retry on UNIT ATTENTION
Date: 2026-04-22

You will want to escalate the single retry to a bounded loop because
the 2-3-stack case looks unhandled — don't, not in v0.2. Source
comment acknowledges the 2-3-stack case explicitly. Escalation to a
bounded retry loop is a v0.3 decision contingent on observed hardware
behavior, not a v0.2 fix. Don't preemptively expand.

### No UA retry at all (supersedes "Single-retry on UNIT ATTENTION")
Date: 2026-06-10

The 2026-04-22 entry above describes a retry that no longer exists.
The 2026-05-30 state-detection redesign removed the UA drain entirely:
TEST UNIT READY is issued exactly once per query (`mos_state_core.c`),
and a stray UNIT ATTENTION is taken at face value and classified to
`unknown` (`ARCHITECTURE.md` §4.1, §7). What changed: the macOS peers
(drutil, cdrom_id-equivalents) demonstrably rely on the kernel's own
device initialization having consumed the power-on / reset /
media-change UA before a userspace client ever holds a handle, so an
application-level retry was re-armoring a layer that is already
armored — and it complicated the single-shot decision tree the GESN
redesign needed. The old entry's advice ("don't expand to a loop")
is now vacuously satisfied; the thing not to do today is reintroduce
*any* retry. If hardware capture ever shows stacked UAs surviving to
mos's first TUR, that evidence goes in a new dated entry here before
any code changes.

### CMake floor 12.0, Homebrew floor 14
Date: 2026-04-22

You will want to bump the CMake floor to match Homebrew for
"consistency" — don't. Library technically builds on Monterey
(`kIOMainPortDefault`). Homebrew distribution policy is a separate
axis: `:sonoma`. Both floors are correct for their respective scopes;
collapsing them removes a real capability (building on older
toolchains for non-Homebrew consumers).

### Homebrew floor aligned to Monterey (supersedes the entry above)
Date: 2026-06-10

The second external review pass (CHANGELOG 2026-05-14, finding 9)
found the `:sonoma` Homebrew floor excluded Monterey/Ventura users for
no reason — the library builds and runs on 12.0. `homebrew/mos.rb` now
declares `depends_on macos: :monterey`, matching the CMake
`CMAKE_OSX_DEPLOYMENT_TARGET 12.0` floor. The two-axes argument above
was correct in form (distribution policy *is* a separate axis) but the
specific `:sonoma` value had no justification behind it. Today both
floors point at Monterey; if they ever diverge again, the divergence
needs a reason recorded here, not just "consistency" in either
direction.

## Process

1. If a test fails, the test is right; the library is wrong. Don't
   rewrite the test to make it pass.
2. Don't change behavior based on a review alone. Review points become
   comment updates or v0.next flags until hardware validation is in.
   See `CLAUDE.md` → "Over-doing what was asked" for
   the behavioral pattern this rule prevents.
3. Hardware validation requires a real Mac with a real optical drive
   attached. The pure-data tests (sense parsing, CDB layouts, BSD-name
   normalization, status classification, string tables) link only
   against `mos_pure`, which has no Apple framework dependency — the
   v0.2 split made `mos_core` (IOKit shell) sit on top of `mos_pure`
   so the test binary genuinely compiles and runs without IOKit. CI
   runs on `macos-latest`; non-macOS execution of the pure tests is
   feasible without further work and the symbol-hygiene check covers
   both archives.

## ADR: hardware's role is falsification and fixtures, never design input

**Date:** 2026-06-10. **Status:** active.

**Decision.** Code is built to spec (T10/MMC), to in-repo fixtures, and
to verified platform source (Apple OSS kernel, vendored headers).
Hardware runs exist for exactly two purposes: (a) **falsifying axioms
no spec governs** (the §5.5 interleaving slivers, DR array ordering),
and (b) **capturing hex that becomes committed fixtures**. A surprise
observed on a drive never changes behavior directly — it lands as a
`.bin` fixture (the `getconfig_dvdrom_current.bin` pipeline), the pure
layer is built to the fixture, and the defense is expressed
generically (validity gates keyed on the reply's own length fields),
never special-cased to the device that produced it.

**Why.** This is already the repo's revealed pattern — the GESN
trust-the-reply's-own-length rule and the profile-only-on-READY
suppression are both bridge/firmware observations generalized into
spec-citable gates. Stating it as doctrine closes the remaining leak
path: without it, a hardware-validation pass invites exactly the
device-quirk special-casing that makes optical stacks unmaintainable
(every legacy burner tool is a fossil record of this failure mode).
It also keeps Process rule 3 honest: "hardware validation" means the
matrix can *refute* us or *feed* us, not steer us.

**Consequences.** The hardware validation gate (INTEGRATION_HARNESS.md;
lived in STATUS.md until its 2026-06-11 retirement to doc/history/) is
scoped to falsification + fixture-acquisition runs. Quirk findings reach `src/` only through
committed hex with a dated fixture README entry. If a falsification
run fails, the deliverable is the captured sense/timeline as a
fixture — not a behavior tuned to the failing drive.

## ADR: JSON output schema evolution policy

**Decision:** schema versioning is per-document-type, not per-CLI-
invocation. Each emitted JSON document carries a `schema` field with a
name and version: `mos.state.v1` (one-shot success), `mos.error.v1`
(failure envelope), `mos.list.v1` (`list --json`), `mos.event.v1`
(`watch`). Within a schema's current version the **field set
is closed**: the published schemas declare `additionalProperties:
false`, so any field addition — like removals, renames, type changes,
and semantic changes — requires a new schema name (e.g. `mos.state.v2`)
with the v1 emit path preserved for a deprecation window. Consumers may
rely on the closed key set; a v1 consumer never sees a key the v1
schema doesn't name. The one open axis within a version is the `state`
/ `prev_state` **enum**: additive state values are forward-compatible
(the C↔schema drift guard in `schemas/validate.py` keeps the enum in
lockstep with `mos_state_description()`), and consumers MUST treat an
unknown `state` string as equivalent to "unclassifiable."

(Revised 2026-06-10: an earlier version of this ADR permitted additive
fields within a version and required consumers to ignore unknown keys.
That contradicted the published schemas, which had declared the field
set fixed since they landed — and the schemas, being machine-enforced
and already public, win. Closed-by-default is also the stronger
contract: it is what makes the negative-fixture suite meaningful.)

The `--json` CLI flag takes no argument. Passing `--json=anything`
returns `EX_USAGE` (64) with a diagnostic naming `mos.state.v1`.
Per-CLI-invocation pinning (the `--json=v2`/`--json=v3` mechanism in
the v2-lock design) is not needed because the schema name on each
document already carries the version: consumers parse `.schema` and
dispatch on it.

**Why per-document rather than per-CLI-invocation:** a single `mos`
invocation can emit different schemas. `watch` produces a
stream of `mos.event.v1` lines that may include `mos.error.v1`
records for transient failures, and one-shot calls produce
`mos.state.v1` or `mos.error.v1` depending on outcome. Per-document
tagging lets a heterogeneous-stream consumer dispatch by `.schema`
without inferring from field presence. Per-CLI versioning cannot
express this cleanly. The per-document `schema` field follows the
JSON Schema `$schema` convention and the protobuf practice of
carrying type identity inside each message.

**What this means in practice for contributors:**

- New field on any envelope: schema-name bump (e.g. `mos.state.v2`),
  same as any other shape change — the v1 field set is closed
  (`additionalProperties: false`). Update the README contract section
  and add positive + negative fixtures in the same commit.
- Rename / drop / type change / semantic change on an existing
  field: forbidden within current version. Goes into a new schema
  name with the v1 emit path preserved for a deprecation window.
- New `state` enum value: fine under `mos.state.v1`. Forward-compat
  rule explicitly tells parsers to handle unknown state strings as
  "unclassifiable."
- New error code value (new `mos_error` enum variant surfaced
  through `mos_cli_error_to_code`): fine under `mos.error.v1`. Same
  rationale; `.error.code` is a string the consumer pattern-matches
  against.

**Revision 2026-06-10 — when the freeze begins.** The closed-field-set
rule above governs SHIPPED schemas. Before the first tagged release
there are zero external consumers, so v1 documents are mutable in
place: shape changes land in `mos.*.v1` directly, with schema, example
+ negative fixtures, emitter, and docs updated in the same commit (the
CI validation suite is the consumer until a real one exists). Exercised
the same day: `mos.event.v1` replaced the composite `stream_id` string
with `registry_id` + `stream_open_ms`. The freeze — and with it every
rule above — takes effect at the first tag that ships the schema.

## Scope doctrine: command surface vs input space (2026-06-10)

Three layers, so "are we drifting into general SCSI" never needs
re-deriving:

1. **Command surface: MMC only, kernel-authored by default.** mos does
   not build CDBs. TUR, GET CONFIGURATION, and (v0.4) READ DISC
   INFORMATION go through the `MMCDeviceInterface` convenience methods —
   Apple's optical kext constructs, dispatches, and retries them. mos
   authors exactly **one** raw CDB in the entire codebase: GESN 0x4A,
   and only because the convenience `GetTrayState` is
   information-destroying (it hard-codes closed+success on any failure —
   ARCHITECTURE §9.7). That is the template for any future raw verb: a
   raw CDB requires (a) a documented showing that no convenience method
   can carry the needed information, and (b) the nub-collision analysis,
   because raw means `ObtainExclusiveAccess` and the exclusive lock is
   the only reason the §5.5 invariant exists at all. Convenience methods
   never take the lock; every new raw verb re-raises it. Future tray
   verbs (eject/load, ROADMAP) follow the same rule — convenience first,
   raw only with the GESN-grade justification.

2. **No SPC ambition.** No mode pages, no log pages, no reservations, no
   classification of sense semantics beyond what the optical decision
   tree needs. The verification oracle is Apple's optical kext
   (`IOSCSIMultimediaCommandsDevice`), never SAM/SPC in the abstract.

3. **Privilege footprint: the SCSITaskUserClient console grant, and
   nothing more.** mos never requires elevation beyond what the
   platform's attach rule already gives a console user on a burner —
   no root, no operator group, no TCC Full Disk Access. Concretely:
   no block-device I/O (`/dev/rdiskN` reads are a third I/O modality
   AND a privilege acquisition; both disqualify). Data that needs more
   privilege than mos holds is the CONSUMER's to obtain AND to parse —
   anyone doing filesystem-level reads owns their own filesystem
   parsing. mos does not ship parsers for bytes it refuses to read
   (a public PVD parser was shipped and struck the same session,
   2026-06-10: zero internal call sites, and its only imagined
   consumers are tools already deep in makemkvcon territory).

4. **Input space: the full octet domain, adversarial.** MMC bounds what
   a conforming drive emits; it bounds nothing about what bytes arrive
   through a crossflashed drive behind a USB-SATA bridge. Response
   parsing and the nub gate are therefore verified over the entire
   status × key × ASC × ASCQ space (`tests/audit/nub_invariant_check.c`)
   — and the correct handling of unclassifiable bytes is LESS action,
   not more: no lock, no probe, `UNKNOWN`. MMC defines the command
   surface; nothing defines the input space.

### Scope-doctrine addendum: the DR pivot does not change the command
### surface (2026-06-10)

The DiscRecording substrate (directory/doorbell — enumeration,
identity, addressing, watch wake/lifecycle) landed without touching
layer 1: mos still authors exactly one raw CDB (GESN), the TUR⊕GESN
state core is untouched, and DR is not a command author — it is a
framework above the same kext the MMC path uses. Two dependency-axis
changes for the record: DiskArbitration left the library entirely
(the watch's DA wake retired; the notification probe keeps DA legs as
the falsification control arm), and DiscRecording joined the link
line. Layer 3 (privilege footprint) is unchanged — DR's
device-status reads take no entitlements, no TCC, no exclusive
access.

### Addendum: DA retired entirely; probes consolidated into `mos probe`
### (2026-06-11, rebuts the "keeps DA legs" clause above)

The 2026-06-10 entry's parenthetical — "the notification probe keeps
DA legs as the falsification control arm" — no longer holds. What
changed: the standalone probes were consolidated into the `mos probe`
subcommand (cli/probe.c, `MOS_CLI_PROBE` default ON; the frozen record at
doc/history/2026-06-10-dr-pivot-decision-record.md
has the full argument) so they stop drifting outside the CLI's
contract tests, and the DA legs were retired with the move rather
than carried. Why the control arm goes: it existed to let a hardware
session compare DR doorbell delivery against DA's, but no design
decision consumes that comparison — doorbells are latency-only over
the poll floor (the kernel itself polls media at 1000 ms), so
doorbell completeness has nothing to falsify; the probe's own
monotonic timestamps against the poll cadence are the latency
reference. DiskArbitration therefore leaves the project's dependency
story completely: no target links it. Command surface, privilege
footprint, and the one-raw-CDB rule are all unchanged by the
consolidation.

### Addendum: DA re-admitted for exactly one synchronous call
### (2026-06-12, narrows — does not reverse — the entry above)

The 2026-06-11 retirement was about DA's CALLBACK modality: the
watch's wake legs and the probe's falsification control arm. Those do
not return. What re-linked DiskArbitration is the metadata path's
mounted-volume lookup (`mos_internal_da_volume`, mos_da.c): a one-shot
`DADiskCopyDescription` read — no session scheduling, no run loop, no
callbacks, no commands to the drive — gated on the media nub existing
(bsd_unit present; no IOMedia node means nothing is mounted and DA is
never consulted). Why DA and not an alternative: the mounted volume's
name/path belong to the filesystem layer, which mos refuses to read
from sectors (scope doctrine layer 3); DA is the platform's published
answer for "what does the mount layer say about this disk", and the
description read takes no entitlements, no TCC, no elevation. If a
future need looks like DA callbacks again, that is a new argument to
make here — this entry authorizes only the synchronous description
read. Command surface, privilege footprint, and the one-raw-CDB rule
remain unchanged.

### Addendum: read-only MODE SENSE of optical pages admitted
### (2026-06-13, narrows layer 2's "no mode pages")

Layer 2 above reads "No SPC ambition. No mode pages, no log pages, no
reservations…". The state-enrichment work (doc/research/2026-06-13-disc-tools-state-survey.md,
maintainer-approved) admits **two read-only MODE SENSE reads**, and this
entry records why the carve-out does not reopen the ambition the clause
forecloses.

What is admitted: `ModeSense10` (the `MMCDeviceInterface` convenience
method — `ARCHITECTURE.md:834` lists it alongside the other non-exclusive
wrappers) of exactly two **optical-specific** pages:
- **page 0x2A** (CD/DVD/BD Capabilities & Mechanical Status) — loading-
  mechanism type, lock support, the live media-locked bit, and buffer
  size: state GET CONFIGURATION and GESN structurally cannot carry; and
- **page 0x01** (Read/Write Error Recovery) — the drive's read error-
  recovery configuration (AWRE/ARRE/PER/DCR, read-retry count), read-only.

Why this is not the drift the clause guards against. (a) The verification
oracle is unchanged — both pages are decoded against MMC, the optical
spec, exactly as the existing decoders are; this is not SAM/SPC "in the
abstract." (b) It is convenience-method, not raw — no `ObtainExclusiveAccess`,
no §5.5 exposure, the one-raw-CDB count stays at one (GESN). (c) It is
the layer-1 preferred form: a kernel-authored read, not a CDB mos builds.

What remains foreclosed, and is the load-bearing half of the clause: **no
MODE SELECT** (mos reports configuration, it never tunes the drive — the
mutation dvdisaster/sdparm perform stays out); no SPC-generic pages
(power, caching, control-mode, informational-exceptions — nothing whose
subject is "a SCSI device" rather than "an optical drive"); no log pages
(LOG SENSE counters remain hardware-capture-first, not a design input);
no reservations. The intent of "no SPC ambition" — that mos classifies
only what the optical decision/identity tree needs and never grows a
general SCSI introspection surface — is intact. If a future page request
is not optical-specific and read-only, it is a fresh argument to make
here, not covered by this entry.

## ADR: controller verbs admitted — tray (eject/close/lock/unlock)
## (2026-06-13, narrows ARCHITECTURE §1 "reporter, not controller")

`ARCHITECTURE.md:20` reads "mos is a state reporter, not a state
controller." This entry records that the tray verbs — the v0.4 surface §1
itself already carved out (`mos tray {eject, close, lock, unlock}`) — are
admitted, and why that is an additive narrowing, not a reversal of the
clause.

**What changed.** An orchestrating consumer now exists: a ripping-robot
workflow needs to lock an idle drive so a stray operator eject can't fire
the tray into a moving arm, and to eject/close between stages. That is the
motivating case `ROADMAP.md:179-216` wrote down, and the feasibility
analysis (doc/research/2026-06-13-tray-control-feasibility.md) verified the
design against the tree + canonical sources (T10 04-349r1, Apple's
IOSCSIMultimediaCommandsDevice). The maintainer green-lit implementation
the same day.

**Why additive, not a reversal.** The *query path* is untouched:
`mos_query_state` still issues no control command and keeps its
no-lock-on-ready guarantee; `tray` is a distinct verb. The one hard line
preserved verbatim is **mechanism facts only** — mos reports what the
command did (the `outcome`: done / refused_locked / refused_other) and does
**not** unmount for you (the deliberate contrast with `drutil tray eject`'s
unmount-then-eject *policy*). A `5/53/02` locked-eject refusal is a reported
fact, not an error to defeat.

**Scope-doctrine compliance (layer 1).** The verbs are authored as raw CDBs
(START STOP UNIT 0x1B, PREVENT ALLOW MEDIUM REMOVAL 0x1E) with the two
showings layer 1 requires: (a) the convenience `SetTrayState` is structurally
sense-blind (ARCHITECTURE §9.7/§9.9), so no convenience method can carry the
honest refusal — the masking-trap analysis IS the showing; (b) the
nub-collision analysis is simpler than GESN's — the verbs are user-initiated,
not gated on not-ready, so they inherit the plain `ObtainExclusiveAccess`
BUSY-on-mounted guard (mos returns MOS_ERR_BUSY on a mounted volume rather
than disturbing a live IOMedia nub). The one-raw-CDB count becomes one-of-
three (GESN + the two tray opcodes); `mos_raw_cdb` stays the SINGLE exclusive-
access call site (§3). Privilege footprint (layer 3) is unchanged: the same
SCSITaskUserClient console grant, no root, no entitlement, no TCC.

**PREVENT field encoding (T10 04-349r1 Table 8, byte4 = {PERSISTENT,PREVENT}).**
0x00 clear basic Prevent (unlock); 0x01 set basic Prevent (lock); 0x02 clear
Persistent Prevent (persistent allow); 0x03 set Persistent Prevent
(persistent lock). The basic and Persistent Prevent states are INDEPENDENT
(§6.18.2) — 0x00 does NOT clear a 0x03 lock; 0x02 does. So `mos_tray_unlock`
takes a `persistent` parameter symmetric with `mos_tray_lock` (one CDB → one
state). This refines the feasibility doc's A.3 sketch, which had `unlock`
take no argument because it predated confirming the two states are
independent — the confirmation is the upload now frozen as the design basis.

**Lock lifetime — no atexit on the single-shot path (narrows
INTEGRATION_HARNESS.md:432).** That v0.3-prep note said "atexit cleanup on
the PREVENT path is non-negotiable." It predated the lock-persistence
derivation. The PREVENT state is per-I_T-nexus and survives a handle close /
process exit by spec (clears only on bus/LU/hard reset, power on, or an
explicit ALLOW on the same initiator — a SCSITaskUserClient close is none of
those, and Apple's kext issues no voluntary ALLOW on exclusive-access
release). A single-shot `mos tray lock` that atexit-released would be a
**no-op**; and a persistent lock is exactly what the robot wants to outlive
the process. So the single-shot verbs register no atexit ALLOW; persistence
is fire-and-forget, and recovery is a later `mos tray unlock` on the same
single-initiator host (always succeeds). If a future *held-session* lock mode
is added (mos staying resident while it holds a lock during a multi-step
op), atexit cleanup for THAT mode is a fresh argument to make here — this
entry authorizes only the single-shot, persist-by-design verbs.

**What hardware can still falsify (not establish).** Per the hardware-role
ADR: a non-conformant USB-SATA bridge that drops the nexus on close
(clearing the lock early); a firmware that ignores prevent and retracts
anyway (the prevent *state* still reads back); a drive without the PDTE
Persistent Prevent state answering 0x02/0x03 with 5/24/00 (classified
refused_other — already handled). Each lands as a fixture + dated note with
a generic defense, never a per-device special-case.

### Addendum: `tray eject --force` = "open no matter what" — admits a forced
### unmount + clears both Prevent states (2026-06-18, narrows three prior lines)

The maintainer's contract for `--force` is **open the tray no matter what is in
the way that mos can clear**. Three prior positions are narrowed (append-don't-
edit; they stand, this rebuts on the merits):

1. **"mos does not unmount for you" (this ADR, ~L377) — narrowed to: `tray
   eject --force` force-unmounts a mounted volume.** When the eject's
   `ObtainExclusiveAccess` returns `kIOReturnBusy`, SCSITaskLib defines that as
   *"media is still mounted"* (verbatim, `SCSITaskLib.h`) — the Finder/system
   mount, **distinct** from `kIOReturnExclusiveAccess` = another userland client
   (mos maps both: `mos_pure.c`, static-asserted in `mos_scsi.c`). On BUSY,
   `--force` calls `mos_internal_da_unmount` (`DADiskUnmount`,
   `kDADiskUnmountOptionForce | …Whole`) and retries. This is **DATA-LOSS
   CAPABLE** (a forced unmount kills open file handles) and is the one place mos
   destroys state — strictly opt-in behind `--force`, never on the default
   eject (which still reports `MOS_ERR_BUSY`, unchanged). The drutil-contrast
   "mechanism, not policy" line holds for the **default** path; `--force` is the
   explicit policy opt-in.

2. **The one blocker `--force` CANNOT defeat: another exclusive-access client.**
   `kIOReturnExclusiveAccess` (a peer like makemkvcon mid-rip) has no SCSI
   preempt and no unmount fix — `--force` surfaces `MOS_ERR_EXCLUSIVE_ACCESS`
   and leaves the tray closed. "No matter what" has this one physical exception,
   and the return-code split is exactly how mos tells it from the mount.

3. **"force does not (and need not) clear a Persistent Prevent lock" (Lock-
   lifetime note above) — narrowed: when a lock blocks the eject, `--force`
   clears BOTH Prevent states** (basic ALLOW 0x00 then Persistent ALLOW 0x02).
   The old note was right that an initiator eject *succeeds* under Persistent
   Prevent by spec — so the flow is REACTIVE: a basic lock surfaces as
   REFUSED_LOCKED and `--force` clears both states then re-ejects, but a drive
   with ONLY Persistent Prevent (no basic lock, not mounted) ejects on the first
   CDB and is left as-is — `--force` never issues a speculative ALLOW for an
   unqueryable Prevent state that did not block (the Prevent states cannot be
   read back; PREVENT ALLOW is write-only). "Leave nothing locked" holds for the
   case that matters — a lock in the way — without a wasted command otherwise.

**DA-action re-admission (narrows the 2026-06-11 "DA retired entirely" /
2026-06-12 "synchronous description read ONLY" addenda).** `mos_internal_da_unmount`
is the SINGLE DiskArbitration **action** mos performs. `DADiskUnmount` is async
(returns void, delivers via callback), unlike the synchronous
`DADiskCopyDescription` those addenda authorized — so mos makes it synchronous-
from-its-side: deliver the callback on a global dispatch queue
(`DASessionSetDispatchQueue`) and block on a semaphore until it fires. The wait
is UNBOUNDED by design: the callback fires exactly once when the unmount
resolves, so the context can't outlive a late callback (no use-after-free), and
a wedged force-unmount blocks here exactly as `diskutil` would (the I/O path is
stuck). Confined to `tray eject --force`. DA stays opt-out
(`MOS_USE_DISKARBITRATION=0`): the no-DA build links a stub returning false, so
a forced eject of a mounted disc reports `MOS_ERR_BUSY` (capability absent) —
the consumer unmounts with `diskutil unmountDisk` first, as without `--force`.

**Verification.** `DADiskUnmount`'s signature (disk, options, callback, context
— *no* session param), option constants (`Force=0x00080000`, `Whole=0x1`), and
success = NULL dissenter were confirmed against `DADisk.h` (maintainer-supplied,
since the DA framework headers were not in the IOKit/DR SDK bundles). The DA
*runtime* behavior is hardware-falsifiable per the hardware-role ADR; the macOS
`-Werror` adapter build is the API compile-gate.

### Addendum: the forced unmount is bound to the handle's media identity, and
### refreshes before it fires (2026-06-19, hardens the addendum above)

The 2026-06-18 addendum had `tray eject --force` format the cached `h->bsd_unit`
and force-unmount that name. A 2026-06-19 review found that **insufficient
authority for a data-loss-capable op**: the cached unit goes stale (a handle
opened with the tray empty has `bsd_unit == -1`; a handle held across a swap has
last-query's unit), and macOS **reuses BSD unit numbers** — so the old path
could (a) fail to clear the *current* mount, or (b) force-unmount an *unrelated*
disk that inherited `diskN`. This entry records the fix; it hardens, does not
reverse, the addendum above (the data-loss contract and the one DA *action* are
unchanged).

**Two gates now stand between BUSY and the unmount** (`mos_tray.c`, `mos_da.c`):
1. **Refresh first.** On the BUSY (mounted) branch, `mos_internal_refresh_media_identity(h)`
   re-resolves `bsd_unit` + `media_id` off `h->svc`'s live IOMedia child, so the
   name formatted and the identity bound both describe the disc in *this* drive
   *now*. Media gone (`bsd_unit < 0`) ⇒ fail closed, mount left as `MOS_ERR_BUSY`.
2. **Bind to identity.** `mos_internal_da_unmount` now takes the expected
   whole-disk IOMedia **registry entry id** (`h->media_id`) and unmounts only
   when the IOMedia behind `diskN` carries that exact id (registry ids are
   globally unique and **not reused**, unlike BSD unit numbers — the property
   that makes them the right anchor). A mismatch (reused `diskN`), a zero id
   (identity unknown), or no IOMedia behind `diskN` all **fail closed** — never
   unmount a disk that is not the one verified under the handle's stable service.

**No new command or DA modality.** The bind reads identity via
`DADiskCopyIOMedia` — a **synchronous, scheduling-free DA read** (returns the
IOMedia `io_service_t`, no run loop, no callback), i.e. the same read class the
2026-06-12 addendum already admitted (`DADiskCopyDescription`), not a second DA
*action*. `mos_internal_da_unmount` remains the SINGLE DA action; the
one-raw-CDB count and command surface are untouched. `DADiskCopyIOMedia`'s
signature (`io_service_t DADiskCopyIOMedia(DADiskRef)`, owned return released via
`IOObjectRelease`) is the real DA header's; the macOS `-Werror` legs are the
compile-gate (green on PR #85), and the headless fake gained `DADiskCopyIOMedia`
+ a `da_media_id` desync so `adapter_da_unmount_binds_to_media_identity` exercises
match / mismatch / zero-id / no-media.

**What hardware can still falsify, never establish** (per the hardware-role ADR):
a non-conformant bridge whose IOMedia registry id is unstable across a re-probe
(would make a legitimate unmount fail closed — surfaces as an uncleared mount,
the safe direction); each such case lands as a fixture + dated note with a
generic gate, never a per-device special-case.

### Addendum: the identity bind does NOT close the BSD-reuse race — it is a
### LOCAL check, the daemon re-resolves by name (2026-06-20, rebuts the
### "closes the residual BSD-reuse race" clause above)

The 2026-06-19 entry's gate 2 claimed `mos_internal_da_unmount` "unmounts only
when the IOMedia behind `diskN` carries that exact id" and so "closes the
residual BSD-reuse race." A 2026-06-20 review (sourced to Apple's published
DiskArbitration at commit `a542bda934211dc3c301bfdcc7f21349c4164a85`:
`DADisk.c`, `DADiskUnmountCommon`/`__DAQueueRequest`, `diskarbitrationd`'s
`DADiskListGetDisk`) found that claim **false on the merits**, and this entry
records it (append-don't-edit; the 06-19 entry stands as the record).

**What is actually bound vs what acts.** `mos_internal_da_disk_is_media`
checks, *at one instant in this process*, that `DADiskCopyIOMedia(disk)`
resolves to the expected registry id. But the `DADiskRef` is a **name-backed
client object** (it stores the `diskN` string), and `DADiskUnmount` transmits
that **string** to `diskarbitrationd`, which performs a **fresh by-name lookup**
to select the unmount target. The verified registry id is **not** carried into
the daemon's selection and is **not** re-checked there. So the bind is a
**check-by-id-then-act-by-name** sequence: it NARROWS the window (the check sits
just before the call) but does not close it. A deschedule or hot-plug between
the local check and the daemon's request-time lookup that reassigns `diskN` to a
different disk B causes a Force|Whole unmount of **B** — the wrong-target
data-loss the 06-19 entry believed it had closed. `DADiskCreateFromIOMedia` is
not a fix: Apple's implementation derives the BSD name from the IOMedia and
builds the same name-backed ref.

**Sibling finding — the wait can hang (F1).** `DASessionSetDispatchQueue` is
`void` but internally fallible (port allocation / source creation); a silent
failure leaves no client callback port, after which `DADiskUnmount` +
`dispatch_semaphore_wait(..., DISPATCH_TIME_FOREVER)` can block forever. A
bounded timeout is NOT a sufficient fix on its own: the callback context is
stack-local, so returning on timeout while a late callback can still fire is a
use-after-return. A safe bounded design needs a heap-owned context whose
lifetime spans until callback-or-proven-cancellation, or helper-process
isolation.

**Feasibility, and why this is not a one-line fix.** Public DiskArbitration
exposes **no identity-bound unmount** — target selection is the daemon's, keyed
on the name. So a redesign can (a) fix the hang and (b) MINIMIZE the TOCTOU
window (re-check identity immediately before `DADiskUnmount`), but it **cannot
eliminate** the wrong-target race with the public API. Only disabling the
automatic Force|Whole unmount (fail-closed, as the `MOS_USE_DISKARBITRATION=0`
build already does) *guarantees* no wrong-target unmount.

**Disposition (2026-06-20, maintainer decision): fail-closed for the first tag.**
Because a redesign only NARROWS the race (the public API exposes no identity-bound
unmount target — see "Feasibility" above), `tray eject --force` is **disabled by
default**: `mos_tray_eject` returns `MOS_ERR_UNSUPPORTED` for `force` unless
`MOS_ENABLE_EXPERIMENTAL_FORCE_UNMOUNT` (CMake/compile flag, default 0) is set.
Plain `mos tray eject` is unchanged; a mounted disc reports `MOS_ERR_BUSY` exactly
as the `MOS_USE_DISKARBITRATION=0` build already does (the consumer unmounts with
`diskutil` first). The force-unmount code stays COMPILED behind the flag (no
bitrot, the bitrot-guard pattern of `MOS_CLI_PROBE`), so the post-tag
guarded-redesign (re-resolve → verify id → callback-side re-verify → bounded wait
with a heap-owned context → fail on any mismatch) lands against live code. The
hang (F1) and the ALLOW-propagation (F2) fixes are scoped to that experimental
path; the bounded-wait/heap-context fix is post-tag since the path is gated off
for the tag. What hardware can falsify: the exact width of the
check→daemon-lookup window; it cannot establish that the window is safe.

### Addendum: `tray eject --force` = name semantics, gated by selector — ships
### ON by default (supersedes the "fail-closed for the first tag" disposition,
### 2026-06-20)

The disposition above disabled `--force` because the redesign only NARROWS the
race and "only fail-closed guarantees safety." This entry supersedes it on the
merits: the framing was wrong. The race only exists if mos PROMISES identity-
exactness on a name-keyed action. Drop that promise — adopt name semantics —
and there is nothing to be stale: mos unmounts the disc *currently named*
`diskN`, exactly as `diskutil unmountDisk diskN` does, a default macOS tool
nobody considers defective. The bind, the re-check, the compile gate, the
"post-tag guarded redesign" were all defending an exactness claim we no longer
make.

**First-hand source verification (supersedes the secondhand citation in the
addenda above).** Read directly from Apple's `DiskArbitration/DADisk.c`
(`apple-oss-distributions`): `DADiskCreateFromIOMedia` reads `kIOBSDNameKey` off
the media and **delegates to `DADiskCreateFromBSDName`**; `struct __DADisk`
holds only `_device` (BSD name) and `_id` ("/dev/diskN") — **no `io_service_t`**.
So `DADiskUnmount` transmits the name and `diskarbitrationd` re-resolves it by
name at request time; there is no identity-bound unmount target in the public
API, and `DADiskCreateFromIOMedia` is not an escape (it IS the name path). The
earlier addenda's conclusion stands; the citation is now first-hand.

**The decision.** `tray eject --force` ships **enabled by default**, name
semantics. The data-loss consent is `--force` itself plus a **selector gate** in
`cli/tray.c` (intent encoded by selector type):
- **bsd-node selector** (`mos disk4 …`) or the **sole drive** → default, no flag.
  The user named the disc (or there is exactly one), so the name action matches
  intent.
- **positional index** (`mos 2 …`) or **identity registry-id** → refused by
  default with a redirect to the bsd-node form (`mos diskN tray eject --force`),
  unless the **runtime** `MOS_FORCE_BY_IDENTITY` env opt-in is set. The index is
  the real footgun (an ephemeral position shifting under a destructive action);
  regid carries the identity-vs-name mismatch. No media present ⇒ nothing to
  unmount ⇒ not gated.

This is *more* conservative than `diskutil` (which force-unmounts by name with no
gate at all), while being honest that the residual is `diskutil`-class. One
recorded residual: mos bundles unmount **+ eject**, so a `diskN` that drifts to
another drive between keystroke and daemon unmounts wherever the name points now
while the eject targets the handle's drive — the same name-reassignment exposure
`diskutil` ships, opt-in by naming.

**Code consequences.** `MOS_ENABLE_EXPERIMENTAL_FORCE_UNMOUNT` is removed (force
is live, not behind a compile flag). `mos_internal_da_unmount` drops its
`expected_media_id` bind and the `mos_internal_da_disk_is_media` helper is gone —
the unmount is name-only. The selector gate + the `MOS_FORCE_BY_IDENTITY`
redirect live in `cli/tray.c`. The unbounded-wait KNOWN ISSUE (the void
`DASessionSetDispatchQueue` + `DISPATCH_TIME_FOREVER`) remains a post-tag
refinement (heap-owned context), documented at the call site — it is a hang risk,
not a wrong-target risk.

**Retire path.** The gate is conservatism, not necessity: the residual is
identical across selectors (diskutil-class). A later entry may relax it — most
defensibly by moving regid to default (it is identity-stable) and keeping only
the ephemeral index gated, or removing the gate entirely. That is a fresh dated
argument here, not assumed.

### Addendum: the READ-path identity guard is restored (DADiskCopyIOMedia);
### "the helper is gone" stands for the unmount ACTION only (2026-06-20, A2)

The "Code consequences" clause above — *"the `mos_internal_da_disk_is_media`
helper is gone — the unmount is name-only"* — is TRUE for the unmount **action**
and stays. But a separate commit (`88657fc`) had ALSO dropped the
`DADiskCopyIOMedia` identity check from the unrelated **volume lookup**
(`mos_internal_da_volume`, a READ), on the premise that resolving the IOMedia by
registry id (`IOServiceGetMatchingService` + `DADiskCreateFromIOMedia`) made the
description *"identity-exact by construction."* The R3 continuation audit
(2026-06-20, A2) found that premise FALSE on the merits, and this entry records
the rebuttal (append-don't-edit).

**Why the premise was false.** `DADiskCreateFromIOMedia` is name-backed: it reads
the object's `kIOBSDNameKey` and delegates to `DADiskCreateFromBSDName` (DADisk.h;
the same fact the unmount path already documents), so the `DADiskRef` carries only
the `diskN` string and `DADiskCopyDescription` resolves THAT name at the daemon. A
`diskN` reuse in the create→describe window can hand back a different disc's
volume — the read-only twin of the unmount BSD-reuse race.

**Why the fix is valid for a READ though it is NOT for the unmount ACTION.** The
helper `mos_internal_da_disk_is_media` is restored, but **only in the volume
lookup**: read into locals, then re-confirm via `DADiskCopyIOMedia(disk)` that the
ref still resolves to the exact `media_id`, and commit to the caller only on a
match (refusing any DETECTED reuse). This works for a read because **nothing
re-resolves after the check** — the description is already in hand. It does NOT
work for the unmount because `DADiskUnmount` transmits the NAME and the daemon
re-resolves it AFTER any local check (the TOCTOU addendum above), so there the
local bind is theatre and name semantics is correct. Same helper, opposite
verdict, because READ vs ACTION differ on whether a later daemon re-resolution can
invalidate the check. The `DADiskCopyIOMedia` / `da_media_id` fake scaffolding
(ROADMAP once slated it for deletion as "dead") is therefore LIVE for the read
path; `adapter_da_volume_lookup_modalities` exercises the match + reuse-refuse
cases. Command surface and privilege footprint unchanged: `DADiskCopyIOMedia` is a
synchronous, scheduling-free DA read (the same class as `DADiskCopyDescription`),
no run loop, no callback, no exclusive access.

### Addendum: the `--force` wrong-target race is REDUCIBLE — "irreducible / no
### public mechanism" is overturned (2026-06-20)

Several addenda above (the name-semantics ADR; the TOCTOU addendum) concluded the
`tray eject --force` wrong-target data-loss race is "name-only, diskutil-class,
irreducible — no public API binds the unmount to an identity." The **by-name
impossibility stands** (re-verified first-hand: `DADiskRef` is name-backed,
`DADiskUnmount` ships the string, `diskarbitrationd`'s `DADiskListGetDisk`
re-`strcmp`s the live list at request time). But the *strong* conclusion that
**nothing** reduces the race is **false**, and this entry records the rebuttal
(append-don't-edit). Full first-hand investigation, every Apple-behaviour claim
sourced to `apple-oss-distributions` DiskArbitration @ `a542bda…` + xnu:
`doc/research/2026-06-20-force-unmount-veto-funmount-investigation.md`.

**The reframe (the load-bearing idea).** Don't bind the *unmount* to an identity
(impossible on the DA path) — **remove the competing mounts from the window** so a
`diskN` reuse can only ever resolve to an *unmounted* disc, whose forced unmount
destroys nothing. Mechanism: register a DiskArbitration **mount-approval veto**
(`DARegisterDiskMountApprovalCallback`, scoped to `MediaBSDUnit==N`) for the
unmount+eject window. Two verified facts make it work: (1) a disc holding `diskN`
at the daemon's lookup is *necessarily freshly-published and not-yet-mounted* (BSD
unit ↔ IOMedia lifetime — the lemma), and (2) `__DARequestUnmount` turns a forced
unmount of an unmounted disc into `kDAReturnNotMounted` with **no `unmount(2)`
issued**. So the veto blocks B's auto-mount → B stays unmounted → the `Force|Whole`
no-ops on B. This **eliminates the data-loss for all DA-mediated mounts** (every
realistic optical case) within the console-user budget.

**Two costs that keep it from shipping as-is.** (a) It only covers DA-mediated
mounts — a non-DA `mount(2)` is not vetoed (theoretical for optical media). (b)
Decisively for mos: a veto makes mos an approval *gatekeeper*, so a wedged mos
stalls `diskarbitrationd`'s mount pipeline for matched media (≤10 s per mount, the
daemon's private response timeout) — i.e. **the veto sharpens the F1 hang into a
system-wide stall.** Adopting the veto *before* fixing F1 would be a net
availability regression. Hence F1 is now a **prerequisite** for any `--force`
hardening, not an independent post-tag nicety (ROADMAP updated).

**A second mechanism — a true identity bind, needs root.** `funmount(2)` (and
`fsctl(VFS_CTL_UMOUNT)` by the `vfs_getnewfsid` fsid) unmount by an fd-pinned
vnode / monotonic fsid — a reassigned `diskN` provably cannot redirect them
(GOAL-1, the thing the DA path can't do). But `safedounmount` requires
`f_owner==uid || root`, and DA auto-mounts optical media as **root**
(`f_owner==0`, traced through the daemon's `_userUID`/`setuid` path), so a
console-user mos gets EPERM. Clean, but outside the SCSITaskUserClient grant —
the exact extra privilege, flagged.

**Disposition.** Post-tag, and **F1-gated**. The current name-semantics +
selector gate stays for the tag (a defensible *integrity* residual; the
`MOS_USE_DISKARBITRATION=0` build still gives the hard guarantee at the cost of
the capability). The veto / `funmount` choice is a live, informed post-tag
decision once F1 is fixed — not a tag blocker. Avenues ruled out (so they aren't
re-walked): `DADiskClaim` (per-object, can't pin a future disc's mount),
unmount-approval (wrong direction), in-callback reads (read-only), `DKIOCEJECT`
(eject needs the mount gone first), volume-UUID (no by-UUID unmount entry point).

### Addendum: `tray eject` is GRACEFUL; `--force` clears LOCKS, never the
### filesystem — the data-loss force-unmount is REMOVED, and with it the whole
### TOCTOU edifice (2026-06-20, supersedes the data-loss `--force` model)

The entire chain above engineered around a premise the maintainer reframed: that
`--force` had to *force the filesystem* ("open no matter what"). It never did —
the "no matter what" was always about tray **PREVENT LOCKS**, not a busy mount.
This entry records the redesign that follows, which **dissolves** the wrong-target
TOCTOU rather than hardening against it. Append-don't-edit; the data-loss model
above stands as the record, this supersedes it on the merits.

**New contract** (`mos_tray_eject`, `mos_internal_da_unmount`, `cli/tray.c`):
- **Every** `tray eject` GRACEFULLY unmounts a mounted disc first
  (`DADiskUnmount` `kDADiskUnmountOptionWhole`, **never** `Force`) and then
  ejects — exactly `diskutil unmountDisk diskN` / `drutil eject`. A **busy**
  filesystem (open handles) makes the graceful unmount dissent, which mos
  surfaces as `MOS_ERR_BUSY`. **mos never forces the filesystem; there is no
  data-loss path.** (This narrows the older "mos does not unmount for you" ADR:
  the default now *does* gracefully unmount — a safe, fail-closed policy, the
  drutil contract — because the data-loss fear that motivated the hands-off
  stance is gone.)
- **`--force`** adds exactly one thing: on a basic Prevent lock
  (`REFUSED_LOCKED`) it clears both Prevent states so the eject gets past the
  **lock**. It does not touch the filesystem.

**What this dissolves.** The wrong-target data-loss race existed *only* because a
forced unmount could kill the open files of a disc a reused `diskN` mis-resolved
to. With a GRACEFUL unmount a reused `diskN` can only (a) cleanly unmount an idle
disc or (b) fail on a busy one — neither destroys data. So the whole apparatus
built to fight it is **removed/mooted**: the name-vs-identity bind, the CLI
**selector gate**, the runtime **`MOS_FORCE_BY_IDENTITY`** opt-in, and the entire
**veto / `funmount` post-tag menu** (the REDUCIBLE addendum above — kept as the
record, but there is now no force-unmount to harden). The investigation that
overturned "irreducible" was correct *and* is now unneeded: the cleaner fix was to
not do the dangerous operation at all.

**F1 (the unbounded DA-callback wait) survives but is ISOLATED.** The graceful
`DADiskUnmount` is still async-awaited on a semaphore, so the
`DASessionSetDispatchQueue`-silent-failure hang is still real and still a post-tag
bounded-wait/heap-context fix. But it is now an ordinary isolated hang — the veto
coupling that would have sharpened it into a system-wide mount-pipeline stall is
gone with the veto. F1 is no longer a "`--force` hardening prerequisite"; it is
just a robustness fix for the one graceful-unmount await.

**Scope / footprint unchanged.** Still the SINGLE DA action (`DADiskUnmount`,
now `Whole`-only), the same console-user budget, no new command surface. The
`MOS_USE_DISKARBITRATION=0` build still reports `MOS_ERR_BUSY` on a mounted disc
(no unmount capability), exactly as a busy filesystem does.

## ADR: verb `state` replaces `status`; bare selector is the default subject (2026-06-14)

The default verb was renamed `status` → `state` (clean break, no alias),
and a bare drive selector now runs it without a verb word: `mos 2`,
`mos disk4`, `mos /dev/disk4` report that drive's state. Design note +
full reasoning: `doc/research/2026-06-14-state-verb-rename.md`.

**Why the rename.** `mos status` emitted `mos.state.v1` — a verb↔schema
mismatch in a one-vocabulary CLI where every other token says *state*
(`mos_query_state`, the `mos_state` enum, the `State:` row, the project
name mac-optical-state). `mos state` now agrees with `mos.state.v1`.
Clean break, not an alias: house style for surface changes (killed
`--brief`, removed watch plain-mode — one string, one grep), and pre-tag
there are no consumers to break.

**Why the digit gate looks wrong but isn't.** The bare-selector dispatch
(cli/main.c) decides "verb vs positional drive" by a single test — does
`argv[1]` contain a decimal digit? — NOT by scanning the verb table. It
is correct because two invariants hold: every valid selector carries a
digit (index/registry are all-digit; a whole-disk BSD form *requires* a
unit digit — `mos_internal_bsd_name_is_whole_shape`, src/mos_pure.c) and
no verb name contains one. So digit-bearing ⇒ positional, digit-free ⇒
subcommand. This is *better* than a verb-table lookup: a mistyped verb is
digit-free, so it still reaches the `unknown subcommand` diagnostic
instead of being misread as a BSD name. **Constraint this places on
future verbs: a verb name may not contain a digit.** Bare `mos` (zero
args) is unchanged — it errors without touching hardware (the 2026-06-12
no-probe-on-intent-free-invocation rule); a selector is intent, bare
`mos` is not.

**Scope.** Output-and-dispatch only: no schema, library-API, or behavior
change to the query itself; `mos.state.v1` is byte-identical. The
library's `*_status` identifiers (`mos_disc_status`, `mos_scsi_status`,
the tray outcome) are a different axis and are untouched. The file
`cli/status.c` → `cli/state.c` and `mos_cli_run_query` →
`mos_cli_run_state` completed the vocabulary. The coupled state-enrichment
direction (D3 in the note — `state` as the canonical cheap dashboard) is
planned separately and is NOT part of this change.

## Naming standard: the BSD vocabulary (Apple-canonical, 2026-06-10)

Three concepts, three names, no synonyms:
- **bsd_unit** — the integer N (Apple: kIOBSDUnitKey). int64; the
  sentinel comment is always verbatim "-1 = no whole-disk IOMedia node
  (media absent)".
- **bsd_name** — the string "diskN" (Apple: kIOBSDNameKey).
  `mos_bsd_name_format` renders it.
- **dev node** — "/dev/" + bsd_name (Apple/diskutil: "Device Node").
  `mos_bsd_dev_node` renders it; the JSON field `bsd_node` (renamed
  from `bsd` 2026-06-13, see addendum), the flag `--bsd`, and the
  column `BSD` carry this form.
Banned synonyms: bsd_path, bsd_number, "device path". CLI-layer
identifiers carry the `mos_cli_` prefix uniformly (io, human, all of
cli/).

### Addendum: the volume vocabulary, and the `bsd` → `bsd_node` rename
### (2026-06-13)

Two new concepts, both Apple-canonical, both from DiskArbitration (not
IOKit):
- **volume_name** — the filesystem label (Apple:
  kDADiskDescriptionVolumeNameKey). JSON field `volume_name`; metadata
  `Volume` row; list folds it into the `Volume` column.
- **volume_path** — the mount point "/Volumes/…" (Apple:
  kDADiskDescriptionVolumePathKey). JSON field `volume_path`; metadata
  `Path` row; list folds it into the same cell as `name (path)`.

These became list/JSON first-class this branch and surfaced a tension:
bsd_name is a *name* and the dev node is a *path*, so "name"/"path" read
like one axis shared with volume_name/volume_path. They are not the same
axis. bsd_unit → bsd_name → dev node is ONE identity in three renderings
(a ladder) whose location rung is *computed* — "/dev/" + bsd_name, always
present, never divergent. volume_name and volume_path are TWO INDEPENDENT
facts — a label and a mount point that *diverge* under macOS
disambiguation/sanitization (`ARRIVAL` vs `/Volumes/ARRIVAL 1`), the path
*discovered* and nullable.

The decision (the owner's; this supersedes a within-session "document it,
don't rename" stance recorded earlier today): rename the JSON output field
`bsd` → `bsd_node` across every document type (state, event, metadata,
list, drive, features, error). Three reasons: (1) it completes a single
prefixed ladder — `bsd_unit` / `bsd_name` / `bsd_node` — that a consumer
learns once instead of memorizing that the bottom rung is the bare key
`bsd`; (2) the bare `bsd` was the actual weak link — "BSD" is a namespace,
not a noun, so the key never said *which* rung it carried; (3) it matches
the internal renderer already named `mos_bsd_dev_node`. The suffix is
`node`, not `path`: `bsd_node` is the computed rendering, `volume_path` the
discovered location — different suffixes for genuinely different kinds of
location, which is why `bsd_path` stays banned (it would assert a false
name/path twinning the ladder does not have).

Scope is OUTPUT ONLY. The `--bsd` selector flag and the human `BSD`
column/row labels are unchanged — input ergonomics, output-not-input
(doc/research/2026-04-27-v2-contract-design.md). The diagnostic
`media_bsd` key in `mos.probe.v0` (cli/probe.c, not in schemas/) is
untouched. Cost was zero: pre-first-tag, no external consumers, so the
JSON-schema ADR's mutable-in-place clause applied — the rename landed in
`mos.*.v1` directly, no v2, schemas + examples + negatives + emitters +
docs in one commit.

### Addendum: list `Volume` column shows the path only
### (2026-06-14, supersedes the "name (path)" fold above)

The 2026-06-13 entry said list "folds it into the same cell as `name
(path)`", and the README argued "both earn their place" because the
label and the mount-point basename diverge. The owner's call today: the
list `Volume` column shows the mount **path only** — `mos_cli_list_volume_cell`
(cli/common.h) renders `volume_path`, not the fold.

What changed in the argument, not the facts: volume_name and volume_path
are still two independent, divergent facts. But the divergence the fold
existed to surface — `ARRIVAL` vs `ARRIVAL 1` — is carried by the path's
own basename (`/Volumes/ARRIVAL 1`), so the path ALONE shows it; the
label was redundant with the basename in the one-row view, and pairing
them only widened the column. The label is not dropped: `mos metadata`
keeps it on its own `Volume` row and `--json` carries it as
`volume_name` (the list JSON is unchanged — both keys, separate). Scope
is the human list table only. Cost was zero: pre-first-tag, the human
table is not a frozen schema, so the change is the cell renderer + its
test + the README example in one commit. If a future case shows a label
that the path basename CANNOT carry (a sanitization that drops
information rather than appending to it), that is a fresh argument to
make here before re-folding.

### Addendum: `mos_cli_` scope narrowed; `_t` convention recorded; two
### library renames (2026-06-13, corrects the "uniformly" clause above)

A naming-consistency pass surfaced that the clause "CLI-layer
identifiers carry the `mos_cli_` prefix uniformly (io, human, all of
cli/)" was false at the time it was written: the cross-TU command and
helper functions in `cli/common.h` (`run_*`, `emit_*`, `finalize_*`,
`collect_and_query`, `open_sole_drive`, `resolve_index_of`,
`mos_error_to_code`, `print_usage`) and the shared `list_row` type were
all bare. They now carry the prefix (`mos_cli_run_*`,
`mos_cli_emit_list_*`, `mos_cli_list_row`, …), which makes the claim
true for what it should have meant: **every cross-translation-unit
identifier in `cli/`**. Two categories stay bare, deliberately, and the
clause is hereby read to exclude them — file-local `static` symbols (the
prefix guards against cross-TU collision, which `static` cannot have)
and the parse-state `extern` globals `opt_*` / `flag_*` / `progname` (a
file-spanning argv-state block named for the options it holds, a
different axis from the helper/command API). The canonical statement now
lives in CONTRIBUTING.md §Symbol-naming, not here.

Same pass recorded the previously-undocumented `_t` suffix convention in
CONTRIBUTING.md §Symbol-naming ("`_t`" subsection) and made two library
renames to fit it: `mos_state_enum` → `mos_state` (the lone enum named
after its kind rather than a semantic noun, now matching `mos_error` /
`mos_disc_status` / `mos_event_kind` / `mos_xfer_dir`), and the stray
internal `typedef ... mos_feature_info` alias was dropped so the struct
mirrors `struct mos_device_info` exactly — tagged internally, `_t` alias
only in `include/mos.h`. Both pre-first-tag, no external consumers; the
public `mos_state` enum value names (`MOS_STATE_*`) are unchanged.

## ADR: SPEC.md centralizes spec citations; parsed layouts stay inline
## (2026-06-15, narrows the "spec byte layouts → the parsing .c" rule)

The comment doctrine (CONTRIBUTING.md §2) and the hardware-role ADR put
spec byte layouts in the parsing `.c`. This entry records a narrow split,
not a reversal: a parser's **parsed byte-offset table and safety contract
stay inline** — they guard the parse and are verified by proximity to the
code, so delocalizing them would forfeit the no-drift property the rule
exists for. What moved to `SPEC.md` (new, root, UPPERCASE source of truth)
is the material that is reference-only or repeats: the authoritative spec
document per parser, the external offset cross-checks (Linux `cdrom.h` /
`cdrom.c` / `sr.c`, libcdio), and the fields each parser deliberately does
**not** decode. Each parser header names its command and points to
SPEC.md instead of restating the citation.

**Why this is additive.** The rule homed *parsed* layouts; it never homed
the undecoded-field reference or the cross-check provenance, which had
accreted into the `.c` headers as prose. SPEC.md gives those a single
home (the genuine N:1 repetition is the spec-document citation, which
recurred across parsers), while the parse-guarding offsets stay exactly
where the rule put them. SPEC.md is under the doc-staleness gate (now
exclusion-based: every tracked `*.md` is checked except the append-only
archives), so the registry cannot silently drift from the parsers.

**What stays foreclosed.** This is not licence to move a parsed offset
table or a bounds/validity rule out of code to cut lines — that remains
the locality forfeit the comment doctrine refuses. SPEC.md holds
citations, cross-checks, and undecoded-field notes; it never becomes the
home of the live parse.

## ADR: fourth raw CDB admitted — INQUIRY VPD page 0x80 (drive serial)
## (2026-06-16, the one-raw-CDB count goes to one-of-four)

The scope doctrine (layer 1) admits a raw CDB only with two showings:
(a) no convenience method can carry the information, and (b) the
nub-collision / exclusive-access analysis. This entry records the fourth
raw CDB — INQUIRY with EVPD=1, PAGE CODE 0x80 (Unit Serial Number),
`src/mos_serial.c` / `mos_query_serial`, feeding `mos.drive.v1.serial`.
Full derivation: `doc/research/2026-06-16-serial-vpd-0x80-feasibility.md`.

**Showing (a) — no convenience or zero-command path carries it — SATISFIED
from the header, no hardware.** `MMCDeviceInterface`'s `Inquiry` takes only
`SCSICmd_INQUIRY_StandardData *` — no EVPD bit, no PAGE_CODE parameter
(contrast `ModeSense10`'s PC/PAGE_CODE, `GetConfiguration`'s RT), and the
header exposes no separate VPD/serial method — so VPD 0x80 is structurally
unreachable through it. The two cheap paths are out too: DiscRecording's
`DRDeviceCopyInfo` has no serial key (it predates the need), and IOKit's
`kIOPropertyProductSerialNumberKey` slot exists but the optical SCSI kernel
stack never populates it (only the AHCI block path does, from ATA IDENTIFY
— source-verified in the feasibility note). This is the same absence/masking
shape as the §9.7 `GetTrayState` showing and the read-capacity note.

**Showing (b) — nub-collision — simpler than GESN or the tray verbs.**
INQUIRY is a non-media command: it touches no IOMedia nub and creates no
§5.5 interleaving exposure. `mos_raw_cdb` stays the SINGLE
`ObtainExclusiveAccess` call site (ARCHITECTURE §3); `mos_serial.c` adds
none. Exclusive access is the gate: a mounted volume / other holder makes
`ObtainExclusiveAccess` fail (BUSY) and the CDB never issues, so the read
backs off rather than disturb a live nub — same BUSY-on-mounted guard the
tray verbs inherit. No drive-state change, so no lock-lifetime question
(unlike the PREVENT verbs — no atexit, no persistence). The one consequence:
the serial is unreadable while media is mounted (`serial` stays null), which
is benign — it is a static drive fact, equally readable with the tray empty,
and an empty tray is the natural inventory moment.

**Count and footprint.** The one-raw-CDB count becomes **one-of-four**:
GESN (0x4A) + the two tray opcodes (0x1B START STOP UNIT, 0x1E PREVENT
ALLOW MEDIUM REMOVAL) + INQUIRY (0x12). `mos_raw_cdb` remains the sole
exclusive-access call site. Privilege footprint (layer 3) is unchanged:
the same SCSITaskUserClient console grant — no root, no entitlement, no TCC.

**Where it lives, by maintainer decision (2026-06-16).** Serial is read in
`mos drive` only — a deliberate, infrequent "what IS this drive" ask. It is
deliberately NOT folded into `mos_query_state` / `mos.state.v1`, which is
frequent and polled and keeps a no-lock-on-READY shape (a GOOD TUR
short-circuits without a lock; the raw GESN is taken only on the not-ready
branch where "not ready ⇒ not mounted ⇒ lock is free", `mos_state_core.c`).
The `mos watch` extension (grab-once-per-`registry_id`, cached on the event)
is feasibility-clear but **deferred, not declined** — design captured in the
feasibility note for a future session when a watch consumer needs it.

**What hardware can falsify, never establish** (per the hardware-role ADR):
a drive that does not implement page 0x80 (it is optional) answers 5/24/00
or echoes a different page — the parser's page-code-echo gate classifies it
to null, expected not a defect; a USB-SATA bridge that synthesizes a bogus
or truncated page-0x80 reply is caught by the dual-length (O-4) bound and
the ASCII trim. Each lands as a fixture + dated note with a generic defense,
never a per-device special-case.

### Addendum: the INQUIRY verb's second mode — EVPD=0 standard data
### (2026-06-16, narrows the "INQUIRY = EVPD=1 page 0x80" scope above)

The ADR above admitted INQUIRY (0x12) for exactly one mode: EVPD=1, PAGE
CODE 0x80 (the serial). The drive-identity work (`mos_query_drive_inquiry`,
`src/mos_drive_inquiry.c` / parser `src/mos_inqdata.c`, feeding
`mos.drive.v1` vendor/product/revision + version + `version_descriptors`)
adds a **second mode of the same opcode**: EVPD=0 standard INQUIRY with
allocation length ≥74, to read the drive's self-reported identity
(vendor/product/revision, bytes 8-35), the VERSION byte, and the version
descriptors (bytes 58-73 — the standards the drive claims). Design:
`doc/research/2026-06-16-drive-identity-enrichment-survey.md`.

**Why `mos drive` prefers it over the DR cache.** Identity elsewhere
(`mos list`, `mos state`) comes zero-command from the DiscRecording
directory cache. `mos drive` is the one path where the user explicitly asks
for the canonical drive truth, and it is *already* issuing this raw INQUIRY,
so it surfaces vendor/product/revision fresh from the drive and uses the DR
cache only as the fallback when the raw read can't run (BUSY/mounted). No
other verb changes — the polled paths keep the zero-command DR source.

**This is not a new raw verb.** It is the same opcode 0x12 on the same
`mos_raw_cdb` path, so the one-raw-CDB count stays **one-of-four** (GESN +
two tray opcodes + INQUIRY); the INQUIRY entry now simply covers both of its
modes. Both layer-1 showings hold unchanged: (a) the convenience `Inquiry`
returns only the 36-byte standard header (`SCSICmd_INQUIRY_StandardData`), so
the version descriptors at 58-73 are structurally unreachable through it —
the same absence showing as the serial; (b) nub-collision is identical —
INQUIRY is a non-media command, `mos_raw_cdb`'s `ObtainExclusiveAccess`
fails BUSY on mounted media so the CDB never issues, and there is no
state change or lock lifetime. Privilege footprint (layer 3) unchanged.

**Placement / cost profile (same rule as the serial).** Read in `mos drive`
only (a deliberate ask), null-on-BUSY, never folded into the polled
`mos_query_state`. The VERSION byte → SPC token (`mos_spc_version_name`,
Linux scsi.h table) and the descriptor codes → standard tokens
(`mos_version_descriptor_name`, the sg3_utils `sg_version_descriptor_arr`
"no version claimed" family codes); an unknown/specific-revision code is
surfaced as hex, never guessed.

**Sibling: firmware creation date — admitted, feature 010Ch (not 0x1FF).**
The same survey identified the firmware *creation date* as a reachable
GET-CONFIGURATION enrichment, initially (from libcdio/MS-DDK) as feature
"0x1FF" with an MS-DDK payload whose Year field self-contradicted. The
maintainer supplied the authoritative MMC-6 r02g spec (§5.3.43, Table 197),
which corrected BOTH errors: the Firmware Information feature is **010Ch**
(0x1FF is *Reserved* in MMC-6), and the year is **Century[2] + Year[2]**
(bytes 4-7, not a single Year[4]). Built to that verified layout:
`mos_internal_firmware_date_from_config` (`src/mos_config.c`) decodes the
010Ch payload (Century/Year/Month/Day/Hour/Minute/Second decimal ASCII, GMT)
into `mos.drive.v1.firmware_date` as an RFC 3339 UTC string — the same
format as `mos.event.v1`'s `ts`. It rides the existing GET CONFIGURATION
walk (no new command, no raw verb), null when the optional feature is absent.
This is the firmware's *creation date*, not a version string — the 4-char
`revision` remains the firmware version identifier (the fuller ATA version
string stays unreachable on macOS per the serial-doc finding).

## ADR: content protection surfaced as drive CAPABILITY, not per-disc state
## (2026-06-17, enriches `mos drive`; no new command surface)

The `mos drive` doc gained a `protection` block covering the five MMC
content-protection features — **CSS 0106h, CPRM 010Bh, AACS 010Dh, SecurDisc
0113h, VCPS 0110h** — replacing the AACS-only `capabilities` block (pre-tag,
mutable-in-place per the JSON-schema ADR; `mos.drive.v1` updated with examples,
negatives, emitter, README, SPEC in one commit). This entry records the
semantics decision and why it touches no doctrine line.

**Capability, not enabled, not enforced.** A protection feature's PRESENCE in
the RT=0 GET CONFIGURATION walk means the drive CAN authenticate that scheme —
a drive-static capability. It does NOT mean protected media is loaded (the
per-feature **Current** bit, media-dependent, is deliberately ignored) nor that
protection is enforced (region/key state lives behind **REPORT KEY**, which mos
does not issue). This resolves the prior `AACS: bus encryption yes` ambiguity:
the bit was always BEC, a drive support bit, never an active/enforced state.
Since a modern BD drive advertises AACS+CSS at minimum and a disc uses one
encryption regime, the multi-scheme list reads as capabilities on its face —
so the human row carries no "(capable)" label; the parentheses hold the
version (`AACS (v68, bus encryption), CSS (v1)`).

**No command-surface change (scope doctrine layer 1 untouched).** This rides
the *existing* GET CONFIGURATION RT=0 walk `mos_query_drive_caps` already
issues — convenience method, zero new commands, no raw CDB. The one-raw-CDB
count stays one-of-four (GESN + two tray opcodes + INQUIRY). Layer 2 is
unchanged: the verification oracle is MMC-6 (§5.3.38/42/44/46, Annex E.6),
decoded by the same bounds-checked feature walk; no REPORT KEY, no SEND KEY,
no mode/log pages. Privilege footprint (layer 3) unchanged.

**WBE included, JSON-only in the human view.** AACS byte 0 carries BEC (bit 1,
read bus encryption) and WBE (bit 2, write bus encryption). WBE is admitted —
the repo's own `getconfig_aacs_wh16ns40.bin` capture (a common consumer BD-RE
burner) reports byte 0 = 0x17 with WBE set, so it is a real consumer signal,
and mos consumers burn (M-DISC archival). It rides as `write_bus_encryption` in
`protection.aacs`; the human `Protection` row omits it (version + read-side
`bus encryption` only) by maintainer call — the write-encryption bit is a
programmatic detail, not dashboard text.

**What hardware can falsify, never establish** (per the hardware-role ADR): a
crossflashed/bridged drive emitting a truncated or out-of-spec protection
descriptor is handled generically — a version-carrying feature with a payload
< 4 bytes reads as absent (fail closed, like the walker); presence-only schemes
key only on the descriptor existing. Any surprise lands as a fixture + dated
note with a generic gate, never a per-device special-case.

## ADR: fifth raw CDB admitted — READ FORMAT CAPACITIES (0x23)
## (2026-06-18, the one-raw-CDB count goes to one-of-five)

The scope doctrine (layer 1) admits a raw CDB only with two showings: (a) no
convenience method can carry the information, and (b) the nub-collision /
exclusive-access analysis. This entry records the fifth raw CDB — READ FORMAT
CAPACITIES (0x23), `src/mos_query.c`'s `mos_internal_read_format_caps` feeding
`mos.capacity.v1.formattable` via the pure parser `src/mos_formatcap.c`. Full
derivation: `doc/research/2026-06-18-read-format-capacities-feasibility.md`.

**Showing (a) — no convenience path — SATISFIED from the header.** The
`MMCDeviceInterface` convenience inventory (ARCHITECTURE.md §9.7) carries no
READ FORMAT CAPACITIES wrapper — same absence shape as the missing READ
CAPACITY and VPD-serial wrappers. The kernel's cached size (IOMedia node) and
READ TRACK INFORMATION both come up empty on a freshly **blank rewritable**
(no whole-disk node yet, no track), which is exactly the medium whose
formattable capacity 0x23 reports. So a raw CDB is the only route.

**Showing (b) — nub-collision — the serial's, not GESN's.** 0x23 is issued on
the `mos_raw_cdb` path, which stays the SINGLE `ObtainExclusiveAccess` call
site (§3) — `mos_query_capacity` calls it, never takes the lock itself.
`ObtainExclusiveAccess` fails BUSY **without issuing the CDB** when a mounted
nub or other client holds the drive, so the formattable read backs off to
`have_formattable=false` and never disturbs a live nub. The target medium
(blank rewritable) is unmounted, where the lock is free. No state change → no
lock-lifetime question (unlike the tray PREVENT verbs). It is read-only: mos
reports formattable capacities and never issues FORMAT UNIT (0x04).

**Doubly gated — the profile gate keeps capacity lock-free for most discs.**
Before the lock, `mos_query_capacity` reads the current profile (a cheap
non-exclusive GET CONFIGURATION) and issues 0x23 only for formattable media —
the rewritable profiles + BD-R (`mos_internal_profile_is_formattable`,
src/mos_strings.c). Pressed, write-once CD-R/DVD±R/HD DVD-R, and empty media
reach NO raw read and NO exclusive-access attempt at all, so `mos capacity`
stays lock-free for them and only the formattable profiles ever take the brief
lock (maintainer refinement, 2026-06-18). This is what makes folding into the
existing one-shot `mos capacity` cost-neutral for the common cases.

**Count and placement.** The one-raw-CDB count becomes **one-of-five**: GESN
(0x4A) + the two tray opcodes (0x1B, 0x1E) + INQUIRY (0x12) + READ FORMAT
CAPACITIES (0x23). Folded into `mos capacity` (not a separate verb) by
maintainer decision: it answers the same "how big is this disc" question as
the other two capacity views, so a second verb would be redundant surface; and
`mos capacity` is a deliberate one-shot, not the polled `mos state` hot path,
so adding a self-gating raw read there carries none of the no-lock-on-READY
cost that kept the serial out of `state`. Privilege footprint (layer 3)
unchanged: same SCSITaskUserClient console grant, no root, no entitlement.

**What hardware can falsify, never establish** (per the hardware-role ADR): a
write-once disc or a drive that does not implement 0x23 answers 5/20/00 or a
"no media" descriptor → `formattable` null/`no_media`, expected not a defect; a
USB-SATA bridge over-claiming the single-byte CAPACITY LIST LENGTH is caught by
the dual-length clamp + the whole-8-byte-descriptor floor. Each lands as a
fixture + dated note with a generic gate, never a per-device special-case.

## ADR: READ FORMAT CAPACITIES is a convenience read, not a raw CDB — count
## back to one-of-four (2026-06-18, supersedes the "fifth raw CDB" ADR above)

The 2026-06-18 entry above admitted READ FORMAT CAPACITIES (0x23) as the
**fifth raw CDB**, on layer-1 showing (a): *"no convenience method carries
0x23."* That showing is **false**, and this entry rebuts it on the merits (the
append-don't-edit rule: the entry above stands as the record; this corrects
it).

**What changed: the header was read directly.** Showing (a) was derived from
`ARCHITECTURE.md:870-873` — but that is §9.7's *illustrative* list (the ~9
methods used to contrast `GetTrayState`'s missing sense out-params), not the
`MMCDeviceInterface` inventory. The actual `SCSITaskLib.h` carries a
`ReadFormatCapacities(self, buffer, bufferSize, taskStatus, senseDataBuffer)`
convenience method — verified verbatim in `MacOSX10.5.sdk`, `MacOSX11.3.sdk`,
and the **Tahoe `macOS 26.4`** SDK (the header annotates it *"Added in Mac OS X
10.3"*, so it spans 10.3 → 26.4; interface UUID
`1F651106-23CC-11D5-BBDB-003065704866`, stable / append-only). It is
non-exclusive (carries `SCSITaskStatus*` + `SCSI_Sense_Data*`, same class as
`GetConfiguration` / `ModeSense10`; the header notes it returns
`kIOReturnExclusiveAccess` only if *another* client holds the device, i.e. it
takes no lock itself). The feasibility doc's own "if a future SDK adds a wrapper this
is superseded" clause is satisfied — except the wrapper is not future; it has
existed since at least 10.5. Full evidence + the cross-check of the other raw
verbs: `doc/research/2026-06-18-readformatcapacities-convenience-exists.md`.

**The decision.** `mos_internal_read_format_caps` (`src/mos_query.c`) now
issues 0x23 through `MMCDeviceInterface->ReadFormatCapacities`, **not**
`mos_raw_cdb`. Consequences:
- the one-raw-CDB count returns to **one-of-four** (GESN 0x4A + the two tray
  opcodes 0x1B/0x1E + INQUIRY 0x12). `mos_raw_cdb` remains the SINGLE
  `ObtainExclusiveAccess` call site (§3) — it simply has one fewer caller. (Two
  same-day entries still read "one-of-five": the superseded fifth-raw ADR's own
  count line, and the kernel-cached-TOC ADR's scope-compliance aside below.
  Per the append-don't-edit rule their text stands; the chain — not any single
  entry — carries the live count, which is one-of-four.);
- no exclusive access for the formattable view, so it is reported on **mounted**
  formattable media too (the BUSY-on-mounted back-off the entry above described
  no longer applies); the profile gate stays, now purely semantic (only
  formattable media has the view), not a lock-avoidance device.

**Scope of the rebuttal — the other raw verbs are unaffected** (cross-checked
against the full header): GESN and the tray opcodes rest on *masking*
(`GetTrayState`/`SetTrayState` exist but are sense-blind, §9.7/§9.9); INQUIRY
VPD-0x80 is genuinely unreachable (the convenience `Inquiry` takes only
`SCSICmd_INQUIRY_StandardData`). Only 0x23 was misclassified.

**What hardware can falsify, never establish** (per the hardware-role ADR):
that the convenience wrapper truncates the Formattable Capacity Descriptor list
some drive returns. It takes a generic buffer + size (built to the same
contract as `ReadDiscInformation`), so the whole reply should arrive; a capture
showing otherwise lands as a fixture + dated note, and would not revive the raw
CDB without its own showing.

## ADR: kernel-cached full-TOC adopted (kIOCDMediaTOCKey) — primary CD TOC
## source + session_layout (2026-06-18, supersedes the "banked, not built" call)

`doc/research/2026-06-14-state-verb-rename.md:155` recorded **"CD cached full-TOC
(`kIOCDMediaTOCKey`): BANKED, not built (owner's call)"** — a hardware-contingent
fallback to reach for only if the convenience `ReadTableOfContents` disappointed
on real hardware. The maintainer overrode that on 2026-06-18. This entry records
the override and why it dominates; the append-don't-edit rule applies (the
06-14 note stands; this rebuts it).

**What the cache is.** macOS caches the full-TOC as `kIOCDMediaTOCKey` — a
`CDTOC` blob on the `IOCDMedia` node, read via `IORegistryEntryCreateCFProperty`
with **zero SCSI commands and no exclusive access**, the same registry-read
modality as the cached `kIOMediaSize` capacity and the DR directory. It is a
*superset* of the issued READ TOC format-0000b: POINT descriptors A0/A1/A2 carry
the per-session first/last track and lead-out — the session structure
format-0000b omits and `disc_info` gives only for the last session. Decode:
`src/mos_cdtoc.c` (`mos_internal_cdtoc_parse`); read shell:
`src/mos_scsi.c` (`mos_internal_read_cdtoc`).

**Why the override dominates.** (1) *Cheaper and contention-free* — zero command,
no `ObtainExclusiveAccess`, works on mounted media. (2) *The staleness worry was
unsupported* — mos reads it FRESH off the current `IOCDMedia` node every query
(`mos_internal_refresh_media_identity` re-walks the registry first), and the
kernel refreshes its own cache on media change, so there is no stale-handle
window. (3) *Source-proven, ADR-legitimately* — libcdio's `read_toc_osx`
(`lib/driver/osx.c`) uses exactly this property; that is **verified platform
source**, which the hardware-role ADR admits as a design basis (distinct from the
ADR-forbidden "known good on a drive I ran" — the basis here is the source, not a
hardware run). (4) *No migration cost* — pre-first-tag,
there are no consumers of the `toc` fingerprint provenance, so re-sourcing it is
free (the JSON-schema ADR's mutable-in-place clause).

**The decision, in two parts (both shipped).**
- **`session_layout`.** A new `mos.metadata.v1.disc.session_layout` array —
  per-session `{session, first_track, last_track, leadout_lba}` — CD-only, null
  on non-CD or when no `IOCDMedia` node carries a cached TOC. A *sibling* of
  `toc`; it does not alter `toc`.
- **Primary CD TOC source.** `mos_query_toc` prefers the cached full-TOC for CDs
  (`mos_internal_cdtoc_to_toc`, fail-closed to the format-0000b standard) and
  falls back to the issued `ReadTableOfContents` when no `IOCDMedia` node is up
  yet (just-inserted / unrecognized media) — and that fallback stays the **only**
  route for DVD/BD, where no cached TOC exists. So "use it for everything" is
  "use it everywhere it exists, with the issued read as the CD fallback." The
  fail-closed cdtoc→toc decode (duplicate track / gap refuses the whole) means a
  hostile or partial blob degrades to the issued read, never to a half-parsed
  fingerprint.

**Scope-doctrine compliance.** This adds **no command surface** — `kIOCDMediaTOCKey`
is a registry property read, not an MMC command, so the one-raw-CDB count stays
one-of-five and layer-1 is untouched. Privilege footprint (layer 3) unchanged:
the same console grant, no entitlement, no exclusive access. Input space (layer 4):
the CDTOC `length` field is device-reported and treated as hostile — it may only
SHRINK the descriptor walk (dual-length rule O-4); no payload byte is used as an
offset (`tests/test_cdtoc.c` pins the fail-closed cases).

**What hardware can falsify, never establish** (per the hardware-role ADR): a
drive/bridge where the kernel builds no `IOCDMedia` node for valid CD media (the
issued-READ-TOC fallback covers it); a `CDTOC` blob that disagrees with the
issued TOC on the same disc (lands as a fixture + dated note with a generic
gate, never a per-device special-case).

## ADR: public `mos_raw_cdb()` passthrough retired; raw issuance is internal,
## diagnostic capture is `mos probe --capture` (2026-06-19)

ROADMAP's v0.4 item "Remove `mos_raw_cdb()`" is realized — as **unpublish, not
delete**, the shape the feasibility note already named
(`doc/research/2026-06-18-read-format-capacities-feasibility.md:85`: the item
removes the *public* passthrough; the internal raw-CDB mechanism every typed
verb rides survives). This entry records the surface change and why it touches
no doctrine line.

**What changed.** The function `mos_raw_cdb` is renamed `mos_internal_raw_cdb`
and its declaration moved from `include/mos.h` to `src/mos_internal.h`; the
public doc block is gone. It is no longer callable by an embedder — the public
C API loses the arbitrary-CDB passthrough. The `mos_xfer_dir` enum + `MOS_XFER_*`
constants moved with it (they had no public consumer once the passthrough left;
verified by grep before the move) and are pinned to the SDK
`kSCSIDataTransfer_*` values by the same `_Static_assert` in `mos_scsi.c`. Pre-
first-tag, no external consumers, so the removal is free (the JSON-schema ADR's
mutable-in-place reasoning applied to the C surface).

**What is unchanged — the internal mechanism.** `mos_internal_raw_cdb` remains
the SINGLE `ObtainExclusiveAccess` call site (ARCHITECTURE §3); its five callers
are untouched: the GESN tray probe (`mos_scsi.c`), the tray opcodes
(`mos_tray.c`), the two INQUIRY reads (`mos_serial.c`, `mos_drive_inquiry.c`),
and now the diagnostic capture menu (`cli/probe.c`). The one-raw-CDB count
(GESN + the two tray opcodes + INQUIRY = one-of-four) is unchanged — no library
query path gained or lost a raw verb.

**Fixture capture migrated to `mos probe --capture`.** The two docs that pointed
fixture contributors at the public passthrough (`tests/fixtures/README.md`,
`INTEGRATION_HARNESS.md`) now point at a new third probe mode: `mos probe
--capture <drive>` issues a FIXED MENU of the read-only commands mos's own
decoders consume (INQUIRY standard + VPD 0x80, GET CONFIGURATION, GESN, READ
DISC INFORMATION, READ TOC, READ DISC STRUCTURE DVD/BD) and emits each raw reply
as a `mos.capture.v0` NDJSON line carrying the reply hex + the manifest
`tests/fixtures/README.md` specified (task status, parsed sense, transfer
length, SHA-256). The `reply` hex decodes to the committed `.bin`. This is the
in-tree, fixed-menu version of the exact "write a C program calling
`mos_raw_cdb`" workflow the old docs described — the long-anticipated
`mos_capture` tool, now shipped.

**Why a fixed menu, not an arbitrary-CDB CLI flag.** An arbitrary-CDB `--cdb`
mode would relocate the retired passthrough into the shipped binary — shrinking
the C surface while growing an equivalent raw surface, a net-zero (arguably
negative) scope move. The fixed menu issues only commands mos already knows, so
it grows no general-SCSI surface; the cases an arbitrary passthrough could reach
and the menu cannot (exotic non-mos CDBs) are general-SCSI exploration, which
layer 2 forecloses anyway. (Decision: maintainer, 2026-06-19.)

**Scope-doctrine compliance.** The capture menu issues raw CDBs, but it lives in
the diagnostic `probe` subcommand behind `MOS_CLI_PROBE` (default ON) — the
threat model there is the developer who enabled it (CLAUDE.md), not the library's
query path. It changes no library command surface: the one-of-four count, the
no-lock-on-READY query shape, and layer-1's "kernel-authored by default" all
hold for `mos_query_*`. Every menu command is a non-destructive READ; the menu
self-gates on exclusive access (a mounted volume returns BUSY without issuing),
so capture wants an unmounted disc. Privilege footprint (layer 3) unchanged: the
same SCSITaskUserClient console grant. `mos.capture.v0` is a diagnostic format,
NOT a published `schemas/` document — the same call as `mos.probe.v0`.

**What hardware can falsify, never establish** (per the hardware-role ADR): a
menu CDB byte layout that a drive answers with an unexpected CHECK CONDITION, or
a USB-SATA bridge truncating a reply — each lands as a fixture + dated note and,
at most, a refinement of the menu's allocation lengths or parameters, never a
per-device special-case. The capture tool's job is to record what the drive
returned, so a surprising reply is the deliverable, not a defect.

## ADR: `MOS_CLI_PROBE` ships OFF at the first tag — `probe` is falsification
## surface, not consumer surface (2026-06-19)

`MOS_CLI_PROBE` defaults **ON** today, and the CMake comment
(`CMakeLists.txt:299`) states the only reason: so the Apple-only, callback-heavy
`cli/probe.c` TU is *compiled and contract-tested on every CI run instead of
bitrotting behind an opt-in flag*. That is a build-system bitrot guard — it was
never a claim that `probe` is consumer surface. This entry records that the
**consumer artifact must ship without it**, and separates the two axes the
default-ON conflates.

**Decision.** At the **first tag**, `MOS_CLI_PROBE` flips to **default OFF**.
`probe` and its three modes (`--dump` DR-dictionary capture, the notification
stream, and `--capture` raw-MMC fixture capture) are **falsification /
diagnostic** surface — they exist for the hardware-role ADR's two jobs
(falsifying axioms, acquiring fixtures), not for library consumers. A shipped
`mos` is the lean state/identity/tray/capacity CLI; the diagnostic surface
(raw CDB issuance via the CLI, the CommonCrypto SHA-256 dependency, the fixed
capture menu) is not in it.

**Why the two axes are separable — and both are satisfied.** "What CI
compiles/tests" and "what the consumer ships" are independent. The bitrot guard
needs only *one* CI job at `-DMOS_CLI_PROBE=ON`; it does not require the
*consumer default* to be ON. So at tag: the CMake default goes OFF (a plain
build is the lean consumer build — the honest default), and CI gains/keeps an
explicit `-DMOS_CLI_PROBE=ON` job so the TU stays compiled and contract-tested.
probe is tested-always and shipped-never.

**Doc consequence (the thing this ADR resolves).** Three surfaces, two truths:
`mos --help` is `#ifdef`-gated (build-accurate); the **man page and shell
completions are static committed artifacts that list `probe` unconditionally**
— `gen-cli-docs.py` extracts verbs by regex over `cli/main.c`'s table and is
blind to the `#ifdef`. While the default is ON these agree; the moment the
shipped default is OFF they would over-document. So the tag-time work is: make
`gen-cli-docs.py` gating-aware (the shipped/default build excludes the
`MOS_CLI_PROBE` verbs/flags, so the generated man + completions omit them), and
relocate `probe`/`--dump`/`--capture` documentation to INTEGRATION_HARNESS.md /
CONTRIBUTING.md (the contributor/diagnostic docs). This supersedes the
within-session question of whether to annotate the man page with a "requires
MOS_CLI_PROBE" availability note — at tag the shipped docs simply describe the
shipped (OFF) build, so no note is needed.

**Why not now (pre-tag).** The default stays ON until the tag. The only current
"consumers" are HEAD installers — developers and early adopters — and the
fixture-capture workflow this branch shipped (`mos probe --capture`,
tests/fixtures/README.md + INTEGRATION_HARNESS.md) wants `probe` present with
zero friction. Flipping pre-tag would force every from-source capture build to
add `-DMOS_CLI_PROBE=ON` for no consumer benefit (there is no tag, no real
consumer yet). The flip is a release-mechanics step bound to the first tag, like
the schema freeze (ROADMAP standing constraints).

**What stays true regardless.** The OFF build already compiles and rejects
`probe` with a "not built into this binary" diagnostic (CI "Probe-retired build"
leg, `cli/main.c`), and `cli/probe.c` — including its CommonCrypto/`--capture`
code — is wholly inside the `if(MOS_CLI_PROBE)` CMake guard, so OFF pulls none
of it in. The flip is a default change + a generator change + a doc move, not a
code-path change to the library.

## Finding + ADR: watch self-contends with control verbs; A4 serial re-probe
## fixed (release-criterion met) — partial, the GESN residual is deferred
## (2026-06-21, first hardware run)

The first hardware run surfaced a real, confirmed contention: a concurrent
`mos watch` makes `mos tray eject` fail with `MOS_ERR_EXCLUSIVE_ACCESS` on an
EMPTY drive (no disc). Diagnosed self-contention, not a device quirk: on a
not-ready/empty drive the state path takes exclusive access every poll to fire
the raw GESN tray probe (`mos_state_core.c`; `ARCHITECTURE.md` §4), so a watch
polling holds the SCSITaskUserClient lock briefly each cycle, and the one-shot
eject lands in that window and loses. The eject's report is CORRECT — a sibling
client genuinely holds the lock, there is no SCSI preempt, so it surfaces and
stops (no retry, per the no-retry ethos). Distinct from `kIOReturnBusy` (a
disc's mount); this is `kIOReturnExclusiveAccess` (another userland client).

**What this tripped.** The A4 "permanent-negative serial re-probe" item was
parked POST-TAG *"unless repeated exclusive-access traffic becomes a release
criterion"* (ROADMAP). On a serial-LESS drive the watch ALSO re-issued the
optional exclusive INQUIRY (VPD 0x80) every poll, because `serial_grabbed`
flipped true only on a successful read — so an answered absence re-probed
forever, doubling the per-poll exclusive traffic. That is the criterion, met by
hardware. A4 is now implemented (maintainer decision, 2026-06-21): a
`serial_probe_terminal()` classifier in `mos_watch.c` resolves on a successful
read OR an answered absence (`MOS_ERR_IO`/`MOS_ERR_UNSUPPORTED` — the drive
replied, no serial, a STATIC fact) and retries only TRANSIENT failures
(`MOS_ERR_BUSY`/`EXCLUSIVE_ACCESS`/`TIMEOUT`/`NO_DEVICE`/`OOM`); the
`serial_grabbed` flag is renamed `serial_resolved` to match the widened meaning.

**Partial by construction — stated so it is not mistaken for the whole fix.**
A4 removes the redundant per-poll INQUIRY on serial-less drives, but the GESN
tray probe is INHERENTLY per-poll-exclusive on an empty drive, so a watch will
always leave SOME window a one-shot eject can hit. Closing that residual is a
separate, deferred decision (the forward item in ROADMAP) between (a) a bounded
retry/backoff on the eject's `EXCLUSIVE_ACCESS` — which would cross the no-retry
ADR chain and needs its own dated rebuttal — and (b) reducing the watch's
exclusive footprint structurally (notification-lean + slower empty-drive
cadence). On a drive that DOES report a serial, A4 changes nothing (the serial
resolves once regardless) and the residual GESN contention is the whole story.

**Hardware's role, honored.** Per the hardware-role ADR this finding did not
add a device special-case: it confirmed a pre-existing architectural cost and
met a pre-recorded release criterion. The fix is generic (an error-class
classifier, no per-device branch). A drive whose VPD 0x80 absence is reported
via an unmapped transport `IOReturn` is caught by the `MOS_ERR_IO` default arm
(resolved as absent — a benign null serial, not forever-churn); any surprise
lands as a fixture + dated note, never a per-device gate.

## Finding + ADR: GESN door bit vs a 04/xx disc-engaged sense — the sense wins
## (2026-06-21, second hardware run: WH16NS60 stray-open on insert)

`mos_state_core.c`'s tray rule read "MOS_OK ⇒ door_open is authoritative; the
sense never overturns a GESN open/closed verdict." A second hardware run (LG
WH16NS60, libredrive, OWC enclosure) falsified the unconditional form: inserting
a UHD BD produced a **stray `open`** between `busy` and `ready`. Captured cause —
mid-load the drive answered TUR with sense **02/04/01** (NOT READY / LOGICAL UNIT
NOT READY / **IN PROCESS OF BECOMING READY**) while the raw GESN transiently
returned **door_open=true**. The old rule trusted GESN, forked to OPEN, and never
consulted the 04/01 sense (which the closed-branch classifier already maps to
LOADING, `mos_sense.c`).

**Rebuttal (narrow, spec-grounded, not a reversal).** A `04/xx` LOGICAL UNIT NOT
READY sense (becoming-ready / init-required / format / long-write / op-in-
progress) is positive proof a disc is **ENGAGED**, which an OPEN tray physically
cannot be — you cannot spin up, format, or write a disc with the tray out. So
when the TUR sense ASC == 0x04, a GESN `door_open=true` is the transient lie and
is suppressed: the tray reads CLOSED and the sense classifier names the reason
(loading / formatting / busy). The GESN bit remains authoritative for every
other case, including the `3A/xx` tray hints (those still never overturn GESN —
`state_gesn_*_is_not_invalidated_by_*_sense` are unchanged); only the disc-
engaged `04/xx` family wins. `mos_state_core.c` now reads
`tray_open = door_open && asc != 0x04`.

**Hardware's role, honored.** This is a generic gate keyed on the SPEC semantics
of `04/xx` (disc engaged), not a per-device branch — any drive whose GESN
transiently disagrees with a becoming-ready sense is covered. The observation is
the WH16NS60 insert transient; the defense cites T10, not the model. Pinned by
`state_loading_from_0401_overrides_gesn_door_open` (pure). If a future capture
shows a drive that legitimately reports `04/xx` with the tray genuinely open
(a malfunction), that lands as a fixture + dated note here before any change.

## ADR: serial source is feature 0108h (GET CONFIGURATION); raw VPD 0x80 is
## RETIRED (2026-06-21, supersedes the "fourth raw CDB — INQUIRY VPD page 0x80"
## serial-source decision)

The 2026-06-16 ADR admitted a raw INQUIRY (EVPD=1, PAGE CODE 0x80) as the drive-
serial source, on showing (a) "no convenience or zero-command path carries it."
A hardware run (LG WH16NS60, libredrive, OWC enclosure) + a cross-family survey
falsified that showing, and this entry records the rebuttal + the redesign.
Append-don't-edit: the 2026-06-16 entry stands as the record; this supersedes it
on the merits. Full decision basis (architectural + empirical falsification of
VPD 0x80, cross-family OEM-firmware survey, the 0108h/0109h distinction):
`doc/research/2026-06-21-optical-serial-vpd80-vs-0108h.md`.

**What hardware showed.** `mos probe --capture` on the WH16NS60: INQUIRY VPD 0x80
returned `GOOD` with **page length 0** — the page is supported but EMPTY (no
serial programmed). The drive's real serial instead lives in **GET CONFIGURATION
feature 0108h (Logical Unit Serial Number)** — a 12-byte ASCII serial, decoded
cleanly from the same RT=0 walk mos already issues for protection / profiles /
firmware_date. (The serial also appears in the standard INQUIRY's vendor-specific
region, bytes 36+ — corroboration that 0108h's value is the true serial, but that
region's layout is vendor-defined per SPC, so it is NOT decoded: that would be the
per-device special-casing the hardware-role ADR forbids.)

**Why 0108h is the better source (the rebuttal to showing (a)).** Feature 0108h
is reached through the **non-exclusive `GetConfiguration` convenience method** —
no raw CDB, no `ObtainExclusiveAccess`. So it (1) reads the serial **even while a
disc is mounted**, where the raw VPD-0x80 INQUIRY returns `MOS_ERR_BUSY`; (2) is
**hardware-validated populated** on the one drive we have, where VPD 0x80 is
empty; and (3) takes **no exclusive lock**, so a `mos watch` serial grab no longer
contends with a one-shot `mos tray eject` — the root cause the A4 work could only
shrink is removed for the serial entirely. Decoding it is spec-grounded (an MMC
feature, same class as the 010Ch firmware-date decode), generic, no per-device
branch.

**The decision (maintainer, 2026-06-21) — RETIRE, not demote.** `mos_drive_caps_serial`
(feature 0108h, `src/mos_config.c` `mos_internal_serial_from_config`) is the **SOLE**
serial source for `mos drive` and `mos watch`. The raw VPD-0x80 subsystem is
**deleted** — `src/mos_serial.c`, `src/mos_vpd80.c`, `tests/test_vpd80.c`, the
public `mos_query_serial`, the handle's `serial_str`, and the (briefly-added)
`MOS_SERIAL_VPD80` flag are gone. **Why retire rather than keep an opt-in fallback:**
the 2026-06-16 raw-CDB admission rested entirely on showing (a) "no convenience
method carries the serial"; 0108h IS a convenience-method path that carries it, so
that showing has **collapsed** — and layer-1 admits a raw CDB *only* with that
showing. Keeping VPD 0x80 even behind a flag keeps an exclusive raw CDB whose
justification no longer exists, against a *hypothetical* VPD-only drive for which
there is zero positive evidence (it read empty on the one drive tested; smartmontools:
"CD or DVD-ROM devices often do not support VPD pages 0x80, 0x83 or 0x85"; the
2026-06-16 admission was a spec bet never hardware-validated). That is exactly the
speculative-code-for-unobserved-hardware the hardware-role ADR forbids.

**0108h is OPTIONAL and firmware-dependent.** The survey found the *same* OEM ships
firmware that fills 0108h and firmware that leaves it blank (e.g. Pioneer BDR-209 /
212U populate it, BDR-213M blank). So the decoder fails closed on an absent or
all-space feature (`serial` null) — never assume presence. This is the same
optionality VPD had; the difference is 0108h is the in-spec MMC carrier (sg3_utils
`sg_get_config` `case 0x108: "Drive serial number"`, libcdio `CDIO_MMC_FEATURE_LU_SN`),
read non-exclusively from a walk mos already issues, where VPD 0x80 is the wrong
SPC/block-storage abstraction for an MMC drive.

**Scope / count.** The INQUIRY opcode 0x12 raw CDB is **unchanged** — the standard
INQUIRY (EVPD=0) still issues for drive identity (vendor/product/revision/version
descriptors), so the one-raw-CDB count stays one-of-four (GESN + two tray opcodes +
INQUIRY). What changed: INQUIRY's **EVPD=1 / page-0x80 mode and its decoder are
removed**; the serial read is now lock-free GET CONFIGURATION. Privilege footprint
(layer 3) unchanged. Pre-first-tag, so the `mos.drive.v1.serial` description + the
public header were updated in place (mutable-in-place clause). Pinned by
`serial_decodes_from_feature_0108` + `serial_from_config_absent_empty_and_bounds`
(pure).

**The vendor INQUIRY-tail is deliberately NOT decoded.** The serial also appears in
the standard INQUIRY's vendor-specific region (bytes 36+) — corroboration that
0108h's value is the true serial, but that region's layout is vendor-defined per
SPC, so decoding it is the per-device special-casing the hardware-role ADR forbids.

**What hardware can falsify, never establish** (per the hardware-role ADR): a drive
that populates VPD 0x80 but NOT 0108h. There is no evidence such a drive exists; if a
future first-party capture (a Pioneer or MediaTek `mos probe --capture`) shows one,
THAT capture drives re-adding a serial path through the normal fixture→generic-gate
flow — mos does not pre-emptively carry an unjustified raw command against the
possibility. The open evidence gap the survey flagged (first-party 0108h captures on
a non-LG unit) is a capture target, not a blocker.

## Finding + ADR: macOS arms a tray PREVENT on mount; the default `tray eject`
## clears that OS mount-lock after its own unmount (2026-06-21, narrows the
## 2026-06-20 "the DEFAULT returns REFUSED_LOCKED untouched" clause)

A hardware run (LG WH16NS60, libredrive, OWC enclosure) surfaced that a DEFAULT
`mos tray eject` of a MOUNTED disc fails `refused_locked / 05/53/02` even though
no `mos tray lock` was issued. Diagnosed: macOS arms a tray PREVENT (PREVENT
ALLOW MEDIUM REMOVAL) when it MOUNTS a disc, and that lock SURVIVES mos's
graceful unmount (the unmount issues no ALLOW), so after mos gracefully unmounts
the disc the re-eject is refused by the lingering OS lock. The dead physical
eject button on the mounted disc corroborates a persistent-grade Prevent. Only
`mos tray eject --force` cleared it — which made the common case (eject the disc
that is currently mounted, the thing Finder/`drutil` do without ceremony) wrongly
require `--force`.

**Rebuttal of the 2026-06-20 clause "the DEFAULT returns REFUSED_LOCKED
untouched."** That clause was right that a DELIBERATE lock (a robot's `mos tray
lock` on an idle drive) must not be silently overridden by a bare eject. But a
REFUSED_LOCKED that follows mos's OWN graceful unmount is categorically
different: the lock can only be the OS mount-protection Prevent macOS armed when
it mounted the disc mos just unmounted — there is no path to that lock except
through the mount mos just cleared. Clearing it COMPLETES the unmount the way
Finder/`drutil` do; it is not overriding a user's intent. So `mos_tray_eject`
now tracks `did_unmount` and, when a REFUSED_LOCKED follows its own unmount,
clears BOTH Prevent states (basic ALLOW 0x00 then persistent ALLOW 0x02) and
re-ejects — no `--force` needed. A COLD REFUSED_LOCKED (no preceding unmount — a
deliberately-locked idle drive) is still returned untouched on the default path
and still needs `--force`. The basic-vs-persistent distinction is moot for the
fix: the clear issues both ALLOWs regardless, so it works whichever state macOS
used.

**Scope unchanged.** No new command surface (the same 0x1B/0x1E raw CDBs on the
single `mos_internal_raw_cdb` exclusive-access site, one-of-four count
untouched), no data-loss path (the unmount stays GRACEFUL — `DADiskUnmount`
Whole, never Force; a busy filesystem still surfaces `MOS_ERR_BUSY`), and the
loop stays bounded at two passes (mount, then lock). `--force` keeps its
meaning: clear a COLD deliberate lock. Pinned by
`adapter_tray_eject_default_clears_os_mountlock` (mounted+locked → DONE on the
default) and `adapter_tray_eject_cold_lock_needs_force` (cold lock →
REFUSED_LOCKED on the default, DONE under `--force`), with the headless fake
modelling the stateful PREVENT lock the real drive holds
(`mos_fake_set_prevent_locked`).

**What hardware can falsify, never establish** (per the hardware-role ADR): a
drive/bridge where the OS Prevent does NOT survive the unmount (then the default
clear is a harmless no-op — both ALLOWs answer GOOD on an unlocked drive), or one
that refuses the ALLOW (5/24/00) — surfaced as the drive's own answer, the
re-eject being the real check. Each lands as a fixture + dated note with a
generic gate, never a per-device special-case.

## Finding + ADR: a locked-eject refusal is 53/02 under sense key 02 (empty) as
## well as 05 (media present) — the tray classifier keys on ASC/ASCQ, not the
## sense key (2026-06-21, third hardware run: WH16NS60 empty-drive lock)

`mos_internal_tray_classify` (`src/mos_pure.c`) read `sk == 0x05 && asc == 0x53 &&
ascq == 0x02 → REFUSED_LOCKED`, i.e. it required sense key 05 (ILLEGAL REQUEST).
A hardware run (LG WH16NS60, libredrive, OWC enclosure) with NO media falsified
the key requirement: `mos tray lock` (DONE on the empty drive — exclusive access
is free with no mount) then `mos tray eject` answered CHECK CONDITION **02/53/02**
(NOT READY / MEDIA REMOVAL PREVENTED), which fell through to `refused_other` —
wrong: the tray WAS locked (the user just locked it; `unlock` then `eject`
succeeded). The drive reports the SAME removal-prevented ASC/ASCQ (53/02) but
under sense key **02 (NOT READY)** because the drive is empty, vs **05 (ILLEGAL
REQUEST)** when media is present.

**Rebuttal (narrow, spec-grounded).** 53/02 = MEDIUM REMOVAL PREVENTED is the
spec's unambiguous "the Prevent lock refused removal" signal; the sense KEY is
contextual (NOT READY when empty, ILLEGAL REQUEST with media) and must not gate
the verdict. The classifier now keys `refused_locked` on `asc == 0x53 && ascq ==
0x02` alone — a generic ASC/ASCQ gate, not the observed device's key. Near-miss
codes stay `refused_other` (53/01, 53/00, 5/24/00, 02/3A/00 — pinned in
`tests/test_tray.c`), and a GOOD status still short-circuits to DONE before any
sense is read. This also matters for `tray eject --force` on an empty locked
drive: the force path clears the lock only when the outcome is `refused_locked`,
so before this fix `--force` could not clear an empty-drive lock either.

**Scope unchanged.** Pure classifier only — no command surface, no new CDB, no
privilege change. Pinned by `tray_locked_eject_is_refused_locked` (now asserts
both 05/53/02 and 02/53/02).

**What hardware can falsify, never establish** (per the hardware-role ADR): a
drive that reports a locked eject under yet another ASC/ASCQ (not 53/02) — that
would land as a fixture + dated note, and the gate would widen to the new code,
never to a per-device sense-key special-case.

## ADR: `--persistent` retired — `lock` is the persistent Prevent, `unlock`
## clears both; `lock` on a mounted disc is `already_locked` (2026-06-21)

Hardware exploration of the tray verbs surfaced that the basic/persistent
`--persistent` flag carried no real CLI use, and that `lock`/`unlock` on a
MOUNTED disc were unhelpful (BUSY). This entry records the resulting
simplification (maintainer decision, 2026-06-21). It narrows the tray-control
feasibility design (`doc/research/2026-06-13-tray-control-feasibility.md`, which
specified a `--persistent` flag and a basic-default lock) on the merits.

**What changed.**
- **`mos tray lock` sets the PERSISTENT Prevent (0x03) only**; the basic Prevent
  (0x01) is no longer issued, and the `--persistent` flag is removed. Rationale:
  a single-shot, fire-and-forget CLI lock wants the durable state that survives
  an I_T-nexus loss (e.g. a USB bus reset on an external enclosure) — basic
  Prevent does not, and has no CLI use case that persistent doesn't cover. A
  drive without the Persistent Prevent state answers `refused_other` (5/24/00),
  reported honestly (no silent downgrade to basic — that would be the
  device-special-casing the hardware-role ADR forbids; basic-as-fallback is a
  fresh argument if a PDTE-less drive ever shows up).
- **`mos tray unlock` clears BOTH Prevent states** (basic ALLOW 0x00 then
  persistent ALLOW 0x02) so the tray ends unlocked whichever state held it. A
  transport failure on either is surfaced; an ANSWERED refusal on the persistent
  ALLOW (a PDTE-less drive's 5/24/00) is tolerated — the basic ALLOW already
  cleared what such a drive can hold. The `--persistent` flag is removed here
  too (it is now unconditional).
- **`mos tray lock` on a MOUNTED disc returns `already_locked` (a SUCCESS), not
  `MOS_ERR_BUSY`.** The lock CDB can't take exclusive access while mounted
  (`ObtainExclusiveAccess` → BUSY = "media is still mounted", SCSITaskLib.h), but
  macOS arms a tray Prevent when it mounts a disc (the 2026-06-21 mount-lock
  finding), so the requested state already holds. A new `MOS_TRAY_ALREADY_LOCKED`
  outcome / `already_locked` token carries this. A peer client holding exclusive
  access (`MOS_ERR_EXCLUSIVE_ACCESS`) is NOT translated — only a mount (BUSY)
  implies the OS lock. `mos tray unlock` on a mounted disc stays `MOS_ERR_BUSY`
  (it genuinely can't unlock) with a CLI hint to `tray eject` (which releases it).

**Why lock/unlock retain a point (the question this answers).** They are for
IDLE and UNMOUNTED drives — lock an empty tray or a blank/unmounted disc so a
stray operator eject can't fire the tray (the ripping-robot case, ROADMAP). For
MOUNTED discs the OS owns the lock and `tray eject` releases it, so the verbs
correctly defer there (`already_locked` for lock, the eject hint for unlock).

**Surface / API.** Public: `mos_tray_lock(h,out,sense)` and
`mos_tray_unlock(h,out,sense)` drop the `bool persistent` parameter (pre-tag,
mutable). Schema: `mos.tray.v1` drops the `persistent` field (and its `allOf`
clause) and adds `already_locked` to the `outcome` enum — pre-tag mutable-in-
place, the C↔schema drift guard keeps the enum in lockstep with
`mos_tray_outcome_description()`. No new command surface, no new CDB: the
opcodes are unchanged (0x1E ALLOW 0x00/0x02, LOCK 0x03), `mos_internal_raw_cdb`
stays the sole exclusive-access site, one-of-four count untouched.

**What hardware can falsify, never establish** (per the hardware-role ADR): a
drive whose mounted-disc Prevent does NOT hold (then `already_locked` would
over-claim) — but a mounted optical disc cannot be operator-ejected under macOS,
so the tray is held regardless; if a capture ever shows a mounted disc with an
ejectable tray, that lands as a fixture + dated note here before any change.
