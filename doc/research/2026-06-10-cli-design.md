# CLI human-output redesign (drutil-inspired)

> **Superseded in part (2026-06-14):** the default verb named `status`
> throughout this doc was renamed to `state` (clean break, no alias), and
> a bare drive selector (`mos 2`, `mos disk4`) now runs it without a verb
> word. Read every `mos status` below as `mos state`. Rationale + the
> digit-gated dispatch are in `doc/research/2026-06-14-state-verb-rename.md`;
> this doc is left as written (append-only history).

Status: IMPLEMENTED 2026-06-10 (pure layer container-verified with
golden-string tests; tools/adapter layer macOS-CI compile-gated). The plain-mode CLI predates everything else in the
tool (single bare word, single-drive assumption) and is replaced by a
drutil-inspired human format at field parity with `--json`.

## Decided

**One vocabulary, verbatim, zero mappings.** Human output uses the
JSON enum strings exactly (`empty_or_open`, never "Empty or Open" and
not `empty/open` either). A `/`-display mapping was adopted and struck
in the same review: as canon the slash fails (cannot be an identifier
or a POSIX filename — our own fixture
`mos.event.v1.empty_or_open.json` — and every consumer would carry the
inverse mapping), and as display-only it buys readability on an enum
you rarely see at the price of a transform table and render code that
otherwise need not exist. Verbatim wins on both fronts: one string,
one grep, no second column. State column width is set by
`media_unreadable` (16).

**Field parity.** The human view formats exactly the fields the
corresponding schema emits — no fabricated rows, no omitted ones.
Fields absent in JSON (suppression rules) are absent in the block.

**`mos` / `mos status` — aligned key block.** Right-aligned keys,
two-space gutter:

```
   State:  ready
 Profile:  0x0040  BD-ROM  (bd)
  Volume:  ARRIVAL_4K  (/Volumes/ARRIVAL_4K)
   Drive:  HL-DT-ST BD-RE WH16NS60 1.00  (/dev/disk4)
```

Priority doctrine, all human views, five tiers:
ANSWER -> EVIDENCE -> MEDIA -> ADDRESSING -> IDENTITY.
The state you asked for; its proof directly beneath (sense triplet,
error stage/code — evidence sits adjacent to the claim); what is in
the drive (profile, volume); how to address it (index, bsd, registry);
the nameplate last. `mos drive` inverts by design, not exception: its
ANSWER is identity. Live views carry `index` (rig-order addressing,
terminal-meaningful); archival documents (metadata) carry registry_id
but NOT index (order is meaningless next month).

Not-ready and error cases carry their JSON fields the same way
(`Sense: 02/3a/01` (raw triplet only — its decoded meaning IS the State line; no sense-text catalog exists or is wanted); `Stage:` /
`Code: 0xE00002C0  kIOReturnNotResponding`).

**`mos list` — the dashboard, WITH State.** The library is named
mac-optical-STATE; the overview verb surfaces it. An earlier
zero-command-list idea was rejected in review: that was a cost-table
entry mistaken for doctrine. Pre-pivot the state column costs one
proven probe per drive (ms each); post-pivot the signal stack resolves
most rows at zero commands anyway. Per-entry error containment: a
drive whose probe fails shows `error` in its row; one sick drive never
kills the rig overview.

```
 Index  State          Volume       BSD          Vendor    Product            Rev
     1  ready          ARRIVAL_4K   /dev/disk4   HL-DT-ST  BD-RE WH16NS60     1.00
     2  empty_or_open  -            -            PIONEER   BD-RW BDR-XS07     1.01
```

`Index` column header — decided `#`, then reversed in the same review:
the header matching the flag name exactly is the same one-vocabulary
rule the enum strings follow, and it makes the table self-documenting
(the one column whose meaning is not evident from its contents names
the selector it feeds). Right-aligned numerals under it.
Volume column lands at stage 1 (DA, mounted only — the original
identical-drives disambiguation ask).

