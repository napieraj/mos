# Gaps surfaced building `examples/disc-ingest.sh` (2026-06-18)

Writing the disc-pile dispatcher (`examples/disc-ingest.sh`, PR #72) and the
community research behind it (auto-rippers, redump/preservation tooling,
copy-protection, audio secure-ripping) exercised the public surface as a real
consumer. This note records what a consumer reached for that `mos` does not
expose. **None of these blocked the example** — it works around each — so this
is a prioritised wishlist for a future session to accept or decline, not a bug
list. Per repo doctrine (AGENTS "surface first; build only after the explicit
override"), nothing here is built; each item names what it would cross.

## Tier 1 — small, in-scope, cheap

### 1. Derived `pre_emphasis` bool on audio TOC tracks
**Surfaced:** the audio secure-ripping survey (whipper/EAC/CUETools) — a
pre-emphasised CD must be flagged (or de-emphasised) or it rips bright; tools
that read the flag only from the TOC miss subchannel-only discs, but the TOC
bit is the cheap first signal.
**What:** `mos.metadata.v1` already carries the raw Q-channel `control` nibble
per track and *derives* `data` from it (control bit 2) — "carried explicitly so
consumers need no bit arithmetic" (schema). Pre-emphasis is control **bit 0**
on an audio track; deriving a `pre_emphasis` bool is the exact same pattern,
zero new commands, pure parse.
**Crosses:** nothing — additive derived field from data already parsed.
**Caveat to document:** TOC-flag pre-emphasis is incomplete (some discs flag
only in the R–W subchannel, which `mos` does not read), so it is best-effort,
like `data`. Worth a one-line schema note.
**Recommendation:** the cleanest pickup here. Pre-tag, mutable-in-place.

## Tier 2 — useful to the example, but cross a deliberate design line

### 2. Disc-completion (blank / appendable / complete) on the watch stream
**Surfaced:** the final PR review — `mos.event.v1` carries `state` and
`media_class`, but a blank recordable is also `state: ready` (the drive can
read it; it has no content), so the README watch loop had to add a per-disc
`mos metadata` call to gate blank media out of the rip branches.
**What would help:** a `disc_status` (or `writable`/`blank`) field on the
identity-carrying events so the gate can live in the `jq select`, stream-side,
with no second query.
**Crosses:** the explicit state-path-stays-cheap decision. `mos_state.c:58-60`:
*"Disc-completion (blank vs finalized) is not enriched here — no state decision
needs it; it ships as an on-demand typed query (mos_query_disc_info)."* Blank
vs written is **only** answerable by READ DISC INFORMATION (0x51) or READ TRACK
INFORMATION — neither is on the TUR⊕GESN poll path. Putting `disc_status` on
every event means issuing 0x51 on the polled hot path, which the architecture
avoids by design (the profile already distinguishes ROM from recordable; it
cannot distinguish blank from written).
**Recommendation:** keep it consumer-side (the per-disc `mos metadata` the
example uses) unless an opt-in "enriched watch" mode is wanted. If built, it is
a schema change to `mos.event.v1` (new field, ADR + drift-guard + fixtures) and
a poll-path cost decision — a maintainer call, not a free add.

### 3. A "volume mounted / settled" signal in the watch stream
**Surfaced:** the watch→mount race. `mos watch` fires `ready` the moment the
drive can read the disc, but DiskArbitration mounts the volume *asynchronously
after*, so `volume_path` is null on the first event and the example needs a
short settle-retry loop before the video-vs-data split.
**Crosses:** the DA-callback retirement ADR (AGENTS 2026-06-11: the watch's DA
wake legs were retired; the stream is drive-state, not mount-state), narrowed
by the 2026-06-12 re-admission of DA for **synchronous description reads only**.
A "volume settled" event would re-open the DA-callback modality that ADR
closed.
**Recommendation:** decline / document. The settle-loop is the consumer's job
and is consistent with the ADR. Noted here so the trade-off is on record.

## Tier 3 — deliberately out of scope (documented so they aren't re-litigated)

- **Drive serial in `mos.metadata.v1`.** The inventory row wants serial (durable
  key) + disc fingerprint from one read; serial lives in `mos drive` / events,
  not metadata, so the example does a second `mos drive` call. This is the
  deliberate disc-fact / drive-fact split — and serial self-gates on exclusive
  access, so it would read null exactly when a disc is mounted (when metadata is
  most useful). Not a gap.
- **Client-side disc fingerprint hash.** The example hashes the closed `disc`
  subtree itself. That is by design — the subtree is "the fingerprint subtree"
  and the hashing/match policy is explicitly the consumer's (JSON-schema ADR).
  `mos` emitting a canonical hash would take a position it deliberately leaves
  open. Not a gap.
- **`media_bytes` vs filesystem size.** `mos capacity` is a useful image-size
  oracle, but the preservation survey flags that drive-reported capacity can
  exceed the ISO9660 filesystem extent (runout/padding); the exact size comes
  from the PVD (Volume Space Size × block size), a filesystem read `mos` refuses
  (scope layer 3). **Worth a Known-Unknown line** so consumers don't treat
  `media_bytes` as the exact recorded-data length (the schema already warns this
  for overwritable media; generalise the note).
- **DVD/BD multisession layout.** `session_layout` is CD-only (from the
  kernel-cached `kIOCDMediaTOCKey`); there is no cached-full-TOC equivalent for
  DVD/BD, and multisession data discs can hide later sessions from a naive read.
  Platform-bounded, not a quick add.
- **Drive region / RPC-2 state (changes remaining).** `mos` surfaces the disc's
  region mask (`copyright.region`); the *drive's* current region and remaining
  changes live behind REPORT KEY, which scope doctrine excludes (no key reads).
  Deliberately out.
- **LibreDrive / UHD-raw / Cinavia.** MakeMKV-firmware and watermark concepts,
  not standard-MMC drive/disc facts — outside the command surface entirely.

## Summary

The one genuinely cheap, in-scope pickup is **#1 (`pre_emphasis` derived bool)**.
**#2 (disc_status on events)** is the most-wanted-by-a-consumer but is not free —
it costs a command on the polled path and a `mos.event.v1` schema change, so it
is a maintainer trade-off, not an obvious add. Everything in Tier 3 is working
as designed and is recorded here only so it isn't rediscovered as a "gap" next
time.
