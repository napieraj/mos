# Held-handle media-identity refresh (2026-06-14)

Closes the v0.4 ROADMAP "Held-handle identity refresh" item. A handle
captured its whole-disk identity — `bsd_unit`, the `media_id` swap
fingerprint, and the kernel-cached `media_bytes`/`media_block_bytes` —
once at `mos_open*`. The drive service is stable for the handle's life,
but the IOMedia whole-disk child under it is media-scoped: it appears on
insert and vanishes on eject. So a handle opened on an empty drive kept
reporting `bsd_unit = -1` even after a later `mos_query_state` returned
READY for newly inserted media (and a handle opened on disc A reported
A's size/node after a swap to B). This was documented in `mos.h` as
"open-time semantics," a known limitation.

**Fix.** A small adapter helper, `mos_internal_refresh_media_identity`
(`mos_scsi.c`), re-runs the existing `mos_internal_bsd_unit` resolver
against the handle's stable `h->svc`. The three media-scoped query entry
points call it first: `mos_query_state` (the named symptom), and
`mos_query_capacity` / `mos_query_volume` (so standalone calls are
consistent). The open path now calls the same helper, so open-time and
query-time take one code path. Open-time semantics become query-time
semantics uniformly; the `mos.h` docstrings are updated.

**Why a registry re-resolve, not the DR dictionary lookup the ROADMAP
named.** The ROADMAP said per-query freshness "under DR is a dictionary
lookup (`kDRDeviceMediaBSDNameKey` in `DRDeviceCopyStatus`), not a
registry walk." Registry re-resolve was chosen on the merits:

1. **It already exists and is proven.** `mos_internal_bsd_unit` is the
   exact resolver the open path and the watch's reopen-per-probe already
   rely on for current media identity. Reusing it adds no new IOKit
   surface — the lowest-risk change, and adapter code cannot be
   compile-tested off-Mac (macOS CI is the only gate), so reuse-of-proven
   beats new-and-untested here.
2. **DR would not eliminate IOKit anyway.** `media_id` (the F1 swap
   fingerprint) has no DR equivalent — `doc/dr-field-mapping.md` records
   that DR carries the registry *path*, not the entry ID, so refreshing
   it still needs `IORegistryEntryFromPath → GetRegistryEntryID`. The
   DR-dict form trades the IOMedia walk for a `DRDeviceCopyStatus` +
   dict parse + a surviving IOKit step, and requires the handle to hold a
   `DRDeviceRef` it does not currently keep.
3. **Cost is acceptable.** The re-resolve is a local IORegistry walk off
   the drive node — no SCSI command, no exclusive access, cheaper than
   the watch's per-probe full reopen that this mirrors. On an empty drive
   the walk is short (no media children).

The DR-dictionary lookup remains an available *optimization* (one
framework call vs one registry walk), not a correctness need; if a
hot-path profile ever shows the walk mattering, that is a fresh argument
to make, with the `media_id` IOKit step still required.

**What hardware can falsify.** A USB-SATA bridge whose IOMedia child
lags the drive's READY (so a query briefly reports READY with `-1` until
the node attaches) — a transient the watch self-corrects; same race the
open path already tolerates. Lands as an observation, never a per-device
special-case.
