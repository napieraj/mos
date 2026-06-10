> **Provenance.** Fourth external reviewer, seam-fidelity audit
> 2026-06-10 (Item 1). Vendored verbatim as a research artifact; the
> post-audit clause statuses live in `doc/seam-contract.md`. One
> correction: B7/V-5's "key-nibble mask unguarded" held only for the
> DESCRIPTOR sense format — `fixed_format_masks_lower_nibble_of_sense_key`
> already pinned the fixed format. Both formats are pinned now.

# Fake-assumption census — mac-optical-state pure suite

**Commission Item 1.** Every behavioral assumption the two pure-suite fakes
encode, classified by evidence tier, with the Item 3 detection status. The
fakes are `fake_watch_ctx` + `fake_mono`/`fake_wall`/`fake_probe`
(`tests/test_watch_core.c`) and `fake_mmc` + its three ops + `make_env` +
the sense helpers (`tests/test_state_core.c`).

Tiers:

- **source-verified** — the real adapter (`src/mos_scsi.c`) or kernel source
  (`IOSCSIMultimediaCommandsDevice.cpp`, `xnu`) enforces the same behavior.
- **spec-derived** — T10 (SPC-4 / MMC-6 / SAM-5) requires it, independent of
  any implementation.
- **unfounded** — the fake invented the behavior; nothing outside the fixture
  requires it, and the suite's green rests on the fake and the code agreeing
  on the invention.

Detection (from Item 3, `fake_mutate.py`): **pinned** (a fake mutant on this
axis breaks ≥1 test), **broad** (breaks many — load-bearing), or
**unexercised** (mutant survives — the suite does not test this axis).

Evidence line numbers reference the audited tree (`dist/mos.c` =
`8f65d640…`); they are documentation pointers, not checker latches.

---

## A. Watch fake (`fake_watch_ctx`, `fake_mono`, `fake_wall`, `fake_probe`)

### A1 — Probe is invoked exactly once per probing pump
`fake_probe` advances `probe_calls` once per call; fixtures script
`probe_state[0..n]` expecting one consumption per pump.
**Tier: source-verified.** `mos_internal_watch_pump` calls `ops->probe`
exactly once per pump that passes the schedule gate (`mos_watch_core.c:312`);
a sleeping pump calls it zero times (`:303` early return).
**Detection: pinned** — the snapshot/no-change tests assert event vs
`SLEEP_UNTIL` on exactly this count.

### A2 — `mono_ms` is read twice around a probe, once when sleeping
The latency fixtures and the backward-clock fixture depend on the pump
reading the monotonic clock at probe-start and again at probe-end, with
`latency = end − start`.
**Tier: source-verified.** Pump reads `mono_ms` at `:300` (schedule gate /
probe-start); if the gate passes it reads again at `:313` (probe-end);
`latency_ms` is their difference. A pump that sleeps (`now < next_poll`)
reads `mono_ms` exactly once and returns before probing.
**Detection: pinned (narrow).** Disabling the fake's auto-advance breaks one
test (`test_latency_ms_measures_probe_duration`). This is the commission's
named seed: the two-read choreography is real and load-bearing, but only the
*fact* of advancement is pinned — see A4 for what is **not**.

### A3 — `wall_ms` is read once per emitted event, never for scheduling
`fake_wall` returns `wall_clock_ms` and counts calls; fixtures hold it orders
of magnitude apart from `mono` to prove the split.
**Tier: source-verified.** `wall_ms` is read once per event, only to format
the RFC 3339 `ts` (`mos_watch_core.c:213`); `stream_open_wall_ms` is captured
once at init (`:154`), not per pump. Scheduling never reads `wall_ms`.
**Detection: broad.** Making `fake_wall` return the mono value breaks 3 tests
(the clock-domain-separation set). Using a wall value for scheduling is the
exact ~55-year-deadline regression the two-clock split fixed.

### A4 — A `mono_ms` read returns the pre-advance value
`fake_mono` returns `t` (the value before adding `mono_auto_advance_ms`),
then advances.
**Tier: unfounded.** No source fixes whether a read observes the value
before or after an internal step; a real `CLOCK_MONOTONIC` read is just a
read. The fixture invented the pre-advance convention.
**Detection: unexercised.** Returning the *post*-advance value survives,
because every latency assertion is a **difference** of two reads and both
shift by the same step, leaving the delta unchanged. The suite is insensitive
to the absolute value at a read — it pins only deltas.

