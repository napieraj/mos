# DriverKit transition investigation

Date: 2026-04-22

Empirical investigation of the macOS DriverKit migration as it
relates to mos's continued reliance on `SCSITaskLib` and
`MMCDeviceInterface`. Extracted from `ARCHITECTURE.md` §9.5 during
the 2026-04-26 prune sweep — the load-bearing summary remains in
ARCHITECTURE; this file holds the trace evidence and investigation
log so the architecture document doesn't carry empirical-snapshot
prose that ages out.

## Validation environment

macOS 26.5 beta 25F5042g (Xcode 26.4.1 / build 17E202), as of
2026-04-22. The current 26.5 beta track may be ahead of this build;
re-validate before tagging v0.2.0 if a newer beta has dropped.

## SDK-side findings

`SCSITaskLib.h` is present in the 26.5 SDK. Copyright header reads
"2001–2009", unchanged through the Tahoe cycle. Full SCSI header set
(`SCSITask.h`, `SCSICmds_INQUIRY_Definitions.h`,
`SCSICmds_REQUEST_SENSE_Defs.h`, `IOSCSIMultimediaCommandsDevice.h`)
all present and consistent with the signatures this library targets.

Grep across `IOKit.framework/Headers/scsi/` and
`IOKit.framework/Headers/storage/` for `API_DEPRECATED`,
`API_UNAVAILABLE`, `__deprecated`, `API_AVAILABLE` yields no hits
against any symbol we depend on. The only deprecation markers are on
the legacy non-`kIO`-prefixed UUID aliases (`OLD_UUIDS` block in
`SCSITaskLib.h`), `REPORT_KEY_V1` (DVD CSS — unrelated), and the
64-bit `SCSILogicalUnitNumber` type (unrelated). None of these touch
our code path.

`<IOKit/storage/IOMedia.h>` has been removed from the userland SDK.
We don't reference anything from it so this is a non-issue; the
include line has been dropped defensively.

## Kernel / runtime findings

`kmutil showloaded` on 26.5 beta confirms all seven kexts we depend
on are active [correction 2026-06-10: the original text said "four"
above a seven-entry list — the count is fixed here per the prose/list
mismatch rule in CLAUDE.md; the list itself was always correct]:

```
com.apple.iokit.IOSCSIArchitectureModelFamily   (545.100.10)
com.apple.iokit.IOSCSIMultimediaCommandsDevice  (545.100.10)
com.apple.iokit.SCSITaskUserClient              (545.100.10)
com.apple.iokit.IOStorageFamily                 (2.1)
com.apple.iokit.IOCDStorageFamily               (1.8)
com.apple.iokit.IODVDStorageFamily              (1.8)
com.apple.iokit.IOBDStorageFamily               (1.8)
```

Version numbers are unchanged from Sequoia 15.x — no code churn in
the SCSI subsystem through the 26 release cycle.

## Third-party corroboration

`otool -L` on MakeMKV 1.18.x (`/opt/homebrew/bin/makemkvcon`, both
arm64 and x86_64 slices) shows linkage against `IOKit`,
`CoreFoundation`, `DiskArbitration`, `Foundation`, `Security`,
`libobjc`, `libSystem`, `libc++`. **No DriverKit linkage.** No
SystemExtensions linkage. Same frameworks we target. No
`SCSIPeripheralsDriverKit.framework` or
`SCSIControllerDriverKit.framework` present in MakeMKV's link map.

## DriverKit replacement path analysis

`SCSIPeripheralsDriverKit` has shipped since macOS 13 Ventura (fall
2022). It exposes `IOUserSCSIPeripheralDeviceType05` (for SCSI
Multimedia Commands — i.e., optical drives) with `UserSendCDB` as
the CDB-issuance path. See
`developer.apple.com/documentation/scsiperipheralsdriverkit`.

This is the correct long-term replacement for SCSITaskLib — for
*driver authors*. It is **not** a drop-in substitute for an
in-process library like ours:

- DriverKit drivers are `.dext` bundles that run out-of-process in
  a System Extension sandbox. They require Apple entitlement, SE
  activation approval, and distribution as part of a signed app
  bundle.
- The CLI / library pattern — a user-space process opening an
  `SCSITaskUserClient` — has no DriverKit analogue. A `.dext`
  cannot be loaded by a CLI tool, and the CLI cannot issue CDBs
  through a `.dext` it doesn't own.
- Migrating `mos` to DriverKit would mean rewriting as a system
  extension distributed alongside the consuming application (or whichever app
  embeds it), with a completely different install story. That's
  not a port; it's a new project with a different shape.

What this means for our clock: `SCSITaskLib`'s survival has never
depended on DriverKit not existing. It depends on Apple continuing
to ship the legacy user-client path alongside DriverKit for
backward compatibility with tools like MakeMKV, libcdio, cdrtools,
and `mos`. That's benign neglect — a survival mode, not a
contract.

Apple has precedent for terminating benign-neglect APIs without a
deprecation cycle: AppleHDA was removed entirely in macOS 26,
FireWire support was removed in 26.1, and neither had a prior
`API_DEPRECATED` annotation. If Apple cuts `SCSITaskUserClient` in
a future release, `mos` has no in-process fallback — the project
ends or forks into a dext-based successor under a different
distribution model.

## Release-notes review

Full read of macOS 26 through 26.5 beta 3 release notes
(developer.apple.com), 2026-04. Zero references to IOKit, SCSI,
SCSITaskLib, MMCDeviceInterface, DriverKit migration, optical
media, kext deprecation, DiskArbitration, or any of our immediate
dependencies. The only notes that touch our distribution story at
all are the Rosetta / Intel deprecation in 26.4 and the "macOS 26
is the last Intel release" statement — see ARCHITECTURE.md §9.5.1.
