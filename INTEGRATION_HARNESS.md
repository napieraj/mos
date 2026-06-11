# INTEGRATION_HARNESS.md

Template and process for contributing hardware-validated fixtures and
state-matrix rows. The library ships with zero real-hardware test data
at v0.2; every entry in this document starts blank.

## Why this exists

`ARCHITECTURE.md` §9 Known Unknowns lists behaviors that cannot be
resolved by desk research. They all require putting a disc in a drive
and observing what the drive reports. This document is the place to
record those observations so future maintainers inherit the data
rather than rediscovering it.

## Test matrix

Run each of the six states against your drive and record the results.
"—" means untested. Fill your drive's row and submit via PR.

| Drive                    | Bus    | macOS     | Open  | Empty | Loading | Ready | Mounted | Authoring? |
|--------------------------|--------|-----------|:-----:|:-----:|:-------:|:-----:|:-------:|:----------:|
| Apple SuperDrive A1379   | USB    | —         |  —    |  —    |  —      |  —    |  —      |  —         |
| LG BH16NS55 (BD writer)  | SATA   | —         |  —    |  —    |  —      |  —    |  —      |  —         |
| LG WH16NS60              | SATA   | —         |  —    |  —    |  —      |  —    |  —      |  —         |
| LG BU40N                 | USB    | —         |  —    |  —    |  —      |  —    |  —      |  —         |
| Pioneer BDR-X13U         | USB    | —         |  —    |  —    |  —      |  —    |  —      |  —         |
| Pioneer BDR-212U         | SATA   | —         |  —    |  —    |  —      |  —    |  —      |  —         |
| Buffalo BRXL-16U3        | USB    | —         |  —    |  —    |  —      |  —    |  —      |  —         |
| LG WH14NS40 (BD-ROM)     | SATA   | —         |  —    |  —    |  —      |  —    |  —      |  —         |

For each cell: ✓ if the tool returned the expected state, ✗ if it
returned something unexpected, with a note explaining what it returned
instead and why.

## States to exercise

For each drive, induce each of these physical situations and record
what `mos --json` prints.

### Open

Eject the tray. No disc loaded. Expected: `"state": "open"`.

### Empty

Close the tray with no disc in it (slot-loading drives: leave the slot
empty). Expected: `"state": "empty"`.

### Loading

Insert a disc and immediately query before the drive finishes
spinning up. Expected: `"state": "loading"` (may need to catch the
timing window).

### Ready

Insert a disc, wait for the drive to spin up, and prevent macOS from
auto-mounting it. (Unmount the volume manually if it mounts: `diskutil
unmount /Volumes/YourDisc`.) Expected: `"state": "ready"` with a
`current_profile` like `0x0040` for BD-ROM.

### Mounted

Insert a disc and let Finder auto-mount it. Expected: `"state":
"ready"` still — the MMC convenience methods do not require exclusive
access, so the mount should not interfere. This is the single most
important test for validating the v0.2 architecture fix.

### Busy (optional)

Launch MakeMKV or another tool that holds the drive with exclusive
access. Expected: `"state": "busy"`.

## DR pivot falsification rows (2026-06-10)

Hardware-session targets added by the DR pivot (plan §coexistence and
execution notes). Hardware falsifies or feeds fixtures — never steers
design (AGENTS hardware ADR):

- **Registry-path shape**: does `kDRDeviceIORegistryEntryPathKey`
  resolve to the IO*BlockStorageDevice node the MMC plug-in attaches
  to? (`mos probe --dump` shows the path; a mismatch appears as
  DRIVER_REJECTED opens and is fixed inside mos_dr.c.)
- **drutil parity**: `mos list` index order vs `drutil list` on a
  multi-drive rig.
- **Identity byte-shape**: DR's pre-parsed identity strings vs the
  SPC-4-trimmed INQUIRY forms (capture both; diff).
- **Doorbell delivery**: do Appeared/Disappeared/StatusChanged fire
  under NULL-object registration at all, and at what latency vs the
  poll floor? `mos probe <drive>` logs the DR legs alongside the
  IOKit-interest legs with monotonic timestamps. (The probe's DA
  control arm was retired 2026-06-11 with DA itself — doorbells are
  latency-only over the poll floor, so completeness needs no
  comparison baseline; the poll timestamps in the same log are the
  reference.)