### A5 — Two clock domains, structurally separate
`fake_watch_ctx` carries `mono_clock_ms` and `wall_clock_ms` as independent
fields.
**Tier: source-verified.** The vtable splits `mono_ms` (scheduling/latency)
from `wall_ms` (timestamps/identity); the adapter wires them to different
sources and monotonicizes wall per process (`mos_watch_core.c:20-22,148`).
**Detection: broad** (same mutant as A3).

### A6 — `bsd_unit` is `−1` exactly when state ∈ {OPEN, EMPTY}, else `4`
`fake_probe` derives the unit from the classified state.
**Tier: unfounded mechanism over a source-verified correspondence.** The
*correspondence* `−1 ⇔ no media` is source-verified: the adapter sets
`bsd_unit = −1` when no whole-disk `IOMedia` child exists
(`mos_scsi.c:195,287` and `mos_internal_bsd_unit`'s `−1` return). But the
adapter derives the unit from the **registry walk**, never from a classified
state enum. The fake's "switch on `state`" is a fixture invention that
*approximates* the node-presence rule. Consequence: the fake can never
produce the transient real-hardware shape *media node still present while
state reads EMPTY*, so that interleaving is untestable through this fake.
**Detection: broad.** Forcing the unit to a constant `4` breaks 3 tests
(empty-drive unit, media-appears-after-empty, late-media-id).

### A7 — `media_id` flows through verbatim from the probe
`fake_probe` copies `probe_media_id[idx]` into `out->media_id`.
**Tier: source-verified.** The adapter captures the whole-disk registry entry
ID into `media_id` (`mos_scsi.c:287`, `mos_internal_bsd_unit`'s
`media_id_out`) and the watch core treats it as an opaque swap fingerprint.
**Detection: pinned.** The media-changed/same-id tests assert on this value's
passthrough and the `0`-means-unavailable rule.

### A8 — Profile: `0xFFFF` ⇒ genuine wire-`0`; `0` ⇒ default `0x0040`
`fake_probe` maps a sentinel because a literal `0` in the fixture array means
"unset, use default," so a real wire-`0` needs a different encoding.
**Tier: unfounded (fixture encoding).** No source uses `0xFFFF` this way; it
exists only because the C zero-initializer collides with a meaningful wire
value. The *meaning* it preserves — that `0x0000` is a real "no current
profile" answer — is source-verified (`mos_scsi.c:587-597` documents exactly
this collision and why `get_current_profile` must signal absence
out-of-band).
**Detection: split.** The `0xFFFF`→`0` mapping is **pinned** (removing it
breaks one test that scripts a genuine wire-0). The specific default value
`0x0040` is **unexercised** — changing it to `0x0000` survives; no test
asserts the fixture's default profile.

### A9 — `fake_probe` zero-initializes the result before populating
`memset(out, 0, sizeof(*out))` at the top of the fake.
**Tier: source-verified.** The adapter zero-inits the result struct before
filling it (`mos_scsi.c:193`, `memset(info, 0, …)`). The fake matches the
adapter.
**Detection: unexercised.** Dropping the fake's `memset` survives — no test
reads a field the fake leaves unset. So buffer-initialization ownership at
the probe seam is faithful to the adapter but **not pinned by any test**.
(Contract clause O-1.)

### A10 — Past `probe_count`, the probe returns `MOS_OK`/`READY` sentinel
A guard `abort()`s if a pump reaches the probe with nothing scripted
(`fake_probe` `probe_count <= 0`); beyond `probe_count` the last entry is
reused.
**Tier: unfounded (fixture safety).** A test-harness convenience with no
adapter analogue. The `abort()` is a fixture-misconfiguration tripwire.
**Detection: unexercised** by mutation (it is a fail-loud guard, not a
behavior the suite asserts), but it is a deliberate fixture safety net, not a
latent assumption — classified to leave no entry unclassified.

### A11 — `flip_mono_on_probe` injects a single backward step to 10
Used only by `test_latency_saturates_on_backward_clock` to make
`probe_end < probe_start`.
**Tier: unfounded (fixture mechanism modelling a spec-forbidden event).**
`CLOCK_MONOTONIC` never steps backward (POSIX), so this models a *buggy
adapter's* ops table, not a real clock — the fixture says as much
(`:79-81`).
**Detection: unexercised, and conflated.** Disabling the flip survives:
with no backward step and no auto-advance the two `mono` reads return the
same value, so `latency == 0` holds for the *static* reason instead of the
*saturation* reason, and the test cannot tell them apart. The code-side
saturation guard is independently killed by this test (the unguarded
subtraction underflows to ~49 days when the flip fires), so Finding 8 is
closed for the **code** — but the **fixture's** backward-step mechanism has
no guard of its own. If a refactor silently neutered the flip, the test would
stay green and the guard would lose its only coverage. (Contract clause C-3.)

