# Design: disc class on not-ready media from the kernel Type key (2026-06-18)

Axis 2 of the cache-fallback plan, taken forward. `mos.state.v1` /
`mos.event.v1` `media_class` is null for every **non-ready** state, by design:
the profile is suppressed off the not-ready branch (stale-profile rule,
ARCHITECTURE §9) and `media_class` is coupled to it
(`mos.state.v1` allOf: *"current_profile_name and media_class are present only
when current_profile is non-zero"*). So a `loading` / `busy` /
`media_unreadable` event says nothing about what the disc *is*.

The kernel already knows. It publishes the disc's media type on the
`IO{CD,DVD,BD}Media` node as `kIO{CD,DVD,BD}MediaTypeKey` (`"Type"`, OSString:
`CD-ROM/-R/-RW`, `DVD-ROM/-R/-RW/+R/+RW/-RAM/HD DVD-*`, `BD-ROM/-R/-RE`) —
**fresh, zero MMC, no exclusive access**, off the same node mos already reads
`kIOCDMediaTOCKey` from.

## Mechanism (fail-safe; no hardware gate)

Read the Type key off the current media node (`mos_internal_refresh_media_identity`
already navigates there), map the string to a class, and use it where the
profile-derived class is unavailable. **Present → enrich; absent → unchanged
from today.**

Why this needs no `ioreg`/hardware confirmation before building:
- **No failure mode.** A registry read returns a value or nothing; an unknown
  or hostile string maps to null (input-space layer 4, fail-closed). It can
  only add information, never corrupt or remove it.
- **No stale-leak — it is compatible with the §9 rule, not a violation of its
  intent.** The §9 rule guards against the *drive* caching the last disc's
  profile for minutes after eject. The kernel tears down the media node on
  media change / eject (no media → no node → no Type), so the Type key cannot
  report an ejected disc's class. It is *safer* than the thing §9 defends.
- **Built to a verified contract.** The Type key is in the SDK from 10.5
  through macOS 26.4 (Tahoe). Per the hardware-role ADR, hardware *falsifies*
  (a capture showing Type absent-when-expected becomes a fixture + note) and
  measures *yield* (how often it is present in the not-ready window) — neither
  is a build precondition. mos has never run on hardware; it is built to spec
  and source, and this is source.

## The real gate: the profile↔class coupling (a schema decision)

Filling `media_class` while `current_profile` is `0x0000` violates the
published allOf above. Two ways to resolve it:

- **Option A — fill `media_class`, relax the coupling.** Loosen the allOf so
  `media_class` may be present with a zero profile when registry-sourced.
  Honors the "fill the field we leave null" framing directly. **Cost:** changes
  a shipped contract — a consumer relying on "`media_class` present ⇒ profile
  present / READY" breaks; and `media_class` then means two things
  (profile-derived OR registry-derived) a consumer cannot tell apart.

- **Option B — new `media_type` field (recommended).** Add an
  independently-nullable field carrying the kernel Type as a normalized token
  (`cd_rom`/`cd_r`/`cd_rw`/`dvd_rom`/…/`bd_rom`/`bd_r`/`bd_re`), present whenever
  the kernel publishes it (including not-ready), **not** coupled to profile.
  `media_class` is untouched. Cleaner: no contract change; honest provenance (a
  distinct field = a distinct source); and it carries **more** than
  `media_class` — ROM-vs-recordable granularity (`bd_r` vs `bd_re` vs `bd_rom`)
  that `media_class` (`bd`) throws away. A consumer wanting "what is it, even
  when not ready" reads `media_type`; the profile↔class invariant stays intact.

**Recommendation: B.** It adds information without redefining an existing field
or breaking the coupling, and the Type string is strictly richer than
`media_class`. Choose A only if a consumer must receive the answer *through*
`media_class` and will not read a new field.

## Cost (either option)

A schema change to `mos.state.v1` + `mos.event.v1` (and the same field is a
natural mirror in `mos.metadata.v1`/`mos.drive.v1` if wanted) → an ADR, the
C↔schema drift guard extended to the new token table, positive + negative
fixtures, the emitter, and the docs, per the JSON-schema ADR (pre-tag, so
mutable-in-place). New code is small and command-free: read the Type key in the
IOKit shell (`mos_scsi.c`, beside the `kIOCDMediaTOCKey` read) + a pure
string→token map (`mos_strings.c`, fail-closed on unknown). No new command, no
exclusive access, no raw CDB — it rides the existing registry-read modality.

## Open decision

A vs B is the maintainer's call (it is a schema-contract decision). Everything
else — mechanism, safety, no-hardware-gate, the read site — is settled above.
Once chosen, implementation is one shell read + one pure map + the schema/ADR/
fixture/emitter/doc set.