**`bsd` everywhere, full node, field renamed.** CLI and JSON both
carry `/dev/disk4` (copy-paste / pipe-ready). Field name is bare
`bsd` — `bsd_path` rejected (the word "path" pulls filesystem-ward and
would sit next to `volume_path`, a mount point; diskutil's own term
for /dev/disk4 is "device node"); `bsd_name` rejected (Apple's BSD
name proper is the bare `disk4`, which we no longer emit). Bare `bsd`
gives exact identity across flag `--bsd`, field `bsd`, column `BSD`,
plus the round-trip property: a NON-NULL `bsd` value is always a valid
`--bsd` argument. Non-null matters: the whole-disk IOMedia node exists
only when media is present (seam clause V-1), so an empty drive has
`bsd: null` by definition — which is precisely why `--index` and
`--registry-id` exist as drive-level selectors. No `rdisk` field:
the raw node is the consumer's one-character transform, and emitting
it would advertise the block-device modality mos keeps out of scope.
mos.list.v1 entries gain `state` (and `volume_name` at stage 1);
schemas are pre-tag mutable, swept in one pass at implementation.

**`mos watch` — NDJSON only.** `--json` becomes a no-op for watch; the
plain token-per-line mode is REMOVED. A stream consumed by
orchestrators has one format; an interactive human runs `mos` twice.

**Multi-drive default.** One drive: no selector needed. More than one:
`mos status` without a selector exits EX_USAGE with a mini-list on
stderr ("N drives present; use --index or --bsd"). No first-burner
magic — the single-drive assumption dies here, predictably.

**Man-page line.** `mos list` talks to hardware (one probe per row
pre-pivot, lock only on the empty-vs-open fork); it is not a
registry-only command. One sentence, no silence.

## Open (decide at implementation)

- Sense line phrasing: raw KK/AA/QQ + decoded description (mocked) vs
  description only. Lean both — the raw triplet is what you compare
  against MMC tables.
- Pre-pivot Drive line in `status` costs the INQUIRY we already make;
  post-pivot it is cache. Lean show (field parity wins).
- Stage-1 `mos drive` / `mos metadata` human blocks: same aligned-key
  treatment mirroring their schemas; no divergence identified.

## Test plan

Golden-string render tests, table-driven like the JSON render suite,
pure (formatter takes the result struct, no I/O). CLI integration
tests updated for: list table shape, EX_USAGE multi-drive path, watch
plain-mode removal.

## Process notes (adopted this session)

Fuzz runs only when pure parsers change — regression armor, not
per-batch ritual. Amalgamation (dist/) regenerates at tag time only.
Per-batch gate: strict build + full test suite + ASan/UBSan.

## Full mock set (canonical — supersedes the fragments above)

Era tags: [now] = restyle of shipped verbs; [s1] = stage 1; [v0.4].
All mocks obey: verbatim enum strings everywhere (including
`current_profile_name` values like `bd_rom`), answer-first ordering,
`bsd` = full /dev node (null/`-` when absent), field parity with the
schema of the verb.

### mos / mos status  [now; Volume/Path lines s1]

ready, mounted:
```
   State:  ready
 Profile:  0x0040  bd_rom  (bd)
  Volume:  ARRIVAL_4K  (/Volumes/ARRIVAL_4K)
   Index:  1
     BSD:  /dev/disk4
Registry:  4295032831
   Drive:  HL-DT-ST BD-RE WH16NS60 1.00
```
(Drive line loses its bsd suffix — BSD has its own row; no field
appears twice. Registry renders DECIMAL, matching our JSON, not
ioreg hex — one vocabulary.)

ready, unmounted (the common rip state — volume null, suppressed):
```
   State:  ready
 Profile:  0x0040  bd_rom  (bd)
   Drive:  HL-DT-ST BD-RE WH16NS60 1.00  (/dev/disk4)
```

