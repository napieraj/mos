# Drive serial (VPD page 0x80) — falsifier resolved from the header

**Date:** 2026-06-16. **Status:** research / decision note. Resolves the
long-open stage-1 falsifier carried in `doc/research/2026-06-10-media-info-design.md`
(item 3, lines 369–371) and `doc/research/2026-06-13-disc-tools-state-survey.md`
(the "does the convenience `Inquiry` surface VPD pages?" open item). Per the
README append rule this is a new dated file; the prior notes are not edited.

## The question

`mos.drive.v1` ships `serial: null` (cli/drive.c, schema `serial`
description). The field is the durable drive-inventory key — the one that
survives replug and machine moves, where `registry_id` is attachment-scoped
(media-info-design §"Durability note"). The open question, recorded twice
and never closed, was binary:

> Does Apple's convenience `Inquiry` (`MMCDeviceInterface`) surface VPD
> pages — specifically page 0x80, Unit Serial Number — or only standard
> INQUIRY?

If yes, the serial is a convenience-method read (no lock, no raw verb). If
no, it is a raw INQUIRY under the AGENTS scope-doctrine layer-1 raw-verb
rule. The 06-13 note said this needed "the modern vendored header
(dev-tree-only, stripped from this checkout) or hardware."

## Resolved from the header — no hardware required

The answer is provable from `SCSITaskLib.h` and needs neither the stripped
`docs/apple/` copy nor a Mac. The §11 public mirror
(`phracker/MacOSX-SDKs`, MacOSX10.2.8.sdk; §11 records its method
signatures as identical to the modern SDK) gives the `MMCDeviceInterface`
`Inquiry` pointer verbatim:

```c
IOReturn ( *Inquiry )( void *                         self,
                       SCSICmd_INQUIRY_StandardData * inquiryBuffer,
                       UInt32                         inqBufferSize,
                       SCSITaskStatus *               taskStatus,
                       SCSI_Sense_Data *              senseDataBuffer );
```

The buffer is typed `SCSICmd_INQUIRY_StandardData *`. There is **no EVPD
bit and no PAGE_CODE parameter** — contrast the same interface's
`ModeSense10`, which *does* take `PC` and `PAGE_CODE` fields, and
`GetConfiguration`, which takes `RT` + `STARTING_FEATURE_NUMBER`. The
convenience `Inquiry` can only ever issue a standard INQUIRY (EVPD=0,
PAGE CODE=0). A full read of the header finds **no separate VPD / unit-
serial method anywhere**.

**Conclusion: VPD page 0x80 is structurally unreachable through the
convenience method.** The falsifier is closed in the "no" direction. No
hardware run can change this — it is a header fact, the same class of
proof as the §9.7 `GetTrayState`-masking finding and the 06-13 read-
capacity note ("`MMCDeviceInterface` has NO READ CAPACITY wrapper").

## The cheap, no-raw-CDB paths also don't carry it (three of them)

Before reaching for a raw CDB, the three no-command sources are ruled out
— including the IOKit-property path, which is the non-obvious one (a serial
need not be *called* "serial", the way firmware hides under "revision"):

