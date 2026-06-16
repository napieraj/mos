# mac-optical-state

> Report what a macOS optical drive is actually doing — tray open,
> tray closed empty, loading, unit ready, busy — by querying the
> drive directly.

`drutil status` can't tell an **open** tray from one **closed on an empty
slot** — both print `No Media Inserted`, and nothing else in Apple's
optical tooling surfaces the difference. `mos` reads it from the drive
directly and distinguishes them, as a command-line tool and an embeddable
pure-C library. No runtime dependencies beyond Apple's own IOKit,
CoreFoundation, DiscRecording, and DiskArbitration frameworks; no
entitlements, no root. A state query costs at most two MMC commands —
TEST UNIT READY, plus a tray probe or a profile read; the decision tree
and sense tables are in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Contents

- [Quickstart](#quickstart)
- [Selecting a drive](#selecting-a-drive)
- [Commands](#commands)
- [Using the library](#using-the-library)
- [Shell integration](#shell-integration)
- [JSON output](#json-output)
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
Registry ID:  4295032831
        BSD:  /dev/disk4
      State:  ready
    Profile:  bd  bd_rom  (0x0040)
     Vendor:  HL-DT-ST
    Product:  BD-RE WH16NS60
   Revision:  1.00
```

Add `--json` for a machine-readable document:

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
  "vendor": "HL-DT-ST",
  "product": "BD-RE WH16NS60",
  "revision": "1.00"
}
```

`mos list` shows every attached drive; `mos watch` streams state changes
as they happen. Both are covered below.

## Selecting a drive

The drive is a positional argument after the subcommand, like
`diskutil info disk4`. Three forms:

- an **Index** from `mos list` — `mos state 1`;
- a **registry_id** from `mos`'s output (the `Registry ID:` row or the
  `registry_id` field) — the drive's attachment identity: never reused,
  and stable across disc swaps (an empty drive keeps it even with no
  `diskN`), so in a script it targets this exact drive or fails cleanly
  instead of landing on the wrong one;
- a **BSD form** — `disk4`, `rdisk4`, or `/dev/disk4`.

With one drive attached you can omit it (`mos state`). With several, an
omitted subject exits 64 and prints the drive table to stderr — `mos`
never guesses a first drive. A bare selector with no verb runs the
default `state` verb: `mos 2` or `mos disk4` reports that drive's state.
Bare `mos` with no arguments prints usage to stderr and exits 64.

Human and JSON output share every enum string verbatim (`ready`,
`bd_rom`), so a terminal report and a `jq` query never disagree.
Identity (`vendor` / `product` / `revision`) reads the same on every
surface too. The `bsd_node` field carries the full device node
(`/dev/disk4`), pasteable as a drive argument; it is `null` on an empty
drive.

## Commands

One-shot verbs emit a single document with `--json`; `watch` streams
NDJSON. The schemas under [`schemas/`](schemas/) are the authoritative
field reference for every document (see [JSON output](#json-output)).

### state (default)

An open tray resolves as exactly that — no media, no BSD node, no
guessing:

```
$ mos state 1
Registry ID:  4295032831
        BSD:  -
      State:  open
     Vendor:  HL-DT-ST
    Product:  BD-RE WH16NS60
   Revision:  1.00
```

### list

Every attached drive with its state — one probe per row; a failing drive
shows `error` in its row rather than killing the overview:

```
$ mos list
 Index  State  Volume            BSD         Vendor    Product         Revision
     1  ready  /Volumes/ARCHIVE  /dev/disk4  HL-DT-ST  BD-RE WH16NS60  1.00
     2  open   -                 -           PIONEER   BD-RW BDR-XS07  1.01
```

A mounted disc shows its mount path in the `Volume` column.
`mos list --json` (`mos.list.v1`) carries the filesystem label and mount
point as separate `volume_name` / `volume_path` keys, both `null` when
unmounted.

### watch

NDJSON unconditionally — one `mos.event.v1` per line, errors included.
With no drive given, `watch` streams the whole bus: every drive,
hot-plug arrivals as `device_appeared`, per-drive `device_removed` with
the stream continuing; zero drives attached is a valid empty stream that
waits. A drive selector narrows to one drive (and the stream ends on its
removal). An insert looks like:

```
$ mos watch
{"schema":"mos.event.v1","event":"snapshot",...,"state":"empty","prev_state":"unknown",...}
{"schema":"mos.event.v1","event":"state_changed",...,"state":"loading","prev_state":"empty",...}
{"schema":"mos.event.v1","event":"state_changed",...,"bsd_node":"/dev/disk4","state":"ready",...}
```

### metadata — disc identity

A finalized M-DISC Blu-ray — write-once archival media — mounts as an
ordinary data volume:

```
$ mos metadata 1
     BSD:  /dev/disk4
  Volume:  FAMILY_ARCHIVE_2026
    Path:  /Volumes/FAMILY_ARCHIVE_2026
 Profile:  bd  bd_r  (0x0041)
    Disc:  complete, 1 session, 1 track
   Media:  BDR  MILLEN/MR1 rev 0
     TOC:  tracks 1-1, lead-out LBA 11826176
   Track:  -
```

`mos metadata` answers *what disc is this*: profile and media class, the
TOC, and — for archival flows — the disc-completion state from READ DISC
INFORMATION, which reads disc structure rather than the filesystem.
`blank` = unburned; `appendable` = open session; `complete` plus a
mounted volume = burned and readable; `complete` with no volume = burned
but unmounted, possibly damaged. It also surfaces, when the medium has
them: the registered disc-maker identity for Blu-ray (manufacturer /
media-type ID — `MILLEN` / `MR1` for Millenniata M-DISC); the physical
format and copyright-management info for DVD/HD-DVD; per-track capacity
and append state; and album/track CD-TEXT for audio CDs.

`mos metadata --json` emits one `mos.metadata.v1` document. Its `disc`
object is a fingerprint subtree — a fixed, closed key set you can hash
for dedup; third-party IDs (MusicBrainz, AccurateRip, dvdid) derive from
these fields client-side. Unreadable facts emit `null`, and partial readability is
the normal regime, not an error — a present-but-unmounted disc reads a
`null` volume, which is the common case for BD/UHD video that doesn't
mount on macOS. Full field reference:
[`schemas/mos.metadata.v1.json`](schemas/mos.metadata.v1.json).

### drive — static facts

```
$ mos drive 1
        BSD:  /dev/disk4
Registry ID:  4295032831
     Vendor:  HL-DT-ST
    Product:  BD-RE WH16NS60
   Revision:  1.00
     Serial:  KL2G7942618WL
       AACS:  version 68, bus encryption yes
   Profiles:  cd_rom, cd_r, cd_rw, dvd_rom, dvd_minus_r, ..., bd_rom, bd_r, bd_re
  Standards:  spc_4 — mmc_6, sbc_3, sam_5, spc_4
     Speeds:  read 10560 kB/s, write 8310 kB/s (max)
       Mech:  tray, buffer 4096 KB
   ErrRecov:  retry 20, PER
```

Static facts that don't depend on the loaded disc: identity, AACS
capability (from one GET CONFIGURATION feature walk), max read/write
speeds, and the two read-only MODE SENSE pages — `mechanical` (loading
mechanism, eject/lock support, the live media-locked bit, buffer size)
and `error_recovery` (the drive's read error-recovery configuration).
The `serial` is the drive's Unit Serial Number (a raw INQUIRY of VPD
page 0x80 — the one identity field DiscRecording does not cache); it is
the durable inventory key that survives replug, where `registry_id` does
not. The raw read backs off on exclusive access, so `serial` is `-`/null
when the drive is busy or has a disc mounted, does not implement the
page, or has no serial programmed. The `profiles` array is the
supported-profile set from the GET CONFIGURATION Profile List feature (the
same walk that yields AACS) — the drive-static disc types this drive can
handle (CD/DVD/BD…), the modern BD-aware "what can this drive read/write"
that supersedes the legacy page-0x2A media bits; the per-disc Current bit
is omitted, so it reflects the drive, not the loaded medium. `version` and
`version_descriptors` come from a raw standard INQUIRY (EVPD=0): the SPC
compliance level and the T10/ISO standards the drive claims (MMC-6, SPC-4,
SBC-3…) — like `serial`, that raw read self-gates on exclusive access, so
both are null/`-` when the drive is busy or has a disc mounted. Read-only
throughout: `mos` reports these settings, it never changes them. Full
fields: [`schemas/mos.drive.v1.json`](schemas/mos.drive.v1.json).

### features — medium writability

`mos features --json` (`mos.features.v1`) is the raw MMC feature list,
one entry per GET CONFIGURATION descriptor with its `current` flag — the
writability answer for the loaded medium. A blank M-DISC is *ready for
archival* when `mos metadata` says the disc is blank **and** `mos
features` shows the matching write feature current (`0x0041` for BD-R).
Codes map against MMC-6 §5.3.

### tray — control

The one part of `mos` that **changes** drive state rather than reporting
it — the query path stays reporter-only.

```sh
$ mos tray eject 1              # eject (START STOP UNIT, LoEj)
$ mos tray eject 1 --force      # ALLOW (unlock) first, then eject
$ mos tray close 1
$ mos tray lock 1               # prevent removal until an unlock
$ mos tray lock 1 --persistent  # robot-grade: the operator eject button
                                # becomes an event, not a retraction
$ mos tray unlock 1             # release a lock (add --persistent to
                                # release a --persistent one — the two
                                # prevent states are independent)
```

`mos tray <action> --json` emits one `mos.tray.v1` document with an
`outcome`: `done`, `refused_locked` (an eject hit a lock — a reported
fact, not a failure; the process still exits 0), or `refused_other`
(carrying the drive's SCSI `sense` triple so a non-lock rejection is
diagnosable). `mos` issues the command and reports what happened; it
**does not unmount for you** — the deliberate contrast with `drutil tray
eject`'s unmount-then-eject policy. A lock targets a drive macOS has not
mounted; on a mounted disc the verb returns busy (`mos.error.v1`, exit
75). A lock **persists past the process** by design: the PREVENT state is
the drive's, cleared only by an explicit unlock, a bus reset, or
power-off — so a ripping robot can lock an idle drive and any later `mos
tray unlock` recovers it.

### capacity

`mos capacity --json` (`mos.capacity.v1`) reports the loaded disc's size,
and works even on a **mounted** disc — where a tool that issued a raw READ
CAPACITY would fail busy. A `recordable` sub-object adds the append-state
view (free blocks, next-writable point, track size) for blank/appendable
media. The two
halves are independently nullable: a pressed disc reports a byte size but
no append point; a blank recordable reports an append point but no
whole-disk size yet. Full fields:
[`schemas/mos.capacity.v1.json`](schemas/mos.capacity.v1.json).

## Using the library

`mos` is a pure-C library; the CLI is a thin client over it. The public
header `<mos.h>` depends only on `<stdint.h>`, `<stdbool.h>`, and
`<stddef.h>` — no Apple headers leak through it — so it compiles
anywhere; wrap calls in `#ifdef __APPLE__` in cross-platform code.

```c
#include <mos.h>
#include <stdio.h>

int main(void) {
    mos_error err;
    mos_handle_t *h = mos_open_by_index(1, &err);  /* 1-based, like drutil;
                                                      or by BSD name / registry_id */
    if (!h) {
        fprintf(stderr, "open: %s\n", mos_error_description(err));
        return 1;
    }

    const mos_state_result *r;
    if (mos_query_state(h, &r) == MOS_OK)
        printf("state: %s\n", mos_state_description(mos_state_result_state(r)));

    mos_close(h);
    return 0;
}
```

Results are opaque, handle-owned, and read through accessors
(`mos_state_result_*`), so the ABI stays stable as fields are appended.
The same shape extends to the typed queries — `mos_query_disc_info`,
`mos_query_toc`, `mos_query_drive_caps`, `mos_query_capacity`, and the
rest — and to `mos_watch_*` for the event stream. The full contract,
including ownership and lifetime rules, is documented in
[`include/mos.h`](include/mos.h).

Embed it in a CMake project:

```cmake
add_subdirectory(vendor/mos)
target_link_libraries(your_app PRIVATE mos_core)
```

Or vendor a single-file drop-in: `./scripts/amalgamate.sh` emits
`dist/mos.h` + `dist/mos.c`; compile `mos.c` and link IOKit,
CoreFoundation, DiscRecording, and DiskArbitration with
`-mmacosx-version-min=12.0`.

## Shell integration

The core pattern — act on every disc that turns readable, on any drive,
hot-plug included — is one pipeline. No polling, no `sleep`:

```sh
mos watch | jq --unbuffered -r '
    select(.event != "error" and .state == "ready")
    | "\(.bsd_node) \(.media_class // "unknown")"' |
while read -r dev class; do
    case "$class" in
        cd)     cdparanoia -B -d "$dev" ;;
        dvd|bd) makemkvcon ... ;;
    esac
done
```

One event per transition drives the loop: inserting a disc fires it
once, swapping discs fires `media_changed`, a drive plugged in mid-run
joins the stream live, and an ejected drive doesn't end it. For a
one-shot answer, `mos state <drive> --json` is the same contract in a
single document. Everything else composes from tools that already exist:
mount control is `diskutil mount` / `unmount`, CD audio is `cdparanoia`,
DVD/BD rip is `makemkvcon`, transcode is `HandBrakeCLI`.

## JSON output

Every `--json` document carries a `schema` field naming its type and
version — `mos.state.v1`, `mos.list.v1`, `mos.event.v1`,
`mos.metadata.v1`, `mos.drive.v1`, `mos.features.v1`, `mos.capacity.v1`,
`mos.tray.v1`, and `mos.error.v1` for failures. Consumers dispatch on
that field. The documents under [`schemas/`](schemas/) are the
authoritative field reference: JSON Schema draft 2020-12 with
`additionalProperties: false`, so a consumer never sees a key the schema
doesn't name. `watch` is the exception to `--json` — it streams NDJSON
(`mos.event.v1` per line) unconditionally.

## Install

### Homebrew (tap)

```sh
brew tap napieraj/tap
brew install --HEAD napieraj/tap/mos
```

HEAD-only until the first tagged release cuts a stable tarball; the
release flow is in [`CONTRIBUTING.md`](CONTRIBUTING.md).

### From source

```sh
git clone https://github.com/napieraj/mos
cd mos
make build      # release build of library + CLI (thin wrapper over cmake)
make test       # pure-data unit tests — no drive or hardware needed
./build/bin/mos list
```

Or drive CMake directly: `cmake -B build -DCMAKE_BUILD_TYPE=Release &&
cmake --build build`. To embed `mos` in your own project, see
[Using the library](#using-the-library).

## Requirements

- macOS 12 Monterey or later (arm64 or x86_64)
- CMake 3.20+ and a C11 compiler (Xcode Command Line Tools suffice)
- An optical drive whose Apple-supplied kext attaches
  `SCSITaskUserClient` — empirically, writer-class drives (CD-R,
  DVD±R/RW, BD-R/RE) do; pure read-only BD-ROM drives historically did
  not, and this has shifted across macOS releases. See Known Unknowns in
  [`ARCHITECTURE.md`](ARCHITECTURE.md).

## License

0BSD. Do whatever you want with it.

## See also

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — command-by-command spec
  references, decision tree, sense-code tables, Known Unknowns
- [`schemas/`](schemas/) — the JSON contract, one schema per document
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — code layout and conventions
- [`INTEGRATION_HARNESS.md`](INTEGRATION_HARNESS.md) — test matrix for
  contributing hardware fixtures

**Prior art:**

- libaacs `src/file/mmc_device_darwin.c` — live, compiled IOKit MMC
  backend with DiskArbitration coordination. Closest shipping reference
  for the pattern we use. (LGPL-2.1; study, don't copy.)
- cdrtools `libscg/scsi-mac-iokit.c` — older SCSITaskLib consumer;
  upstream is dormant since 2021 but the reference is correct.
- libcdio `lib/driver/osx.c` — includes the right headers and names, but
  the MMC code path is gated by `#ifdef GET_SCSI_FIXED` and is not active
  in shipping builds. Structural reference only.
- `DRDevice` / DiscRecording.framework — Apple's public alternative for
  tray-open queries. No SCSI traffic; smaller scope than `mos`. Kodi's
  Darwin storage provider acknowledges the same `drutil` papercut in
  `MediaManager.cpp`.
