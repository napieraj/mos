# JSON Schemas for the mos CLI contract

This directory contains the formal JSON Schema documents for every
machine-readable envelope emitted by `mos`. They make the contract
described in the `cli/` emitters and `ARCHITECTURE.md` mechanically
checkable, rather than only described in prose.

## Files

```
schemas/
├── mos.state.v1.json     # success envelope from `mos [status] --json`
├── mos.error.v1.json     # failure envelope from any one-shot operation
├── mos.list.v1.json      # `mos [list] --json` enumeration result
├── mos.event.v1.json     # NDJSON line from `mos [watch] --json`
├── mos.metadata.v1.json  # disc-identity record from `mos metadata --json`
├── mos.drive.v1.json     # static drive facts from `mos drive --json`
├── mos.features.v1.json  # raw MMC feature list from `mos features --json`
├── mos.capacity.v1.json  # capacity record from `mos capacity --json`
├── mos.tray.v1.json      # tray-control outcome from `mos tray <action> --json`
├── examples/             # positive fixtures (must validate)
├── negative/             # negative fixtures (must be rejected)
├── validate.py           # validator script — used by CI, runnable locally
├── check_readme.py       # checks the top-level README examples vs the emitters
└── requirements-ci.txt   # pinned validator dependencies for CI
```

All schemas use JSON Schema draft 2020-12.

## Versioning rule

One schema = one version. When breaking changes are needed, a new
schema file is added (e.g. `mos.state.v2.json`) and the producer
emits the new default; consumers dispatch by the `schema` field of
the document, not by a CLI flag.

Within a single major version, the field set is **fixed**: each
schema uses `additionalProperties: false`, so unknown fields are a
schema violation. Future additions go through a new schema name.
This is stricter than "additive within a version" and is deliberate
— it makes silent drift impossible.

## Running locally

```sh
pip install jsonschema
python3 schemas/validate.py
```

The script validates every fixture in `schemas/examples/` against the
schema implied by its filename prefix (e.g. `mos.state.v1.foo.json`
validates against `mos.state.v1.json`), and asserts that every
fixture in `schemas/negative/` is rejected. CI runs the same script.

## Adding a new fixture

Positive fixture (must validate): drop a JSON file in
`schemas/examples/` whose name begins with the schema family
(`mos.state.v1.something.json`, `mos.event.v1.something.json`).

Negative fixture (must be rejected): same naming, under
`schemas/negative/`. The filename should describe the violation
(`mos.state.v1.bad_bsd_name.json`).

The validator picks up new fixtures automatically — no need to
register them.

## Relationship to the emitter

The schemas are derived from the emit code in `cli/` (status.c, common.c, watch.c)
(`emit_json`, `emit_unknown_and_fail`, `emit_list_json`,
`emit_watch_ndjson`). The validator script catches the case where the
schema and the emit code drift apart, but the schemas are not
generated from the code. When the emit code changes, update both
the schema and at least one example fixture in the same commit.