### A12 — `registry_id` (incl. `0`) flows through as an opaque value
`mos_internal_watch_init` takes the token; the fake passes `0` in one test.
**Tier: source-verified.** xnu lazily assigns every attached entry a registry
ID ≥ 2³²+256, so a matchable service always has one and `0` never collides;
the adapter fails closed if it can't capture one (`:142-152` notes).
**Detection: pinned** — `test_zero_registry_id_passes_through` asserts it.

---

## B. State-core fake (`fake_mmc`, three ops, `make_env`, sense helpers)

### B1 — TEST UNIT READY is issued exactly once (no UA retry)
`fake_mmc` has a single `tur_*` slot; `state_tur_issued_once` pins
`tur_calls == 1`.
**Tier: source-verified.** The decision tree calls `test_unit_ready` once and
never drains UNIT ATTENTION (`mos_state_core.c` step 1; rationale at
`:`comment "ONE shot"). The adapter issues one TUR per query.
**Detection: pinned.**

### B2 — `get_tray_state` is reached only on a not-ready, non-all-zero CC
The fixture convention: `tray_err == MOS_OK` models a GESN that answered;
`tray_err != MOS_OK` models GESN silent/lock-denied.
**Tier: source-verified.** The lock-reach predicate is the decision tree's
own (`status == CHECK_CONDITION && !(sk==asc==ascq==0)`), and the lock is the
exclusive-access GESN. This is the predicate Item 2 proves against the kernel.
**Detection: pinned** — `state_ready_short_circuits_without_tray_probe`
asserts `tray_calls == 0` on GOOD; the not-ready tests assert the fork.

### B3 — Tray out-param is untouched on error
`fake_get_tray_state` writes `*tray_open` only when `tray_err == MOS_OK`.
**Tier: source-verified.** The adapter returns on transport/lock failure
*without* writing `*tray_open` (`mos_scsi.c:546`, `return e`), leaving the
caller's buffer as-is; the core does not read `door_open` after a tray error
(it forks on sense). Fake matches adapter.
**Detection: unexercised.** Leaking a value into `*tray_open` on error
survives — the suite never checks that the core ignores it. (Contract clause
E-1.)

### B4 — TUR status/sense untouched on error — **DIVERGES from the adapter**
`fake_test_unit_ready` writes `*status`/`sense` only when `tur_err == MOS_OK`.
**Tier: unfounded, and source-contradicted.** The real adapter does the
opposite: on transport failure it **zeroes** both (`mos_scsi.c:577-578,
*status = 0; memset(sense,0,18)`), then returns the mapped error. The fake
leaves them untouched. Both paths happen to feed the core zeros — the adapter
by writing them, the fake because the core's locals are pre-zeroed
(`mos_state_core.c` `status=0, sense[18]={0}`) and the core ignores them on
error — so the divergence is **masked**. The fake is therefore not a faithful
model of the adapter's TUR error path.
**Detection: unexercised.** Setting status/sense unconditionally (the
adapter-unlike behavior, and also the fake-unlike behavior) survives. (This is
the seam-fidelity gap the commission anticipated: a fake diverging from the
adapter with the suite blind to it. Contract clause E-2; note the adapter is
itself inconsistent — B3 leaves untouched, B4 zeroes.)

### B5 — Profile out-param untouched on error
`fake_get_current_profile` writes `*profile` only on `MOS_OK`.
**Tier: source-verified.** The adapter returns `MOS_ERR_IO` on a
truncated/short reply without committing a profile (`mos_scsi.c:632-634`),
and `0x0000` is reserved as a real in-band answer signalled out-of-band
(`:587-597`). Fake matches the adapter's out-of-band-absence contract.
**Detection: unexercised.** Leaking a value on error survives. (Contract
clause E-1.)

### B6 — Fixed-format sense layout: `0x70` @0, key @2, ASC @12, ASCQ @13
`fake_set_fixed_sense` writes those offsets.
**Tier: spec-derived.** SPC-4 §4.5.3 fixed-format sense data byte layout.
**Detection: broad.** Swapping the ASC/ASCQ offsets breaks 9 tests — the
fixed-format sense layout is load-bearing across the sense-fork suite.

### B7 — Descriptor-format sense layout: `0x72` @0, key @1, ASC @2, ASCQ @3
`fake_set_descriptor_sense` writes those offsets.
**Tier: spec-derived.** SPC-4 §4.5.2 descriptor-format sense data layout.
**Detection: pinned** — `state_descriptor_sense_*` exercises this format;
the parser reads both formats and the test asserts equivalence.

