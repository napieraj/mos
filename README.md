# mac-optical-state

> Report what a macOS optical drive is actually doing — tray open,
> tray closed empty, loading, unit ready, busy — by querying the
> drive directly.

`drutil status` cannot tell a tray that is **open** from one
**closed on an empty slot** — both print the same prose. `mos` asks
the drive and distinguishes them, as a CLI and an embeddable pure-C
library.

No runtime dependencies beyond Apple's own IOKit, CoreFoundation,
DiscRecording, and DiskArbitration frameworks; no entitlements, no
root. A status
query costs at most three MMC commands — the command-by-command
rationale, decision tree, and sense tables are in `ARCHITECTURE.md`.
Disc details beyond state (blank / appendable / finalized, session
and track counts) are available on demand via `mos_query_disc_info()`.

## Usage

```
mos [subcommand [drive]] [options]
```

The drive subject is positional after a subcommand, like
`diskutil info disk4`: an Index from `mos list`, a `registry_id`
(pasteable from any JSON output — the two digit forms cannot collide,
xnu starts registry IDs above 2^32), or a BSD form (`disk4`,
`rdisk4`, `/dev/disk4`). With one drive attached it may be omitted
(`mos status`); with several, `mos status` without a subject exits 64
and prints the drive table to stderr — no first-drive guessing. Bare
`mos` prints a hint and the usage text to stderr and exits 64.

Human views and JSON share every enum string verbatim
(`ready`, `bd_rom`) — a terminal report and a jq query can
never disagree. Identity is the same three fields on every surface
(`vendor` / `product` / `revision`), captured from the platform's
device directory once at device attach; when verifying a firmware
flash, confirm `registry_id` changed too, or you're reading the
pre-flash cache. The `bsd` field carries the full device node
(`/dev/disk4`) in both surfaces: pasteable, pipeable, and always a
valid drive argument when non-null (an empty drive has none).

### Status (default)

```
$ mos status 1
Registry:  4295032831
     BSD:  /dev/disk4
   State:  ready
 Profile:  bd  bd_rom  (0x0040)
  Vendor:  HL-DT-ST
 Product:  BD-RE WH16NS60
     Rev:  1.00
```

```
$ mos status 1 --json
{
  "schema": "mos.state.v1",
  "state": "ready",
  "bsd": "/dev/disk4",
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

An open tray is resolved as exactly that — no media, no BSD node,
no guessing:

```
$ mos status 1
Registry:  4295032831
     BSD:  -
   State:  open
  Vendor:  HL-DT-ST
 Product:  BD-RE WH16NS60
     Rev:  1.00
```

### List

Every attached drive with its state (each row costs one probe; a
failing drive shows `error` in its row rather than killing the
overview):

```
$ mos list
 Index  State  Volume  BSD         Vendor    Product         Rev
     1  ready  -       /dev/disk4  HL-DT-ST  BD-RE WH16NS60  1.00
     2  open   -       -           PIONEER   BD-RW BDR-XS07  1.01
```

`mos list --json` emits `mos.list.v1` with the same fields per entry.

### Watch

NDJSON unconditionally — one `mos.event.v1` per line, errors included;
`--json` is a no-op here. With no drive given, `watch` streams the
whole bus: every drive, hot-plug arrivals as `device_appeared`,
per-drive `device_removed` with the stream continuing — zero drives
attached is a valid empty stream that waits. A drive selector narrows
to one drive (and the stream then ends on its removal). An insert
looks like:

```
$ mos watch
{"schema":"mos.event.v1","event":"snapshot",...,"state":"empty","prev_state":"unknown",...}
{"schema":"mos.event.v1","event":"state_changed",...,"state":"loading","prev_state":"empty",...}
{"schema":"mos.event.v1","event":"state_changed",...,"bsd":"/dev/disk4","state":"ready",...}
```

### Metadata (disc identity)

```
$ mos metadata 1            # pressed DVD video — these genuinely mount
     BSD:  /dev/disk4
  Volume:  ARRIVAL
    Path:  /Volumes/ARRIVAL
 Profile:  dvd  dvd_rom  (0x0010)
    Disc:  -
     TOC:  tracks 1-1, lead-out LBA 3824640
