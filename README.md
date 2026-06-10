# mac-optical-state

> Report what a macOS optical drive is actually doing — tray open,
> tray closed empty, loading, unit ready, busy — by querying the drive
> directly over IOKit's `MMCDeviceInterface`.

`drutil status` can tell you a disc is *not present* but cannot tell
you whether the tray is **open** or **closed on an empty slot**. They
produce the same output. `mos` distinguishes them by
talking to the drive over SCSI MMC.

Pure C. No Swift. No runtime dependencies beyond Apple's own IOKit,
CoreFoundation, and DiskArbitration frameworks.

## What it does

Issues up to four MMC commands — no entitlements, no root. Presence is read
with the non-exclusive `MMCDeviceInterface` convenience methods; the tray bit
is a raw `GET EVENT STATUS NOTIFICATION` CDB that takes *temporary* exclusive
access, and only on the not-ready path (where no disc is mounted, so the lock
is free). See §3 of ARCHITECTURE.md.

| Opcode | Command                         | Purpose                                                           |
|:-------|:--------------------------------|:------------------------------------------------------------------|
| `0x12` | INQUIRY                         | Vendor / product / revision strings for identification             |
| `0x4A` | GET EVENT STATUS NOTIFICATION   | Tray open vs closed — raw CDB via `mos_raw_cdb()` (NOT the `GetTrayState` convenience wrapper, which masks a GESN failure as "closed"); exclusive access, only when not ready |
| `0x00` | TEST UNIT READY                 | Presence / ready probe (convenience, non-exclusive, one shot); sense disambiguates empty / loading / formatting / unreadable / fault / busy |
| `0x46` | GET CONFIGURATION               | Current profile — identifies CD / DVD / BD media type             |

A fifth command, `0x51` READ DISC INFORMATION, is not currently
issued — planned for the v0.4 typed APIs (a `mos_disc_info` accessor)
to surface Disc Status, session count, and track count for
distinguishing blank-writable from finalized media. Not required for
any state decision the default path makes. Until then, the underlying
MMC operation is reachable today via `mos_raw_cdb()`.

## Output

Two surfaces, one vocabulary. Human views and JSON share every enum
string verbatim (`empty_or_open`, `bd_rom`) — a terminal report and a
jq query can never disagree. Human views follow a five-tier priority:
the answer (State), its evidence (Sense / error Stage+Code), media
context (Profile, Volume), addressing (Index, BSD, Registry), identity
last. The `bsd` field carries the full device node (`/dev/disk4`) in
both surfaces — pasteable, pipeable, and always a valid drive argument
when non-null (an empty drive has none: the whole-disk node exists
only with media present).

## Usage

```
mos [subcommand] [drive] [options]
```

The drive subject is positional, like `diskutil info disk4`: an Index
from `mos list` (all digits) or a BSD form (`disk4`, `rdisk4`,
`/dev/disk4`). With one drive attached it may be omitted; with several,
`mos status` without a subject exits 64 and prints the drive table to
stderr — no first-drive guessing.

### Status (default)

```
$ mos status 1
   State:  ready
 Profile:  0x0040  bd_rom  (bd)
   Index:  1
     BSD:  /dev/disk4
Registry:  4295032831
   Drive:  HL-DT-ST BD-RE WH16NS60 1.00
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

An empty/open drive shows the honest pre-disambiguation state with its
sense evidence directly beneath:

```
$ mos status 1
   State:  empty_or_open
   Sense:  02/3a/01
   Index:  1
     BSD:  -
Registry:  4295032831
   Drive:  HL-DT-ST BD-RE WH16NS60 1.00
```

### List

The rig dashboard — every drive with its state (each row costs one
probe; a failing drive shows `error` in its row rather than killing
the overview):

```
$ mos list
 Index  State          Volume      BSD         Vendor    Product         Rev
     1  ready          -           /dev/disk4  HL-DT-ST  BD-RE WH16NS60  1.00
     2  empty_or_open  -           -           PIONEER   BD-RW BDR-XS07  1.01
```

`mos list --json` emits `mos.list.v1` with the same fields per entry
(`index`, `state`, `volume_name`, `bsd`, `registry_id`, identity).

### Watch

NDJSON unconditionally — one `mos.event.v1` per line, errors included;
`--json` is a no-op here. An insert looks like:

```
$ mos watch 1
{"schema":"mos.event.v1","event":"snapshot",...,"state":"empty","prev_state":"unknown",...}
{"schema":"mos.event.v1","event":"state_changed",...,"state":"loading","prev_state":"empty",...}
{"schema":"mos.event.v1","event":"state_changed",...,"bsd":"/dev/disk4","state":"ready",...}
```

## Install

### Homebrew (tap)

Until the first stable tarball release is tagged, install the HEAD formula:

```sh
brew tap napieraj/tap
brew install --HEAD napieraj/tap/mos
```

After the first stable tag and the formula gets a stable tarball URL +
SHA256, `brew install mos` works without `--HEAD`. The release
workflow that fills these is documented in `CONTRIBUTING.md`.

### From source

```sh
git clone https://github.com/napieraj/mos
cd mac-optical-state
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bin/mos
```

or, via the convenience Makefile:

```sh
make build
./build/bin/mos
```

### Embedding in a C/C++ project

Two lines in your top-level `CMakeLists.txt`:

```cmake
add_subdirectory(vendor/mac-optical-state)
target_link_libraries(your_app PRIVATE mos_core)
```

Include `<mos.h>` — the public header depends only on `<stdint.h>`,
`<stdbool.h>`, `<stddef.h>`. Wrap calls in `#ifdef __APPLE__` from your
generic code.

For absolute minimum friction, use the stb-style single-file
distribution:

```sh
./scripts/amalgamate.sh
# Outputs: dist/mos.h + dist/mos.c
# Drop both into your source tree; link IOKit, CoreFoundation, and
# DiskArbitration; build with -mmacosx-version-min=12.0.
```

## Requirements

- macOS 12 Monterey or later (arm64 or x86_64)
- CMake 3.20+ and a C11 compiler (Xcode Command Line Tools provide this)
- An optical drive whose Apple-supplied kext attaches
  `SCSITaskUserClient`. **See Known Unknowns in ARCHITECTURE.md** —
  empirically, writer-class drives (CD-R, DVD±R/RW, BD-R/RE) do;
  pure read-only BD-ROM drives historically did not, and this has
  shifted across macOS releases.

## Building and testing

```sh
make build      # release build of library + CLI
make test       # pure-data unit tests (no hardware needed)
make probe      # builds the hardware smoke-test binary (mos_probe)
./build/bin/mos_probe   # runs against a real drive
make clean
```

Unit tests exercise the pure-data layer directly — sense parsing,
state-machine decision tree, watch core, BSD-name normalization, SAM-5
status classification, IOReturn mapping, JSON/safe-ASCII rendering —
with no IOKit involvement, so they run identically on a CI runner with
no optical drive present.

## Why this exists

`drutil status` conflates "tray closed, no disc" with "tray open" and
returns English prose that every consumer has to regex-parse. There is
a partial public alternative — `DRDevice.trayIsOpen` in the
DiscRecording framework — which answers the tray-open case cleanly in
Objective-C. It does not expose the fuller state machine (loading,
ready, busy, with MMC profile-byte detection for media class), which
is where `mos` earns its place.

Kodi's Darwin storage provider has a matching papercut
acknowledged in `MediaManager.cpp`. `mos` is a small,
auditable, embeddable library for anyone building macOS-native
optical tooling from 2026 onward.

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
