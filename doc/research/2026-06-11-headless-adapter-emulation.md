# Headless adapter emulation — design record and build brief

**Date:** 2026-06-11. **Status:** approved for build; not yet implemented.
**Status update (2026-06-11, same day):** phase 1 LANDED —
`tests/fake/mos_fake_apple.{c,h}`, `tests/test_adapter_phase1.c`, the
`adapter-fake` CI job (ASan/UBSan, macos-latest), eight scenarios green;
seam-contract O-1/O-3 moved to CI (INTEGRATION_HARNESS item 0). Phase 2
(watch lifecycle) remains open. Known phase-1 fake limits are recorded
at their sites: by-name lookups ignore the name (N1), no IOReturn
injection on convenience methods (N2) — both phase-2 controls.
**Audience:** this is written to be a self-contained brief for a fresh
session told "build what this describes." It records the goal, the
chosen mechanism, the phased plan, the validation discipline, and —
deliberately — what it does and does **not** let us retire.

This is a design record, not a plan that steers behaviour. It follows
the AGENTS hardware ADR: emulation is a test instrument, never a source
of truth. Code is still built to spec (MMC-6 / T10) and to committed
fixtures; this document adds a way to exercise more of the tree against
those fixtures without a drive.

---

## 1. What we want to achieve

Today the pure layer (`mos_pure`, no Apple dependency) is exhaustively
tested headless, but the **Apple adapter TUs** — `src/mos_scsi.c`,
`src/mos_dr.c`, `src/mos_watch.c` — are only ever compiled and
type-checked off-device; their runtime behaviour is unverified until a
real Mac with a real drive runs them. That overloads the hardware gate:
it currently owes us *four* things — adapter behaviour, watch lifecycle,
IORegistry/identity semantics, and "does Apple's own kext/DiscRecording
behave as documented" — when only the last is genuinely hardware's to
answer.

**Goal:** emulate the Apple framework layer beneath the adapter so the
adapter TUs run headless, in CI, fed hex-perfect MMC replies from
committed fixtures (the same `.bin` fixture discipline the pure layer
already uses). This drives the full path —
`mos_open*`/`mos_query_state`/`mos_watch_*` → adapter → pure decoders →
classified state — under our control of every byte and every callback.

**The payoff is the hardware gate collapsing to falsification-only**
(AGENTS ADR §"hardware's role is falsification and fixtures"): once the
adapter, watch lifecycle, and identity semantics are headless-tested,
the only thing left that genuinely requires a drive is confirming that
*Apple's* layer behaves as its headers imply — which is precisely what
an emulator we wrote cannot vouch for. Everything else moves into CI.

---

## 2. The chosen mechanism: link-seam fake (Track 1a)

Build a fake static library that **provides the ~36 Apple C symbols the
adapter imports**, and link the adapter test binary against it instead
of the real frameworks. The references resolve to our archive at
static-link time; CoreFoundation is linked *real*.

Why this works cleanly (not luck — mechanics): macOS two-level namespace
records each import as a `(symbol, source-library)` pair. The
"two-level namespace blocks substitution" problem only bites when you
try to *replace* a symbol still bound to a real dylib. We sidestep it
entirely by **never putting IOKit.framework / DiscRecording.framework on
the link line** — there is no real binding to fight. Real CoreFoundation
coexists fine: its symbols and our fake symbols come from different
libraries and don't collide (IOKit/DR names are disjoint from CF's).
This is the textbook Feathers link seam, and it is a natural extension
of the existing `mos_pure`/`mos_core` split (which already lets the test
binary link without IOKit) up one layer to the adapter boundary.

Properties: SIP-irrelevant, no entitlements, no runtime injection, no
`DYLD_*`. **Runs on GitHub-hosted `macos-latest` — and, because the fake
is pure C, likely on Linux CI too** (the adapter TUs are pure C; only
the SDK headers' compile-time `_Static_assert`s on Apple constants need
satisfying — see §6 open question). `dyld __interpose` +
`DYLD_INSERT_LIBRARIES` is the documented fallback if we ever must
intercept a *real* framework's internal calls; it works under SIP for
our own non-hardened test binaries, but it is strictly more machinery
and is not needed for this plan.

### The seam inventory (from `doc/research/2026-06-10-seam-census.md`
### and the 2026-06-11 re-census)