### B8 — Sense key is masked to the low nibble (`key & 0x0F`)
`fake_set_fixed_sense`/`_descriptor_sense` mask the key.
**Tier: spec-derived.** SPC-4: the sense key is the low 4 bits of its byte
(the upper nibble holds FILEMARK/EOM/ILI/SDAT_OVFL or is reserved).
**Detection: unexercised.** Removing the mask survives — every fixture passes
keys already in `0x0..0xF`, so the masking is never triggered. Faithful to the
spec, untested.

### B9 — `make_env`: `bsd_unit = 4`, vendor `"FAKE"`, product `"FIXTURE"`
Fixed identity for the env under test.
**Tier: unfounded (fixture constants).** Arbitrary literals; no source
requires these specific values. They exercise identity passthrough
(`state_real_bsd_unit_passthrough`, `state_empty_bsd_unit_passthrough`).
**Detection: pinned** for the passthrough behavior (the value `4` / `−1` flows
to `out->bsd_unit`); the specific strings are not asserted beyond
non-crashing.

### B10 — Error-code vs in-band status are orthogonal channels
The fake models a contended drive as `tur_err ∈ {EXCLUSIVE_ACCESS, BUSY}`
(transport) and a not-ready disc as `MOS_OK` + `CHECK_CONDITION` (in-band).
**Tier: source-verified.** The adapter surfaces transport/lock failures as
`mos_error` and a reached-but-unusable drive as `CHECK_CONDITION` with valid
sense (`mos_scsi.c:567-568` comment, and the user-client exclusivity gate the
decision tree cites). The two channels are independent.
**Detection: pinned** — `state_tur_transport_busy_maps_busy`,
`state_tur_transport_error_is_returned`, and the contention-status test pin
both channels; but the *error-path out-param* corner (B3/B4/B5) is not.

---

## C. Cross-cutting — the nub invariant (Item 2 result)

### C1 — Lock-reach predicate = `CC ∧ ¬(all-zero triple)`
The state fake's whole purpose is to drive this predicate.
**Tier: source-verified** (the decision tree itself; validated against the
real core by `nub_invariant_check.c`'s self-audit over 268,435,456 inputs,
zero predicate-vs-core mismatches on this tree).

### C2 — §5.5 "same predicate" equivalence — **refuted at the predicate level**
ARCHITECTURE §5.5 claims the kernel's nub-creation predicate and mos's
GESN-skip predicate coincide over the whole sense table.
**Tier: source-contradicted.** `nub_invariant_check.c` (kernel side from
`PollForMedia`, mos side the real core), exhaustive over status×key×ASC×ASCQ,
finds the dangerous quadrant non-empty: exactly **11 inputs**,
`{CHECK_CONDITION, key ∈ {0x1,0x5,0x6,0x7,0x9–0xF}, ASC/ASCQ 00/00}` — senses
whose key hits `PollForMedia`'s switch `default` (kernel keeps the nub) while
mos's full-triple gate fails to fire (key ≠ 0 ⇒ mos takes the lock). §5.5
describes the intermediate `mediaFound` flag (lines 3890/3986) but omits the
`shouldEjectMedia` reset (4012–4052), which is why its "exactly two cases" is
incomplete. The residual is a predicate-level sliver, not a demonstrated field
collision: optical drives emit NOT_READY with specific ASCs (3A/xx, 04/01, …)
in the relevant window, none of which are in the dangerous set.
**Detection: by Item 2 checker** (exit 1, every divergence printed with the
deciding kernel line). Not a fake assumption per se, but it is the central
seam claim the fakes are trusted to uphold, so it is censused here.

---

## Summary by tier

- **source-verified:** A1, A2, A3, A5, A7, A9, A12, B1, B2, B3, B5, B10, C1
- **spec-derived:** B6, B7, B8
- **unfounded (fixture invention):** A4, A6 (mechanism), A8 (encoding), A10,
  A11, B4, B9
- **source-contradicted (divergence found):** B4 (masked), C2 (Item 2)

No unclassified entries. The two that matter beyond bookkeeping: **B4** (the
fake's TUR error path diverges from the adapter and nothing catches it) and
**C2** (the §5.5 equivalence is false on 11 inputs). Everything tagged
*unexercised* is a candidate test; everything tagged *unfounded* is a clause
the seam contract must state so the fakes are audited to it rather than being
trusted as it.