- **Coexistence**: `mos probe diskN` + `mos watch diskN` through
  tray cycles — does mos's temporary exclusive GESN window make DR's
  own observers mis-observe (the §9.7 collapse on DR's side)?
- **watch-all hot-plug**: join/leave ordering, device_appeared on
  plug, per-drive device_removed on unplug, two-drive interleave
  fixture.

## Hardware validation gate

(Moved here from STATUS.md when that file was retired to
doc/history/, 2026-06-11 — this is the live copy.)

This gates the FIRST TAG (none exists yet; the tree is v0.4.0-dev —
v0.3 was never tagged and won't be, its line continued into v0.4
development). The contract surface being validated is stable.
Validation should:

1. Full CMake build + `ctest` on an Apple-toolchain host.
2. Manual smoke per matrix drive in each state:
   - `mos --json`: confirm `"schema": "mos.state.v1"`,
     `"bsd"` (full dev node since the 2026-06-10 CLI redesign; was `"bsd_name"`), `"current_profile"`,
     `"current_profile_name"` populated.
   - `mos --json --index 99` (no-drive): confirm
     `mos.error.v1` envelope with nested
     `error.{code, message, context, recoverable}` and
     `exit_code: 66`.
   - Bare `mos --index 99`: confirm empty stdout, stderr
     diagnostic, exit 66.
   - `mos --json --watch --bsd diskN`: confirm initial
     `snapshot` event, then `state_changed` on tray / media
     transitions, then `device_removed` on detach.
3. If a query-failure can be deliberately induced, confirm
   partial-failure `mos.error.v1` envelope surfaces `bsd_name`
   correctly.
4. `mos --json=v2` (or any `=value`): confirm `EX_USAGE` (64)
   with diagnostic naming `mos.state.v1`.
5. Capture per-drive fixtures via
   `mos status --json > fixtures/<drive>/<state>.json` (plus
   `mos probe --dump` for the DR dictionaries).

Once 1-4 pass, the first tag is cuttable. Step 5 informs the
typed-API design (ROADMAP v0.4).

### Falsification runs (post-2026-06-10 scope reduction)

The blanket "adapter unvalidated" framing has shrunk: the adapter seam
is now verified against the kernel's own source — GetTrayState masking,
the §5.5 nub invariant, TUR exclusivity, IOReturn pins, the GESN CDB
(ARCHITECTURE §9.7, §5.5, §11). What hardware still owes us is
**falsification + fixture acquisition**, never design input
(AGENTS.md doctrine):

0. **Seam contract UNGUARDED clauses** (doc/seam-contract.md appendix —
   the three obligations the pure suite structurally cannot see, all
   adapter-shaped). *Status 2026-06-11: the adapter-fake CI job (phase 1
   of doc/research/2026-06-11-headless-adapter-emulation.md,
   tests/test_adapter_oneshot.c) moved O-1 and O-3 out of this list —
   both now run headless on every push: O-1 as the profile==0
   assertion on every non-READY scenario, O-3 as ASan-checked
   vendor/product reads on a live result through the REAL adapter. The
   same job pins the §5.5 lock balance (acquired-exactly-once AND
   returned-to-zero) and the GESN CDB bytes.*
   *Status 2026-06-11, phase 2 (tests/test_adapter_watch.c): the
   watch lifecycle and identity semantics joined the same job — the
   REAL mos_watch.c on a fake clock replays snapshot/state-change/
   removal (notification AND poll-floor paths), the F1 and replug
   registry-ID re-mints, stream_open_ms constancy and uniqueness
   across streams, the error-backoff cadence in exact fake
   milliseconds, watch-all join/leave/rejoin with the doorbell-or-fail
   open gate, and the IOReturn-mapper arms including
   kIOReturnNoDevice → terminal removal. What that CANNOT vouch for is
   unchanged in kind: whether APPLE's kext/DiscRecording behave as the
   fake models them — which is exactly items 1 and 6 below, plus
   fixture capture (item 2). What hardware still owes THIS item is
   only V-1:*
   - **V-1**: eject and re-insert while running one-shot queries;
     confirm `bsd_unit` flips to -1 and back driven by NODE absence
     (e.g. state `loading` with unit still -1 mid-spin-up is correct),
     not by the state enum — every fake (the pure suite's AND the
     adapter fake's registry model) scripts unit and state together,
     so the real-kext coupling is untestable headlessly.

1. **Insert-under-watch** (pass/fail): `mos --watch` at default rates
   during a real insert; compare mount latency to a no-watch baseline,
   watch IORegistry for repeated nub teardown. Retires the §5.5
   backward-flip and UA slivers. While there: two rapid `mos --watch`
   opens on the same drive must show distinct `stream_open_ms` values.
   *(2026-06-11: the monotonicization mechanism itself is now pinned
   headless — adapter-fake phase 2's replug scenario asserts distinct
   stream_open_ms across two streams on a controlled wall clock; what
   this run still owes is the real-kext/DR delivery behaviour around
   it.)*
2. **Fixture acquisitions** for paths currently spec-only:
   descriptor-format sense (0x72/0x73) from a real device; a bridge
   that omits `media_id` (exercises the profile-fallback swap path);
   an LG stale-profile sequence (pins the §9 suppression with real
   bytes); opportunistically, a damaged disc to timestamp the kernel
   auto-eject corollary.
3. **Index-order comparison**: `drutil list -xml` vs `mos list`,
   repeated across hotplug (doc/research/2026-06-10-drutil-contract.md
   tiering; retires the Inferred tier).

4. **Multi-drive `mos watch` guard** (2026-06-11 fix, not headless-
   testable): with two drives attached and no selector, `mos watch`
   must print the mini-list to stderr and exit 64 — same contract as
   `mos status`. One drive: implied, as before.

5. **`bsd_unit` fallback branch on real bridges** (2026-06-11 fix):
   does any owned bridge actually expose its BSD name on a non-IOMedia
   node (taking `mos_internal_bsd_unit`'s fallback), and does that
   node's registry ID survive a disc swap? One `mos probe --dump`
   before/after a swap answers both. The branch now mints media_id 0
   (don't-infer-a-swap sentinel); a captured fixture either pins that
   or retires the question.

6. **DR doorbell setup failure in practice**: `mos_watch_open_all` now
   fails the open if `DRNotificationCenterCreate` / run-loop source
   creation fails. (The CI doorbell guard proves driveless
   REGISTRATION is accepted on every push; adapter-fake phase 2
   exercises the fail-the-open gate and full callback delivery through
   OUR fake center — what stays here is delivery through APPLE's real
   DiscRecording, and any environment where registration fails.)
   If a real Mac ever shows this failing, that observation funds the
   rescan-fallback decision parked in ROADMAP (2026-06-11 intake
   remainders).

A surprise observed on hardware lands as a committed `.bin` fixture and
the pure layer is built to the fixture — defenses generic, never
device-special-cased.

## Anticipated quirks from prior art

The closest peer in the FOSS world is systemd's `cdrom_id`
(`src/udev/cdrom_id/cdrom_id.c`, ~1023 lines): a userspace MMC
prober that opens a device, runs the same five-or-so commands mos
runs (INQUIRY, GET CONFIGURATION, READ DISC INFO, READ TOC, plus
TUR via the kernel's CDROM_DRIVE_STATUS shortcut), and reports
state. Same shape, same target hardware, same kind of consumer
(udev rather than a consuming application, but structurally equivalent). When we
need to know "how does a careful userspace optical-state prober
handle X," cdrom_id is the answer, not the kernel.

The kernel files (`drivers/scsi/sr.c`, `drivers/cdrom/cdrom.c`)
operate one layer below mos and cdrom_id — they handle block-device
retry loops, kernel-context error recovery, and sysctl-driven user
policy. Most of their accumulated wisdom applies to a layer mos
doesn't have, but a handful of "watch for this on hardware" notes
crossed over and are worth recording.

What follows is curated from both sources, with the citation
indicating which body of code carries the lesson. The distinction
matters: cdrom_id lessons are directly applicable; kernel lessons
are mostly cross-validation that mos's design choices match what a
mature implementation does at a different layer.

### TUR-on-repeat hangs

`sr_check_events` (drivers/scsi/sr.c) carries an explicit comment:
*"Note that there are devices which hang if asked to execute TUR
repeatedly."* The kernel mitigates by gating TUR behind the
DISK_EVENT_MEDIA_CHANGE clearing path rather than firing it on every
poll cycle.

mos v0.2 issues exactly one TUR per `mos_query_state` call, which is
one per CLI invocation — the v0.2 hardware test pass won't trip this.
v0.3 `--watch` will be the first scenario where mos issues TUR
repeatedly against the same handle. Hardware fixture capture should
include "100 consecutive `mos --json` invocations against a closed
empty drive within 10 seconds" as a smoke test before `--watch`
work begins. If any drive in the matrix locks up, we have an
empirically-measured upper bound on poll frequency rather than
copying the kernel's heuristic on faith.

### GES and TUR disagree on some firmware

Same file, `sr_check_events` again: the kernel maintains a
per-device `tur_mismatch` counter, and after eight consecutive
disagreements between `GET EVENT STATUS NOTIFICATION` and
`TEST UNIT READY`, prints the warning *"GET_EVENT and TUR disagree
continuously, suppress GET_EVENT events"* and sets `ignore_get_event
= true` for the device. mos's design — TUR as the definitive
presence signal, raw GESN consulted only on the not-ready branch
(ARCHITECTURE §3) — anticipates this; the kernel's heuristic
is what happens when you have to deal with the same problem in a
context where you can't afford to fall through cheaply.

Hardware capture should include *deliberate* GES/TUR disagreement
scenarios: tray-flap events, freshly-powered drives with stacked
UAs, and post-mount-unmount transitions. We're not implementing the
8-strikes-and-out heuristic in v0.2, but we should know which drives
in the matrix would have triggered it.

### Media-presence inferred from sense, not just status

`sr_check_events` continues: *"Media is considered to be present if
TUR succeeds or fails with sense data indicating something other
than media-not-present (ASC 0x3a)."* This matches mos's sense-to-
state mapping (`mos_internal_state_from_sense_closed` routing
ASC=0x3A to EMPTY on the closed branch; the tray fork itself is
GESN's, with the 3A qualifier as fallback — ARCHITECTURE §5.3/§5.4).
Useful as cross-validation: the kernel and mos arrived
at the same heuristic independently from the same T10 source.

Implication for fixture capture: when capturing the CHECK CONDITION
path, log the full sense bytes. Some drives return non-spec ASCQ
values for media-not-present (we've seen 0x3A/0x00 instead of 0x3A
/0x02 in libcdio's tracker history), and mos's switch on ASCQ falls
through to EMPTY by default — but only the fixture corpus will tell
us which BH/WH firmware revisions land on which ASCQ.

### Two-pass GET_CONFIGURATION (v0.3 prep)

The canonical implementation is in systemd's `cdrom_id`
(src/udev/cdrom_id/cdrom_id.c, function `cd_profiles`), not in the
kernel — cdrom.c has the same two-pass *shape* but uses it for
READ_DISC_INFO (`cdrom_get_disc_info`), not GET_CONFIGURATION.

cdrom_id issues GET_CONFIGURATION with allocation length 8 to read
the BE32 data-length header, then reissues with `len = max(parsed,
sizeof(features))`. On the second pass it *re-parses the length*,
with the comment *"in case the drive decided to have other
features suddenly :)"* — defensive against firmware that returns
different answers between sequential calls. Truncation discipline:
if parsed length exceeds the 65530-byte buffer, log and clamp; if
parsed length is less than 8, treat as garbage and use the full
buffer.

The cdrom_id approach also includes a sense-key-driven fallback
(see next section) that mos v0.3 should adopt rather than
re-deriving. Not a v0.2 test item; this is recorded as a v0.3
reference implementation, particularly for `mos_enumerate_features()`.

### Pre-MMC2 fallback via sense match (v0.3 prep)

cdrom_id's `cd_profiles` distinguishes "command failed" from
"command failed *in this specific way*" — when GET_CONFIGURATION
returns CHECK CONDITION with `SK==0x5 && (ASC==0x20 || ASC==0x24)`
(ILLEGAL REQUEST + INVALID COMMAND OPCODE or INVALID FIELD IN CDB),
the prober falls through to `cd_profiles_old_mmc`, which uses
READ_DISC_INFO instead. This is the pattern for graceful
degradation against drives that predate MMC-2's GET_CONFIGURATION.

The reference rig (BH16NS55, WH16NS60, A1379) all postdate MMC-2 by
roughly two decades — we will not exercise this path on hardware.
But mos v0.3's typed APIs should structure their error handling
the same way: not a binary "feature query failed → give up" but a
sense-keyed dispatch that maps `SK=5/ASC=0x20|0x24` to a
"this drive doesn't speak this command, try the older alternative
or report unsupported" branch. The pattern matters even if no drive
in our matrix triggers it, because it sets the architecture for
how typed APIs handle command-not-supported responses generally.

### Vendor-firmware behavior is not predictable from vendor string

The cdrom-standard.rst design-history document (David van Leeuwen,
1996–1997) records the original motivation for the uniform-driver
abstraction: *"some drivers close the tray if an open() call occurs
when the tray is open, while others do not. Some drivers lock the
door upon opening the device, to prevent an incoherent file system,
but others don't, to allow software ejection. Undoubtedly, the
capabilities of the different drives vary, but even when two drives
have the same capability their drivers' behavior was usually
different."*

cdrom_id carries the same lesson forward into present-day code with
a concrete inline workaround in `cd_media_info`: *"fresh DVD-RW in
restricted overwrite mode reports itself as 'appendable'; change it
to 'blank' to make it consistent with what gets reported after
blanking, and what userspace expects."* The drive's READ DISC INFO
response is technically not wrong — it just doesn't match what
userspace expects, so the prober normalizes. This is the exact kind
of vendor-firmware quirk our fixture corpus should be capturing.

Concrete consequence for the mos fixture matrix: don't infer
expected behavior from INQUIRY VENDOR_IDENTIFICATION. An LG drive
with HLDS firmware revision 1.00 may handle a tray-flap differently
from the same drive with firmware 1.02. The fixture corpus should
record firmware revision (already in the matrix) and treat
behavior as drive+firmware tuple, not just vendor.

### INQUIRY peripheral-type validation (v0.3 prep)

cdrom_id's `cd_inquiry` opens with a defensive check before issuing
any MMC-specific commands: `if ((inq[0] & 0x1F) != 5) return "Not
an MMC unit."` Peripheral device type 5 is MMC (T10 SPC-4 §6.4.2).
The kernel handles this through SCSI-bus matching at module-load
time (`MODULE_ALIAS_SCSI_DEVICE(TYPE_ROM)`); cdrom_id, running in
userspace against an arbitrary `/dev/sr*` node, has to validate
inline.

mos's IOKit matching path filters by IOSCSIPeripheralDeviceType=5
already, so v0.2 doesn't strictly need this check. But v0.3's typed
APIs that operate on a `mos_handle_t` constructed from a BSD name
(via `mos_open_by_bsd_name`) effectively receive an arbitrary
device path from the caller; an inline INQUIRY-type check inside
`mos_handle_init` would catch the "user passed `disk2` thinking it
was their optical drive but it's actually a USB stick" case before
any command issue. Recording here so the v0.3 work surfaces it
explicitly.

### Profile-byte sentinel choices differ — note ours

Linux uses 0xFFFF as the "couldn't read MMC3 profile" sentinel
(`cdrom_mmc3_profile`: *"if (cdi->ops->generic_packet(cdi, &cgc))
mmc3_profile = 0xffff;"*). mos uses 0x0000, which happens to match
MMC-6's defined "no current profile" value. Both choices are
defensible; ours is better because it doesn't collide with any
real profile code, while 0xFFFF in MMC-6 is reserved. Recorded
here so the fixture-capture review doesn't get confused by the
divergence when cross-referencing kernel behavior.

### Lock-lifetime discipline (v0.3 prep)

Two cross-validating examples. The kernel side: `cdrom_release` in
drivers/cdrom/cdrom.c always calls `lock_door(cdi, 0)` on the way
out (debug log: *"door unlocked"*) — every PREVENT MEDIUM REMOVAL
must be paired with an ALLOW MEDIUM REMOVAL, including on abnormal
process exit. Persistent prevent (sense code 3, "MEDIUM REMOVAL
PREVENTED") survives reboot on some drives, and a CLI tool that
exits without releasing leaves the user's drive locked with no
obvious recovery.

The userspace side: cdrom_id's `media_lock` does something subtler
that's worth recording. Before issuing `CDROM_LOCKDOOR`, it calls
`ioctl(fd, CDROM_CLEAR_OPTIONS, CDO_LOCK)` to disable the kernel's
lock policy. Translation: "I'm taking over the lifecycle from here,
don't let the kernel's autoclose/autoeject layer also try to
manage it." This is a two-layer policy pattern — the kernel offers
a lock policy that's on by default; cdrom_id explicitly opts out
before applying its own policy on top. macOS doesn't have a direct
equivalent (no ambient kernel lock policy on optical drives that I
can find), but the *pattern* — "if there's a wrapping policy
layer, opt out before applying your own" — would matter if mos
ever had to coexist with DiskArbitration's own ideas about device
state.

When v0.4 introduces the tray verbs, `atexit` cleanup on the
PREVENT path is non-negotiable. Recording here so the v0.4
hardware test plan includes a "kill -9 the process while it holds
PREVENT" scenario. The cdrom_id `media_lock` shape is the
reference implementation if we need a userspace example; the
kernel side is the discipline-on-abnormal-exit example.

### UA handling matches the userspace peer (cross-validation)

cdrom_id has *no* application-level UNIT ATTENTION retry. It
relies entirely on the Linux sg layer's automatic UA drain — the
kernel runs the retry loop and returns the first non-UA answer to
userspace. mos, since the 2026-05-30 redesign, also has none:
TEST UNIT READY is issued exactly once per query and a stray UA
classifies to `unknown` (ARCHITECTURE §4.1, §7). The reasoning is
the macOS analogue of cdrom_id's: by the time a userspace client
holds a handle, the kernel's own device initialization has already
consumed the power-on / reset / media-change UA, so the
application-level retry an earlier mos iteration carried was
re-armoring an already-armored layer. (doc/history/CHANGELOG.md records the
drain loop's removal; AGENTS.md carries the superseding ADR.)

Hardware capture should still include stacked-UA scenarios —
freshly powered drives, tray-flap, post-mount-unmount — to confirm
no UA survives to mos's first TUR on any drive in the matrix. If
one ever does, that evidence goes through the AGENTS.md append
process before any retry is reintroduced.

## How to run

```sh
# Build CLI (the probe subcommand rides it; MOS_CLI_PROBE default ON)
# and tests
cmake -B build
cmake --build build

# Query the default (first) drive in JSON
./build/bin/mos --json

# List all drives, JSON envelope
./build/bin/mos --list --json

# Watch a specific drive (NDJSON stream)
./build/bin/mos --watch --bsd diskN --json

# One-shot DiscRecording Info/Status dictionary capture
./build/bin/mos probe --dump

# Raw notification event stream for one drive (NDJSON, until Ctrl-C)
./build/bin/mos probe diskN
```

## Capturing fixtures

State observations are captured through the same public-API path every
consumer uses; the probe's `--dump` mode captures the raw DiscRecording
dictionaries (registry path, identity byte-shape).

```sh
# Each of these with the appropriate physical state
./build/bin/mos status --json 2>&1 | tee tray_open_$(hostname -s).log
drutil tray close
./build/bin/mos status --json 2>&1 | tee tray_closed_empty_$(hostname -s).log
# ... insert CD, DVD, BD in turn, repeat

# DR dictionary capture (once per drive/media combination)
./build/bin/mos probe --dump 2>&1 | tee dr_dump_$(hostname -s).log
```

These logs go in `tests/fixtures/` as human-readable records. They are
reference material for reviewers, not automated-test inputs.

For true raw-response byte captures (the `.bin` files we eventually
want under `tests/fixtures/`), write a small diagnostic C program that
calls `mos_open_by_bsd_name()` then `mos_raw_cdb()` with a specific CDB
(for example 0x4A GET EVENT STATUS NOTIFICATION) and dumps the
returned buffer. This is an open TODO — see issue tracker. The
`mos_raw_cdb` function itself is public API and works today; only the
fixture-capture helper doesn't exist yet.

## Reporting results

Open a PR titled `fixtures: <drive model> on macOS <version>` with:

1. The `.bin` fixture files.
2. Your row filled in in the matrix above.
3. A one-paragraph note on any surprises (unexpected sense codes,
   profile values, timing oddities).

## Sensitive information

Fixtures are **raw drive response bytes**. They do not contain any
serial numbers or personally identifying information from the
*machine*, but the INQUIRY response does include the drive's vendor,
product, firmware revision, and sometimes serial number. If you care
about that, redact the relevant bytes before committing. Per SPC-4
§6.4.2 the standard INQUIRY layout is: T10 vendor at bytes 8-15,
product identification at 16-31, **product revision level at 32-35**,
and bytes 36-43 are vendor-specific — that vendor-specific region is
where drive serial numbers commonly live, so it is the part worth
redacting; the 4-byte revision is harmless and useful to keep. This is
usually not a concern for consumer drives.
