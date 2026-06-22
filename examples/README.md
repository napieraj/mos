# examples

Worked integrations that show what `mos` is for: one `mos` query tells you
what a disc **is**, and that is enough to hand it to the tool that already
owns the job. `mos` identifies; the example acts.

## `disc-ingest.sh`

A disc-pile dispatcher. A finalized M-DISC, an audio CD, a blank BD-R, a
movie DVD, and a data backup can come off the same spindle; one
`mos metadata --json` read per disc routes each to the right tool:

The file is laid out so the **delegation reads first** — config, then `main`
and the `ingest_*` branch dispatchers (the "what mos says → what tool runs"
logic), then the helper functions the branches call. Read top-down to see the
routing; drop into a helper when you care how a step works.

| Disc | How `mos` identifies it | Action |
|------|-------------------------|--------|
| **Audio CD** | `disc.class == "cd"` with audio tracks (`toc.tracks[].data == false`) | MusicBrainz Disc ID (computed from the TOC `mos` emits) → **release lookup**: artist / album / year / tracklist name the rip `Artist/Album (Year)/`, with `cover.jpg`, per-track `NN.tags`, `disc-ids.json` and the raw `musicbrainz.json` beside it. The match is **cross-checked** against the disc — every track's TOC length vs MusicBrainz's — and a mismatch drops a `NEEDS_REVIEW.txt` rather than mis-tagging. CD-TEXT (`disc.cdtext`) is the offline fallback name. The drive's **AccurateRip read offset** is logged with the TOC fingerprint; then a byte-perfect [`redumper`](https://github.com/superg/redumper) dump. Pre-emphasised tracks (`toc.tracks[].pre_emphasis`) are flagged |
| **Blank / appendable** | `disc.disc_info.status` is `blank`/`appendable` | report "ready to write" + the current write features (`mos features`); for a blank CD-R/RW, the **ATIP** pre-groove maker code + type (`disc.atip`) |
| **Video DVD/BD** | class `dvd`/`bd`, mounted with `VIDEO_TS`/`BDMV` **or** unmounted | `makemkvcon -r info` is parsed in full — every title's **duration, chapter count, size and output name** — the **main feature** (longest, or MakeMKV's `FPL_MainFeature` verdict) is identified, the **disc name becomes the output dir**, a `titles.tsv` manifest is written, then it decrypts + rips all titles. No titles → it's data, so it falls through to the archive branch. Dual-layer break (`physical.end_sector_l0`) and protection/region are surfaced |
| **Data / archive disc** (incl. M-DISC) | a mounted data volume with no video layout; M-DISC is the registered `MILLEN`/`MR1` manufacturer ID | error-tolerant 1:1 image with [`ddrescue`](https://www.gnu.org/software/ddrescue/), verified against `mos capacity` **and** the `disc_structure.physical` sector extent |

This is a **reference**, not a product: it shows what `mos`'s JSON unlocks with
as few knobs as possible, and it *reports and identifies* — it does not control
the drive (no tray/eject; that is `mos`'s separate control surface, demonstrated
on its own, not smeared across rip handlers).

Beyond routing, it uses `mos` as more than a classifier:

- a real **MusicBrainz release lookup** (`inc=artist-credits+recordings+release-groups+labels`,
  rate-limited to the service's 1 req/s) parsed into artist / album / year /
  tracklist — and a **length cross-check** that catches a fuzzy `toc=` match or
  the wrong disc of a multi-disc set before it mistags the rip;
- **every ecosystem's disc ID from one `mos` TOC read** — MusicBrainz, freedb/
  CDDB, and both AccurateRip ids (ARID1/ARID2), plus the AccurateRip
  results-DB URL — written to `disc-ids.json` beside the rip. No second tool
  touches the drive; this is the spotlight that mos's TOC *is* the identity
  primitive (the schemes differ only in framing — MusicBrainz/CDDB use LBA+150,
  AccurateRip uses raw LBAs);
- **cover art + portable tags** for the audio rip, both keyed on the release
  MBID mos's TOC resolved: the front cover from the **Cover Art Archive**
  (`cover.jpg`, release-group fallback) and a per-track `NN.tags` file in
  `metaflac --import-tags-from` format (the Vorbis-comment core + the
  `MUSICBRAINZ_*` id set Picard round-trips);
- a **`drive` report** (`./disc-ingest.sh drive <sel>`) — mos's drive identity
  plus the community-DB **lookup keys + URLs** (LibreDrive/MakeMKV, UHD
  crossflash, redump CD compat), honest that only AccurateRip's offset DB is
  machine-readable (auto-resolved) while the rest are forum/wiki you paste the
  key into; a coarse local table flags known UHD-capable models;
- **protection prediction** before a multi-minute scan: `predict_protection`
  compares the disc's `copyright.protection_name` (`mos metadata`) against the
  **drive's** `protection` capability (`mos drive`), so an AACS disc in a
  non-AACS drive (or CSS in a non-CSS drive) is **warned** up front (makemkvcon
  stays the authority — the prediction informs, never blocks). BD+ / UHD-AACS2.0
  stay honestly out of scope — mos predicts only to the standard-MMC line;
- a full **`makemkvcon -r info` parse** (the stable `apdefs.h` attribute ids:
  `2` name, `8` chapters, `9` duration, `11` bytes, `27` output name) into a
  ranked title table + a `titles.tsv` manifest — and the **main feature** honors
  MakeMKV's `FPL_MainFeature` verdict (its answer to fake-playlist obfuscation)
  over raw longest-duration; AACS/decrypt **failure messages** are surfaced
  verbatim from the info pass;
- an append-only **inventory** (JSONL) keyed by the drive's durable **serial**
  (`mos drive`) plus a **sha256 of mos's closed `disc` fingerprint subtree** —
  the one dedup key that works even for Blu-ray, where no standard disc ID
  exists. The row carries the resolved title (artist/album, or disc name +
  main title). `./disc-ingest.sh fingerprint <drive>` prints just that hash;
- a per-rip **provenance manifest** — `manifest.json` inside an audio/movie rip
  dir, or `<image>.mos.json` beside an archived `.iso` — carrying the
  `mos metadata` + drive identity + a sha256 of every output file;
- **content-protection / region** notes (CSS/CPRM/AACS) read from `mos`'s own
  fields, and **CD-Extra** detection from `disc.session_layout`;
- a **`mos watch`** loop that switches on each ready event's `media_type` — the
  rich profile axis (ROM vs write-once vs rewritable: `bd_rom`/`bd_r`/`bd_re`,
  not just `bd`) read **zero-MMC** off the kernel media node, alongside
  `writable` — for a first cut before any metadata read. A consumer wanting only
  pressed discs, or only blank recordables, can gate there with no second query;
- **pre-emphasis** flagging on audio CDs (`toc.tracks[].pre_emphasis`) so an
  emphasised track is preserved or de-emphasised, not ripped bright.

Two boundaries the script draws on purpose, because they are real limits of
what a drive can tell you:

- **Video vs. data on a DVD/BD** is not in any drive answer — it is in the
  filesystem, which `mos` refuses to read from sectors (scope doctrine). So
  `mos` names the *class* and the script peeks at the *mount* (`VIDEO_TS` /
  `BDMV`) to split video from data. That division — `mos` identifies, the
  consumer inspects the mounted filesystem — is the whole design in one line.
- **M-DISC detection is Blu-ray only.** `MILLEN`/`MR1` is the registered
  Millenniata Disc Manufacturer/Media ID, and it lives in the BD Disc
  Information structure. A DVD M-DISC reports no `disc_structure`, so it is
  archived as ordinary write-once data, not flagged as M-DISC. `mos` surfaces
  the registered ID bytes; the `MILLEN` → "M-DISC" reading is the consumer's.

### Running it

```sh
./disc-ingest.sh                 # follow `mos watch`: act on every disc that
                                 #   turns ready, hot-plug included
./disc-ingest.sh 1 disk6         # one-shot on the given drive selector(s)
./disc-ingest.sh identify 4      # full plan, read-only — no rip, no writes
./disc-ingest.sh drive 4         # drive identity + community-DB lookup keys
mos list --json | jq -r '.drives[].registry_id' | ./disc-ingest.sh -
DRY_RUN=1 ./disc-ingest.sh 1     # print the command each branch WOULD run
```

`identify` (and `DRY_RUN=1`) still run the **read-only** probes — the
MusicBrainz lookup and `makemkvcon -r info` — so the printed plan is the real
one (resolved title, title table, length cross-check); only the writes and the
rip itself are suppressed.

Set `MB_USER_AGENT` to a string with a **real contact** before hitting
MusicBrainz — the service rejects a bare `curl` user agent and rate-limits to
one request a second. The full config surface is just six env vars: `RIPS_DIR` /
`ARCHIVE_DIR` (output roots), `INVENTORY` (JSONL path; empty disables),
`MINLENGTH=120` (makemkvcon title filter, seconds), `MB_USER_AGENT`, and
`DRY_RUN=1`. Everything else (cover art, provenance manifests, disc-ids, the
length cross-check) is unconditional — a reference shouldn't make its best parts
opt-in.

Every rip branch writes a **provenance record**: a `manifest.json` inside an
audio/movie rip dir (mos metadata + drive identity + fingerprint + sha256 of
each output file), or `<image>.mos.json` beside an archived `.iso`. Audio rips
also get `disc-ids.json`, `tracklist.txt`, the raw `musicbrainz.json`,
`cover.jpg`, per-track `NN.tags`, and a `NEEDS_REVIEW.txt` when the length
cross-check fails.

### Dependencies

`jq` and `mos` are required; every action tool is optional and its branch is
skipped (with a note) if it is missing, so you only install what you use:
`openssl` + `curl` (MusicBrainz, cover art, AccurateRip), `redumper`,
`ddrescue`, `makemkvcon`. `shasum` / `diskutil` are built into macOS.

The script is 0BSD like `mos` — copy it, cut the branches you don't want,
make it yours.

### AccurateRip read offset

On the audio branch (and in `./disc-ingest.sh drive <sel>`) the script resolves
a drive's audio **read offset** — the per-drive sample correction that makes an
audio rip bit-identical across drives (AccurateRip/EAC/whipper). The offset is
**not a value any drive reports**: there is no MMC command, mode page, or IOKit
property for it. It lives in AccurateRip's community DB, keyed on the drive
**identity** `mos` *does* report. So `mos` ships the key; the script's
`accuraterip_offset` does the lookup — the consumer half of the boundary
`ROADMAP.md` draws ("AccurateRip … permanently consumer-side"). The source is the
DiscImageCreator TSV mirror (`VENDOR - MODEL <TAB> offset <TAB> …`), tab-delimited
so it parses with `awk` alone — no HTML scrape, no `python3`. It applies
AccurateRip's vendor renaming (`HL-DT-ST` → `LG Electronics`, `JLMS` → `Lite-ON`,
`Matshita` → `Panasonic`, `TSSTcorp` → `Toshiba Samsung`) and falls back to
`whipper offset find` when a drive is unlisted.
