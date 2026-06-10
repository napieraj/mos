# DR substrate pivot — field source mapping

What each `mos_state_result` / `mos_watch_event` field is sourced from **today**
(MMC commands we issue + parse) vs the **DR key** that would source it after the
pivot. Drawn from the vendored headers in `docs/apple/DiscRecording/`
(`DRCoreDevice.h`). **Note (fourth review):** `docs/apple/` is a
dev-tree-only reference copy — release-preflight strips it from every
shipped archive (not redistributable under 0BSD), so claims cited to it
are verifiable only in a development checkout; public historical copies
with identical declarations are linked in ARCHITECTURE §11. Citations
below use the header's own section banners and key symbols, not line
numbers, so they cannot rot silently against a file the staleness gate
cannot see. Two DR dictionaries:

- **Info** (`DRDeviceCopyInfo`) — device-static: identity + capabilities.
  Keys under the header banner "Keys for the dictionary returned by DRDeviceCopyInfo".
- **Status** (`DRDeviceCopyStatus`) — live: tray/busy/media. Keys under
  the banners "Keys for the dictionary returned by DRDeviceCopyStatus"
  and "Keys for the media info dictionary" (the `kDRDeviceMediaInfoKey`
  sub-dictionary),
  refreshed via `kDRDeviceStatusChangedNotification`.

## Identity / device-static

| mos field | today | DR key | dict | note |
|---|---|---|---|---|
| vendor | INQUIRY → copy_scsi_string | `kDRDeviceVendorNameKey` | info | pre-parsed CFString; retires INQUIRY + field-width handling |
| product | INQUIRY | `kDRDeviceProductNameKey` | info | " |
| revision | INQUIRY | `kDRDeviceFirmwareRevisionKey` | info | " |
| (write caps — v0.4 `get_features` stub) | GET CONFIGURATION feature walk | `kDRDeviceCanWrite{CD,DVD,DVDR,BD,BDR,...}Key` (≈30 keys) | info | the entire deferred feature surface, already decoded |
| (interconnect — not currently exposed) | n/a | `kDRDevicePhysicalInterconnect{,Location}Key` | info | new metadata (USB/SATA/internal-external) if wanted |

## State (the mapping that changes meaning — needs care)

| mos field | today | DR key(s) | dict | note |
|---|---|---|---|---|
| state = OPEN | GetTrayState hint | `kDRDeviceIsTrayOpenKey` (CFBoolean) | status | direct |
| state = BUSY | EXCLUSIVE/BUSY from wrappers | `kDRDeviceIsBusyKey` (CFBoolean) | status | direct |
| state = EMPTY | TUR → not-ready + sense | `kDRDeviceMediaStateKey == None` | status | direct |
| state = LOADING | inferred from TUR UA / sense | `kDRDeviceMediaStateKey == InTransition` | status | **DR gives this directly** — today we infer it |
| state = READY | TUR → GOOD | `kDRDeviceMediaStateKey == MediaPresent` | status | NB: "media present" is not identical to our TUR-ready; see open question |
| sense_key/asc/ascq | TUR CHECK CONDITION → parse_sense | **no DR equivalent** | — | DR abstracts sense away; we lose raw sense unless we keep an MMC path |

## Media / disc-info (retires mos_discinfo.c)

| mos field | today | DR key | dict | note |
|---|---|---|---|---|
| current_profile | GET CONFIGURATION byte 6-7 | `kDRDeviceMediaTypeKey` / `kDRDeviceMediaClassKey` | status | symbolic (CDR/DVDROM/BDRE...) — retires mos_profile_name table |
| (disc status: blank/appendable/complete) | READ DISC INFORMATION parse | `kDRDeviceMediaIsBlankKey`, `IsAppendableKey`, `IsOverwritableKey`, `IsErasableKey` | status | retires mos_discinfo.c entirely |
| (sessions/tracks) | READ DISC INFORMATION | `kDRDeviceMediaSessionCountKey`, `MediaTrackCountKey` | status | " |
| (free/used blocks — not exposed) | n/a | `kDRDeviceMediaBlocksFree/UsedKey` | status | new metadata |

## Identity for open/reopen (the registry-ID fork)

| mos field | today | DR | note |
|---|---|---|---|
| bsd_unit (diskN) | walk IOMedia child, parse unit | `kDRDeviceMediaBSDNameKey` | status | media node directly; **media-scoped (absent when no media)** — same shape as today |
| 1-N index | sort enumerated services by registry ID (approximates drutil) | position in `DRCopyDeviceArray` | — | **identical to drutil by construction** — drutil sits on this same array |
| media_id (F1 swap fingerprint) | IORegistryEntryGetRegistryEntryID (uint64) | **no direct DR equivalent** | — | DR has registry *path* (`kDRDeviceIORegistryEntryPathKey`), not entry ID. To keep F1 unchanged: path → IORegistryEntryFromPath → GetRegistryEntryID = the one surviving IOKit step |
| device reopen target | IORegistryEntryIDMatching (atomic, TOCTOU-safe) | `DRDeviceRef` (opaque, held) OR `DRDeviceCopyDeviceForIORegistryEntryPath` | — | DRDeviceRef may be the stable handle to hold; or recover entry-ID per above |

## Two things the headers settle

1. **Index matches drutil by construction, not convention.** `DRCopyDeviceArray`
   is the same snapshot array drutil enumerates. Today's registry-ID sort only
   *approximates* that order; DR makes it identical. Wrinkle: the array is "not
   guaranteed to stay current for the lifetime of a process" — it's a snapshot,
   same as our enumeration, so the index is only stable within an
   enumerate→open window. The header explicitly steers toward the
   Appeared/Disappeared notifications for a live list.

2. **Snapshot dicts are the real saving on the watch path.** vendor/product/
   firmware/tray/busy/media-state/blank/profile all come from two dict reads,
   refreshed by DR via notification — not re-issued per probe. Today the watch
   re-opens + re-INQUIRYs + TUR + (would) GET-CONFIG every poll. The cost moves
   into DR and is amortized/event-driven. (Unverifiable from headers: whether a
   single one-shot query does MORE total work building the full dict than our 4
   targeted commands — likely yes for one-shot, win for watch.)

## Open questions the table surfaces

- **Sense data has no DR home.** If raw sense_key/asc/ascq stays in the public
  API (mos_state_result_sense / mos_watch_event_sense), the pivot can't fully
  retire the MMC path — a CHECK CONDITION still needs TUR+parse_sense. Decide:
  drop raw sense from the typed API (DR's media-state booleans replace its
  *purpose*), or keep one MMC command for it.
- **MediaPresent ≠ our READY.** DR's MediaPresent means media is loaded and
  recognized; our READY means TUR returned GOOD. A spun-down or
  becoming-ready disc could read MediaPresent in DR while TUR would still say
  not-ready. The state mapping needs a decision on which definition mos exports.
- **F1 fingerprint costs one IOKit step under DR** (path→entry→ID), or changes
  identity model to DRDeviceRef/path. The audit consolidated the registry-ID
  rationale so this is now a clean either/or, but DR retiring discinfo+features+
  INQUIRY means that one IOKit step is the lone MMC/IOKit holdout in an
  otherwise-DR file — weakens the "cheap to keep" case.
