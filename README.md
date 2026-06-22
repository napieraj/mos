# mac-optical-state

[![GitHub release](https://img.shields.io/github/v/release/napieraj/mos?label=release)](https://github.com/napieraj/mos/releases)
[![CI](https://github.com/napieraj/mos/actions/workflows/ci.yml/badge.svg)](https://github.com/napieraj/mos/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/napieraj/mos)](https://github.com/napieraj/mos/blob/main/LICENSE)

> Report what a macOS optical drive is actually doing — tray open, tray
> closed empty, loading, unit ready, busy — by querying the drive directly.

`drutil status` can't tell an **open** tray from one **closed on an empty
slot** — both print `No Media Inserted`. `mos` reads the drive directly and
distinguishes them, as a command-line tool and an embeddable pure-C library.
No entitlements, no root, no dependencies beyond Apple's own frameworks. The
decision tree and sense tables are in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Contents

- [Quickstart](#quickstart)
- [Selecting a drive](#selecting-a-drive)
- [Commands](#commands)
- [JSON output](#json-output)
- [Library](#library)
- [Shell integration](#shell-integration)
- [Install](#install)
- [Requirements](#requirements)

## Quickstart

```sh
brew tap napieraj/tap
brew install --HEAD napieraj/tap/mos     # HEAD-only until the first tagged release
mos state
```

```
$ mos state 1
        BSD:  /dev/disk4
Registry ID:  4295032831
      State:  ready
      Media:  bd — bd_rom
   Writable:  no
     Vendor:  HL-DT-ST
    Product:  BD-RE WH16NS60
   Firmware:  1.00
```

Add `--json` to any verb for a machine-readable document:

```
$ mos state 1 --json
{
  "schema": "mos.state.v1",
  "state": "ready",
  "bsd_node": "/dev/disk4",
  "registry_id": 4295032831,
  "index": 1,
  "current_profile": "0x0040",
  "current_profile_name": "bd_rom",
  "media_class": "bd",
  "media_type": "bd_rom",
  "writable": false,
  "speeds": {"speed_count": 1, "max_read_kbps": 35980, "max_write_kbps": 0},
  "vendor": "HL-DT-ST",
  "product": "BD-RE WH16NS60",
  "revision": "1.00"
}
```

## Selecting a drive

The drive is a positional argument after the verb, like `diskutil info disk4`.
Three forms:

- an **Index** from `mos list` — `mos state 1`;
- a **registry_id** — the drive's attachment identity (never reused, stable
  across disc swaps, kept even when the tray is empty) — `mos state 4295032831`;
- a **BSD form** — `disk4`, `rdisk4`, or `/dev/disk4`.

With one drive attached, omit it (`mos state`). With several, an omitted subject
exits 64 and prints the drive table to stderr — `mos` never guesses. A bare
selector with no verb runs `state`: `mos 2`, `mos disk4`. Human and JSON output
share every enum string verbatim, so a terminal report and a `jq` query never
disagree.

## Commands

Every one-shot verb takes `--json`; `watch` streams NDJSON. The per-document
field reference is its schema under [`schemas/`](schemas/).

### `state` (default) — what the drive is doing now

States: `open`, `empty`, `empty_or_open`, `loading`, `ready`, `busy`,
`formatting`, `device_fault`, `media_unreadable`, `unknown` (the full
`mos.state.v1` enum; the first six cover everyday use). An **open** tray
resolves as `open` with no media and no BSD node — the case `drutil` can't
distinguish from a closed empty slot. A loaded disc adds the `Media:` and
`Writable:` rows shown in the [Quickstart](#quickstart); an empty tray has
neither. In `--json`, `media_type` and `writable` are read with zero SCSI
commands and match the [`watch`](#watch) stream, so a poll and the stream agree.
On a READY disc the document also carries a `speeds` object — the loaded disc's
max read/write (kB/s). Reading it never locks the drive or interrupts another
program using it, so it is safe to poll. `watch` reads it once per disc (the
value is fixed for a given disc), so a polling loop doesn't re-query it.

### `list` — every attached drive

```
$ mos list
 Index  State  Volume            BSD         Vendor    Product         Firmware
     1  ready  /Volumes/ARCHIVE  /dev/disk4  HL-DT-ST  BD-RE WH16NS60  1.00
     2  open   -                 -           PIONEER   BD-RW BDR-XS07  1.01
```

One probe per row; a failing drive shows `error` in its row rather than killing
the overview. A mounted disc shows its mount path in `Volume`.

### `watch` — stream state changes (NDJSON)

```
$ mos watch
{"schema":"mos.event.v1","event":"snapshot",...,"state":"empty",...}
{"schema":"mos.event.v1","event":"state_changed",...,"state":"loading",...}
{"schema":"mos.event.v1","event":"state_changed",...,"state":"ready","serial":"KL2G7942618WL",...}
```

One `mos.event.v1` per line, errors included. With no drive it streams the whole
bus — hot-plug arrivals as `device_appeared`, removals as `device_removed`, the
stream continuing; a selector narrows to one drive. Events carry `media_type`,
`writable`, and the durable `serial`, so a consumer can gate inserts without a
second query.

### `metadata` — what disc is this

```
$ mos metadata 1
     BSD:  /dev/disk4
  Volume:  FAMILY_ARCHIVE_2026
    Path:  /Volumes/FAMILY_ARCHIVE_2026
 Profile:  bd — bd_r
    Disc:  complete, 1 session, 1 track
   Media:  BDR  MILLEN/MR1 rev 0
     TOC:  tracks 1-1, lead-out LBA 11826176
   Track:  -
```

Profile and media class, completion state (`blank` / `appendable` / `complete`),
TOC, the registered disc-maker identity (e.g. `MILLEN`/`MR1` for Millenniata
M-DISC; for CD-R/RW the raw `atip` pre-groove identity — lead-in M:S:F, disc
type, capacity), CD-TEXT, and a CD `session_layout`. In `--json`, the `disc`
object is a closed, hashable fingerprint subtree — third-party IDs (MusicBrainz,
AccurateRip, dvdid) derive from it client-side (see
[Shell integration](#shell-integration)).

### `drive` — static drive facts

```
$ mos drive 1
           BSD:  /dev/disk4
   Registry ID:  4295032831
        Vendor:  HL-DT-ST
       Product:  BD-RE WH16NS60
      Firmware:  1.00 (2019-01-07T13:20:43Z)
        Serial:  KL2G7942618WL
    Protection:  AACS (v68, bus encryption), CSS (v1)
      Profiles:  cd_rom, cd_r, cd_rw, dvd_rom, dvd_minus_r, ..., bd_rom, bd_r, bd_re
     Standards:  spc_4 — mmc_6, sbc_3, sam_5, spc_4
    Mechanical:  tray, buffer 4.1 MB
Error Recovery:  retry 20, PER
```

Disc-independent facts: identity, `serial` (the durable inventory key that
survives replug where `registry_id` does not), content-protection *capability*,
`write_protect` *capability* (the drive's Write Protect Feature 0004h bits —
what it can report/change, not per-disc
state), the supported-profile set, and the mechanical and error-recovery
configuration.
Read-only — `mos` reports these, never changes them. (Read/write **speeds** are
media-dependent, so they live on [`state`](#state-default--what-the-drive-is-doing-now)
and [`watch`](#watch), not here.)

### `features` — medium writability

```sh
mos features 1 --json     # mos.features.v1 — one entry per GET CONFIGURATION descriptor
```

The writability answer for the loaded medium, with each feature's `current`
flag. A blank M-DISC is ready for archival when `metadata` reports `blank` and
the matching write feature is current.

### `capacity` — disc size and append/format views

```sh
mos capacity 1 --json     # mos.capacity.v1
```

Byte size (works even on a mounted disc), a `recordable` view (append point,
free blocks) for blank/appendable media, and a `formattable` view for blank
rewritables. The three are independently nullable for the disc you have.

### `tray` — eject / close / lock

The one verb that **changes** drive state; every query path stays reporter-only.

```sh
mos tray eject 1                 # graceful unmount (if mounted) + eject
mos tray eject disk4 --force     # also clear a COLD Prevent LOCK (deliberately
                                 #   locked, unmounted); never forces the filesystem
mos tray close 1
mos tray lock 1                  # removal lock on an idle drive (blocks the
                                 #   front-panel eject button); mounted disc →
                                 #   already_locked (macOS already locked it)
mos tray unlock 1                # clear the lock (both Prevent states)
```

`--json` emits `mos.tray.v1` with an `outcome`: `done`, `refused_locked` (an
eject hit a lock — a reported fact, still exit 0), `refused_other` (carries the
SCSI `sense` triple), or `already_locked` (see `lock` below). A plain eject
**gracefully unmounts** a mounted disc first (exactly like `diskutil unmountDisk
diskN` / `drutil eject`) and then ejects; if the filesystem is **busy** (open
file handles) the unmount fails and the eject returns busy (exit 75) — `mos`
**never** forces the filesystem, so it never kills your open files. (Close the
files, or `diskutil unmountDisk` first.) macOS arms a tray Prevent lock when it
mounts a disc and that lock survives the unmount, so a plain eject also **clears
that OS mount-lock** after its own unmount — ejecting a mounted disc Just Works,
no `--force` needed.

`--force` (eject only) extends the lock-clearing to a **COLD** lock — a
deliberately-locked idle drive (a robot's `mos tray lock`) with no mount in
play, which a plain eject reports as `refused_locked`. It clears both Prevent
states so the locked tray opens. It does **not** touch
the filesystem — a busy disc still reports busy under `--force`. The one blocker
neither path can clear is another program holding the drive exclusively, which
surfaces as a `mos.error.v1` error with code `exclusive_access` — not a tray
`outcome` (the outcome enum is `done` / `refused_locked` / `refused_other` /
`already_locked`).

`tray lock` sets the **basic** Prevent state — the hard removal block that
refuses the front-panel eject button. `tray unlock` clears **both** Prevent
states so the tray ends unlocked. A lock **persists past the process** — it
survives a handle close, clearing only on a bus reset or power-cycle — so any
later `mos tray unlock` recovers it. Lock/unlock act on **idle or unmounted**
drives: on a
**mounted** disc the PREVENT command can't take exclusive access, but the disc
is already removal-locked by macOS — so `lock` reports `already_locked` (a
success), and `unlock` reports busy with a hint to `tray eject` (which releases
it). Lock several idle drives with a loop over `mos list` (one `outcome` and
exit code per drive):

```sh
for id in $(mos list --json | jq '.drives[].registry_id'); do
    mos tray lock "$id"
done
```

## JSON output

Every `--json` document carries a `schema` field naming its type and version —
`mos.state.v1`, `mos.list.v1`, `mos.event.v1`, `mos.metadata.v1`, `mos.drive.v1`,
`mos.features.v1`, `mos.capacity.v1`, `mos.tray.v1`, and `mos.error.v1` for
failures. Consumers dispatch on it. The documents under [`schemas/`](schemas/)
are the authoritative field reference: JSON Schema draft 2020-12 with
`additionalProperties: false`, so you never see a key the schema doesn't name.
`watch` streams `mos.event.v1` NDJSON unconditionally.

## Library

`mos` is a pure-C library; the CLI is a thin client over it. `<mos.h>` depends
only on `<stdint.h>`, `<stdbool.h>`, and `<stddef.h>` — no Apple headers leak
through it.

```c
#include <mos.h>
#include <stdio.h>

int main(void) {
    mos_error err;
    mos_handle_t *h = mos_open_by_index(1, &err);   /* or by BSD name / registry_id */
    if (!h) { fprintf(stderr, "open: %s\n", mos_error_description(err)); return 1; }

    const mos_state_result *r;
    if (mos_query_state(h, &r) == MOS_OK)
        printf("state: %s\n", mos_state_description(mos_state_result_state(r)));

    mos_close(h);
    return 0;
}
```

Results are opaque and handle-owned, read through accessors, so the ABI stays
stable as fields are appended. Embed via CMake:

```cmake
add_subdirectory(vendor/mos)
target_link_libraries(your_app PRIVATE mos_core)
```

Or vendor a single-file drop-in with `./scripts/amalgamate.sh` (emits
`dist/mos.h` + `dist/mos.c`). Full contract in [`include/mos.h`](include/mos.h).

## Shell integration

`mos` says what a disc *is*; existing tools do the work. Act on every disc that
turns readable, on any drive, hot-plug included, in one pipeline — no polling, no
`sleep`:

```sh
mos watch | jq --unbuffered -r '
    select(.event != "error" and .state == "ready")
    | "\(.bsd_node) \(.media_class // "unknown")"' |
while read -r dev class; do
    case "$class" in
        cd)     # audio CD — compute a MusicBrainz Disc ID from disc.toc, then rip
                ;;
        dvd|bd) makemkvcon mkv "dev:${dev/disk/rdisk}" all "$HOME/Rips" ;;
    esac
done
```

The full dispatcher — [`examples/disc-ingest.sh`](examples/disc-ingest.sh) —
routes a disc pile from one `mos metadata` read per disc: MusicBrainz lookup, the
drive's AccurateRip read offset, and byte-perfect `redumper` dumps for audio CDs;
error-tolerant `ddrescue` imaging for M-DISC and data archives (sized against
`mos capacity`); `makemkvcon -r` robot mode for video; and a "ready to write"
report for blank recordables. See [`examples/`](examples/).

## Install

### Homebrew (tap)

```sh
brew tap napieraj/tap
brew install --HEAD napieraj/tap/mos
```

HEAD-only until the first tagged release; the release flow is in
[`CONTRIBUTING.md`](CONTRIBUTING.md).

### From source

```sh
git clone https://github.com/napieraj/mos
cd mos
make build      # release build of library + CLI
make test       # pure-data unit tests — no drive or hardware needed
./build/bin/mos list
```

`cmake --install` (and the Homebrew formula) also install a `mos(1)` man page and
bash/zsh/fish completions; raw files live in [`completions/`](completions) and
[`man/mos.1`](man/mos.1).

The zsh completion completes verb names statically with no extra dependencies.
Install [`jq`](https://jqlang.org) for full autocompletion: with `jq` on PATH,
`mos <TAB>` and `mos --bsd <TAB>` also offer the attached drives by BSD node
with their current state and identity (e.g. `/dev/disk4:ready LG BD-RE WH16NS60`).

## Requirements

- macOS 12 Monterey or later (arm64 or x86_64)
- CMake 3.20+ and a C11 compiler (Xcode Command Line Tools suffice)
- An optical drive whose Apple-supplied kext attaches `SCSITaskUserClient` —
  empirically, writer-class drives (CD-R, DVD±R/RW, BD-R/RE) do; pure read-only
  BD-ROM drives historically did not, and this has shifted across macOS releases.
  See Known Unknowns in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## License

0BSD. Do whatever you want with it.

## See also

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — decision tree, sense-code tables, spec
  references, Known Unknowns
- [`schemas/`](schemas/) — the JSON contract, one schema per document
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — code layout and conventions
- [`examples/`](examples/) — worked consumer integrations

**Prior art** (fuller reference list in [`ARCHITECTURE.md`](ARCHITECTURE.md) §11):

- libaacs `src/file/mmc_device_darwin.c` — live, compiled IOKit MMC backend with
  DiskArbitration coordination; closest shipping reference for the pattern
  (LGPL-2.1).
- cdrtools `libscg/scsi-mac-iokit.c` — older SCSITaskLib consumer; dormant since
  2021 but correct.
- libcdio `lib/driver/osx.c` — the IOKit platform driver; right headers and
  names, but the MMC path is gated behind `#ifdef GET_SCSI_FIXED`; structural
  reference only.
- libcdio `lib/driver/mmc/` (and `src/cd-info.c`) — the cross-platform MMC
  command layer mos's metadata side mirrors: READ DISC INFORMATION, GET
  CONFIGURATION profiles, and media-type detection (CD / DVD±R / DVD±RW /
  DVD-RAM / BD) — the disc facts `mos metadata` and `mos drive` decode.
