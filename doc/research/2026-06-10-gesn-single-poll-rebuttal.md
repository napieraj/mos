# GESN single-poll note: partial rebuttal (tray-bit issuance adopted)

**Date:** 2026-06-10
**Status:** Active. Rebuts specific claims in
`2026-05-29-gesn-single-poll.md` per the append rule in this
directory's README; the original is preserved unmodified.
**Scope:** Which of the 2026-05-29 note's claims survived the
2026-05-30 state-detection redesign, and which were inverted by it.

## What the redesign did

One day after the note was written, the state-detection redesign
(CHANGELOG 2026-05-30) replaced the `GetTrayState` convenience call
with a raw GESN (`0x4A`, Media class, Polled) issued through
`mos_raw_cdb()` as the default tray probe on the not-ready branch,
decoded by the pure `mos_internal_gesn_media_door_open` (NEA gate,
Media-class check, full-span reject). GESN issuance is therefore no
longer "deferred and unlikely" — mos issues it on every not-ready
query today.

## Claims rebutted

1. **"Tray already has a clean `GetTrayState` convenience wrapper."**
   Inverted. The wrapper is structurally unable to report a
   command-level failure: its signature carries no
   `SCSITaskStatus`/sense out-parameters (uniquely among the
   command-issuing convenience methods), its documented IOReturns
   cover only transport, and its binary output domain has
   `kMMCDeviceTrayClosed = 0`, so a failed GESN collapses into a
   confident "closed." Header-provable — see ARCHITECTURE §9.7,
   which now cites the signature directly. "Clean" was the premise
   the whole cost comparison rested on, and it was wrong.

2. **"GESN issuance ... deferred and, given the DiscRecording
   substrate, unlikely."** Inverted in the narrow sense: single-bit
   Media-class issuance shipped as the tray probe the very next day.
   The note's *broad* claim — that mos should not adopt GESN as a
   multi-signal single-poll source replacing the DA-wake /
   TUR-sense / registry-ID architecture — still stands and is not
   rebutted here.

3. **"Issuance would be raw `SCSITaskInterface` work ... the most
   expensive integration path."** True as stated, and the cost was
   paid anyway, because the alternative was trusting a wrapper that
   fabricates verdicts. The expense argument was sound; the thing it
   was weighed against was not real.

## Claims upheld

- The **pure-decoder discipline** in the note's "if pursued" section
  was followed almost verbatim: bounds-safe decode modeled on the
  GET CONFIGURATION walker, NEA bit as the validity gate, hostile
  lengths only ever shrinking the trusted region, fuzzed headless.
  The note earned its keep there.
- **"DR feeds the signals, mos owns the state machine"** remains the
  project's framing (ROADMAP, "Architectural").
- **No Device Busy / Operational Change classes** are polled; the
  Media-class tray bit is the only GESN consumption. The note's
  deferral of the wider event surface stands.

## Lesson recorded

The note marked its own descriptor-layout knowledge as "to-confirm,
not load-bearing" but did not apply the same hedge to the
`GetTrayState` characterization, which *was* load-bearing for its
conclusion. Hedge the premise the conclusion stands on, not just the
details.