empty_or_open with sense (evidence directly under the answer):
```
   State:  empty_or_open
   Sense:  02/3a/01
   Index:  1
     BSD:  -
Registry:  4295032831
   Drive:  HL-DT-ST BD-RE WH16NS60 1.00
```

empty / open (post-GESN resolved):
```
   State:  open
   Drive:  HL-DT-ST BD-RE WH16NS60 1.00  (-)
```

probe error:
```
   State:  error
   Stage:  probe
    Code:  0xE00002C0  kIOReturnNotResponding
   Drive:  -  (-)
```

`--json` -> mos.state.v1 (current shape + decided renames/additions):
```json
{
  "schema": "mos.state.v1",
  "state": "ready",
  "bsd": "/dev/disk4",
  "current_profile": "0x0040",
  "current_profile_name": "bd_rom",
  "media_class": "bd",
  "volume_name": "ARRIVAL_4K",
  "volume_path": "/Volumes/ARRIVAL_4K",
  "vendor": "HL-DT-ST",
  "product": "BD-RE WH16NS60",
  "revision": "1.00"
}
```

### mos list  [now; Volume column s1]

```
 Index  State          Volume       BSD          Vendor    Product            Rev
     1  ready          ARRIVAL_4K   /dev/disk4   HL-DT-ST  BD-RE WH16NS60     1.00
     2  empty_or_open  -            -            PIONEER   BD-RW BDR-XS07     1.01
     3  error          -            /dev/disk6   ASUS      BW-16D1HT          3.10
```

`--json` -> mos.list.v1, entries gain `state` (+`volume_name` s1):
```json
{
  "schema": "mos.list.v1",
  "drives": [
    { "index": 1, "state": "ready", "volume_name": "ARRIVAL_4K",
      "bsd": "/dev/disk4", "vendor": "HL-DT-ST",
      "product": "BD-RE WH16NS60", "revision": "1.00" },
    { "index": 2, "state": "empty_or_open", "volume_name": null,
      "bsd": null, "vendor": "PIONEER",
      "product": "BD-RW BDR-XS07", "revision": "1.01" }
  ]
}
```

### mos watch  [now] — NDJSON only, no plain mode

```
{"schema":"mos.event.v1","event":"snapshot","registry_id":4295032831,"stream_open_ms":1746526503000,"seq":1,"ts":"2026-06-10T21:04:11Z","bsd":null,"state":"empty","prev_state":"unknown","current_profile":"0x0000"}
{"schema":"mos.event.v1","event":"state_changed","registry_id":4295032831,"stream_open_ms":1746526503000,"seq":2,"ts":"2026-06-10T21:04:38Z","bsd":null,"state":"loading","prev_state":"empty","current_profile":"0x0000"}
{"schema":"mos.event.v1","event":"state_changed","registry_id":4295032831,"stream_open_ms":1746526503000,"seq":3,"ts":"2026-06-10T21:04:52Z","bsd":"/dev/disk4","state":"ready","prev_state":"loading","current_profile":"0x0040","media_class":"bd"}
```

### mos drive  [s1]

```
  Vendor:  HL-DT-ST
 Product:  BD-RE WH16NS60
     Rev:  1.00
  Serial:  M63IBOA5100
    AACS:  yes  (version 68, bus encryption)
   Index:  1
     BSD:  /dev/disk4
Registry:  4295032831
```
("Bus Enc" dissolved, not relabeled: bus encryption is an AACS
feature-0x010D flag, so it folds into the AACS line as a qualifier —
grouping fixes what no label could. JSON keeps separate booleans.)

`--json` -> mos.drive.v1 (shape frozen in the media-info design doc;
`bsd` rename applies).

### mos metadata  [s1]

BD, unmounted (the rig case):
```
 Profile:  0x0040  bd_rom  (bd)
    Disc:  complete, 1 session, 1 track, not erasable
     TOC:  -
  Volume:  -
Timestamp: 2026-06-10 21:05:02 UTC
     BSD:  /dev/disk4
Registry:  4295032831
```

