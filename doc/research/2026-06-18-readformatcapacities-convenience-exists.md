# `MMCDeviceInterface->ReadFormatCapacities` exists — the fifth raw CDB's "no convenience method" showing is false (2026-06-18)

**Status: surfaced for maintainer decision. Rebuts** the header finding in
`doc/research/2026-06-18-read-format-capacities-feasibility.md` (line 30, "no
convenience method — it is a raw verb") and therefore showing (a) of the AGENTS
ADR *"fifth raw CDB admitted — READ FORMAT CAPACITIES (0x23)"*. Per the
append-don't-edit rule this entry argues against the original on the merits; it
changes no code.

## The claim being rebutted

The feasibility doc and the ADR admit a **raw CDB** for READ FORMAT CAPACITIES
(0x23) on layer-1 showing (a): *no convenience method carries it*. The doc
derived that from **`ARCHITECTURE.md:870-873`** — and says so explicitly
(feasibility doc lines 32-39): it read §9.7's enumerated methods (`Inquiry`,
`TestUnitReady`, `GetConfiguration`, `ModeSense10`, `ReadTableOfContents`,
`ReadDiscInformation`, `ReadTrackInformation`, `ReadDVDStructure`,
`GetPerformance`), found no format-capacities entry, and concluded "no
`ReadFormatCapacities` / `GetFormatCapacities` wrapper." The code carries the
same claim (`src/mos_query.c:8`, `:376-380`: "no convenience method carries
0x23"). The same false premise also appears in
`doc/research/2026-06-13-read-capacity-feasibility.md:31,121` ("no
`ReadFormatCapacities` wrapper").

## The evidence it is false

`§9.7`'s list is **not** the `MMCDeviceInterface` inventory — it is the
illustrative subset §9.7 uses to contrast `GetTrayState`'s missing
`SCSITaskStatus*`/`SCSI_Sense_Data*` out-parameters (the masking-trap
argument). The **actual** `SCSITaskLib.h` `MMCDeviceInterface` carries a
`ReadFormatCapacities` convenience method, verified verbatim in two SDKs from
the `napieraj/MacOSX-SDKs` fork (interface UUID `1F651106-23CC-11D5-BBDB-
003065704866`, stable/append-only across the range):

```c
/* MacOSX10.5.sdk and MacOSX11.3.sdk,
   IOKit.framework/Headers/scsi/SCSITaskLib.h, MMCDeviceInterface: */
IOReturn ( *ReadFormatCapacities )( void * self,
                                    void * buffer,
                                    SCSICmdField2Byte bufferSize,
                                    SCSITaskStatus * taskStatus,
                                    SCSI_Sense_Data * senseDataBuffer );
```

The full 11.3 inventory is **20 methods** (10.5 had 19; 11.3 only appends
`SetStreaming`): the nine §9.7 lists, plus `SetWriteParametersModePage`,
`SetTrayState`, `GetSCSITaskDeviceInterface`, `GetPerformanceV2`, `SetCDSpeed`,
**`ReadFormatCapacities`**, `ReadDiscStructure`, `ReadDiscInformationV2`,
`ReadTrackInformationV2`, `SetStreaming`. `ReadFormatCapacities` has been
present since at least 10.5 — it is not a "future SDK adds a wrapper" case (the
exact contingency the feasibility doc, line 47-48, said would supersede the
finding). `ReadFormatCapacities` carries `SCSITaskStatus*` + `SCSI_Sense_Data*`
like the other command-issuing convenience methods, so it is **non-exclusive**
(no `ObtainExclusiveAccess`), same class as `GetConfiguration`/`ModeSense10`.

## Consequence

By mos's own layer-1 doctrine (convenience-first; a raw CDB requires a
documented showing that *no* convenience method can carry the information),
READ FORMAT CAPACITIES should be a **no-lock convenience read** through
`MMCDeviceInterface->ReadFormatCapacities`, **not** a raw CDB on the
`mos_raw_cdb` exclusive-access path. If converted:
- the one-raw-CDB count returns from **one-of-five to one-of-four** (GESN +
  two tray opcodes + INQUIRY);
- `mos_query_capacity`'s `formattable` read no longer needs the brief
  `ObtainExclusiveAccess`, so it works on a **mounted** disc too (the BUSY-on-
  mounted back-off the ADR documented disappears), and the profile-gating that
  exists only to keep the lock off the common path becomes optional;
- the §5.5 exclusive-lock surface the ADR reasoned about does not arise for
  0x23 at all.

## I cross-checked the other raw CDBs against the full header (not cherry-picked)

The incomplete-`§9.7`-inventory root cause could, in principle, undermine other
"no convenience method" showings. It does not — only 0x23 is affected:
- **GESN (0x4A):** the showing is *masking*, not absence — `GetTrayState`
  exists but is sense-blind (§9.7). Valid, unchanged.
- **Tray (START STOP UNIT 0x1B, PREVENT ALLOW 0x1E):** `SetTrayState` exists
  but is sense-blind and does eject/load only; there is **no**
  `PreventAllowMediumRemoval` wrapper in the header (10.5 or 11.3). Valid.
- **INQUIRY VPD 0x80 (serial):** the header's `Inquiry` takes only
  `SCSICmd_INQUIRY_StandardData *` — no EVPD/PAGE_CODE — so VPD 0x80 is
  genuinely unreachable through it (`src/mos_serial.c`). Valid.

So the rebuttal is bounded to the READ FORMAT CAPACITIES verb.

## What to confirm before converting (maintainer's call)

This is an architecture decision touching a shipped ADR + working code
(`src/mos_query.c`, `src/mos_formatcap.c`), so it is the maintainer's to make,
not a silent edit. One thing to verify before flipping 0x23 to the convenience
method: that `ReadFormatCapacities(buffer, bufferSize, …)` passes the **whole
reply through** (it takes a generic buffer + size, so the 4-byte Capacity List
header + the Current/Maximum descriptor + the Formattable Capacity Descriptor
list mos's `mos_formatcap.c` parses should arrive intact — but the wrapper's
internal allocation-length handling should be confirmed against a capture). The
pure parser (`mos_formatcap.c`) is unaffected either way — it decodes the same
reply bytes regardless of which path issues the command.

**Recommendation:** if accepted, convert `mos_internal_read_format_caps`
(`src/mos_query.c`) from `mos_raw_cdb` to `MMCDeviceInterface->ReadFormatCapacities`,
drop the exclusive-access gating there, and append a superseding ADR entry
(one-raw-CDB count → one-of-four). Pre-tag, so the schema/behaviour change is
mutable-in-place. If declined for a reason not captured here (e.g. the
convenience wrapper is known to truncate the descriptor list), record that
reason so the next reader does not re-discover the wrapper and re-open this.

## Update (2026-06-18): implemented

Maintainer approved. `mos_internal_read_format_caps` (`src/mos_query.c`) now
issues 0x23 via `MMCDeviceInterface->ReadFormatCapacities` instead of
`mos_raw_cdb`; the superseding AGENTS.md ADR records the count returning to
one-of-four. Comments in `src/mos_pure.h`, `src/mos_formatcap.c`, `SPEC.md`,
the README capacity section, and `schemas/mos.capacity.v1.json` updated to
match. **Confirmed on the latest SDK:** the full Tahoe (`macOS 26.4`) stack
carries `IOKit.framework/Headers/scsi/SCSITaskLib.h` with `kIOMMCDeviceInterfaceID`
intact and `MMCDeviceInterface->ReadFormatCapacities` at the same signature
(the header annotates it *"Added in Mac OS X 10.3"*). So the convenience method
spans **10.3 → 26.4** — the conversion holds on the current SDK, and mos's
convenience layer as a whole (which this rides) is intact in 26.4. (An earlier
optical-only subset of 26.4 lacked `SCSITaskLib.h`, which briefly looked like a
retirement; the full stack settles it — it was curation, not removal.)