Practical cut: **fake IOKit + DiscRecording, link real CoreFoundation.**
~36 link symbols:

- **IOKit: 17 functions + `kIOMainPortDefault`.** Registry iteration
  (`IORegistryEntryCreateIterator`, `IOIteratorNext`,
  `IORegistryEntryCreateCFProperty`, `IOObjectConformsTo`,
  `IORegistryEntryGetRegistryEntryID`, `IOObjectRelease`,
  `IOObjectRetain`), the COM factory
  (`IOCreatePlugInInterfaceForService`, `IODestroyPlugInInterface`),
  by-ID reopen (`IORegistryEntryIDMatching`,
  `IOServiceGetMatchingService`), path resolution
  (`IORegistryEntryFromPath`, `IORegistryEntryGetPath`), and the watch
  notification port (`IONotificationPortCreate`,
  `IONotificationPortGetRunLoopSource`, `IONotificationPortDestroy`,
  `IOServiceAddInterestNotification`).
- **DiscRecording: 9 functions + 9 `CFStringRef` key constants.**
  `DRCopyDeviceArray`, `DRDeviceCopyInfo`, `DRDeviceCopyStatus`,
  `DRDeviceCopyDeviceForBSDName`,
  `DRDeviceCopyDeviceForIORegistryEntryPath`,
  `DRNotificationCenterCreate`,
  `DRNotificationCenterCreateRunLoopSource`,
  `DRNotificationCenterAddObserver`,
  `DRNotificationCenterRemoveObserver`; plus the `kDRDevice*Key`
  constants the code reads.
- **COM vtables: 15 methods behind ONE factory.** All of
  `MMCDeviceInterface` (TestUnitReady, GetConfiguration,
  ReadDiscInformation, GetSCSITaskDeviceInterface, Release),
  `SCSITaskDeviceInterface` (ObtainExclusiveAccess, ReleaseExclusiveAccess,
  CreateSCSITask, Release), and `SCSITaskInterface`
  (SetCommandDescriptorBlock, SetScatterGatherEntries, SetTimeoutDuration,
  ExecuteTaskSync, Release) are reached only through pointers minted by
  `IOCreatePlugInInterfaceForService` → `QueryInterface`. Faking that
  one factory puts all 15 under our control. **The adapter never uses
  the async path, `GetTrayState`, or `ReadTableOfContents`** — the
  surface is smaller than the headers suggest.

CLI scope note: `cli/probe.c` (under `MOS_CLI_PROBE`) adds ~6 IOKit
functions + 3 data constants. Out of scope for phase 1; in scope only if
probe contract tests are wanted headless later.

---

## 3. What the fake must model statefully

Trivial symbols (release/retain/getters) are stubs. The state lives in
five models:

1. **IORegistry table:** id ↔ path ↔ service-object, child iteration
   (drive → whole-disk IOMedia child carrying `kIOBSDNameKey` + `Whole`
   bool + `IOMedia` conformance — `mos_scsi.c` walks exactly this),
   refcounts, **re-mint-on-replug** for drive IDs and
   **re-mint-on-media-swap** for the whole-disk IOMedia ID (the F1 swap
   fingerprint), and terminated-entry behaviour (by-ID reopen returns
   NULL after termination → the watch's terminal-removal path).
2. **DR directory:** device-array order (the public index contract),
   per-device Info dict (vendor/product/revision/registry path) and
   Status dict (media BSD name), consistent with the by-name/by-path
   lookups.
3. **Exclusive-access lock:** per-device; convenience methods never take
   it, the GESN raw path acquires+releases per call. A fake that tracks
   lock balance can **assert the §5.5 invariant on every test run** —
   free coverage the pure layer cannot reach.
4. **Notification delivery:** IOKit interest callbacks
   (Terminated/PropertyChange) and DR callbacks
   (StatusChanged/Appeared/Disappeared), delivered on the pump thread
   while `CFRunLoopRunInMode` is parked, wakeable at scripted monotonic
   times; plus the degraded paths (no source → poll-only; DR-create
   failure → all-mode open fails, per the 2026-06-11 fix).
5. **Per-call MMC reply scripts:** TUR (status + 18-byte sense),
   GetConfiguration, ReadDiscInformation, and the raw GESN task path
   (inspect the 10-byte CDB, return reply + status + sense +
   realizedByteCount), with arbitrary `IOReturn` injection for the
   error mapper. **These reply bytes are the committed `.bin` fixtures.**

---

## 4. Phased build plan

The split falls straight out of the model list: items 1, 2, 5 are the
one-shot paths; items 3, 4 are the watch lifecycle.

**Phase 1 — open / query / enumerate (weekend-sized).** Registry table +
DR directory + MMC reply scripting + the one COM factory. Replays the
existing one-shot fixtures (`getconfig_*`, sense, RDI) through the *real*
`mos_scsi.c`/`mos_dr.c`/`mos_state.c` and asserts the classified state.
Delivers: every non-watch adapter path headless, the §5.5 lock-balance
assertion, the seam-contract UNGUARDED clauses O-1 (result zero-init) and
O-3 (identity-string lifetime, under ASan) moved from hardware-gated to
CI. **Start here.** High value, low risk, proves the mechanism.

**Phase 2 — watch lifecycle (1–2 weeks, real state modelling).**
Notification delivery + run-loop wake timing + registry re-mint for
replug/swap. Replays watch scenarios (snapshot → state_changed →
device_removed; all-watch hot-plug join/leave; the F1 swap fingerprint;
`stream_open_ms` constancy). This is the part that is a genuine
subsystem, not a shim — the difference between "adapter TUs link and
run" and "watch lifecycle replays deterministically."

Both phases land behind a build flag (e.g. `MOS_BUILD_ADAPTER_FAKE`),
off by default, with a dedicated CI leg — mirroring how the fuzz and
strict-adapter legs are structured today.

---

## 5. The circular-oracle problem and cross-validation recipe

An emulator we write encodes our own MMC reading, so testing mos against
it proves **consistency, not correctness**. Worse: the candidate
external oracles are not fully independent — QEMU's SCSI CD model mirrors
its own IDE/ATAPI one (count it as **one** oracle, not two), and QEMU,
CDEmu, Linux `sr.c`/`cdrom.c`, systemd `cdrom_id`, and Apple's kext were
all written against the same MMC / INF-8090 (Mt. Fuji) drafts and the
same population of real drives. Their agreement rules out a *mos-specific*
misreading, not a misreading the whole community shares.

That residual is exactly why fixtures are **authored to the paper spec**,
not to an emulator. Five legs, different failure modes:

1. **Paper leg (authoring discipline).** Every fixture field carries an
   MMC-6 r02g clause citation in its README. The only leg that catches a
   community-wide misreading. Draft is freely mirrored (see §8);
   **checksum-pin a local copy while it is reachable** — T10's own copy
   is members-only.
2. **Synthesizer leg A — QEMU under Linux.** Boot a Linux guest with a
   `scsi-cd` (and separately ATAPI) drive, issue the exact CDB set with
   `sg_raw` (GESN polled/media-class, TUR+REQUEST SENSE, GET
   CONFIGURATION, READ TOC, READ DISC INFORMATION), hexdump replies;
   drive insert/eject/tray over QMP (`blockdev-change-medium`, `eject`).
   Diff **field-by-field** (classified fields, not byte-for-byte —
   lengths/ordering/vendor strings legitimately differ) with an explicit
   known-deltas list (QEMU: profiles 0x08/0x10 only, no async GESN, no
   blank-recordable model).
3. **Synthesizer leg B — CDEmu (or tcmu-runner `file_optical`) on a
   Linux VM.** Stronger for recordable/blank scenarios, sense detail, and
   TOC/RDI richness (MMC-3-complete). `tcmu-runner`'s `file_optical.c` is
   the lighter option (userspace, no kernel module vs CDEmu's vhba).
   Where QEMU and CDEmu disagree, the MMC draft adjudicates — those
   disagreements are the most informative output of the exercise.
4. **Consumer leg.** Run `cdrom_id` (prints its `ID_CDROM_MEDIA_*`
   conclusions) and observe `sr`/`cdrom.c` classification against the
   same emulated drives. `cdrom_id` builds its own CDBs over SG_IO, so it
   is genuinely independent of the kernel's parsing. Validates
   interpretation, not just bytes.
5. **Field-grammar validator.** Run synthesized fixtures through
   **Wireshark's `packet-scsi-mmc.c`** dissector — an independent
   field-by-field decode of GESN / GET CONFIGURATION payloads — to catch
   layout errors independent of our own encoder.
6. **Hardware leg — unchanged.** Real-drive captures enter only as
   falsification runs and committed fixtures, never as design input. A
   real byte contradicting legs 1–5 is the "crossflashed drive behind a
   USB-SATA bridge" case the adversarial-input doctrine already
   anticipates.

All external models are copyleft (see §7) — **reference and black-box
capture only, never copy code or struct tables**. Re-derive from the
MMC-6 draft.

---

## 6. What this retires — and (mostly) does NOT — from current testing

The user's framing is right that the new tests sit *closer to the Apple
API* than the pure-layer tests, and an adapter test that feeds bytes
through the real adapter into the pure decoders **transitively exercises
those decoders too**. The temptation is to treat the higher-level tests
as superseding the lower-level ones. **Resist it.** The honest accounting:

**Retire nothing by default.** The adapter emulation and the
pure-layer fuzz/exhaustive suites have *different threat models* and the
overlap is shallow:

- **Pure-layer fuzzing (1.5M+ iterations, 7 phases) and
  `nub_invariant_check` (full 2^28 status×key×ASC×ASCQ domain) are
  adversarial and exhaustive.** The adapter emulator replays a *fixed,
  hand-authored set of conformant-ish scenarios*. Retiring the fuzzer
  because adapter tests exist would trade a proof/﻿exhaustion for a
  sample — a strict, large loss of coverage. The fuzzer's whole point is
  the hostile octet domain MMC does not bound; the emulator's fixtures
  live inside the conformant subset. **Keep both. They are orthogonal.**
- **The nub invariant is a mathematical property over the entire input
  space**, mechanically proven against a transcribed kernel predicate.
  No quantity of scenario fixtures replaces a domain proof. **Keep.**
- **Pure tests run on any platform linking only `mos_pure`** (Linux CI,
  no Apple anything). Even if the adapter fake also runs on Linux, the
  pure suite stays the cheapest, most portable coverage. **Keep.**
- **`test_state_core.c` fakes the `mos_mmc_ops_t` vtable at the
  pure-core boundary.** The adapter emulator exercises the *same decision
  tree through the real adapter*. There is genuine scenario overlap here
  — but the pure-core test is the **spec oracle** (it is what the
  C↔schema drift guard and the §5.5 proof are wired to) and is
  platform-independent. The adapter test validates the *plumbing above
  it*, not the decision tree itself. **Keep the pure-core test as
  authoritative; the adapter test is additive.**

**What legitimately changes (additions and a small audit, not deletions):**

- **The hardware gate shrinks** (§9) — items move *out of* the
  hardware-gated list into CI. That is the real retirement: not test
  code, but the "needs a drive" status of O-1, O-3, V-1, the watch
  lifecycle, and identity semantics.
- **Candidate dedup audit (follow-up, evaluate — do not assume).** A
  handful of pure-layer fixtures may exist *only* to stand in for "what
  the adapter would have passed down" on a path the adapter test now
  drives end-to-end. Those specific fixtures *might* be re-homed (owned
  by the adapter test instead of duplicated) — but this is a per-fixture
  judgement, made with the project's verify-before-destroy discipline
  (CLAUDE.md rule 7: propose removals as a list, don't act), and only
  after both suites are green. The default for every fixture is **keep**;
  the burden is on showing redundancy, not on showing necessity.

Net: this work is **almost entirely additive**. The only thing it
*removes* is hardware-gated status from a set of checks; the pure
layer's adversarial and exhaustive guarantees are not substitutable and
stay exactly as they are.

---

## 7. Rejected alternatives (verdicts, so they are not re-litigated)

- **DriverKit virtual SCSI dext (`SCSIControllerDriverKit`,
  `IOUserSCSIParallelInterfaceController`).** Architecturally the
  highest fidelity — the kernel's SAM matching is provably
  transport-agnostic (INQUIRY → `kIOPropertySCSIPeripheralDeviceType` →
  type-05 personality → `IOSCSIMultimediaCommandsDevice`), so a type-5
  LUN behind a dext *should* yield a real IORegistry optical node the
  whole stack (kext, DR, drutil, mos) sees. **But:** nobody has publicly
  demonstrated a type-5 LUN behind a dext; the family entitlement is
  Apple-approval-gated for distribution (dev-variant entitlements exist
  for paid accounts, so local experiment is possible); and **dext
  activation is impossible on hosted CI** (interactive approval, no
  recoveryOS, SIP state uncontrollable). Verdict: **dev-Mac fidelity
  option only; never CI. Parked, not pursued.**
- **VM full-stack (macOS guest + QEMU emulated optical).** Apple-silicon
  Virtualization.framework has **no optical device class at all** (kills
  Tart/Anka/UTM-VZ); hosted runners can't nest VMs; the only theoretical
  lane (Intel Mac + QEMU/KVM + USB `media=cdrom`) hinges on an
  unresolved, unreported experiment (does the guest's MMC stack attach?).
  Verdict: **lab curiosity, not infrastructure.** (QEMU stays useful as a
  *cross-validation capture rig under Linux* — §5 leg 2 — which is a
  different, settled use.)
- **`hdiutil attach` / disk images / Remote Disc.** An attached image is
  an `IOBlockStorageDevice`-family stack top to bottom; it never touches
  the SCSI Architecture Model, never gets `SCSITaskAuthoringDevice`, and
  is invisible to `MMCDeviceInterface`, `DRCopyDeviceArray`, and
  `drutil`. Remote Disc is filesystem-level. Verdict: **cannot reach the
  code under test. Dead end.**
- **`scsi_debug ptype=5`.** Sets the INQUIRY type byte only; implements
  no MMC opcode. Verdict: **dead end beyond INQUIRY.**

---

## 8. Licensing and the re-derive rule

Every executable MMC model found is copyleft: QEMU
(MIT atapi.c / LGPL scsi-disk.c / GPLv2+ sense tables), CDEmu (GPL-2.0+),
libcdio (GPL-3.0+), Linux kernel (GPL-2.0), Aaru.Decoders (LGPL-2.1).
**mos is 0BSD — none of this code or its struct/constant tables may be
copied or translated into the tree.** They are reference-reading and
black-box capture oracles only. Fixture layouts are re-derived from the
MMC-6 r02g draft, which is what every one of those implementations was
itself written against, making it the correct reference rather than a
compromise.

---

## 9. What the hardware gate becomes

Before: hardware owes adapter behaviour + watch lifecycle + identity
semantics + Apple-layer conformance + fixture capture.

After phase 1: O-1, O-3 and the one-shot adapter paths are CI-tested.
After phase 2: watch lifecycle and registry/identity semantics are
CI-tested. **What remains genuinely hardware-only:** (a) does Apple's
actual kext / DiscRecording behave as the headers imply — the one thing
our own fake cannot vouch for — and (b) fixture capture from real drives.
That is exactly the AGENTS ADR's definition of the gate, reached by
construction. Update `INTEGRATION_HARNESS.md`'s gate and falsification
list accordingly when each phase lands.

---

## 10. First task for the build session

Phase 1, smallest end-to-end slice:

1. Add `MOS_BUILD_ADAPTER_FAKE` (off by default) and a fake static lib
   target providing the IOKit + DiscRecording symbols, real CF linked.
2. Implement the registry table + DR directory + the one COM factory,
   with **one** scripted scenario: a single DVD-ROM drive, media present,
   replying from an existing committed fixture
   (`getconfig_dvdrom_current.bin` + a TUR sense + an RDI reply).
3. One test: `mos_open_by_index(1)` → `mos_query_state` → assert
   `MOS_STATE_READY` + profile, running the *real* `mos_scsi.c` /
   `mos_state.c`. Assert the §5.5 lock balance returned to zero.
4. Wire a CI leg (build with the flag, run the test). Confirm it runs on
   `macos-latest`; **check whether it also runs on Linux** (open
   question: the SDK headers' compile-time `_Static_assert`s on Apple
   constants — if they block a Linux build, either provide surrogate
   constant headers in the fake or keep this leg macOS-only).

Then iterate scenarios (empty, open-tray via GESN, loading, the error
paths) before starting phase 2.

Cross-validation (§5) is a parallel workstream, not a phase-1 blocker:
stand up the QEMU-under-Linux `sg_raw` capture and the Wireshark
field-diff so new fixtures are validated against an independent oracle as
they are authored.

---

## 12. Phase-2 pickup brief (appended 2026-06-11, after phase 1 landed)

Phase 1 was built from this doc PLUS session context that was never
written down. This section records that context so a fresh session can
start without rediscovering it. (The Linux-build open question in §10
item 4 is settled: macOS-only by design — the fake populates real SDK
vtable structs.)

**Load before writing anything:**
1. This doc: §2 (the watch symbols are already in the inventory),
   §3 items 1 and 4 (the two stateful models phase 2 IS), §4 (scope +
   the scenario list).
2. `src/mos_watch.c` IN FULL — it is the spec the fake must satisfy:
   the DR callback signatures are visible in its own callback
   definitions (`dr_status_changed_callback` etc.), the private
   run-loop mode constant, the pump's deadline math, the degraded
   paths, and the all-mode open's doorbell-or-fail gate.
3. `src/mos_watch_core.c` header comment (the ops contract, the
   mono/wall clock domains) and `tests/test_watch_core.c` — the
   expected event sequences phase-2 scenarios must reproduce through
   the REAL adapter instead of fake ops.
4. `tests/fake/mos_fake_apple.{c,h}` and `tests/test_adapter_phase1.c`
   — extend, don't replace. N1 (by-name matching) and N2 (IOReturn
   injection) are recorded at their sites as phase-2 controls.
5. **Exact SDK signatures — fetch, never write from memory.** Phase 1
   pulled them from the phracker/MacOSX-SDKs GitHub mirror, e.g.
   `raw.githubusercontent.com/phracker/MacOSX-SDKs/master/
   MacOSX10.9.sdk/System/Library/Frameworks/IOKit.framework/Versions/
   A/Headers/IOKitLib.h` (notification-port functions live there;
   SCSITaskLib.h and CFPlugInCOM.h were already harvested for phase 1).
   DiscRecording is silently absent from current SDKs but unchanged —
   old SDKs in the same mirror carry DRCoreDevice.h. Zero
   signature-mismatch CI iterations in phase 1 is attributable to this
   step.

**Authoring workflow (the dev container is Linux, no Apple SDK):** the
fake and adapter TUs cannot compile locally. The loop that worked:
edit → `gcc -fsyntax-only` the Apple-header-free test TU → push →
`workflow_dispatch` CI (runs ~40 s) → adapter-fake job verdict. CI is
the compiler; budget round-trips for it. Build wiring: add
`src/mos_watch.c` to the `mos_adapter_fake_tests` target and the
notification symbols to the fake (the CMake comment marks the spot).

**Source-availability map (verified 2026-06-11) — what each fake
component can and cannot be checked against:**
- *DiscRecording*: headers SDK-only (silently dropped from current
  SDKs, unchanged in older ones — the phracker mirror carries
  DRCoreDevice.h etc.). **Source was never published** — not in
  apple-oss-distributions, aosm, or opensource.apple.com. The DR side
  of the fake has NO source oracle: headers + observed behaviour only.
- *IOKit SCSI/MMC userspace headers* (`SCSICmds_*`,
  `SCSICommandOperationCodes.h`, `SCSITaskLib.h`,
  `IOSCSIPeripheralDeviceType05.h`, `IOCDTypes.h` family): current and
  complete in the SDK.
- *IOSCSIArchitectureModelFamily* (the SAM kext:
  IOSCSIMultimediaCommandsDevice, the SCSITaskUserClient stack): the
  apple-oss-distributions repo EXISTS but its last tag is
  **139.0.2, February 2005** (verified; aosm and PureDarwin mirror the
  same lineage). The shipping driver has ~20 years of closed drift.
  Consequence: every kernel-predicate citation in this repo — the
  §5.5 PollForMedia transcription, the TUR-under-exclusivity gate, the
  GetTrayState masking — is built on Tiger-era source. The proofs
  hold as "equals the published predicate"; the published-vs-shipping
  gap is the hardware falsification leg's to close, and the vintage
  makes that leg MORE load-bearing than ARCHITECTURE currently
  conveys (candidate doctrinal annotation, not made here).
  *Drift calibration (verified 2026-06-11):* the CURRENT SDK
  (MacOSX26.4) still ships `SCSITaskLib.h` with
  "Copyright (c) 2001-2009" and availability markers stopping at
  10.6 — the userspace SAM interface has been byte-frozen for ~17
  years, a strong prior that the implementation is quiescent and the
  published-vs-shipping gap small. A prior, not a proof: binaries
  change behind frozen headers, and the Ventura 13.2 internal-SATA
  optical breakage (hackintosh reports, 2023) proves nonzero drift
  SOMEWHERE in the optical stack — plausibly the ATAPI transport
  below SAM, unconfirmed. Net: the gap is probably small; the
  hardware leg still owns the residual.
- *IOStorageFamily* (the layer above: IOCDBlockStorageDriver,
  IOMediaBSDClient): current and open (IOStorageFamily-323, 2025).
- *xnu*: current and open; carries the IOKit SCSI headers at source
  level.

**Two design problems §3 names but does not solve — settle these
FIRST, they are the actual phase-2 work:**
- *Callback delivery into a parked run loop.* The adapter blocks in
  `CFRunLoopRunInMode` (private mode); the fake's
  `IONotificationPortGetRunLoopSource` /
  `DRNotificationCenterCreateRunLoopSource` must return REAL
  `CFRunLoopSourceRef`s (CF is linked real) and the fake must wake the
  loop at scripted points. Candidate mechanism: version-0
  CFRunLoopSource, signalled (`CFRunLoopSourceSignal` +
  `CFRunLoopWakeUp`) from inside the fake's scripted probe hooks, so
  events land between pump steps deterministically and without
  threads. Unvalidated — prove it on CI before building scenarios on
  it.
- *Time control.* `mos_watch.c` has its own static `monotonic_ms()`
  over real `clock_gettime` — poll-deadline scenarios are
  nondeterministic unless time is seamed too. Candidate: define
  `clock_gettime` in the fake TU; calls from TUs statically linked
  into the same executable should resolve to it ahead of libSystem.
  **This is the load-bearing unknown of phase 2 — verify with a
  one-assert CI probe before anything else.** Fallback if it fails:
  real-time tests with generous tolerances (slower, softer
  assertions), or accept that wake-ORDER is assertable and wake-TIMING
  is not.

---

## 11. Source index (verified during the 2026-06-11 research)

Confidence and provenance for the load-bearing external claims:

- **Link seam / two-level namespace / dyld interpose under SIP:** Apple
  TwoLevelNamespaces release notes; Apple DTS forum 731358 (protected-
  binary gating); dyld OSS (`DyldProcessConfig.cpp` AMFI gate). High
  confidence.
- **Seam inventory:** repo-local, mechanical grep over the adapter TUs
  (`doc/research/2026-06-10-seam-census.md` + 2026-06-11 re-census).
  Exact.
- **DriverKit SAM matching transport-agnostic:** apple-oss-distributions
  `IOSCSIArchitectureModelFamily` (`IOSCSIPeripheralDeviceNub.cpp`
  publishes `kIOPropertySCSIPeripheralDeviceType`),
  `IOSCSIParallelFamily`; WWDC20 session 10210. Matching chain verified
  at source; type-5-behind-dext **not** demonstrated publicly (the one
  unverified link).
- **CI cannot load dexts/kexts:** actions/runner-images #8162 (SIP
  uncontrollable), discussion #10159 (interactive approval). High
  confidence.
- **QEMU / CDEmu MMC surface:** verified against current source
  (`hw/ide/atapi.c`, `hw/scsi/scsi-disk.c`; cdemu `device-commands.c`).
  Both media-event-class + polled GESN only; QEMU READ DISC INFO
  hardcoded; CDEmu MMC-3, no UA stacking / no NOT-READY progression.
- **hdiutil never reaches SAM:** Apple SAM Device Interface Guide +
  `SCSITaskLib.h` (`SCSITaskAuthoringDevice` published only by SAM);
  structurally certain, no counterexample found.
- **Paper reference:** MMC-6 r02g (T10/1836-D) public mirror at
  13thmonkey.org; SPC-3 likewise; T10 `asc-num.txt` live. Mirror
  stability not guaranteed — checksum-pin on first download.
- **Prior-art negative:** no project in the optical ecosystem replays
  hex-perfect MMC fixtures through a faked OS layer (libcdio tests above
  MMC; whipper mocks tool text; umockdev has no SG_IO). sg3_utils
  `inhex/` + `--inhex` is the closest neighbour (manual, disk-flavoured).
  mos's combination is novel; each half has precedent.

Full per-track research transcripts (twelve threads, most double-covered)
were produced in the 2026-06-11 session that generated this record.
