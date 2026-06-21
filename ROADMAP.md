# ROADMAP

Forward-looking only — the plan, not the record. What shipped and the
decision back-and-forth that got us here live in the `AGENTS.md` ADRs, the
dated `doc/research/` notes, `doc/history/` (frozen), and git history;
design rationale is in `ARCHITECTURE.md`. This file states what is *not yet
built* and does not relitigate what is.

**Reality check.** mos has never run on a real drive. The pure decision
layer is exercised in CI, but every IOKit / MMC / DiskArbitration assumption
below is off-Mac supposition until the reference rig confirms it. Treat
unbuilt items as hypotheses, not commitments — including the ones written as
if settled.

---

## The bar — spec conformance (not drive agreement)

mos is a spec-defined-MMC library for embedding. Its correctness criterion is
canonical accuracy against MMC, not agreement with any one drive: a single
drive can neither validate conformance nor define it, and baking in what one
happens to do would break the spec-only contract. That bar is
drive-independent and is held by the pure suite + fuzz (the GET CONFIGURATION
walker against MMC §5.2, `state_from_sense_closed` against the ASC/ASCQ table,
hostile inputs exhausted).

The one thing NOT spec-derivable, and so the only standing outstanding check:
the Apple-framework layer mos sits on — whether the `MMCDeviceInterface`
convenience wrappers behave as the headers imply, whether the PREVENT/ALLOW
bit survives handle close, whether `kIOMessageServicePropertyChange`
self-triggers a probe loop. These need *a* Mac and *a* drive — any drive
answers them; they are macOS plumbing, not drive conformance — and they are a
one-time adapter smoke test, a ship-discipline item, **not a design gate**.
Run it before tagging a release shipped; do not block design on it.

Coverage matrix the decision layer must classify correctly (drive-independent,
spec-derived fixtures; real captures optional): pressed audio CD vs pressed
data CD-ROM (both profile 0x0008 — the data-vs-audio split is the canonical
drutil-fails case mos exists to solve, decided by READ TOC CONTROL byte, not
GET CONFIGURATION); pressed DVD-Video (0x0010); pressed BD-ROM (0x0040); blank
CD-R (0x0009) / DVD-R (0x0011) / DVD+R (0x001B); M-Disc BD-R (0x001E); empty
closed tray; open tray.

Optional fixture realism (not the gate). Reference rig: BH16NS55 + WH16NS60
1.00 firmware, OWC Mercury Pro (ASMedia ASM1153E bridge). Capture into
`tests/fixtures/bh16ns55-wh16ns60-1.00/`: `sg_get_config -H`, `sg_inq -e`,
`sg_logs`, `sg_readcap -H`, and READ DISC INFORMATION via `sg_raw` (0x51).
These corroborate the spec-derived fixtures; they gate nothing.

## Standing constraints (carry forward into every item below)

- **Schema freeze.** `mos.*.v1` documents are mutable in place until the
  first tag that ships them; the freeze — and the any-new-shape-is-`.v2`
  rule — takes effect at that tag. Canonical: the AGENTS.md schema ADR.
- **Ship lean: `probe` OFF at tag.** `MOS_CLI_PROBE` defaults ON *today* only
  to keep the Apple-only `cli/probe.c` TU compiled and contract-tested every CI
  run (CMakeLists.txt:299) — a bitrot guard, not a consumer signal. `probe` and
  its `--dump`/`--capture` modes are falsification/diagnostic surface, not
  consumer surface. **At the first tag the default flips to OFF** so the shipped
  binary is lean; CI keeps a `-DMOS_CLI_PROBE=ON` job so the TU stays tested,
  `gen-cli-docs.py` becomes gating-aware so the shipped man page + completions
  describe the OFF build (today they list `probe` unconditionally — regex over
  the verb table, blind to the `#ifdef`), and `probe`/`--dump`/`--capture`
  documentation moves to INTEGRATION_HARNESS.md / CONTRIBUTING.md. Canonical:
  the AGENTS.md probe-default ADR. Pre-tag the default stays ON (HEAD installers
  are developers, and the fixture-capture workflow wants `mos probe --capture`
  present with zero friction).
