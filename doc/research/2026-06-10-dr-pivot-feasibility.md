# DR pivot: feasibility and advisability on current macOS (June 2026)

Web-verified status check on the DiscRecording substrate pivot
(`doc/dr-field-mapping.md`, ROADMAP "Architectural"): would it work on
current macOS, and is building on DiscRecording in 2026 advisable?
Method: five parallel research angles (deprecation status, header
availability, open-source stack, behavioral caveats, forward-survival
risk), claims verified against primary sources where one exists — the
decisive ones against the vendored headers themselves (see
"Artifacts" below). Confidence per claim: HIGH = primary source
fetched and quoted; MEDIUM = secondary/user reports.

## Verdict

**Feasible: yes, header-proven. Advisable: yes for the enumeration +
watch substrate, with the MMC path retained for sense — pending one
hardware validation pass on Tahoe.** Nothing found contradicts the
pivot; two findings materially strengthen it (API frozen-stable for
~19 years; writers-only coverage is identical to today's, so nothing
is lost). The real risk is shared, not DR-specific: Apple's optical
stack as a whole is in maintenance-only mode, and DR sits on the same
kext mos already depends on — the pivot does not add a substrate that
can outlive the current one, only a layer that could in principle be
removed first (no signal found that it will be).

## 1. Deprecation / availability status

- **Not formally deprecated.** Every relevant symbol in the newest
  public SDK (15.5) carries `AVAILABLE_MAC_OS_X_VERSION_10_2_AND_LATER`
  and the header contains **zero** `DEPRECATED` markers (HIGH —
  verified by grep against the vendored 15.5 header: 137 availability
  macros, 0 deprecation macros).
