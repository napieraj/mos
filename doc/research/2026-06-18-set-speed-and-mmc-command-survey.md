# Set-speed control and the MMC command frontier: feasibility survey (June 2026)

**Question (user):** revisit the previously-rejected *set speed* commands now
that the tray controller verbs have crossed the reporter-only line — are they
feasible/admissible? — and, while the frontier is being re-examined, inventory
the **other MMC commands that are inside, or just barely outside, mos's remit**
and could be considered.

**Method.** Verification against the tree and canonical sources, not
recollection. The scope doctrine (`AGENTS.md` layer 1–4), the reporter-vs-
controller line (`ARCHITECTURE.md` §1, narrowed by the 2026-06-13 controller-
verbs ADR), the "no MODE SELECT / never tunes the drive" half of the MODE SENSE
ADR (`AGENTS.md`, 2026-06-13), the convenience-method inventory
(`ARCHITECTURE.md` §9.7, header-confirmed against public 10.2.8 `SCSITaskLib.h`),
and the shipped command set (`src/`). Opcodes cross-checked against T10 MMC-6 /
SPC-4. No `src/`, schema, or ADR changes; this file is the deliverable. Per the
hardware-role ADR, nothing below is a design input from a drive — these are
spec/source derivations a rig could only falsify.

**Confidence per claim:** HIGH = in-repo source, T10/MMC spec, or Apple header
quoted; MEDIUM = inferred from spec + the convenience inventory but turning on a
macOS transport detail not separately confirmed on hardware (see
`ROADMAP.md`).

---

## Verdict

**Set-speed: decline, and the tray precedent does not reopen it.** The two
commands (SET CD SPEED 0xBB, SET STREAMING 0xB6) are technically issuable on the
existing raw path — the masking-trap argument that *forced* tray onto raw CDBs
does not even apply to them (they return a normal status, so "what the command
did" is reportable). They fail on the two grounds that the tray verbs *passed*:
(1) **no consumer** — the tray ADR's pivot was literally "an orchestrating
consumer now exists" (the ripping robot); set-speed has none named, and a state
library that sets write speed but never writes is an orphaned primitive whose
only real consumers (xorriso/libburn burn engines) already own the axis; and
(2) **wrong side of a line mos just drew on purpose** — set-speed is the *write*
side of the GET PERFORMANCE read mos already does, i.e. it is exactly the
"tune the drive" mutation the MODE SENSE ADR's load-bearing half foreclosed
three lines after admitting the read. The tray verbs report a *mechanism fact*
and change a *removal* state mos already models; set-speed tunes a *policy* mos
does not model and cannot honestly read back.

**The MMC frontier worth keeping warm is entirely read-only — and is mostly
already built.** Correction to this note's first draft (caught when the user
asked to be walked through "the strongest one"): **READ TRACK INFORMATION (0x52)
and READ DISC STRUCTURE (0xAD) are already shipped**, both as non-exclusive
convenience reads — the draft wrongly listed them as not-yet-built candidates
without grepping the tree (`src/mos_trackinfo.c`, `src/mos_physstruct.c` /
`src/mos_discstruct.c`; full inventory in Part 4). The genuinely-unbuilt
read-only reads are only two, and both are weak: **MECHANISM STATUS** (0xBD —
changer slot state, the one mechanical fact 0x2A and GESN both omit) and **READ
BUFFER CAPACITY** (0x5C — live buffer fill), each deferrable behind a named
consumer that does not yet exist. The more honest "frontier" is the **residue
inside already-shipped commands** — chiefly that the 0x52 read is *first-track
only* (no multi-track / multi-session walk). Everything past these (MODE SELECT,
FORMAT UNIT, BLANK, WRITE, CLOSE TRACK/SESSION, SEND/REPORT KEY) is firmly out
and stays out.

**Recommendation: record set-speed as a dated decline (this note is the record);
keep GET PERFORMANCE read-only; park the three reads in ROADMAP as consumer-gated
enrichments, not commitments.**

---

## Part 1 — the two set-speed commands, precisely

Two distinct T10 opcodes have carried "set the drive's speed" across MMC
revisions (HIGH — MMC-6 / MMC-3 opcode tables):

- **SET CD SPEED (0xBB)** — 12-byte CDB; bytes 2-3 = read speed (kB/s), bytes
  4-5 = write speed (kB/s), with `0xFFFF` = "fastest." The legacy verb,
  CD-era; carried forward but marked **obsolete** in MMC-5/6 in favour of SET
  STREAMING. The "rotational control" field (CAV/CLV) lives in byte 1.
