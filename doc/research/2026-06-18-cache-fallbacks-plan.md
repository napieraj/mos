# Plan: kernel-cache fallbacks for fields mos leaves null (2026-06-18)

**Question (maintainer).** When exclusive access blocks a raw read, is there a
zero-MMC registry/cache property that could populate a field mos otherwise
leaves null? More broadly: where can the kernel cache fill a field we currently
leave unpopulated?

Two axes, and the fruitful one is not the one the question started from. The
platform inventory this draws on is `SPEC.md` → "macOS IOKit platform surface".

## Axis 1 — fields blocked by exclusive access (the raw verbs)

After the ReadFormatCapacities conversion (the same-day ADR), `mos_raw_cdb`'s
exclusive lock is taken by exactly four verbs: GESN (only on the **not-ready**
branch — not-ready ⇒ not mounted ⇒ the lock is free), the two **tray** verbs
(state-changing, user-initiated), and **INQUIRY** (the serial VPD-0x80 read +
the standard INQUIRY in `mos drive`). Only INQUIRY is a *query-path* read that
exclusive access can block on a **mounted** disc. What it leaves null then, and
whether a cache fallback exists:

| Field | Raw source | Null when | Kernel-cache fallback |
|-------|-----------|-----------|-----------------------|
| `serial` | INQUIRY VPD 0x80 (`mos_serial.c`) | busy / mounted | **None.** `kIOPropertyProductSerialNumberKey` exists but the optical SCSI stack never populates it — only the AHCI block path does (AGENTS.md:629, 2026-06-16 serial doc). |
| `version`, `version_descriptors` | standard INQUIRY (`mos_drive_inquiry.c`) | busy / mounted | **None.** Not cached anywhere; DiscRecording caches vendor/product/revision, not the SPC version or descriptors. |
| `vendor` / `product` / `firmware` | standard INQUIRY (`mos drive`) | — | **No gap** — already falls back to the DR / IOKit cache on BUSY. |

**Conclusion (axis 1): nothing to chase.** The one *disc-dependent* field
exclusive access used to block — `formattable` — is already fixed (it is now
the non-exclusive `ReadFormatCapacities` convenience read, populated on mounted
media). What remains lock-blocked is only **static drive identity** (serial /
version), which has no kernel-cache fallback and is readable any time the tray
is empty. The real lever was *convenience-over-raw*, and it has been pulled; no
fillable exclusive-access gap remains.

## Axis 2 — fields null for OTHER reasons, fillable from the registry (the win)

The cache's value is less about exclusive access than about paths where mos has
**no command answer at all**:

1. **Disc class on not-READY media.** `mos.state.v1` `media_class` / profile are
   `0x0000`/null for *every* non-ready state, deliberately — the runtime
   suppresses the profile off the not-ready branch because some drives cache the
   last disc's profile for minutes after ejection (stale-profile leak;
   `mos.state.v1.json:142`, ARCHITECTURE §9). But the registry
   `kIO{CD,DVD,BD}MediaTypeKey` (`"Type"`) gives the disc class — and
   ROM-vs-recordable — **fresh off the current media node, zero MMC**, with no
   such staleness (the kernel rebuilds the node on media change). So a
   `loading` / `busy` / `media_unreadable` event that today carries
   `media_class: null` could still say "it's a Blu-ray." **Candidate:** fall
   back to the Type key for `media_class` when the profile is unavailable.
   *Crosses* the no-profile-unless-READY rule — but that rule guards against a
   *stale cached profile*, and the Type key is fresh off the live node, so the
   hazard it defends may not apply here. ADR-grade; surface, don't assume.

2. **Coarse writable / blank signal.** `kIOMediaWritableKey` + the Type key give
   *writable* and ROM-vs-recordable for free even where disc-info (0x51) or GET
   CONFIGURATION can't or won't run — the watch-stream enrichment already
   captured in the gaps note (`2026-06-18-disc-ingest-surfaced-gaps.md` #2).

## Recommendation

- **Axis 1:** no action. The convenience conversion closed the only fillable
  exclusive-access gap; serial/version are static, empty-tray-readable, and
  un-cached.
- **Axis 2:** the genuine "use the cache to populate a previously-null field"
  wins are the **`Type`-key fallback for `media_class` on not-ready media** and
  the **writable/Type watch enrichment**. Both are additive and each crosses a
  documented rule or is an `mos.event.v1`/`mos.state.v1` schema addition, so
  each needs its own ADR + the C↔schema drift guard + fixtures, and an
  `ioreg -c IOMedia` / `-c IOCDMedia` dump on real media to confirm exactly
  when the kernel publishes `Type` / `Writable` (per the hardware-falsification
  doctrine) before building.

The reframing is the deliverable: exclusive access is mostly a solved problem
once convenience methods are preferred; the kernel cache's real value is
filling fields on paths that have **no command answer** (not-ready media,
blank-vs-writable), not racing the lock.