- **`tray eject` is GRACEFUL; `--force` clears LOCKS, never the filesystem**
  (supersedes "name semantics, selector-gated"; AGENTS.md addendum 2026-06-20).
  Every eject gracefully unmounts a mounted disc (`DADiskUnmount` Whole, **not**
  Force) then ejects — like `diskutil unmountDisk` / `drutil eject`; a busy
  filesystem surfaces `MOS_ERR_BUSY`, **mos never forces / never destroys open
  files**. `--force` only clears a tray Prevent LOCK in the way. The data-loss
  force-unmount, the selector gate, and `MOS_FORCE_BY_IDENTITY` are all REMOVED —
  and with them the wrong-target TOCTOU (a graceful unmount of a reused `diskN` is
  harmless), so the veto/`funmount` menu below is **mooted**. The unbounded-wait
  KNOWN ISSUE (void `DASessionSetDispatchQueue` / `DISPATCH_TIME_FOREVER`) on the
  graceful unmount's await is **FIXED** — a bounded run-loop wait
  (`MOS_DA_UNMOUNT_TIMEOUT_SEC`, src/mos_da.c); see the v0.4 post-tag item 2.
- **Division of labour.** DR enumerates and hands over cheap coarse status (a
  passive, GESN-fed snapshot "not guaranteed current"); mos owns the
  synchronous, fully-checked state machine and the deep rip-relevant metadata
  DR omits. The MMC state engine must not become a DR passthrough.

---

## Now — v0.4 — finish the typed surface, drop `raw_cdb`

The typed verbs that justify removing the raw passthrough, plus the removal.
`metadata`, `drive`, `features`, `tray`, and `capacity` shipped — the
reserved-name surface is now empty (the reserved-name machinery retired with
it). What remains:

- **Remove the public `raw_cdb` passthrough** — DONE. The typed verbs cover
  the diagnostic cases, so the public passthrough was retired: the function is
  now `mos_internal_raw_cdb` (internal-only, still the sole
  `ObtainExclusiveAccess` site), and the diagnostic fixture-capture workflow it
  served moved to the fixed-menu `mos probe --capture` (`mos.capture.v0`).
  Decision record: AGENTS.md. This is the major-version (public-surface)
  reduction the item called for.