- **SET STREAMING (0xB6)** — 10-byte CDB with a parameter-list data-out phase
  carrying a **Performance Descriptor** (start/end LBA, read size/time, write
  size/time). The modern, MMC-5+ verb; the *write* counterpart to GET
  PERFORMANCE (0xAC), which mos already issues **read-only** as
  `mos_query_drive_perf` (`include/mos.h:844`). It is the command real burn
  engines use to clamp speed for buffer-underrun-free or quiet operation.

Both are **mutations**: a data-out / parameter CDB that changes drive behaviour,
not a query. That is the whole of why they were set aside, and it is worth being
precise about *which* objection survives the tray precedent and which does not.

### What the convenience inventory says (HIGH — header)

Neither has a convenience wrapper. `ARCHITECTURE.md:931-933` records, header-
confirmed: *"`SetMediaAccessPermission` and `SetCDSpeed` do not exist in the
10.2.8 header (only `GetPerformance`/`GetPerformanceV2` cover the speed axis,
read-only)."* So, like the tray verbs and INQUIRY-VPD, any set-speed verb would
be **raw-CDB-only** — a sixth-or-seventh entry on the `mos_raw_cdb` path, each
re-raising the §5.5 exclusive-access invariant.

That much *looks* like the tray situation. It is not, and the next part is why.

## Part 2 — why the tray precedent does not carry set-speed

The controller-verbs ADR (`AGENTS.md`, 2026-06-13) admitted tray eject/close/
lock/unlock past the `ARCHITECTURE.md:20` "reporter, not controller" line. It is
tempting to read that as "the controller dam is broken, speed flows through."
It is not — the ADR was explicit that it is an **additive narrowing**, and it
named the two tests the tray verbs had to pass. Set-speed fails both.

**Test 1 — a real orchestrating consumer (tray PASSED, speed FAILS).** The tray
ADR's load-bearing sentence is *"An orchestrating consumer now exists: a
ripping-robot workflow needs to lock an idle drive so a stray operator eject
can't fire the tray into a moving arm."* That is the entire justification for
crossing §1 — not the *capability*, the *caller*. The "agreeing with the
maintainer is not the same as helping" failure mode (`CLAUDE.md`, 2026-06-17)
applies in mirror image here: building the happy path of a controller verb
*without* a named consumer is exactly the out-of-scope construction the doctrine
guards against. **No consumer has asked mos to set drive speed.** The motivating
robot wants the *tray locked*; it does not want mos choosing its burn speed —
the thing that would set speed is the burn engine, and the burn engine
(xorriso/libburn, MakeMKV) issues 0xB6/0xBB itself, holding its own exclusive
access for the write. A `mos speed set` that the burn engine cannot use (because
mos releases the lock on return, Part 3) and that nothing else wants is a verb
with no caller.

**Test 2 — the right side of the line mos just drew (tray PASSED, speed FAILS).**
The MODE SENSE ADR (`AGENTS.md`, 2026-06-13) admitted *read-only* MODE SENSE of
the optical pages and, in the same breath, fixed the load-bearing exclusion:
*"no MODE SELECT (mos reports configuration, it never tunes the drive — the
mutation dvdisaster/sdparm perform stays out)."* SET CD SPEED / SET STREAMING
are the **speed-axis instance of exactly that exclusion**: they are the *write*
side of the GET PERFORMANCE read mos already does, the optical equivalent of a
MODE SELECT on the speed parameters. mos reads the speed surface (0xAC) and
*reports* it; setting it is the mutation that ADR drew the line against. The
tray verbs are not on this axis at all — they change a **removal** state (tray
position, prevent bit) that mos already classifies and reports, and they report
a **mechanism fact** about what the command did (done / refused_locked /
refused_other). Speed is not a state mos models, and (Part 3) mos cannot
honestly report that a set "stuck."

