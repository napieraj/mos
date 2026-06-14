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
registry walk."

*Correction (2026-06-14, same day — a reviewer caught the original
rationale here overstating the cost of the DR form).* DR is already
linked and used, and the DR media-BSD read already exists:
`mos_internal_dr_bsd_unit_from_status` (`mos_dr.c`) pulls exactly
`kDRDeviceMediaInfoKey → kDRDeviceMediaBSDNameKey` out of
`DRDeviceCopyStatus`, feeding enumeration today. So the `bsd_unit` half
of the DR plan is NOT new surface — the first draft of this note framed
it as such and was wrong. The registry re-resolve is still the right
choice, for two reasons that survive that correction:

1. **DR can't supply `media_id`, so a DR refresh would be a HYBRID.**
   `media_id` (the F1 swap fingerprint) has no DR equivalent —
   `doc/dr-field-mapping.md` records that DR carries the registry *path*,
   not the entry ID, so it still needs an IOKit step
   (`IORegistryEntryFromPath → GetRegistryEntryID`). A DR-based refresh
   would read `bsd_unit` from the DR status dict and `media_id`/size from
   IOKit — two sources that can skew. The single IOMedia walk yields all
   four fields (bsd_unit, media_id, media_bytes, media_block) from one
   traversal of the same kernel node, consistently.
2. **The synchronous query path must not consume DR's snapshot — the
   no-DR-passthrough constraint.** Standing rule (ROADMAP "Standing
   constraints"; AGENTS division-of-labour): DR hands over "a passive,
   GESN-fed snapshot, *not guaranteed current*"; "the MMC state engine
   must not become a DR passthrough." `bsd_unit` feeds `mos_query_state`,
   the synchronous, fully-checked path — pulling its identity from DR's
   not-guaranteed-current status dict is exactly that passthrough. The
   IOMedia node is the kernel's current media object; the walk reads
   truth, not a cached snapshot. (DR's status dict is the right source
   for the *enumeration* snapshot, where coarse + possibly-stale is the
   stated contract — which is why `mos_internal_dr_bsd_unit_from_status`
   lives on that path and not this one.)

Reuse also keeps risk low: `mos_internal_bsd_unit` is the exact resolver
the open path and the watch's reopen-per-probe already trust, and adapter
code is macOS-CI-compile-gated only. Cost is a local IORegistry walk off
the drive node — no SCSI command, no exclusive access, cheaper than the
watch's per-probe full reopen this mirrors; short on an empty drive.

So the DR-dictionary form is not a deferred optimization with a cost edge
— for the synchronous identity path it is the *wrong* source by the
project's own division of labour. It stays where it belongs: enumeration.

**What hardware can falsify.** A USB-SATA bridge whose IOMedia child
lags the drive's READY (so a query briefly reports READY with `-1` until
the node attaches) — a transient the watch self-corrects; same race the
open path already tolerates. Lands as an observation, never a per-device
special-case.