CD with TOC:
```
 Profile:  0x0008  cd_rom  (cd)
    Disc:  complete, 1 session, 12 tracks, not erasable
     TOC:  tracks 1-12, leadout 210895
  Volume:  -
Timestamp: 2026-06-10 21:07:44 UTC
     BSD:  /dev/disk5
Registry:  4295032831
```
(Timestamp label + human format: T->space, Z->" UTC" — a display
transform for non-enum data; JSON keeps RFC3339 `captured_at`.)

Human TOC is the summary line only; the full track table lives in
`--json` (mos.metadata.v1, shape frozen: disc fingerprint subtree,
toc required-nullable). Anyone hashing or tagging is a JSON consumer
by definition.

### mos tray {eject, close, lock, unlock}  [v0.4]

Success: SILENT, exit 0 — Unix convention; the action's receipt is
the exit code. `--json`: emits the POST-ACTION mos.state.v1 (state
after the verb completed) — confirmation with zero new schemas, and
orchestrators get the state they were about to query anyway. Human
`-v`: the status block, same formatter. Failure: the existing error
envelope / human error block, nonzero exit per sysexits.
```
$ mos tray eject --json
{"schema":"mos.state.v1","state":"open","bsd":null, ...}
```

### Usage failure (multi-drive, no selector)  [now]

```
$ mos
mos: 3 drives present; select one with --index or --bsd:
 Index  State          BSD          Vendor    Product            Rev
     1  ready          /dev/disk4   HL-DT-ST  BD-RE WH16NS60     1.00
     2  empty_or_open  -            PIONEER   BD-RW BDR-XS07     1.01
     3  error          /dev/disk6   ASUS      BW-16D1HT          3.10
```
-> stderr, exit 64 (EX_USAGE). The mini-list IS the list formatter
(minus Volume) — one table implementation.

### --brief  [KILLED in review]

The bare word does not survive. JSON is the machine interface; a
second machine interface in miniature is redundant surface, and the
shell one-liner reads fine through jq (`[ "$(mos status 1 --json |
jq -r .state)" = ready ]`). The old CLI's only output mode is thus
fully retired.

## Schema additions forced by the priority pass (pre-tag mutable)

- `mos.state.v1` += `registry_id`, `index`. The correlation gap was
  real before it was cosmetic: event.v1 identifies by registry_id and
  state.v1 carried no identity beyond a nullable bsd — a one-shot
  status and a watch stream of the same drive shared NO join key.
  Also fixes `--registry-id` discoverability (its values were visible
  nowhere in human output; now: `mos status -i N`).
- `mos.list.v1` entries += `registry_id` (machines need it for the
  selector; the HUMAN table deliberately does not grow the column —
  width — discovery path is status).
- `mos.metadata.v1` context += `registry_id` (correlate a capture to
  its watch session); deliberately NO index in archival documents.

## Positional subject (decided in review)

Single-subject verbs take the drive as a positional argument:
`mos status 2`, `mos status disk4`, `mos status /dev/disk4`,
`mos metadata 2`, `mos drive 1`, later `mos tray eject 2`. Precedent
is the dominant convention and platform-native: `diskutil info disk4`,
`smartctl -a /dev/sda`, `systemctl status <unit>`; drutil's `-drive N`
flag is the outlier we had copied unexamined. Disambiguation is
SYNTACTIC, never heuristic: all-digits -> index, anything else ->
bsd (normalizers already accept disk4 and /dev/diskN). `--index` and
`--bsd` remain as explicit flag forms; `--registry-id` is FLAG-ONLY
(large decimal would collide with the index grammar; it is a machine
selector). The collision still fails loud: `mos status 4295032831` =
"no drive at index 4295032831", EX_USAGE. The multi-drive usage
failure teaches the positional form ("select one, e.g. `mos status
2`") — the retry path is now shorter than the flag it used to teach.