- **Force-unmount redesign + R3 hardening follow-ups** (POST-TAG, none a tag
  blocker). The pre-tag R3 adapter-audit fixes shipped: gate `--force` off;
  ALLOW transport-failure propagation via `mos_internal_tray_cmd` (tolerating
  answered refusals); write GET PERFORMANCE transport-failure propagation with
  command-level write-speed kept best-effort + documented; the `_Static_assert`
  watch re-home tripwire; and the exclusive-access release fix (#88). The
  post-tag work:
    1. ~~Reintroduce `tray eject --force` behind the experimental gate with a
       guarded identity redesign.~~ DONE differently: shipped as name semantics +
       selector gate (AGENTS.md ADR "name semantics, gated by selector"). The
       identity redesign was abandoned — DADisk.c verification confirmed the
       public API cannot bind the daemon's unmount to a registry id, so there is
       nothing to "verify"; embracing name semantics dissolved the problem. (The
       `DADiskCopyIOMedia` / `da_media_id` fake scaffolding once slated for
       deletion as "dead" was NOT removed — the R3 continuation audit (2026-06-20,
       A2) showed it is live on the READ path: the volume lookup's endpoint
       identity guard uses it, see the "Shipped" bullet below.)
    2. ~~Heap-owned, bounded DA callback context with leak-or-reap-on-timeout (the
       void `DASessionSetDispatchQueue` / `DISPATCH_TIME_FOREVER` hang).~~ **DONE
       (the F1 hang is fixed; it was an ISOLATED hang once the graceful-eject
       redesign mooted the veto — item 3).** `mos_internal_da_unmount` (src/mos_da.c)
       no longer uses a dispatch queue + `DISPATCH_TIME_FOREVER` semaphore. It now
       schedules the DA session on THIS thread's run loop in a private mode and
       pumps `CFRunLoopRunInMode` until the callback fires or `MOS_DA_UNMOUNT_TIMEOUT_SEC`
       (10 s) elapses. This bounds the wait — a silent `DASessionScheduleWithRunLoop`
       failure leaves the mode empty so `CFRunLoopRunInMode` returns
       `kCFRunLoopRunFinished` at once (fast false), and a wedged daemon is cut off
       at the timeout. The prescribed heap-owned context turned out **unnecessary**:
       single-threaded run-loop delivery means the callback only runs INSIDE
       `CFRunLoopRunInMode` on this thread, so after the loop exits +
       `DASessionUnscheduleFromRunLoop` + session release, no source remains for a
       late callback to arrive on — the stack-local ctx is safe without a heap copy
       or a deliberate leak (the use-after-return that forced the heap idea only
       exists in the concurrent dispatch-queue model). Regression:
       `adapter_da_unmount_bounded_when_callback_never_fires` (fake
       `unmount_never_completes`).
    3. ~~The `--force` wrong-target race (veto / `funmount`)~~ — **MOOTED** by the
       graceful-eject redesign (AGENTS addendum 2026-06-20). With a GRACEFUL
       unmount (never `Force`) a reused `diskN` is harmless (cleanly unmounts an
       idle disc or fails on a busy one — no data loss), so there is no
       wrong-target data-loss to reduce. The veto / `funmount(2)` investigation
       (`doc/research/2026-06-20-force-unmount-veto-funmount-investigation.md`)
       correctly overturned "irreducible", but the cleaner fix was to remove the
       data-loss operation entirely. Kept here as the record; not a build item.
    4. Write-speed presence/error observability (a `has_write_kbps` tri-state) if
       the public `mos_drive_perf` struct can change — so a write command-level
       failure is distinguishable from a legitimately-absent write speed, the gap
       the pre-tag best-effort carve-out leaves open.
    5. Replace the `_Static_assert` size tripwire with an X-macro / generated
       borrowed-field audit (stronger than the size floor; no current bug).
  Source: R3 macOS adapter audit; AGENTS.md TOCTOU addendum (2026-06-20).

- **R3 continuation audit (2026-06-20) — A1/A2/A3 SHIPPED, A4 post-tag.**
    - **A2 (volume lookup not identity-exact) — SHIPPED.** `88657fc` had dropped
      the `DADiskCopyIOMedia` endpoint check on the false premise that resolving
      the IOMedia by registry id made the lookup "identity-exact by construction";
      but `DADiskCreateFromIOMedia` is name-delegated (reads `kIOBSDNameKey`,
      delegates to `DADiskCreateFromBSDName` — DADisk.h confirms), so a `diskN`
      reuse in the create→describe window could attribute another disc's volume.
      Restored #85's discipline on the READ path: read into locals, re-confirm via
      `DADiskCopyIOMedia` that the ref still resolves to our exact `media_id`,
      commit only on a match. Valid for a read (no daemon re-resolution after the
      check), unlike the unmount ACTION where the daemon re-resolves by name.
    - **A3 (`timeout_ms == 0` never drained a ready source) — SHIPPED.** `mos.h`
      promises zero is "drain a ready event, do not sleep", but `next_event`
      returned `MOS_ERR_TIMEOUT` before ever running the loop, so an empty
      all-watch never serviced a queued Appeared. Fixed: one guarded non-blocking
      `CFRunLoopRunInMode(…, 0, …)` before returning timeout, then re-pump.
      Adapter-fake regression: empty all-watch + queued Appeared + timeout 0 emits
      `device_appeared` without advancing the fake clock.
    - **A1 (DA-callback hang) — SHIPPED.** Fixed by the bounded run-loop wait in
      `mos_internal_da_unmount` (item 2 above): the `DISPATCH_TIME_FOREVER` /
      dispatch-queue model is gone, so the never-delivered sub-case fails fast and a
      wedged daemon is bounded at the timeout. It was an ISOLATED hang once the
      graceful-eject redesign mooted the veto; not a `--force`-hardening prerequisite.
    - **A4 (permanent-negative serial re-probed every poll) — POST-TAG.** Watch
      probes set `serial_grabbed` only on a successful read, so an unsupported VPD
      0x80 (or answered-empty page) re-attempts the optional INQUIRY/exclusive
      path every ~2 s stable poll, per slot. Separate "serial resolved" from
      "serial present" (UNTRIED / RETRYABLE / RESOLVED_ABSENT / RESOLVED_PRESENT):
      cache an answered permanent absence, retry only transient
      BUSY/exclusive/timeout. Efficiency, not correctness — post-tag hardening
      unless repeated exclusive-access traffic becomes a release criterion.

- **GESN realized-count waiver — hardware-gated revisit** (HELD, not a tag
  blocker). R3's mos_state.c audit (2026-06-20) re-raised the O-4 GESN waiver
  (third review to file it; external F1 declined it 2026-06-13): a device that
  CLAIMS a full 6-byte Media descriptor but delivers only 4 GOOD bytes makes the
  decoder read byte 5 from zero-fill — a confident "door closed" that, on the
  not-ready 02/3A/02 path, misclassifies OPEN as EMPTY. Behavior is HELD
  (maintainer, 2026-06-20): the waiver accommodates real USB-bridge under-
  reporting, and bounding by realized count degrades those drives to TUR-sense
  fallback; per the hardware-role ADR a crafted-bytes hypothetical moves only via
  a captured fixture, never a review. The doc cost was corrected in place
  (`doc/seam-contract.md`: the worst case is a confident wrong bit, not the
  understated "false return / nothing compounds"). REVISIT trigger: the rig A/B's
  `realizedByteCount` against bridge behavior — evidence either retires the waiver
  (bound by realized count, fall back to TUR sense on a short transfer) or pins it
  with a fixture. The companion refresh-coherence finding from the same audit
  SHIPPED (one media snapshot + S1/S2 retry; commit on this branch).

- **`eject_requested` watch event** — the cooperative soft-eject the tray
  `lock --persistent` verb sets up: under Persistent Prevent the operator
  button raises a GESN EjectRequest instead of ejecting. Surfacing it on
  `--watch` needs an edge-triggered GESN event-drain distinct from the
  state-diff machinery, and is hardware-gated on whether the event survives
  the kernel's own GESN poll. Design + the rig-check-first build order:
  `doc/research/2026-06-13-eject-request-watch-event.md`.

- **Watch all-mode static audit (W1–W4) — DISPOSITIONED, no release blocker.** A
  macOS-only static audit of `mos_watch.c` filed four findings; verified
  vendor-blind against the tree + the 26.4 `DRNotificationCenter.h`. None is an
  undocumented defect:
  - **W1 (DR enumerates only writable devices)** — STRUCK outright. The §9.1
    attach rule blocks `SCSITaskUserClient` on read-only drives, so DR's
    writable-only boundary coincides with mos's openable set (DR-pivot decision
    record); the audit didn't know the attach rule.
  - **W2 (run-loop / thread affinity)** — CLOSED, not a defect.
    `DRNotificationCenter.h` *mandates* the affinity verbatim ("posted to the
    runloop it was created on"; receive on another loop ⇒ create the center from
    that loop), so capturing `CFRunLoopGetCurrent()` at open + the single-thread
    contract is correct, documented SDK use. The `CFRetain` of the captured loop
    makes off-thread misuse MEMORY-SAFE only — it stays a contract violation
    degraded to polling/sleep (the wakes target the origin loop, so off-thread
    they never break the current thread's sleep; the pump falls back to its poll
    cadence), NOT a "safe no-op". An enforcing thread-id assert is not added (it
    would reject the degraded-but-safe path). `mos_watch.c` comment reworded off
    the "safe no-op" phrasing.
  - **W3 (one-shot Appeared recovery)** — ACCEPTED known limitation, deferred to
    v0.next (hardware-gated, not a pre-tag blocker). The one-shot rescan already
    narrowed the loss window from one snapshot failure to two consecutive
    (`mos_watch.c` struct comment), so a drive can still be missed after TWO
    consecutive resolution failures. The deferred fix must be STRONGER than
    re-arming the Boolean: a bounded retry with a deadline/backoff that (a)
    spreads attempts across pump cycles — the current rescan fires immediately
    off the Appeared `CFRunLoopStop`, so both attempts hit the *same* unsettled
    IORegistry; spacing them lets it settle — and (b) terminates after the
    bound, so an empty all-watch cannot re-arm `all_rescan_pending` and sleep
    forever. Make it CONVERGENT: `mos_internal_dr_copy_snapshot` reports
    completeness, and an incomplete copy re-arms within the bound rather than
    clear-before-copy. Built only on rig evidence per the hardware-role ADR.
  - **W4 (full-table overflow drop)** — CLOSED by contract. The all-watch holds
    up to `MOS_WATCH_ALL_CAP` = 64 drives; arrivals beyond that are dropped for
    the plug session and recovered by a REPLUG (re-fires Appeared), NOT by a
    later slot free — the code never re-scans previously-dropped devices when a
    slot frees. Doc drift fixed: the public cap text (`mos.h`) read 16, now 64;
    the internal `watch_all_add_device` comment ("drop until a slot frees")
    implied a reconsideration the code does not do, and now states the
    replug-recovery contract.

  (Held-handle identity refresh — `bsd_unit`/`media_id`/size now
  re-resolved per media-scoped query, not captured once at open —
  shipped 2026-06-14; decision record:
  `doc/research/2026-06-14-held-handle-refresh.md`. The DR-dictionary
  lookup the earlier plan named remains an unbuilt optimization, not a
  correctness need.)

