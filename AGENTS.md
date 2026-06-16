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
