# examples

Worked integrations that show what `mos` is for: one `mos` query tells you
what a disc **is**, and that is enough to hand it to the tool that already
owns the job. `mos` identifies; the example acts.

## `disc-ingest.sh`

A disc-pile dispatcher. A finalized M-DISC, an audio CD, a blank BD-R, a
movie DVD, and a data backup can come off the same spindle; one
`mos metadata --json` read per disc routes each to the right tool:

| Disc | How `mos` identifies it | Action |
|------|-------------------------|--------|
| **Audio CD** | `disc.class == "cd"` with audio tracks (`toc.tracks[].data == false`) | MusicBrainz Disc ID (computed from the TOC `mos` emits) → release lookup, then a byte-perfect [`redumper`](https://github.com/superg/redumper) dump |
| **Blank / appendable** | `disc.disc_info.status` is `blank`/`appendable` | report "ready to write" + the current write features (`mos features`) |
| **Video DVD/BD** | class `dvd`/`bd`, mounted with `VIDEO_TS`/`BDMV` **or** unmounted | `makemkvcon -r` (robot mode) confirms it has rippable titles and reads the disc name, which names the output dir; then it decrypts + rips. No titles → it's data, not a movie, so it falls through to the archive branch |
| **Sealed / data disc** (incl. M-DISC) | a mounted data volume with no video layout; M-DISC is the registered `MILLEN`/`MR1` maker ID | error-tolerant 1:1 image with [`ddrescue`](https://www.gnu.org/software/ddrescue/), verified against `mos capacity` |

Beyond routing, it uses `mos` as more than a classifier:

- an append-only **inventory** (JSONL) keyed by the drive's durable **serial**
  (`mos drive`) plus a **sha256 of mos's closed `disc` fingerprint subtree** —
  the one dedup key that works even for Blu-ray, where no standard disc ID
  exists. `./disc-ingest.sh fingerprint <drive>` prints just that hash;
- a per-archive **sidecar manifest** (`<image>.mos.json` + `.sha256`) — the
  `mos metadata` + drive identity + image checksum an archivist keeps beside
  the `.iso`;
- an optional **`mos tray` lifecycle** — lock an idle drive during a rip so a
  stray operator eject becomes a reported event, not a retraction mid-read;
  eject when done (the "swap me" signal). No FOSS ripper does PREVENT-locking;
- **content-protection / region** notes (CSS/CPRM/AACS) read from `mos`'s own
  fields, and **CD-Extra** detection from `disc.session_layout`.

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
mos list --json | jq -r '.drives[].registry_id' | ./disc-ingest.sh -
DRY_RUN=1 ./disc-ingest.sh 1     # print the command each branch WOULD run
```

Set `MB_USER_AGENT` to a string with a **real contact** before hitting
MusicBrainz — the service rejects a bare `curl` user agent and rate-limits to
one request a second. Other config (environment overrides): `RIPS_DIR` /
`ARCHIVE_DIR` (output roots), `INVENTORY` (JSONL path; empty disables),
`SIDECAR=1`, `EJECT_WHEN_DONE=0`, `LOCK_DURING_RIP=0`, `MINLENGTH=120`
(makemkvcon title filter, seconds).

### Dependencies

`jq` and `mos` are required; every action tool is optional and its branch is
skipped (with a note) if it is missing, so you only install what you use:
`openssl`/`curl` (MusicBrainz), `redumper`, `ddrescue`, `makemkvcon`,
`HandBrakeCLI` (transcode, if you add a branch). `diskutil` is built into
macOS.

The script is 0BSD like `mos` — copy it, cut the branches you don't want,
make it yours.
