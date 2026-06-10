# drutil contract, the drive index, and mos selector design

**Date:** 2026-06-10
**Status:** Active. Source basis: drutil(1) man page (Apple ADC archive,
contract unchanged since 10.3 per its own HISTORY line) and
`DRCoreDevice.h` (10.13 SDK public copy; signatures stable per the
vendored modern headers). drutil itself is closed-source; every claim
below is tiered accordingly.

## The drive-selection contract (Documented)

drutil's `-drive` is a **composable filter chain**, not an index
lookup. The candidate list starts as all attached burning devices; each
`-drive` argument narrows it:

- a positive decimal — a **1-based index into the current candidate
  list** ("per the output of list"). Relative, not absolute:
  `-drive usb -drive 2` means the second USB drive.
- a bus/location keyword: `internal, external, usb, firewire, atapi,
  scsi` (case-insensitive).
- any other string — exact vendor OR product match (case- and
  whitespace-insensitive).

Zero survivors → error. Multiple survivors → **the verb fans out to
all of them**. Notably, drutil cannot select by BSD name at all
(`/dev/diskN` appears only as the raw argument to `dumpiso`/`dumpudf`).
Machine output exists: `-xml` on `list/info/status/discinfo/trackinfo`.
`drutil poll` streams DR device/media notifications (the drutil
analogue of `mos --watch`). `drutil tray eject` is documented as
unmount-then-eject, failing if unmountable — drutil does policy, the
deliberate contrast to mos's mechanism-facts-only v0.4 verbs.

## Header mapping (Documented)

drutil's entire selection vocabulary is the `DRDeviceCopyInfo`
dictionary, verbatim: bus keywords ↔ `kDRDevicePhysicalInterconnectKey`
values (ATAPI / FireWire / USB / SCSI; the header also has FibreChannel,
which the CLI does not expose), internal/external ↔
`kDRDevicePhysicalInterconnectLocationKey`, the string match ↔
`kDRDeviceVendorNameKey` / `kDRDeviceProductNameKey`.

## Index tiering

1. index N ≡ Nth row of `drutil list` — **Documented** (man page).
2. `drutil list` order ≡ `DRCopyDeviceArray` order — **Inferred**
   (strongly: the selection vocabulary being pure DR info-dict keys is
   the circumstantial proof that drutil's candidate set is the DR
   device array; drutil is closed, so it stays Inferred).
3. `DRCopyDeviceArray` ordering is deterministic across processes —
   **Undocumented.** The header documents the array only as a snapshot
   "not guaranteed to stay current," with **no ordering contract**.

Falsifier (hardware gate, machine-checkable): `drutil list -xml`
versus `mos list --json`, repeated across hotplug events. One
comparison retires tier 2 and characterizes tier 3.

## Decision: mos selectors (filter grammar evaluated and REJECTED)

The filter-chain grammar was considered for a drutil-compatible
`--drive` and rejected:

- **Bus keywords are a 2004 contract.** Every 2026 optical path is a
  USB bridge (incl. via Thunderbolt/hubs); macOS 27 is
  Apple-Silicon-only (§9.5.1), so the keyword set selects among one
  bus.
- **The vendor/product match fails its own use case.** A multi-drive
  rig is *identical* drives — vendor strings collide; only position or
  registry identity disambiguates.
- **Fan-out dies with the filters.** mos verbs stay single-drive;
  multi-drive orchestration is the caller's loop over `mos list`
  (examples/README composition doctrine).

What ships instead — three separate flags, no shared grammar (a bare
decimal would be ambiguous between index and registry id):

- `--index` — 1-based, DR-array positional once the DR pivot lands;
  drutil-aligned at tier 2 above. **Addressing, not identity** — the
  man page's own candidate-relative semantics prove the point: the
  same "2" denotes different drives under different filters.
- `--bsd` — the selection capability drutil lacks; one I/O format
  among several, media-dependent by nature.
- `--registry-id` (v0.4, new) — **attachment** identity: stable for
  the life of one plug session, surviving media insert/eject cycles,
  BSD-name churn, and media-less periods. It deliberately does NOT
  survive a replug: xnu assigns the ID at first attach from a global
  monotone counter (`++gIORegistryLastID`,
  `iokit/Kernel/IORegistryEntry.cpp`) — never hardware-derived, never
  reused within a boot — so a replugged drive is a NEW ID by
  construction. That non-reuse is a feature, not a gap: it is the
  watch's TOCTOU defense (reopen-by-ID terminates with
  `device_removed` instead of silently rebinding to whatever came
  back). The same authority the watch already uses; the DR pivot
  bridges it natively (`DRDeviceCopyDeviceForIORegistryEntryPath`
  exists alongside `DRDeviceCopyDeviceForBSDName`). Hardware identity
  *across* replugs (INQUIRY serial, VPD 0x80) is a different concept,
  unreliable through USB bridges, and out of scope — `--index` covers
  the practical re-selection case.

The role split this fixes in vocabulary: **registry entry ID =
attachment identity** (stream identity, reopen; per plug session, per
boot), **index = addressing/interop**
(drutil piping; unstable across hotplug, which the snapshot caveat
concedes for drutil too), **BSD name = an I/O format**.