**Test 0 — does the masking trap even force raw? No (and that's telling).** The
tray verbs were *forced* onto raw CDBs because `SetTrayState` is sense-blind —
it structurally cannot report a `5/53/02` locked-eject refusal (§9.7/§9.9), so
the convenience method *cannot honor the mechanism-facts contract*. Set-speed
has no such trap: a raw 0xB6/0xBB returns GOOD or CHECK CONDITION like any
command, so "what the command did" *is* reportable. The masking-trap argument —
the one affirmative reason the tray verbs needed raw — **does not apply**, which
removes the only structural pull toward implementing it and leaves only the two
failed tests above.

## Part 3 — even on the merits, speed is a poor fit for mos's model

Three model-level mismatches, independent of the doctrine tests (MEDIUM —
spec-derived, transport-dependent):

1. **No durable readback.** mos's whole shape is acquire-on-call / release-on-
   return (§3). A set-speed under that model is fire-the-CDB-and-release. But
   GET PERFORMANCE reports the drive's *capability/negotiated* descriptors,
   media-dependent and drive-clamped — it is not a guaranteed mirror of "the
   speed I just set." So `mos speed set` could not be followed by a `mos speed
   get` that confirms the set stuck, the way `mos tray lock` is confirmable by
   the prevent state being per-I_T-nexus durable (the tray ADR's whole Part 4
   derivation). A controller verb whose effect mos cannot read back is the worst
   kind for a *state* library.

2. **The effect is transient and not a state.** Drive speed resets on media
   change, on many drives on power/reset, and is renegotiated per-operation by
   the writing initiator. It is a per-operation *policy input*, not a disc/drive
   state with a place in the enum. mos classifies states; speed has no cell.

3. **Burn-quality adjacency.** Speed selection is a burn-engine concern, and the
   ROADMAP "Out of scope" list already fences the neighbouring vendor knobs
   (PureRead, AudioMaster, GigaRec/AutoStrategy, quiet/acoustic mode) as
   "belong to higher-layer media libraries." Set-speed is the standards-track
   member of that same family. Admitting it invites the quirk-by-quirk drive-
   tuning surface the scope doctrine exists to refuse.

If a consumer ever *does* surface (a held-session burn-prep verb where mos stays
resident and holds the lock across a multi-step op — the same shape the tray ADR
parked for a future held-lock mode), the argument changes and that is a fresh
dated entry to make then. Today: decline.

## Part 4 — the read-only MMC frontier (what *is* worth considering)

The honest other half of the question — corrected after auditing the actual
command surface (the first draft listed shipped commands as candidates). These
are read-only, oracle-is-MMC, genuinely in or one-step-outside the remit.

**Already shipped — the in-remit reads mos issues today** (full audit:
`grep '(\*h->mmc)->' src/*.c` plus the `mos_raw_cdb` call sites):

- *Convenience methods (non-exclusive, no raw CDB):* TEST UNIT READY, GET
  CONFIGURATION, MODE SENSE 10 (pages 0x2A / 0x01), READ DISC INFORMATION (0x51),
  READ TOC/PMA/ATIP (0x43, format 0000b + CD-TEXT 0101b), **READ TRACK
  INFORMATION (0x52)**, **READ DISC STRUCTURE (0xAD, DVD/HD-DVD physical +
  copyright, BD disc-info)**, GET PERFORMANCE (0xAC, read-only).
- *Raw CDBs (exclusive, `mos_raw_cdb`):* GET EVENT STATUS NOTIFICATION (0x4A,
  state), START STOP UNIT (0x1B) + PREVENT ALLOW (0x1E) (tray), INQUIRY (0x12,
  standard + VPD 0x80), READ FORMAT CAPACITIES (0x23). Count: **one-of-five**.

So the two reads the first draft nominated as the strongest candidates were
already built — 0x52 feeds `mos.capacity.v1` (recordable view) and
`mos.metadata.v1` (`track_info`); 0xAD feeds `mos drive` physical-format
identity. What actually remains is thin:

| Command (opcode) | In-remit? | What it would add | Path | Status / gate |
|---|---|---|---|---|
| **0x52 multi-track / multi-session walk** | **Residue of a shipped read** | Per-track / last-incomplete-track NWA — the *append point of a multi-session disc*, which the current first-track-only read (`mos_query_track_info`, ADDRESS=1) does not reach | convenience, already wired — would loop over tracks from READ DISC INFORMATION's last-track | The only "in-remit, not built" item with a plausible consumer (multi-session append tooling). Today's first-track read is correct for single-track pressed/blank media; a multi-session appendable disc needs the last incomplete track. Park behind that consumer. |
| **MECHANISM STATUS (0xBD)** | **Edge — yes for changers** | Changer slot occupancy, current slot, door/tray open, mechanism fault | no convenience wrapper → would be a sixth raw CDB | The unique residue 0x2A and GESN both omit: *multi-slot* mechanical state. Only earns a raw verb if a changer/jukebox consumer appears; single-tray drives get nothing new over GESN tray state. Park. |
| **READ BUFFER CAPACITY (0x5C)** | **Edge — burn-adjacent** | Drive buffer total + *live* fill | no convenience wrapper → raw | Read-only and spec-clean, but its only use is burn-underrun monitoring — a burn-engine concern, same family as set-speed. The *static* buffer size already comes from MODE SENSE 0x2A (shipped). Decline absent a burn consumer. |
| **READ CAPACITY (0x25)** | Yes, read-only | Last-LBA / block size for a mounted disc | convenience absent → raw | **Already analyzed** — `doc/research/2026-06-13-read-capacity-feasibility.md`: deferred behind a falsifier; the kernel's cached IOMedia size + READ FORMAT CAPACITIES cover its cases. No change. |
| **READ CD (0xBE) / READ(10)(12) (0x28/0xA8)** | **No** | Raw/cooked sector data | raw, exclusive | *Reading media content* — the third I/O modality scope-doctrine layer 3 disqualifies (block-device I/O + privilege). Filesystem/sector parsing is the consumer's. Out. |

**Hard out (mutation / write / DRM / SPC-generic), restated so the line is on
the record):**

- **MODE SELECT (0x15 / 0x55)** — the mutation the MODE SENSE ADR foreclosed;
  set-speed's sibling. Out.
- **FORMAT UNIT (0x04), BLANK (0xA1), WRITE(10/12) (0x2A/0xAA), CLOSE TRACK/
  SESSION (0x5B), SYNCHRONIZE CACHE (0x35)** — the burn pipeline. mos reports
  formattable capacity (0x23, read-only) but never issues the format/blank/write
  itself. xorriso/libburn/MakeMKV territory. Out.
- **SEND KEY / REPORT KEY (0xA3/0xA4), AACS/CSS structure formats** — DRM
  handshake / key extraction, explicitly out (`ROADMAP.md:193-194`, the
  content-protection ADR reports *capability*, never key state). Out.
- **LOG SENSE (0x4D), SPC power/caching/control mode pages, RESERVE/RELEASE** —
  the "no SPC ambition" of layer 2. LOG SENSE counters remain hardware-capture-
  first, never a design input. Out.

---

## Summary table

| Item | Verdict | Why |
|---|---|---|
| SET CD SPEED (0xBB) | **Decline** | obsolete; mutation; no consumer; wrong side of "never tunes the drive" |
| SET STREAMING (0xB6) | **Decline** | modern speed-*set* = write side of GET PERFORMANCE read; no consumer; not readback-able; burn-engine owns it |
| READ TRACK INFORMATION (0x52) | **Already shipped** | first-track read feeds `mos.capacity.v1` + `mos.metadata.v1` |
| READ DISC STRUCTURE (0xAD) | **Already shipped** | DVD/HD-DVD physical + copyright, BD disc-info → `mos drive` |
| 0x52 multi-track / multi-session walk | **Park — only real read candidate** | last-incomplete-track NWA for multi-session append; needs that consumer |
| MECHANISM STATUS (0xBD) | **Park — changers only** | unique multi-slot residue; would be a raw verb; needs a changer consumer |
| READ BUFFER CAPACITY (0x5C) | **Decline absent burn consumer** | live-fill is burn-adjacent; static buffer already via 0x2A |
| READ CAPACITY (0x25) | **No change** | already deferred (2026-06-13 note) |
| READ CD / READ(10/12) | **Out** | reading media content — layer-3 disqualified |
| MODE SELECT / FORMAT / BLANK / WRITE / CLOSE / SEND-REPORT KEY / LOG SENSE | **Out** | mutation / write / DRM / SPC-generic |

**Net:** the set-speed door the tray precedent seemed to open is closed on the
merits, by the tray ADR's own two tests and the MODE SENSE ADR's own exclusion —
no new doctrine needed, this note is the dated record. The read-only "frontier"
turned out to be mostly already built (0x52, 0xAD shipped); what genuinely remains
is one plausible enrichment (the 0x52 multi-session walk) and two weak edge reads
(0xBD changers, 0x5C burn buffer), each parked behind a consumer that does not yet
exist. If any is later built, it lands as a feasibility note + ADR in the
established pattern, not on the strength of this survey.
