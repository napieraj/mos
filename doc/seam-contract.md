# The mos adapter ↔ pure-core seam contract

> **Provenance & post-audit status.** Authored by the fourth external
> reviewer (seam-fidelity audit, 2026-06-10), vendored with amendments
> marked **[RESOLVED]** where the same day's fixes changed a clause's
> status. Headline resolutions: CO-3's gate divergence is FIXED (the
> core now mirrors the kernel's full nub predicate, eject reset
> included; `tests/audit/nub_invariant_check.c` exits 0 exhaustively and
> runs in CI); E-1 is now suite-enforced (the fakes poison every
> out-param on error); E-2 is resolved by adopting **undefined-on-error**
> as the contract rule; C-3's flip is guarded (a no-op flip now fails the
> latency test); V-5's mask is pinned for BOTH sense formats (the fixed
> format already was — a small correction to the census — and the GESN
> class-mask width from the rider is pinned too). The remaining
> UNGUARDED set is `{O-1, O-3, V-1}`, all hardware-shaped, wired into
> STATUS.md's Mac-smoke checklist.

**Commission Item 4.** The obligations the adapter (`src/mos_scsi.c`,
`mos_state.c`, `mos_watch.c`) owes the pure core (`mos_state_core.c`,
`mos_watch_core.c`) and vice versa. This is the spec a second adapter builds
against, the checklist a Mac smoke test validates, and the contract the
suite's fakes are audited **to** — rather than the fakes being the contract.

Every clause carries two tags:

- **Evidence** — the strongest tier establishing the clause:
  `source` (adapter/kernel enforces it), `spec` (T10), or `fixture` (only the
  fake asserts it today; promote to `source`/`spec` or accept as a convention).
- **Detection** — whether the pure suite would catch a violation today
  (from Item 3): `pinned`, `broad`, or **`UNGUARDED`** (a fake/adapter
  divergence here is invisible to the suite — the clauses that most need a
  Mac smoke check or a new pure test).

Census IDs (`A1`…`C2`) cross-reference `seam_census.md`.

---

## 1. Call ordering and counts

**CO-1.** Per query, the core issues TEST UNIT READY exactly once and never
retries for UNIT ATTENTION. *(Evidence: source — decision tree step 1, adapter
issues one TUR. Detection: pinned, `state_tur_issued_once`. Census B1.)*

**CO-2.** **[AMENDED by the CO-3 fix]** The core reaches `get_tray_state`
(the exclusive-access GESN, "the lock") **iff** TUR returned `MOS_OK`,
status is `CHECK_CONDITION`, and the sense is NOT a kernel-nub-preserving
`00/00` — i.e. it locks when ASC/ASCQ is non-zero, or when the triple is
`{key in {0x2,0x3,0x4,0x8}, 00/00}` (the kernel ejects those, so no nub
survives). On GOOD, on contended status, on transport
error, and on a CC with an all-zero triple, the lock is **not** taken.
*(Evidence: source — decision tree. Detection: pinned, `state_ready_short_
circuits_without_tray_probe` + the not-ready set. Census B2, C1.)*

**CO-3.** The adapter must keep CO-2's "not-ready ⇒ not mounted ⇒ lock free"
property true against the **kernel's actual nub predicate**, which is
`mediaFound` (PollForMedia 3890/3986) **minus** the `shouldEjectMedia` reset
(4012–4052) — not the flag alone. Known exception: for `{CC, key ∈
{0x1,0x5,0x6,0x7,0x9–0xF}, ASC/ASCQ 00/00}` the kernel keeps a nub while the
core takes the lock (11 inputs). *(Evidence: source — `IOSCSIMultimedia
CommandsDevice.cpp`, exhaustively via `nub_invariant_check.c`. Detection:
Item 2 checker, exit 1. Census C2.)* **[RESOLVED — third option taken]:** neither the blunt tighten nor
doc-only. The gate now mirrors the kernel's FULL predicate: CC+00/00
skips the lock for every nub-preserving key and still probes for the
four keys the kernel ejects (`{0x2,0x3,0x4,0x8}`) — closing all 11
inputs while PRESERVING the DEVICE_FAULT/MEDIA_UNREADABLE
classifications the blunt fix would have lost. `nub_invariant_check.c`
(vendored, predicate updated) exits 0 over the full domain;
`state_kernel_nub_preserving_sense_never_locks` (bites: old gate fails
it) and `state_kernel_ejecting_sense_still_probes` pin both arms.

**CO-4.** Per pump, the watch core invokes `probe` once if the schedule gate
opens, zero times if it sleeps. *(Evidence: source — `mos_watch_core.c:303,
312`. Detection: pinned, snapshot/no-change tests. Census A1.)*

**CO-5.** Per **probing** pump, the core reads `mono_ms` twice — once at the
schedule gate / probe-start, once after the probe — and `latency_ms` is their
difference; a **sleeping** pump reads `mono_ms` once and returns before
probing. *(Evidence: source — `:300, :313`. Detection: pinned-narrow, latency
test. Census A2.)*