- **Transitional-state poll escalation** (contingent on hardware evidence).
  A drive persistently classifying EMPTY_OR_OPEN/UNKNOWN (GESN failing through
  a bridge) polls at transition rate — 200 ms cycles — indefinitely, with no
  analog of the error path's 200→2000 ms backoff. Cadence is behavior, so per
  the hardware ADR this waits for an observed case, not a review point.

- **Deferred signal-source work** (designed, parked pending the hardware
  capture): three-tier backoff cadence, a GESN-first probe with cached state,
  and the full signal-source hierarchy (wake → GESN → TEST UNIT READY). The
  `--watch` wake source shipped; these refine its cadence and add fallbacks.

- **Stage-2 media info** (deferred with named falsifiers in the design
  addendum): UDF volume names; the BG-format REQUEST SENSE
  progress-percent (the 2-bit BG Format Status itself shipped
  2026-06-14); and — within CD-TEXT, whose album Title/Performer and
  per-track titles + performers shipped 2026-06-14 — the other field
  types (songwriter/composer/genre/ISRC/UPC/…) and multi-language blocks.
  Third-party ids (MusicBrainz / AccurateRip / dvdid / BDMV) are
  permanently consumer-side.

- **Parked test/robustness remainders:**
  - *EPIPE-path CLI tests* — the `mos_cli_stdout_finalize` classification
    (cli/io.c) now has fork-isolated unit coverage of both branches
    (EPIPE → pipe-closed, other errno → write-error), pinning the
    errno-freshness argument; `tests/test_io.c`, runs on Linux too. What
    remains untested is the trivial exit-split SWITCH in cli/common.c
    (`mos_cli_finalize_oneshot_stdout`: pipe-closed → EX_OK, write-error →
    EX_IOERR) — it lives in an IOKit-dependent TU, so pinning it needs the
    macOS CLI contract test (`test_cli.sh`), where forcing a non-EPIPE
    write error portably is the fiddly part left.
  - *All-watch directory-rescan fallback* — `mos_watch_open_all` fails
    honestly when the DR doorbell can't be set up. If hardware sessions ever
    observe `DRNotificationCenterCreate` failing in practice, add a
    slow-cadence `mos_internal_dr_copy_snapshot` reconciliation pass; build it
    on that evidence, not before.

