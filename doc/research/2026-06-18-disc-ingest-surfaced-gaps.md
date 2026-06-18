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

## Tier 2 — watch-stream enrichments to weigh

### 2. Cheap watch enrichment: media `Type` + `writable` (zero MMC)
**Surfaced:** the final PR review — `mos.event.v1` carries `state` and
`media_class`, but a blank recordable is also `state: ready` (the drive can
read it; it has no content), so the README watch loop had to add a per-disc
`mos metadata` call to gate blank media out of the rip branches. The question
was whether that gate can be cheap. **It can** — the kernel publishes the
needed facts as registry properties, no MMC command, confirmed verbatim
against the IO{CD,DVD,BD}StorageFamily / IOStorageFamily sources:

| Node | Key (string) | Values | Command |
|------|--------------|--------|---------|
| IOCDMedia  | `kIOCDMediaTOCKey` ("TOC")     | full-TOC blob (mos already reads this) | none, **CD only** |
| IOCDMedia  | `kIOCDMediaTypeKey` ("Type")   | `CD-ROM` `CD-R` `CD-RW` | none |
| IODVDMedia | `kIODVDMediaTypeKey` ("Type")  | `DVD-ROM` `DVD-R` `DVD-RW` `DVD+R` `DVD+RW` `DVD-RAM` `HD DVD-ROM/-R/-RW/-RAM` | none |
| IOBDMedia  | `kIOBDMediaTypeKey` ("Type")   | `BD-ROM` `BD-R` `BD-RE` | none |
| IOMedia (all) | `kIOMediaWritableKey` ("Writable", OSBoolean) | true / false | none |
| IOMedia (all) | whole-disk node presence | already the event's `bsd_node` | none |

So a stream-side gate is achievable without 0x51, and — unlike the CD-only
cached TOC the maintainer flagged — these work for DVD/BD too:
- **media `Type`** → ROM vs recordable, the cross-class registry signal (every
  optical media class exposes a `"Type"` key);
- **`Writable`** → a recordable accepting writes (blank/appendable) vs a
  ROM/finalized disc;
- **`bsd_node: null`** → no whole-disk node = blank/unrecorded — mos's own
  schema already says `media_bytes`/`bsd_node` are null for "a blank/unrecorded
  recordable disc", so this proxy is *already in today's stream*.

mos already reaches optical-family media properties (it reads
`kIOCDMediaTOCKey` off IOCDMedia), so reading `kIO{CD,DVD,BD}MediaTypeKey` /
`kIOMediaWritableKey` is the same `IORegistryEntryCreateCFProperty` call —
proven feasible, zero new command.

**What still needs MMC:** only the precise `blank` / `appendable` / `complete`
tri-state. That bit (`blank:1` in IODVDTypes.h / IOBDTypes.h) lives in the READ
DISC INFORMATION reply struct, reachable only via the disc-info command — off
the TUR⊕GESN poll path by design (`mos_state.c:58`: completion "is not enriched
here — no state decision needs it"). So the *coarse* gate (recordable +
writable + has-content) is free; only the *exact* tri-state costs 0x51.

**Recommendation:** the cheapest win is to surface `media_type` (from the
`"Type"` key) and `writable` (kIOMediaWritableKey) on identity-carrying events
— zero MMC, all classes — so the README's blank gate can live in the `jq
select` with no second query, and `media_class` gains ROM-vs-recordable
resolution for free. (This corrects this note's first framing, which assumed
the gate necessarily cost an MMC command.) It is still additive to
`mos.event.v1` (new fields → ADR + the C↔schema drift guard + fixtures), and
the optical `Writable`/blank semantics want an `ioreg -c IOMedia` dump on real
media to confirm before building (hardware-falsification doctrine). Surfacing
the cached CD `TOC` on watch is a separate, CD-only cheap win on top.

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

Two cheap, in-scope pickups, both confirmed against Apple source: **#1
(`pre_emphasis` derived bool)** and **#2 (media `Type` + `writable` on watch
events)** — the latter is the consumer-requested blank gate, and it turns out
to be a *zero-MMC registry read* (the `"Type"` key exists on all three optical
media classes; the cached `TOC` is the CD-only bonus), not the MMC cost this
note first assumed. Both are `mos.event.v1` schema additions, so each needs an
ADR + the drift guard + fixtures, and the optical `Writable` semantics want a
real-media `ioreg` dump first — but neither costs a command. Only the precise
blank/appendable/complete tri-state still needs 0x51, and that stays off the
poll path by design. Everything in Tier 3 is working as designed and is
recorded so it isn't rediscovered as a "gap" next time.