```

`mos metadata --json` emits one `mos.metadata.v1` document. Its `disc`
object is the fingerprint subtree: a fixed, closed key set
(profile/class/TOC/disc-info/volume-name, each required and nullable)
whose canonical serialization consumers hash for dedup — third-party
ids (MusicBrainz, AccurateRip, dvdid) are pure functions of these
fields, computed consumer-side. Unreadable facts emit `null`; partial
readability is the normal regime, not an error.

For archival flows (M-DISC and friends) the `disc_info` row answers
fresh-vs-burned directly, because READ DISC INFORMATION reads disc
structure, not the filesystem: `blank` = unburned, ready to write;
`appendable` = open session; `complete` plus a mounted Volume =
burned and readable; `complete` with Volume null = burned but the
filesystem did not mount — possibly damaged (when even the TOC won't
read, `mos status` reports `media_unreadable`). `erasable`
distinguishes wipe-and-reuse media (RW/RE) from one-shot (R/M-DISC). The volume name/path
are mount-sourced only (one synchronous DiskArbitration description
read): a present-but-unmounted disc reads `null` by design — sector
reads are the consumer's privilege and parsing burden. That null is
the NORMAL reading for BD/UHD video: those discs typically do not
mount on macOS (the rip workload's common case), so their identity
rides on profile/TOC/disc-info while `volume_name` carries
DVD-video, audio-CD ("Audio CD"), and data/archival labels — a
finalized M-DISC backup mounts like any data volume and is the
fixture-pinned archival example (`mos.metadata.v1.mdisc_archive`).

For Blu-ray media `disc.disc_structure` adds the disc's REGISTERED
identity from READ DISC STRUCTURE — disc type (`BDR`/`BDW`/`BDO`),
manufacturer ID, media-type ID, revision. This names the actual disc
maker (`CMCMAG`, `VERBAT`, `RITEK`…) and, for Millenniata M-DISC,
reports manufacturer `MILLEN` / media type `MR1`. mos surfaces the
registered bytes faithfully; classifying `MILLEN` as M-DISC is the
consumer's call (same division as the third-party ids). These BD
identity fields are null on CD/DVD/HD-DVD, where the DI structure does
not exist.

For the DVD/HD-DVD family, `disc.disc_structure` instead carries two
sibling objects from READ DISC STRUCTURE media-type 0 (null on BD/CD):
`physical` — the Physical Format Information (book type + token name,
disc size, rate, layer layout, densities, BCA, and the data-area sector
boundaries; `end_sector_l0` is the layer break on dual-layer OTP media)
— and `copyright` — the Copyright Management Information (protection
system type + token, region mask). Offsets follow the canonical kernel
wire parse (`drivers/cdrom/cdrom.c`); the numeric fields are raw spec
codes with `*_name` token mappings, and the copyright bytes are surfaced
faithfully without classifying region semantics or reading any keys
(scope doctrine). The structure is named `physical`, not `dvd`, because
the same media-type-0 reply carries HD-DVD book types.

### Drive (static facts)

```
$ mos drive 1
     BSD:  /dev/disk4
Registry:  4295032831
  Vendor:  HL-DT-ST
 Product:  BD-RE WH16NS60
     Rev:  1.00
  Serial:  -
    AACS:  version 68, bus encryption yes
```

`mos drive --json` emits `mos.drive.v1`: identity (open-time directory
data, zero commands), `serial` (null in stage 1 — pending the VPD-0x80
convenience-reachability check on hardware), and spec-grounded AACS
capabilities from one GET CONFIGURATION feature walk. `bus_encryption`
is the drive-reported support bit; the cryptographically signed BEC
flag lives in the AACS drive certificate, which mos does not fetch.
There is deliberately no `libredrive` field: that status is a MakeMKV
database property, not a drive property.

### Features (medium writability)

`mos features --json` emits `mos.features.v1`: the raw MMC feature
list, one entry per GET CONFIGURATION descriptor — code, `current`,
`persistent`, version. The `current` flags are the writability
answer for the mounted medium: a blank M-DISC is *ready for archival*
when `mos metadata` says the disc is blank **and** `mos features`
shows the matching write feature current (0x0041 for BD-R). Codes map
against MMC-6 §5.3; mos ships no name table.

### Shell integration

The core pattern — act on every disc that turns readable, on any
drive, hot-plug included — is one pipeline. No polling, no `sleep`:

```sh
mos watch | jq --unbuffered -r '
    select(.event != "error" and .state == "ready")
    | "\(.bsd) \(.media_class // "unknown")"' |
