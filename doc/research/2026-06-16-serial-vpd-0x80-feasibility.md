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

## The two cheap paths also don't carry it

Before reaching for a raw CDB, the two no-command sources are ruled out:

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