- **Docs abandoned, not the binary.** The DiscRecording release notes
  are frozen at OS X 10.5 (2007) with "This document is no longer
  being updated"; `developer.apple.com/documentation/discrecording`
  404s (HIGH). An Apple Developer Forums thread notes no deprecation
  was ever announced — "the legacy document probably just got
  overlooked in the shuffle"
  (https://developer.apple.com/forums/thread/65690).
- **Still ships.** Headers present in the macOS 15.5 SDK (HIGH —
  downloaded). Runtime: Toast 20 reportedly works on Tahoe 26
  (MEDIUM, forums), the Finder burn UI still exists in Tahoe (MEDIUM),
  drutil man page still distributed (MEDIUM). Finder burning broke in
  Sequoia 15.0–15.1 and was fixed by 15.2–15.3; ISO 9660 burning
  reportedly remains broken (MEDIUM, Apple Communities) — relevant as
  a QA-priority signal, not as an enumeration/status defect.
- **One contradicting source, refuted:** the pyobjc-framework-
  DiscRecording changelog claims "the entire DiscRecording framework
  was removed in macOS 10.15." False at face value: the headers exist
  in the 15.5 SDK (primary evidence, vendored) and the framework
  demonstrably functions post-10.15 (drutil, Toast, Finder burning in
  15.2+). Likely a PyObjC-side packaging note gone wrong.

## 2. Header stability (the strongest pro-pivot finding)

`DRCoreDevice.h` is byte-identical from the 10.13 SDK through 11.3
(same MD5 across phracker mirrors) and differs in the 15.5 SDK only
by `#include` → `#import` cosmetics — no symbol, signature, or doc
change since the 2002–2007 copyright era (HIGH — hashes + diff via
mirror fetches). An API that survived the 32→64-bit cull (Catalina),
the arm64 transition, and 12 major releases without a single header
edit is as stable as unmaintained Apple API gets. The
dr-field-mapping table was re-checked against the 15.5 copy: both
dictionary banners and all keys cited there are present unchanged.

## 3. Coverage: writers-only on BOTH substrates (concern dissolved)

The 15.5 header states `DRCopyDeviceArray` "Returns an array of all
**writable** devices connected to the system" (HIGH — vendored header,
function doc comment). So DR enumeration excludes pure readers — but
so does mos today: ARCHITECTURE §9.7 documents that the
SCSITaskUserClient attach gate already filters non-authoring drives
out of `mos_enumerate_devices()`. The pivot changes nothing about
which drives are visible. (Same header section confirms the array is
a snapshot and steers to Appeared/Disappeared notifications for a
live list — matching dr-field-mapping's note.)

## 4. Behavioral caveats (all manageable, one hardware-test gate)

- **Runloop:** DR notifications are runloop-delivered
  (`DRNotificationCenterCreateRunLoopSource`, vendored
  `DRCoreNotifications.h`). The watch adapter needs a CFRunLoop on
  the listening thread — same model as today's DiskArbitration
  session in `mos_watch.c`, so this is a known pattern, not new cost
  (HIGH).
- **No entitlements/TCC found** for read-only enumeration/status; the
  sandbox temporary-exception machinery applies to *burning*
  (MEDIUM — absence of evidence across Apple docs/forums).
- **Status without media / tray state:** `kDRDeviceIsTrayOpenKey` and
  `kDRDeviceMediaStateKey` exist precisely for the no-media cases
  (HIGH — header). Whether tray-open vs closed-empty is reported
  *accurately* on current macOS for our fixture drives is the open
  empirical question — same GESN-vs-GetTrayState class of doubt that
  drove the 2026-05-30 redesign. **This is the hardware-validation
  gate before committing the pivot.**
- **Status queries need no reservation/exclusive access** —
  `DRDeviceAcquireExclusiveAccess` / media reservation are separate,
  burn-path APIs (HIGH — header). The pivot would *remove* mos's one
  exclusive-access taking (the raw-GESN tray probe), not add one.
- No evidence of XPC/daemon spawn or Apple Silicon-specific DR bugs
  (MEDIUM — absence of reports).

## 5. Forward-survival risk

- **The substrates are stacked, not parallel.** drutil and DR drive
  the same SCSITaskUserClient/kext stack mos uses directly (§9.7).
  DR cannot outlive the IOKit substrate; conversely, removing DR
  would break drutil + Finder burning while leaving IOKit intact. So
  the pivot's *additional* risk is exactly "Apple deletes the
  framework but keeps the kext" — possible (QuickTime precedent), but
  no signal found, and the framework's arm64/Catalina survival plus
  the still-shipping burn UI argue against near-term removal.
- **IOKit side independently healthy:** this repo's own
  2026-04-22-driverkit-investigation.md verified SCSITaskLib present
  and undeprecated on macOS 26.5 beta; Apple staff have stated
  user-space IOKit access is not going away
  (https://developer.apple.com/forums/thread/131311) (HIGH).
- **Maintenance-only signals for optical generally:** SuperDrive
  discontinued/delisted (2024–2025); Sequoia burn regressions shipped
  and lingered; FireWire removed in Tahoe (MEDIUM). None block the
  pivot; all say "don't expect Apple to fix what breaks" — equally
  true of the current substrate (see Ventura 13.2 breaking Pioneer
  drives at the kext level, fixed 13.2.1).
- **Kernel source visibility is already gone either way:**
  apple-oss-distributions/IOSCSIArchitectureModelFamily's newest tag
  is 139.0.2 — Tiger-era, Feb 2005 (HIGH — `git ls-remote` verified,
  19 tags total; absent from the distribution-macOS manifests for
  current releases). ARCHITECTURE §11's kernel citations are to that
  frozen source; current kext behavior is observable-only. The pivot
  neither worsens nor improves this.

## Recommendation

Proceed with the pivot design (enumeration, status, watch on DR;
keep the raw-MMC seam for sense/diagnostics per dr-field-mapping's
open question), gated on one hardware session on Tahoe validating:
tray-open vs closed-empty reporting via `DRDeviceCopyStatus` across
the fixture drives, notification latency vs the current poll loop,
and `kDRDeviceMediaBSDNameKey` presence/absence semantics. If tray
reporting proves as unreliable as `GetTrayState` was, the fallback
is the hybrid already sketched: DR for enumeration/identity/index
(where it is provably better) + retained GESN tray probe.

## Revision (2026-06-10, same day): the tray gate was wrong-shaped

The Recommendation above frames the hardware session as a gate that
could *validate* DR's tray reporting. The vendored kernel source shows
that framing is epistemically broken, so this revision supersedes it
(kept in place per the same-day revision pattern in AGENTS.md ADRs;
the original stands above as the audit trail).

`IOSCSIMultimediaCommandsDevice::GetTrayState`
(`IOSCSIMultimediaCommandsDevice.cpp:782`, vendored 139.0.2) collapses
**every** GESN failure — transport error, timeout, CHECK CONDITION —
to `*trayState = 0` (closed) **and rewrites the error to
`kIOReturnSuccess`** ("// Assume the tray is shut."; the banner hedges
"(if possible)", and the failure-arm comment mis-attributes all
failures to "doesn't support"). The chain
`MMCDeviceInterface::GetTrayState` → user-client method #14
(`SCSITaskUserClient.cpp:1570`) → the kernel method means every
convenience-path consumer inherits the collapse, and **no userspace
caller can detect it** — success status either way.

Consequences:

1. **A passing hardware run proves nothing about this property.** It
   exercises only the GESN-succeeds branch; the masking fires only on
   the failure branch and is then indistinguishable from a true
   closed. "drutil tray reporting works on my drive" cannot falsify a
   kernel-level failure mode. Only a GESN-*failing* device can — by
   diffing mos's raw GESN (real sense visible) against the
   convenience/DR report on the same drive at the same instant.
2. **Whether `kDRDeviceIsTrayOpenKey` rides this path is unverifiable**
   (DR is closed-source), and per (1) no positive observation can
   distinguish a DR that issues its own GESN from one that inherits
   the kernel collapse.
3. **Therefore the hybrid is the design, not the fallback**: DR for
   enumeration/identity/index/watch notifications (where the headers
   prove it better), with the **TUR⊕GESN state core retained as a
   unit** — not "raw GESN for the tray bit" alone. The convenience
   TUR is the raw GESN's precondition, not an optional companion:
   the §5.5 nub gate that decides whether the exclusive lock may be
   taken is computed from the TUR sense bytes
   (`mos_state_core.c` step 1→2), which DR's status dictionary does
   not expose; the TUR sense is also GESN's failure fork (3A/01,
   3A/02, 3A/00→EMPTY_OR_OPEN) and the sole sense source. The §9.7
   rationale is untouched by the pivot. The hardware session's role
   reverts to what the hardware ADR already prescribes:
   falsification and fixture capture (on a GESN-failing bridge/drive
   if one enters the fixture set), never blessing.

## Artifacts (dev-tree only, `docs/apple/` — preflight-stripped)

- `docs/apple/DiscRecording/` — DiscRecording.h, DRCoreDevice.h,
  DRCoreNotifications.h, DRCoreObject.h from the **macOS 15.5 SDK**
  (newest publicly mirrored), fetched 2026-06-10 from
  github.com/alexey-lysiuk/macos-sdk (`main`,
  `MacOSX15.5.sdk/System/Library/Frameworks/DiscRecording.framework/
  Versions/A/Headers/`). Older identical copies: phracker/MacOSX-SDKs
  up to 11.3.
- `docs/apple/IOSCSIArchitectureModelFamily/` — full source at tag
  139.0.2 (newest available, APSL), from
  github.com/apple-oss-distributions/IOSCSIArchitectureModelFamily.
  `IOSCSIMultimediaCommands/IOSCSIMultimediaCommandsDevice.cpp`
  `GetTrayState` at :782 — the §11 citations resolve.

## Sources

- Vendored headers (primary): see Artifacts above.
- https://developer.apple.com/library/archive/releasenotes/MusicAudio/RN-DiscRecording/index.html
- https://developer.apple.com/forums/thread/65690 (no-deprecation observation)
- https://developer.apple.com/forums/thread/131311 (user-space IOKit not going away)
- https://github.com/alexey-lysiuk/macos-sdk · https://github.com/phracker/MacOSX-SDKs
- https://github.com/apple-oss-distributions/IOSCSIArchitectureModelFamily (tags)
- https://discussions.apple.com/thread/255833454 (Sequoia burn regression/fix)
- https://discussions.apple.com/thread/256175963 (ISO 9660 regression)
- https://www.macrumors.com/2023/02/03/macos-13-2-breaks-pioneer-cd-dvd-drives/
- https://support.apple.com/en-us/102181 (SuperDrive)
- https://pypi.org/project/pyobjc-framework-DiscRecording/ (refuted claim)