while read -r dev class; do
    case "$class" in
        cd)     cdparanoia -B -d "$dev" ;;
        dvd|bd) makemkvcon ... ;;
    esac
done
```

One event per transition drives the loop: inserting a disc fires it
once, swapping discs fires `media_changed`, a drive plugged in
mid-run joins the stream live, and an ejected drive doesn't end it.
For a one-shot answer, `mos status <drive> --json` is the same
contract in a single document. Everything else composes from
tools that already exist: tray control is `drutil tray eject` /
`drutil tray close`, mount control is `diskutil mount` / `diskutil
unmount`, CD audio is `cdparanoia`, DVD/BD rip is `makemkvcon`,
transcode is `HandBrakeCLI`. Keep your integration shallow; the power
is in composition.

## Install

### Homebrew (tap)

```sh
brew tap napieraj/tap
brew install --HEAD napieraj/tap/mos
```

HEAD-only until the first tagged release cuts a stable tarball; the
release flow is documented in `CONTRIBUTING.md`.

### From source

```sh
git clone https://github.com/napieraj/mos
cd mos
make build      # release build of library + CLI (thin wrapper over cmake)
make test       # pure-data unit tests — no drive or hardware needed
./build/bin/mos list
```

Or drive CMake directly: `cmake -B build -DCMAKE_BUILD_TYPE=Release &&
cmake --build build`.

### Embedding in a C/C++ project

```cmake
add_subdirectory(vendor/mac-optical-state)
target_link_libraries(your_app PRIVATE mos_core)
```

Include `<mos.h>` — the public header depends only on `<stdint.h>`,
`<stdbool.h>`, `<stddef.h>`; wrap calls in `#ifdef __APPLE__` in
cross-platform code. For a single-file drop-in,
`./scripts/amalgamate.sh` emits `dist/mos.h` + `dist/mos.c`: link
IOKit, CoreFoundation, DiscRecording, and DiskArbitration; build with
`-mmacosx-version-min=12.0`.

## Requirements

- macOS 12 Monterey or later (arm64 or x86_64)
- CMake 3.20+ and a C11 compiler (Xcode Command Line Tools suffice)
- An optical drive whose Apple-supplied kext attaches
  `SCSITaskUserClient` — empirically, writer-class drives (CD-R,
  DVD±R/RW, BD-R/RE) do; pure read-only BD-ROM drives historically
  did not, and this has shifted across macOS releases. See Known
  Unknowns in `ARCHITECTURE.md`.

## License

0BSD. Do whatever you want with it.

## See also

- `ARCHITECTURE.md` — command-by-command spec references, decision
  tree, sense-code tables, Known Unknowns
- `INTEGRATION_HARNESS.md` — test matrix for contributing hardware
  fixtures
- `CONTRIBUTING.md` — code layout and symbol-naming conventions

**Prior art:**
- libaacs `src/file/mmc_device_darwin.c` — live, compiled IOKit MMC
  backend with DiskArbitration coordination. Closest shipping
  reference for the pattern we use. (LGPL-2.1; study, don't copy.)
- cdrtools `libscg/scsi-mac-iokit.c` — older SCSITaskLib consumer;
  upstream is dormant since 2021 but the reference is correct.
- libcdio `lib/driver/osx.c` — includes the right headers and names,
  but the MMC code path is gated by `#ifdef GET_SCSI_FIXED` and is
  not active in shipping builds. Structural reference only.
- `DRDevice` / DiscRecording.framework — Apple's public alternative
  for tray-open queries. No SCSI traffic; smaller scope than `mos`.
  Kodi's Darwin storage provider acknowledges the same `drutil`
  papercut in `MediaManager.cpp`.
