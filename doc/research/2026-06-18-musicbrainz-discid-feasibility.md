# MusicBrainz Disc ID as a first-class `mos` fingerprint — feasibility

**Date:** 2026-06-18. **Status:** CLOSED — declined in-library; resolved as a
documented consumer recipe (see "Decision" at the end). Retained as the record of
why the in-library path was costed and not taken.
**Branch:** `claude/musicbrainz-toc-fingerprint-cli`.
**Depends on:** the CD-TOC work on `claude/set-speed-feasibility-lty091`
(`src/mos_cdtoc.c`, `mos_query_toc`, the `mos_toc` accessors) — this analysis is
grounded in that branch's TOC structures and **must not land before it merges**.

## Question

The earlier survey (this branch, first pass) concluded the MusicBrainz Disc ID
was consumer territory: readily-available CLI tools (`cd-discid --musicbrainz`,
libdiscid's `discid`, `mbdiscid`; all in Homebrew) already surface it, and
`ROADMAP.md:105` files third-party IDs as derive-from-primitive. The maintainer
overrode that on cost-and-use-case grounds (recorded below). This note answers
the remaining question: **grounded in the actual TOC implementation, how cheap
and how correct is computing the ID inside `mos`?**

Verdict: **trivial to compute, zero new command surface, fully offline-testable.
The only real costs are (a) ~180 lines of net-new pure crypto/encoding the tree
does not currently carry, and (b) the doctrine override, which is the maintainer's
call and is logged in AGENTS.md, not here.**

## Grounding: the inputs are already parsed and accessor-exposed

`mos_query_toc` (set-speed branch) issues READ TOC/PMA/ATIP format 0000b through
the **non-exclusive `ReadTableOfContents` convenience method** — no raw CDB, no
`ObtainExclusiveAccess`, no BUSY-on-mounted problem. It also accepts the
kernel-cached CDTOC blob as the primary source (`mos_internal_cdtoc_to_toc`,
zero SCSI commands). Either way the parsed result is `struct mos_toc`
(`src/mos_pure.h`):

```
first_track, last_track, track_count, have_leadout, leadout_lba,
tracks[i] = { track, adr, control, start_lba }
```

Exposed read-only via `mos_toc_first_track` / `_last_track` / `_have_leadout` /
`_leadout_lba` / `_track_count` / `_track_number(i)` / `_track_start_lba(i)`.

That is the **entire** MusicBrainz Disc ID input. Nothing else is read.

## The algorithm (canonical, cited — not from memory)

Grounded in libdiscid `src/disc.c` (`create_disc_id`) and the MusicBrainz
Disc ID Calculation page:

1. Build an ASCII string by concatenating uppercase hex:
   - `%02X` first track number
   - `%02X` last track number
   - 100 × `%08X` **frame offsets**:
     - offset[0] = lead-out absolute frame
     - offset[1..99] = track *N* absolute frame, or `00000000` if track *N* absent
2. SHA-1 of that ASCII string → 20 bytes.
3. Base64 the digest with libdiscid's URL-safe alphabet
   (`A–Z a–z 0–9 . _`, padding `-`): standard base64 with `+`→`.`, `/`→`_`,
   `=`→`-`. Result is always 28 characters.

**The +150 framing is the one detail that bites.** MusicBrainz offsets are
*absolute frame addresses* = LBA + 150 (the 2-second / 150-frame lead-in
pregap). `mos`'s `start_lba` / `leadout_lba` already have the 150 **subtracted**
(`mos_internal_cdmsf_to_lba` in `mos_cdtoc.c` does `frames - 150`). So the mapping
is: `mb_offset = mos_lba + 150`.

Mapping table:

| MB Disc ID field        | `mos_toc` source                          |
|-------------------------|-------------------------------------------|
| first track (`%02X`)    | `first_track`                             |
| last track (`%02X`)     | `last_track`                              |
| offset[0] (lead-out)    | `leadout_lba + 150` (gate on `have_leadout`) |
| offset[track N]         | `start_lba(of track N) + 150`             |
| offset[absent track]    | `0x00000000`                              |

Track offsets are indexed by **track number**, not array position. `mos_toc` is
fail-closed contiguous (no gaps in first..last — `mos_internal_cdtoc_to_toc`
refuses a gap), so `offset[t] = tracks[t - first_track].start_lba + 150` for
t in first..last, zero elsewhere. Data tracks are included verbatim (their
`control` data bit is irrelevant to the ID — MB hashes offsets only); CD-Extra's
data track contributes its offset like any other. The lead-out is the **disc**
lead-out (highest session), which is exactly what both `mos_toc` sources already
yield.

## Committed test vector (no hardware needed)

The canonical worked example from the MusicBrainz Disc ID Calculation page is a
complete unit-test fixture for the pure function:

```
first_track = 1, last_track = 6
track LBAs : 0, 15213, 32164, 46442, 63264, 80339      (+150 → 150, 15363, 32314, 46592, 63414, 80489)
leadout LBA: 95312                                       (+150 → 95462)
expected   : 49HHV7Eb8UKF3aQiNmu1GR8vKTY-
```

This pins the hex layout, the +150, the SHA-1, and the base64 alphabet in one
assertion — the whole computation is verifiable in `tests/` against `mos_pure`
with no Apple framework and no drive, the same as every other pure decoder.

## Implementation cost (the honest part)

**New code the tree does not have today:** `grep` confirms **no SHA-1 and no
base64** anywhere in `src/`. So this adds, to the pure layer:

- a SHA-1 (public-domain reference implementation, ~150 lines) — `mos_sha1.c`;
- the URL-safe base64 variant (~30 lines) — fold into the disc-id source;
- `mos_internal_musicbrainz_discid(const mos_toc *, char out[29])` — the pure
  function doing the hex build → SHA-1 → base64 (~40 lines), `mos_discid.c`.

Total ≈ 220 lines, all pure, all offline-tested. This is the largest *code*
footprint of any recent enrichment (firmware-date, serial, protection all rode
existing parses with no new primitives). It is still small, self-contained, and
0BSD-compatible (use a public-domain SHA-1, not an LGPL one — do **not** vendor
libdiscid). Worth stating plainly so the "trivial" claim is scoped: trivial in
*command surface and risk*, modest in *line count*.

**Integration points** (all additive, pre-tag mutable-in-place per the
JSON-schema ADR):

- `include/mos.h`: `const char *mos_toc_musicbrainz_discid(const mos_toc *t)`
  (NULL when `!have_leadout` or no tracks — a lead-out-less TOC has no identity,
  matching the schema's existing warning). Handle-owned 29-byte buffer on the
  `mos_toc`, computed lazily or at parse time.
- `cli/metadata.c`: emit `"musicbrainz_discid"` inside the existing `toc` object
  (after `leadout_lba`), null-propagating.
- `schemas/mos.metadata.v1.json`: add the key to `toc` (required-and-nullable);
  **update the two prose strings** that currently say MB/CDDB ids are "computed
  consumer-side" (schema root description + the `toc` description) — those become
  the override's visible cost.
- examples/negatives, SPEC.md (cite this note + the algorithm), README contract
  section, `tests/test_discid.c` (the vector above + a lead-out-less → null case).

## Command surface / scope doctrine — clean

- **Zero new commands.** Rides the existing `ReadTableOfContents` convenience
  read (or the zero-command cached CDTOC). The one-raw-CDB count stays
  **one-of-five** (GESN + two tray opcodes + INQUIRY + READ FORMAT CAPACITIES).
- **No exclusive access, no lock lifetime, no nub collision.** Pure post-processing
  of bytes already in hand.
- **Layer 2 (no SPC ambition) untouched** — this is not a SCSI introspection
  surface; it is a pure function of an already-decoded optical primitive.
- **Privilege footprint (layer 3) unchanged.**

So the scope-doctrine command/privilege axes are *not* what this crosses.

## What it actually crosses (the override ledger)

1. **`ROADMAP.md:105`** files MusicBrainz / AccurateRip / dvdid / BDMV as
   derive-from-primitive, **consumer territory**. Surfacing the ID makes `mos`
   compute an external-ecosystem identifier, not report a spec primitive.
2. **The set-speed schema's own prose** says the disc/TOC subtree is the
   fingerprint and MB/CDDB ids are "computed consumer-side." This override
   rewrites that sentence.
3. **Camel's nose.** The freedb/CDDB id is the *same* inputs with a simpler hash
   (near-free); AccurateRip and dvdid are the next asks. The override should
   state where the line now sits — recommend: **MusicBrainz Disc ID only**, on
   the stated archival/robotic use case; freedb/AccurateRip/dvdid remain declined
   until a use case of their own appears. A future sibling is a fresh argument,
   not covered by admitting this one.
4. **Algorithm provenance is non-spec** (a MetaBrainz convention, not T10/MMC).
   Per the SPEC.md ADR it cites libdiscid + the Disc ID Calculation page and is
   built to the committed vector above — clean, but it is the first non-spec
   algorithm in the tree.

### Maintainer override rationale (recorded 2026-06-18)

> So easy and cheap that it outweighs the house rules: giving customers a solid
> fingerprint in the JSON means no forking on profile, no second read by another
> tool, easy DB lookups; a common case for ripping/archival workflows and robotic
> solutions.

These are sound and verified: `mos` already holds the TOC (no second read/tool);
the ID is the MB lookup key (easy lookups); a consumer reads one field that is
null for non-CD and present for CD (no profile fork); and ripping robots are
`mos`'s stated consumer. The override is therefore on the merits, not just
authority — but it **is** an override of (1) and (2) and belongs as a dated
AGENTS.md append before code lands.

## Hardware can falsify, never establish (per the hardware-role ADR)

The canonical vector establishes correctness for the common single-session case
offline. The items hardware can only *falsify* — each landing as a `.bin`/vector
fixture + dated note with a generic gate, never a per-device special-case:

- **CD-Extra / multisession**: confirm `mos`'s "disc lead-out = highest session"
  matches libdiscid's disc id for an enhanced CD (audio session 1 + data session 2).
  This is the one place the offset set could diverge; it is a fixture, not a
  redesign.
- A drive/bridge returning an incoherent TOC already fails closed upstream
  (`mos_internal_toc_parse` / `_cdtoc_to_toc`), so the disc-id function only ever
  sees a coherent, contiguous TOC or `NULL`.

## Recommendation

Feasible and cheap on every axis that matters for `mos`'s invariants (no command
surface, no privilege, no lock, fully offline-tested). Proceed **iff** the
maintainer accepts the doctrine override, scoped to **MusicBrainz Disc ID only**.
Sequence:

1. Land the set-speed TOC branch first (hard dependency).
2. Dated AGENTS.md append: override `ROADMAP.md:105` + the schema prose; fix the
   line at MB-only; cite this note.
3. Pure `mos_sha1.c` + `mos_discid.c` built to the canonical vector;
   `tests/test_discid.c` green against `mos_pure` (no hardware).
4. Accessor + metadata emit + schema/examples/negatives + README/SPEC in the
   commit that flips the "computed consumer-side" prose.
5. Multisession fixture as a follow-up falsification item.

## Decision (2026-06-18) — declined in-library, documented as a recipe

The maintainer's resolution: **do not** add the hashing to the library. The two
costs above (≈220 lines of net-new SHA-1 + base64, and the override of
`ROADMAP.md:105` plus the schema's "computed consumer-side" prose) both vanish if
the ID is computed *consumer-side*, which the feasibility work proved is trivial —
`mos` already emits the complete input. The better demonstration of mos's power is
showing how short that consumer recipe is, not absorbing a crypto primitive the
library otherwise has no use for.

So instead of a `mos_discid.c`, the README "Shell integration" section now carries
a verified `mos_discid()` shell function: `jq` over `disc.toc` builds libdiscid's
hash input (LBA + 150 framing, `%02X%02X` + 100×`%08X`), the system `sha1` +
URL-safe base64 finish it. It was run against the canonical reference disc here and
reproduces `49HHV7Eb8UKF3aQiNmu1GR8vKTY-` byte-for-byte. This **vindicates the
original survey conclusion** (third-party IDs are consumer territory) and keeps
`ROADMAP.md:105` and the fingerprint-subtree schema prose intact — no doctrine
override is needed, so none is logged. The line stays exactly where it was: `mos`
ships the primitive; the four-line recipe stays the consumer's.

## Sources

- libdiscid `src/disc.c` (`create_disc_id`) and `src/base64.c` (URL-safe alphabet
  `A–Za–z0–9._`, padding `-`).
- MusicBrainz, *Disc ID Calculation* (offset = LBA + 150; SHA-1 over
  `%02X%02X` + 100×`%08X`; worked example → `49HHV7Eb8UKF3aQiNmu1GR8vKTY-`).
- In-tree: `src/mos_cdtoc.c`, `src/mos_pure.h` (`mos_toc`), `include/mos.h`
  TOC accessors, `cli/metadata.c`, `schemas/mos.metadata.v1.json`
  (all on `claude/set-speed-feasibility-lty091`).
