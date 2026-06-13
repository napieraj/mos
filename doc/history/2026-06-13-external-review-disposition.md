# External code-review disposition — 2026-06-13

An external reviewer audited a zip snapshot and filed 16 findings. Every
finding was verified against the live tree (line numbers matched — live
code, not a stale snapshot). This note records the disposition of each
under the project doctrine: **AGENTS.md Process rule 2** (don't change
behavior on a review alone; review points become comment updates or
v0.next flags until hardware validation), **CLAUDE.md R6** (verify before
conceding), and the **hardware-role ADR** (a hypothesised quirk lands as a
captured fixture, never a behavior change).

## Fixed this pass (Tier C — genuine, hardware-independent defects)

| # | Defect | Fix |
|---|--------|-----|
| F5 | `mos_query_drive_perf` set `have = (rd_cnt>0 \|\| wr_cnt>0)`, contradicting its own comment ("read read is the gate") and the public header (`include/mos.h`: speeds meaningful only when have, `>= 1` descriptor; `descriptor_count` is read-only). | `src/mos_scsi.c`: `have = (rd_cnt > 0)`. Regression test deferred — see note below. |
| F6 | `mos features` human output claimed "--json carries the full list", but `emit_json` is capped at `FEAT_CAP` (was 128) identically to human output. | `cli/features.c`: `FEAT_CAP` 128 → 256 (the walk's own ceiling, 1024B/4), so both renderings carry every descriptor walked; reworded the now-unreachable overflow guard to drop the false claim. |
| F11 | `scripts/mutation-pass.py` PURE/TESTS lists were stale (5 sources, 6 test modules missing), so it compiled an incomplete binary and reported "BASELINE NOT GREEN". | Updated both lists to match the `mos_pure`/`mos_tests` targets in `CMakeLists.txt`, with a comment to keep them in lockstep. |
| F13 | `MOS_ERR_UNSUPPORTED` description "not implemented in this build" contradicted the enum comment ("command not supported by drive") and its `kIOReturnUnsupported` mapping (drive/driver, not build). Already flagged 2026-06-11 (review-triage A5), disposition "reword to cover both". | `src/mos_strings.c`: "operation unsupported by this drive, driver, or build". The schema error-*code* token `"unsupported"` (`cli/common.c`, `mos.error.v1`) is unchanged. |

F5 regression test deferred (not landed blind): the only path is the
Apple-only adapter-fake suite, which has no perf coverage and whose fake
`mmc_GetPerformance` returns a zeroed buffer with no per-direction reply
control. A real test needs the fake extended to return a non-zero read
descriptor count for one direction and zero for the other — unverifiable
in a non-macOS container, so it is a follow-up rather than blind-landed.

## Declined (Tier A — deliberate, documented decisions the review missed)

- **F1 — GESN raw read passes `sizeof resp`, not the realized transfer
  count (the review's flagship "high severity").** Declined. The discard
  is documented and deliberate: `src/mos_scsi.c:505-509` states the buffer
  span is the bound by design and the decoder trusts the reply's own Event
  Data Length "not the transport's realized count (some USB bridges
  under-report it)" — the AGENTS.md "trust-the-reply's-own-length rule".
  The proposed fix (`if (xferred < 6) return MOS_ERR_IO`) would
  **reintroduce** the USB-bridge false-failure the design exists to avoid.
  The reviewer half-concedes ("if the comment reflects real hardware, make
  that a compatibility path backed by fixtures"). Per the hardware-role
  ADR this changes only via a captured fixture, never a crafted-bytes
  hypothetical.
- **F2 — convenience decoders trust `sizeof buf`.** Declined. Same
  dual-length rule (O-4); the pure decoders gate on the reply's own length
  fields, which the reviewer concedes ("most pure parsers do apply strong
  internal gates").
- **F10 — state core does not carry the GESN failure reason into the
  result.** Declined as a defect. Deliberate: "GESN's open/closed is never
  overturned by the sense" (`src/mos_state_core.c:136-151`). Surfacing the
  probe error is an additive schema/API change → v0.next, not a fix.
- **F14 — enum-width `_Static_assert` breaks `-fshort-enums` consumers.**
  Declined. Deliberate FFI ABI-pin policy (`include/mos.h:58-60`),
  documented; the trade-off is intended.

## Acknowledged, deferred to maintainer / v0.next (Tier B)

Real observations, but design/contract judgment calls rather than defects;
left untouched this pass:

- **F3** — `mos_tray_eject(force=true)` discards the pre-step ALLOW outcome
  (documented contract, `src/mos_tray.c:95-105`). Exposing both the
  pre-step and eject outcomes is an API/CLI shape change.
- **F4** — one-shot CLI finalizes stdout after `mos_close(h)`; `errno`
  could be clobbered between the latched `ferror` and the read. Reorder is
  cheap but a behavior nuance on the broken-pipe exit-code path.
- **F7 / F16** — enumeration/list/watch caps drop entries without
  surfacing truncation; adding `total_seen`/`truncated` is a `mos.list.v1`
  schema enhancement.
- **F8** — public escapers dereference NULL when `out==NULL && out_cap>0`
  (documented contract); making them total is a safety nicety.
- **F12** — JSON string OOM emits the literal `"<oom>"`; error propagation
  is the clean fix but invasive.
- **F15** — `stream_epoch_wall_ms` / watch-deadline additions can wrap
  `UINT64_MAX` (reviewer: "mostly theoretical").

## Positive observations the reviewer noted (preserved for the record)

Pure/adapter split, no-OOB parsing discipline (`mos_internal_trusted_len`),
centralized JSON/terminal escaping, the TUR-short-circuit state machine,
and the CMake pure/Apple gating all called out as strengths.