**CO-6.** Per emitted event, the core reads `wall_ms` once (for the `ts`);
`stream_open_wall_ms` is captured once at init, never per pump. *(Evidence:
source — `:154, :213`. Detection: broad, clock-domain set. Census A3.)*

---

## 2. Clock semantics

**CL-1.** `mono_ms` is a monotonic millisecond counter used **only** for
scheduling and latency; `wall_ms` is Unix-epoch milliseconds used **only**
for `ts` and `stream_open_wall_ms`. The core must never schedule on a wall
value. *(Evidence: source — vtable split, the ~55-year-deadline regression it
fixed. Detection: broad, `test_clock_domains_separate`. Census A5.)*

**CL-2.** A non-monotonic `mono_ms` (end < start across a probe) must clamp
`latency_ms` to 0, never underflow. *(Evidence: source — core saturates;
spec — POSIX forbids backward `CLOCK_MONOTONIC`, so this guards a buggy
adapter. Detection: pinned for the **code** (`test_latency_saturates_on_
backward_clock` kills the raw-subtraction mutant); the **fixture's** backward
step is itself UNGUARDED — see C-3. Census A11.)*

**CL-3.** The absolute value returned by any single `mono_ms` read is not
contractual — only differences are. An adapter whose clock has constant skew
is conformant. *(Evidence: fixture — the suite tests deltas only. Detection:
UNGUARDED, by construction; A4's mutant survives. Census A4.)* This is a
**deliberate** latitude, recorded so a second adapter is not over-constrained.

**CL-4.** The adapter monotonicizes `wall_ms` per process so the
`(registry_id, stream_open_wall_ms)` identity pair is stable across a watch
lifetime even if the system wall clock steps. *(Evidence: source —
`:148-149`. Detection: pinned, `…uses_wall_ms`. Census A3, A12.)*

---

## 3. Buffer ownership and initialization

**O-1.** The party that fills `mos_state_result` zero-initializes it first;
unset fields read as zero. The adapter does this (`mos_scsi.c:193`); a probe
implementation must too. *(Evidence: source. Detection: UNGUARDED — dropping
the fake's `memset` survives; no test reads an unset field. Census A9.)*
**Mac-smoke candidate.**

**O-2.** The `sense[18]` buffer is caller-owned and exactly 18 bytes; the
adapter copies 18 bytes into it (`memcpy(sense,&sense_struct,18)`), the core
reads within 18. *(Evidence: spec — SPC-4 fixed/descriptor sense ≤ 18 here;
source — adapter copy. Detection: broad — the sense-fill mutant breaks 15
tests. Census B6, and Item-3 "TUR does not copy sense.")*

**O-3.** Identity strings (`vendor`/`product`/`revision`) are borrowed
pointers owned by the handle, valid for the lifetime of the result that
borrows them; the core does not free or retain them past that. *(Evidence:
source — `make_env`/adapter pass literals/handle-owned storage. Detection:
pinned indirectly (passthrough tests); lifetime itself UNGUARDED in the pure
suite — a headless test cannot outlive a handle. Census B9.)* **Mac-smoke
candidate.**

**O-4.** **[ADDED post-audit — the forward-looking clause for v0.4 RT=0
enrichment]** Every variable-size transfer has three lengths from three
authorities: **allocated** (caller's buffer/request — ours),
**transferred** (the transport's realized byte count — ours-adjacent),
and **claimed** (the device header's self-described length — hostile).
The trusted parse region is `min(allocated, transferred)`, computed
once at the seam by `mos_internal_trusted_len`; the device claim is
data that may only SHRINK that bound, never set or grow it, and any
header-derived total (e.g. GET CONFIGURATION's `Data Length + 4`) must
be computed in `uint64_t` before the clamp so it cannot wrap on the way
in. This is O-2 generalized from the fixed 18-byte sense buffer to
drive-sized replies; it forecloses the allocation-length-overread class
(header claims 0xFFFF over an 8-byte transfer) by construction. Any
v0.4 parser that derives a bound any other way violates this contract.
*(Evidence: source — `mos_pure.c`, with four boundary/wrap unit tests
and fuzz phase 6's standing property check (I1–I4: bounded by each
authority; monotone in the claim). Detection: pinned + fuzz.)*

---

## 4. Value domains

**V-1.** `bsd_unit == −1` **iff** no whole-disk media node exists ("no
media"); otherwise a non-negative unit. The adapter derives this from the
IOKit registry, **not** from the classified state. *(Evidence: source —
`mos_scsi.c:195,287`. Detection: broad for the correspondence
(`unit-always-4` mutant breaks 3); the **derivation mechanism** is fixture-
approximated — the fake keys on state, so the real shape "media node present
while state EMPTY" is untestable here. Census A6.)* **Mac-smoke candidate**
(verify a real eject/insert drives `bsd_unit` independently of the state
enum).

**V-2.** `current_profile == 0x0000` is a **real** drive answer ("no current
profile"), distinct from "enrichment failed." The adapter signals
enrichment failure out-of-band (`MOS_ERR_IO`), never by returning `0x0000`.
*(Evidence: source — `mos_scsi.c:587-597`. Detection: pinned — the `0xFFFF`
sentinel test depends on a genuine wire-0 being representable. Census A8.)*
The fixture's `0xFFFF`→`0` sentinel exists solely because the C zero-init
collides with this value; a real adapter has no `0xFFFF` sentinel.

**V-3.** `media_id` is an opaque 64-bit swap fingerprint; `0` means
"unavailable this probe" and must never overwrite a known non-zero id.
*(Evidence: source — registry entry ID; core's zero-guard. Detection: pinned,
media-changed/transient-zero tests. Census A7.)*

**V-4.** `registry_id` is an opaque token; `0` is a legal passthrough value
(real kernel IDs are ≥ 2³²+256, so `0` never collides). *(Evidence: source —
xnu reservation. Detection: pinned, `test_zero_registry_id_passes_through`.
Census A12.)*

**V-5.** Sense key is the low nibble of its byte; ASC/ASCQ occupy their
SPC-4 offsets (fixed: 2/12/13; descriptor: 1/2/3). *(Evidence: spec — SPC-4
§4.5.2/§4.5.3. Detection: broad for offsets (9 tests); the key-nibble **mask**
is UNGUARDED — all fixtures pass ≤4-bit keys, so masking is never triggered.
Census B6, B7, B8.)*

---

## 5. Error / status orthogonality

**E-0.** Two independent channels: a transport/lock failure is a negative
`mos_error`; a reached-but-unusable drive is `MOS_OK` with an in-band
`CHECK_CONDITION` status and valid sense. The core must not conflate them.
*(Evidence: source — `mos_scsi.c:567-568`, user-client exclusivity gate.
Detection: pinned, transport-busy / transport-error / contention tests.
Census B10.)*

**E-1.** On a **non-OK** return, the core must treat all out-params as
meaningless and not read them. *(Evidence: source — tray and profile adapters
return before committing a value (`:546`, `:632-634`); core ignores them.
Detection: UNGUARDED — leaking values into `*tray_open` / `*profile` on error
survives. Census B3, B5.)* **Mac-smoke / new-pure-test candidate.**

**E-2.** The adapter's **own** error-path out-param behavior is currently
**inconsistent and partially unmodelled**: TUR *zeroes* `*status`/`sense` on
transport error (`:577-578`), while tray/profile leave their out-params
*untouched* (`:546`, `:632`). The fakes leave all three untouched, so the
fake's TUR path diverges from the adapter and the suite is blind to it.
*(Evidence: source-contradicted — adapter zeroes, fake does not; masked by the
core pre-zeroing its locals and ignoring-on-error. Detection: UNGUARDED — the
"set status/sense on error" mutant survives. Census B4.)* **Action:** pick one
adapter convention (zero-on-error everywhere, or untouched-everywhere),
document it as the contract, and make the fakes mirror it. Until then a second
adapter has no authoritative rule to follow here.

---

## 6. Clauses that are contract but only the fixture asserts today

These have **no** source/spec backing beyond the fake; they are conventions,
flagged so they are either promoted or consciously accepted.

**C-1.** The watch fake's `0x0040` (BD-ROM) default profile. *(Evidence:
fixture. Detection: UNGUARDED — changing it survives. Census A8.)* Pure
convention; not a real adapter behavior. Keep only as fixture sugar.

**C-2.** `make_env`'s identity literals `"FAKE"`/`"FIXTURE"` and `bsd_unit=4`.
*(Evidence: fixture. Detection: pinned for passthrough, values themselves not
asserted. Census B9.)* Convention.

**C-3.** The latency saturation test depends on `flip_mono_on_probe` actually
firing, but nothing guards the flip itself; if it silently no-ops, the test
goes green for the static-clock reason and the saturation guard loses its only
coverage. *(Evidence: fixture. Detection: UNGUARDED — disabling the flip
survives. Census A11.)* **Action:** add a fixture self-check (assert the two
`mono` reads differ in that test) so the backward-step path can't rot
silently.

---

## Appendix — the UNGUARDED set (where the suite cannot see a divergence)

These are the clauses a Mac smoke test or a new pure test should cover; they
are exactly the seam-fidelity blind spots this audit set out to find:

| Clause | What a faithful adapter must do that the suite won't catch |
|---|---|
| O-1 | zero-init the result before filling it |
| O-3 | keep borrowed identity strings valid for the result's lifetime |
| V-1 | derive `bsd_unit = −1` from node-absence, not from the state enum |
| V-5 | mask the sense key to its low nibble |
| E-1 | ignore out-params on a non-OK return |
| E-2 | follow one consistent error-path out-param rule (TUR vs tray/profile diverge today) |
| C-3 | (fixture) keep the backward-clock injection effective |

And the one **source-contradicted** finding outside the UNGUARDED table:

| Clause | Status |
|---|---|
| CO-3 / C2 | §5.5 nub equivalence false on 11 inputs `{CC, key∈{1,5,6,7,9–F}, 00/00}`; predicate-level sliver, hardware-empirically unreached, fix is a judgment call |

No clause in this document is untagged.