**Open empirical questions** (need the rig): does
`kIOMessageServicePropertyChange` self-trigger a probe loop; does brief
exclusive access contend with another application holding the drive; does GET
EVENT STATUS NOTIFICATION report tray/media events reliably across drive
families. (The prevent-bit-survives-close question is now spec-resolved —
per-I_T-nexus state, a SCSITaskUserClient close clears nothing; what the rig
can still *falsify* is whether a given mechanism physically honors prevent,
the per-drive cooperative-button matrix, and a non-conformant bridge dropping
the nexus on close.)

---

## Later — v1.0 — production-ready, embeddable

- **Multi-drive fixtures.** The MT1959HWDN unification means one drive per
  silicon family covers many SKUs. Honest budget for full architectural
  coverage is €700–900 across ~eight rows (see canonical drives below).
  Acquisition order: Apple SuperDrive first (discontinued Aug 2024 →
  secondary-market only), Lite-On iHAS124 W second (cheap, validates the
  Renesas/NEC dispatcher path), Pioneer third (firmware-date discipline),
  Plextor PlexWriter Premium 2 fourth (only acquirable Sanyo silicon), rest
  opportunistically.

- **Decision-tree integration tests.** Largely in place
  (`tests/test_state_core.c`, the headless adapter-fake suite); what remains
  is filling any matrix gaps the rig surfaces.