1. **DiscRecording `DRDeviceCopyInfo`** — the zero-command identity dict
   that already feeds vendor/product/revision. Its documented key set
   (`doc/dr-field-mapping.md`, "Identity / device-static") is
   SupportLevel / IORegistryEntryPath / VendorName / ProductName /
   FirmwareRevision / PhysicalInterconnect{,Location} / WriteCapabilities /
   LoadingMechanism{CanEject,CanInject,CanOpen} / WriteBufferSize.
   **No serial key.** Re-verified 2026-06-16 against the live modern header
   (`DRCoreDevice.h`, MacOSX15.5.sdk public mirror, §11): the string
   "serial" does not appear anywhere in the file, and no `DRDevice*`
   function or key exposes a unit serial. The 06-10 design's "DR has no
   serial key and predates AACS" holds.

   **The 06-13 survey's "drutil … serial" claim is wrong — checked
   2026-06-16.** It was the basis for asking this whole question ("drutil is
   a DR wrapper, so if it shows a serial, DR must carry one"). It does not.
   drutil's DiscRecording-sourced identity output is vendor / product / rev
   / bus / support-level — the exact `DRDeviceCopyInfo` key set above, no
   serial column (`drutil list` / `status` / `info`); the documented
   behavior even notes status "does not directly display the serial number."
   And the premise is false anyway: drutil is **not** a pure DR wrapper — its
   `getconfig` / `trackinfo` / `subchannel` / `cdtext` subcommands dump raw
   MMC command output, so it issues SCSI directly. If any drutil surface ever
   printed a serial it would be drutil's own raw INQUIRY, not a DR key. Either
   way `mos`'s zero-command DR surface cannot carry the serial — confirmed,
   not assumed.

2. **Convenience `Inquiry` standard-INQUIRY tail (bytes 36+).** Standard
   INQUIRY data has a vendor-specific region from byte 36, and
   `INTEGRATION_HARNESS.md` notes drive serials "commonly live" in bytes
   36–43. A larger `inqBufferSize` would return that tail with no lock. This
   path is **rejected**: the byte-36 region is vendor-specific in offset,
   length, and presence — it is not the standardized Unit Serial Number
   format the schema's `serial` field claims ("VPD page 0x80"), and decoding
   it would be exactly the per-device-quirk special-casing the hardware-role
   ADR (AGENTS.md) forbids. It would also re-introduce a convenience INQUIRY
   call that the DR pivot retired (ARCHITECTURE §9.6). Vendor-tail bytes are
   a redaction concern for fixtures, not a portable serial source.

3. **IOKit IORegistry "Serial Number" property (checked 2026-06-16).** This
   is the right place to look — mos already walks IOKit at open (bsd_unit,
   registry_id), and a serial could ride a non-obvious key. IOKit *does*
   define one: `kIOPropertyProductSerialNumberKey` = `"Serial Number"`,
   inside the `kIOPropertyDeviceCharacteristicsKey` ("Device
   Characteristics") dictionary (`IOStorageDeviceCharacteristics.h`,
   alongside the Vendor/Product/Revision keys the kernel *does* populate).
   So the slot exists. **The optical stack never fills it.** Verified
   against the open-source kernel layers that build an optical drive's
   IORegistry node (apple-oss / aosm `IOSCSIArchitectureModelFamily`):
   - `IOSCSIPrimaryCommandsDevice.cpp` — the SCSI base class for every
     peripheral type including optical — parses **standard** INQUIRY and
     sets vendor/product/revision into Device Characteristics; it issues
     **no** INQUIRY for VPD page 0x80 and sets **no** serial key. (Standard
     INQUIRY carries no serial; VPD 0x80 is a separate command the kernel
     does not send.)
   - `IOSCSIProtocolServices.cpp` — sets no serial key.
   - `IOATABlockStorageDevice.cpp` (legacy ATA transport) — sets
     vendor/product/revision, **not** serial, and does not read ATA
     IDENTIFY words 10–19.

   Where the key *is* populated is the block-storage/disk path: AHCI
   (`IOAHCIBlockStorage`, a closed binary kext) sets it for SSDs/HDDs from
   ATA IDENTIFY — the hard-disk stack, not the optical one. Net: for an
   optical drive the IORegistry "Serial Number" is generally **absent**.
   The one place it might appear is a USB-attached drive, where the USB
   mass-storage transport can surface the **bridge's** `iSerialNumber`
   descriptor — the enclosure's identity, not the drive's MMC unit serial,
   and exactly the "unreliable through USB bridges" identity the
   drutil-contract note (2026-06-10) already put out of scope. So the IOKit
   property is at best a provenance-impure, often-null best-effort read, and
   conflating it with the schema's `serial` ("VPD page 0x80") would mix two
   different identities under one key. It is **not** a substitute for the
   drive serial; if ever surfaced it must be a distinct, clearly-named
   field. The reason this path fails is the load-bearing one: **the kernel
   does not read VPD 0x80 for us**, so there is no free serial to harvest.

## Therefore: serial = raw INQUIRY VPD 0x80, under the layer-1 raw-verb rule

The only spec-standardized route is a raw INQUIRY CDB with EVPD=1,
PAGE CODE=0x80. That puts the serial squarely under AGENTS.md scope-
doctrine layer 1 (raw CDB requires two showings). Status of each:

- **(a) "No convenience method can carry the information" — SATISFIED**
  by the header finding above. This is the same shape of showing the tray
  verbs made via `SetTrayState`'s sense-blindness (§9.7/§9.9) and the
  read-capacity note made via the missing READ CAPACITY wrapper. The
  masking/absence analysis *is* the showing.

- **(b) Nub-collision / exclusive-access analysis — straightforward, with
  one real consequence.** Raw means `mos_raw_cdb`, the single
  `ObtainExclusiveAccess` call site (ARCHITECTURE §3), which returns
  `kIOReturnBusy` on mounted media. INQUIRY itself is a non-media command
  (it touches no IOMedia nub and creates no §5.5 interleaving exposure — it
  is safer in that respect than the GESN tray probe, which is gated on
  not-ready precisely to dodge the mounted case). The consequence is the
  same one the read-capacity note flagged: **the serial is unreadable while
  a disc is mounted** (the open returns BUSY). Unlike capacity, this is
  benign — the serial is a static drive fact, equally true with the tray
  empty, and the natural time to inventory a drive is when it has no disc.
  The degradation is graceful: `serial` stays `null` on BUSY, exactly as it
  is today, and the field's existing null semantics already cover it. No
  atexit, no lock lifetime question (INQUIRY changes no state), so none of
  the tray-verb PREVENT-lifetime machinery applies.

This makes the one-raw-CDB count GESN + the two tray opcodes + INQUIRY
(one-of-four), with `mos_raw_cdb` still the sole exclusive-access site.

## Implementation shape (if pursued — not built here)

Per Process rule 2 and the hardware-role ADR this note does not change
behavior; it records the design so a future session can execute it. The
schema field, human row, and JSON key already exist (drive.c, schema), so
the work is additive plumbing, not a contract change.

- **CDB (SPC INQUIRY, 6-byte):** `12 01 80 AA AA 00` — opcode 0x12,
  byte1 bit0 EVPD=1, byte2 PAGE CODE=0x80, bytes3–4 ALLOCATION LENGTH
  (big-endian; a small fixed buffer, 0xFF–0x100 is ample for a serial),
  byte5 CONTROL=0. Issued through `mos_raw_cdb(... MOS_XFER_FROM_TARGET ...)`.

- **Pure parser** (`src/mos_vpd80.c` or fold into an existing pure TU),
  matching the established decoder pattern (bounds-checked, hostile-input,
  fuzzed against the full octet domain):
  - Validate byte1 == 0x80 (the drive echoes the page code; reject a drive
    that answered standard INQUIRY or a different page).
  - Serial length = byte3 (PAGE LENGTH), **bounded by the actual transfer
    count** — the repo's dual-length validity rule (O-4): trust the reply's
    own length field only up to the bytes actually returned.
  - Bytes 4..(3+len) are the ASCII serial; trim trailing spaces/NULs per
    the existing identity-string handling, and escape at the output sink
    (`mos_cli_json_str` / `mos_safe_ascii`) like every other identity
    string (§9.6). All-spaces / zero-length → `null` (a drive that supports
    the page but has no serial programmed is common; null, not empty
    string).
  - Confirm the exact byte offsets against SPC-4 (Unit Serial Number VPD
    page) at implementation and add the citation to `SPEC.md` under a new
    `src/mos_vpd80.c` entry; the parsed offset table stays inline per the
    SPEC.md ADR.

- **Tests** (`tests/`, pure layer, no IOKit): a positive fixture (a real
  page-0x80 reply, ideally captured hex per the fixture pipeline), plus
  negatives — wrong page code echoed, PAGE LENGTH overrunning the transfer
  count, all-spaces serial, zero-length, non-ASCII bytes. These link
  `mos_pure` only, same as the other parser tests.

- **CLI wiring** (cli/drive.c): replace the hard-coded `serial: null` /
  `"Serial", NULL` with the parsed value when the raw INQUIRY succeeded,
  null on BUSY/failure. A new `mos_query_*` accessor on the handle, in the
  `mos_query.c` verb surface, keeps drive.c free of transport detail.

- **Fixture redaction note:** capturing a page-0x80 fixture commits a real
  drive serial. `INTEGRATION_HARNESS.md` already calls serials out as the
  redaction-worthy region; a captured 0x80 fixture should use a synthetic
  serial, with the README fixture entry saying so.

## What hardware can falsify, never establish

Per the hardware-role ADR, a run can refute the design but never steered
it. Candidate falsifiers, each landing as a fixture + dated note with a
generic defense, never a per-device special-case:

- A drive that does not implement page 0x80 (legal — the page is not
  mandatory) answers 5/24/00 INVALID FIELD IN CDB or returns a different
  page; the parser's page-code echo check classifies it to null. Expected,
  not a defect.
- A USB-SATA bridge that synthesizes a bogus or truncated page-0x80 reply
  — handled by the dual-length gate and the ASCII trim, no special case.
- A drive whose serial is in the standard-INQUIRY vendor tail but *not* in
  page 0x80 — out of scope by the rejection above (vendor-tail is not a
  portable serial); it stays null, correctly.

## Decision / recommendation

The blocking unknown is resolved: **the serial requires a raw INQUIRY VPD
0x80; no convenience or zero-command path can deliver it.** Whether to
*build* it is a scope call, not a feasibility one:

- **For:** it is the schema's stated durable inventory key (the whole
  reason `mos.drive.v1` opens at all was "the two fields DR lacks: serial
  and AACS capabilities" — media-info-design §table). AACS shipped; serial
  is the other half, still null. The layer-1 showing is complete and the
  raw verb is *simpler* than the tray verbs (no state change, no lock
  lifetime, INQUIRY-on-empty is the normal case).
- **Against / scope:** it adds a fourth raw CDB and a new parser+fixtures
  for a field whose only consumer is cross-machine drive inventory. If no
  consumer needs it yet, leaving it null is honest and costs nothing — the
  field and its null semantics already ship.

Recommendation: **build it when an inventory consumer materializes**; until
then the field stays null with this note as the resolved-feasibility
record. The expensive unknown (does the platform even let us read it
without a convenience method? what surface?) is now answered, so the build
is a bounded, spec-grounded task whenever it is wanted. Pickup checklist:
CDB above → pure parser + SPEC.md entry → negative fixtures → `mos_query_*`
accessor → drive.c null→value → synthetic-serial capture fixture.

## Update (2026-06-16): build authorized for `mos drive`; `mos state` out; `mos watch` deferred

The recommendation above ("build when an inventory consumer materializes")
was overridden by the maintainer the same day: build it now, in `mos drive`.
The consumer-materialization test is satisfied in the obvious way — the
operator running `mos drive` *is* the inventory consumer; the verb is a
deliberate "tell me about this drive" ask, not a polled hot path. This
section records what shipped, what was deliberately excluded, and the one
extension that is **deferred, not declined**, with its design captured so a
later session can execute it without re-deriving the conversation.

### Shipped: serial as a `mos drive` fact

`mos.drive.v1.serial` now carries the VPD-0x80 read. Files:
`src/mos_vpd80.c` (pure decoder), `src/mos_serial.c` (the `mos_query_serial`
verb — named for the datum, not the generic INQUIRY command set),
`cli/drive.c` wiring, plus the schema/example/README/SPEC.md/`dist/`
updates. The raw read self-gates: `mos_raw_cdb` calls `ObtainExclusiveAccess`
first and, if anything holds the drive (a mounted IOMedia nub, another
client), returns BUSY *without issuing the CDB* — so even a `mos drive`
run mid-rip degrades to `serial: null`, never a disturbed mount. The lock
is held only for the one INQUIRY and released on every exit path.

### Excluded by decision: `mos state`

Serial is NOT folded into `mos_query_state` / `mos_state_result`. That path
is frequent and polled (it backs `mos state` and the watch loop), and it
keeps a deliberate **no-lock-on-READY** shape: a GOOD TUR short-circuits to
READY with no exclusive access; the raw GESN is taken only on the not-ready
branch, where "not ready ⇒ not mounted ⇒ lock is free"
(`mos_state_core.c:123`). Folding serial into the shared result would add a
raw INQUIRY + a lock-*attempt* to the currently lock-free READY path on
every state query — the wrong cost profile for a hot path, and it would
leak serial into `mos.state.v1`. (Note for the record: an earlier draft of
this analysis mis-stated the state path as "never takes a lock"; it does, on
the not-ready branch. The correct invariant is no-lock-*on-READY*, and the
serial grab must ride that same "raw only when nothing can be harmed"
discipline — which `mos_query_serial`'s BUSY back-off already enforces.)

### Deferred (not declined): serial in `mos watch`

Watch is the one place a *cache* earns its keep: a single-shot `mos drive`
opens, reads, closes, so caching buys nothing there, but a watch session is
resident and bound to a `registry_id`, and the serial is immutable for that
id. The design below is sound and feasibility-clear; it is deferred only
because no watch consumer needs the serial in the event stream *yet*, and
because it is a `mos.event.v1` schema evolution plus a raw command in the
poll loop — worth landing deliberately, not reflexively. When a consumer
materializes, this is the build:

- **Grab once per `registry_id`, piggybacked on the probe handle.**
  `watch_probe` / `watch_slot_probe` (`src/mos_watch.c`) already reopen a
  handle by `registry_id` every poll and cache device-static identity
  (vendor/product/revision) once per target/slot. Serial slots in the same
  way: on each probe, if serial is not yet grabbed, call `mos_query_serial`
  on the handle the probe **already opened**; on success cache it in a new
  `serial[64]` field (single-target block + `mos_watch_slot`) and stop
  trying; on BUSY/IO leave it ungrabbed and retry next poll. Zero extra
  opens; at most one successful raw INQUIRY per session per id.

- **Self-gated, never disturbs a mount.** The grab inherits
  `mos_query_serial`'s BUSY back-off, so attempting it on every probe until
  the first success is safe: on mounted/ready media `ObtainExclusiveAccess`
  fails and the CDB never issues; on the first empty/not-ready poll — where
  the walk's lock is already free and the serial needs no disc — it lands
  and sticks. Consequence to document: `serial` is `null` in early event
  lines until a free window occurs, then populated for the rest of the
  session.

- **Watch-adapter-scoped, NOT in `mos_state_result`.** Carry serial on
  `mos_watch_event` and re-home it to the watch-static buffer before
  `mos_close` (the pointer-lifetime invariant at `src/mos_watch.c:165`),
  exactly as vendor/product/revision are. Keeping it off the shared state
  result is what preserves the `mos state` exclusion above.

- **Schema:** add a nullable `serial` to `mos.event.v1` on the
  snapshot / state_changed / media_changed field sets; **absent** from
  `error` and `device_removed` (mirrors how those events already forbid
  vendor/product/revision). Pre-first-tag, so mutable-in-place: emitter +
  examples + negatives + docs in one commit.

- **`--all` hot-plug:** per-slot by construction — a replug re-mints
  `registry_id`, so the new slot grabs a fresh serial; no stale carry-over.

- **Pickup checklist (watch):** `serial[64]` on the single-target block and
  `mos_watch_slot` → grab-once logic in `watch_probe` / `watch_slot_probe`
  → `serial` on `mos_watch_event` + re-home before close → `mos.event.v1`
  schema + examples + negatives → `cli/watch.c` emit → README/event-doc
  note on the null-until-free-window behavior.
