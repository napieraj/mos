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

*Correction (2026-06-14, same day — this rationale was wrong twice and
is rewritten here; the wrong drafts are described so the audit trail
shows what was rejected and why.)*

DR is already linked and used, and the DR media-BSD read already exists:
`mos_internal_dr_bsd_unit_from_status` (`mos_dr.c`) pulls exactly
`kDRDeviceMediaInfoKey → kDRDeviceMediaBSDNameKey` out of
`DRDeviceCopyStatus`, feeding enumeration today. Two rejected arguments:

- **Draft 1 — "DR is new surface."** Wrong: see the helper above. DR and
  the media-BSD read are already in the tree.
- **Draft 2 — "the no-DR-passthrough constraint forbids DR here."** Also
  wrong. That constraint (ROADMAP "Standing constraints"; ARCHITECTURE
  §9.7) is specifically about the **synchronous STATE classification**
  not deferring to DR's stale **tray / media-present bits** — the
  `GetTrayState` masking lineage, where DR's GESN-fed snapshot "may
  PRE-ANSWER, never OVERRULES a fresh mos GESN" (media-info-design
  signal-stack). It is scoped to the tray/state bits, NOT to identity
  fields like the media BSD name — which mos already sources from
  `DRDeviceCopyStatus` on the enumeration path. There is no doctrinal bar
  to reading `bsd_unit` from DR.

**The one reason that actually holds.** `media_id` (the F1 swap
fingerprint) has no DR equivalent — `doc/dr-field-mapping.md` records
that DR carries the registry *path*, not the entry ID, so refreshing it
needs an IOKit step regardless (`IORegistryEntryFromPath →
GetRegistryEntryID`). The IOMedia whole-disk node yields `bsd_unit`,
`media_id`, `media_bytes`, and `media_block` together from one atomic
traversal. Since IOKit is unavoidable for `media_id`, sourcing `bsd_unit`
from DR's status dict would add a *second* source for a field the one
walk already returns — a redundant hybrid that can skew the
(bsd_unit, media_id) pair across a swap, not a saving. So the choice is
single-source pragmatics, not doctrine: reuse `mos_internal_bsd_unit`,
the exact resolver the open path and the watch's reopen-per-probe already
trust. Cost is a local IORegistry walk — no SCSI command, no exclusive
access, cheaper than the watch's per-probe full reopen this mirrors.

A maintainer who prefers the ROADMAP's DR-named form could adopt it; the
only real cost is that hybrid two-source read. It is a legitimate
alternative, not a forbidden one — the earlier drafts that implied
otherwise were overreach.

**What hardware can falsify.** A USB-SATA bridge whose IOMedia child
lags the drive's READY (so a query briefly reports READY with `-1` until
the node attaches) — a transient the watch self-corrects; same race the
open path already tolerates. Lands as an observation, never a per-device
special-case.