- **Documentation polish.** A fresh root `CHANGELOG.md` (Keep-a-Changelog)
  generated from git tags at tag time. (Done 2026-06-14: the
  `mos_open_by_index` positional-selector race is documented in `mos.h`
  with `mos_open_by_registry_id` / `mos_open_by_bsd_name` marked preferred
  for hotplug-stable selection; the lock-composability property — acquire-
  on-call / release-on-return, coexists with other drive users — is
  explicit in `ARCHITECTURE.md` §3.)

---

## Canonical reference drives (fixture acquisition)

By silicon family — one drive per family covers many SKUs; multiples of one
family are wasted spend.

- **MediaTek MT1959HWDN (owned).** The reference rig speaks for the whole
  late-LG family — BH14/16NS\*, WH16NS\*, BU/BP slim & USB variants, and the
  ASUS/Buffalo/Dell/HP/Verbatim/Vinpower rebadges.
- **Renesas R8J32xxx (Pioneer).** BDR-XD08 / BDR-212. *Firmware-date
  discipline:* the December-2022 cutoff flips identical silicon between two MMC
  dispatcher behaviors (bus-encryption 0x1B→0x13, mandatory AACS handshake,
  READ DISC STRUCTURE starts refusing) — ask the seller for firmware version
  before buying.
- **Apple bridge + 0xEA gate.** A1379 SuperDrive — late units are `HL-DT-ST
  DVDRW GX50N` inside (the Mac-only enforcement and 0xEA wake live in the
  soldered USB bridge firmware). Discontinued 2024; buy in the cheap window.
  Genuine Panasonic UJ-8A8 is a separate row.
- **Renesas R8J32091NP (Lite-On outlier).** iHAS124 W — a second NEC-lineage
  dispatcher at €15–30.
- **Rest, for full coverage:** Panasonic MN103S (UJ-8A8 / UJ-265), Sanyo
  (PlexWriter Premium 2 / PX-716SA / PX-760SA), MediaTek MT18xx/MT19xx (Lite-On
  iHAS124 B/F, iHBS212), NEC/Renesas (AD-7260S / AD-7280S), MediaTek MT1887 USB
  (Samsung SE-208), ASUS Pioneer-OEM slim (SBW-06D2X-U), Yamaha CRW-F1 (exotic).

**Skip:** generic Chinese USB drives (unpredictable bridge chips → bad fixture
data); IDE-only drives (connection cost > fixture value); post-2017 "Optiarc
AD-5290S" (Vinpower-firmware Lite-On, not Sony silicon).

---

## Out of scope

mos and its typed verbs are for spec-defined MMC only. Out: firmware flashing;
AACS handshake / BD+ / key extraction / LibreDrive microcode (MakeMKV
territory); raw descrambled sector reads; vendor write-quality features that
superficially resemble state — PureRead (Pioneer), AudioMaster (Yamaha),
GigaRec/AutoStrategy (Plextor); M-Disc capability-bit detection (feature
0x0028 — speculative v1.x only); BD-R DL/TL/QL profile differentiation (all
0x001E today, fine for state); BD-RE rewritable state-machine variants (v1.x
if anyone asks); quiet/acoustic mode (unimplementable on HLDS via standard
MMC). Those belong to higher-layer media libraries; mos's acquire-on-call /
release-on-return lock discipline is built to coexist with them, not host them.

---

## Dependency graph (remaining work)

```
spec-conformance (the bar) ─► pure suite + fuzz green ─► decision layer correct
          └─► adapter smoke run on any Mac+drive ─► tag shipped (not a design gate)

v0.4 ─► raw_cdb retired to internal (DONE) ─► API stable
          └─► eject_requested watch event (rig-gated, optional)

v1.0 ─► multi-drive fixtures ─► integration-test gaps ─► CHANGELOG ─► ship
```

Largest risk: mistaking a drive capture for the correctness bar. mos is
correct iff it is spec-accurate, verified drive-independently (pure suite +
fuzz). The only thing a Mac+drive establishes is that the Apple-framework
adapter is plumbed correctly — a one-time smoke test, not a design gate. Don't
hold the spec-driven work hostage to it.
