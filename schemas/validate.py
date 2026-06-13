#!/usr/bin/env python3
"""Validate every example payload against its schema and verify that
every negative fixture is correctly rejected.

The schema family is identified by filename prefix: a fixture named
`mos.state.v1.foo.json` validates against `mos.state.v1.json`.

Exit codes: 0 on full success, 1 if any positive case fails to
validate or any negative case unexpectedly validates.

Used by CI; runnable locally with `python3 schemas/validate.py`.
"""
import json
import re
import sys
from pathlib import Path

try:
    from jsonschema import Draft202012Validator
except ImportError:
    print("jsonschema package not installed. `pip install jsonschema`",
          file=sys.stderr)
    sys.exit(2)


def schema_name_from_filename(name: str) -> str:
    """Extract schema family name from a fixture filename.

    mos.state.v1.example.json -> mos.state.v1
    mos.event.v1.snapshot.json -> mos.event.v1
    """
    m = re.match(r'^(mos\.[a-z_]+\.v\d+)\.', name)
    return m.group(1) if m else ""


def check_state_enum_drift(here: Path) -> int:
    """Guard against the C state-string table and the JSON schema state
    enums drifting apart — the exact failure where a new mos_state_enum
    value is emitted by the program but rejected by mos.event.v1. The
    source of truth is mos_state_description() in src/mos_strings.c; both
    schema enums (mos.state.v1 state, mos.event.v1 state + prev_state)
    must equal that set. Returns a failure count."""
    src = (here.parent / "src" / "mos_strings.c").read_text()
    # Isolate the mos_state_description body, then pull each returned literal.
    m = re.search(r'mos_state_description\b.*?\{(.*?)\n\}', src, re.DOTALL)
    if not m:
        print("  FAIL drift-check: could not locate mos_state_description in mos_strings.c")
        return 1
    c_states = set(re.findall(r'return\s+"([a-z_]+)"', m.group(1)))

    def enum_at(path: Path, key: str) -> set:
        node = json.loads(path.read_text())["properties"][key]
        return set(node["enum"])

    state_schema = enum_at(here / "mos.state.v1.json", "state")
    event_state  = enum_at(here / "mos.event.v1.json", "state")
    event_prev   = enum_at(here / "mos.event.v1.json", "prev_state")
    list_state   = set(json.loads((here / "mos.list.v1.json").read_text())
                       ["properties"]["drives"]["items"]
                       ["properties"]["state"]["enum"])

    print("\nState-enum drift (C string table vs schema enums):")
    failures = 0
    for label, s in (("mos.state.v1.state", state_schema),
                     ("mos.event.v1.state", event_state),
                     ("mos.event.v1.prev_state", event_prev)):
        if s != c_states:
            print(f"  FAIL {label} != mos_state_description():")
            print(f"    only in C schema: {sorted(c_states - s)}")
            print(f"    only in {label}: {sorted(s - c_states)}")
            failures += 1
        else:
            print(f"  ok   {label} matches the C string table")
    # mos.list.v1 rows carry the same enum plus the list-only "error"
    # row marker (a failing drive's row; emitted by query_row).
    if list_state != c_states | {"error"}:
        print("  FAIL mos.list.v1.drives[].state != mos_state_description() + 'error':")
        print(f"    only in C+error: {sorted((c_states | {'error'}) - list_state)}")
        print(f"    only in schema:  {sorted(list_state - (c_states | {'error'}))}")
        failures += 1
    else:
        print("  ok   mos.list.v1.drives[].state matches (C states + 'error' row marker)")
    if not failures:
        print(f"  ok   all enums match {sorted(c_states)}")
    return failures


def check_cli_enum_drift(here: Path) -> int:
    """Same drift guard as check_state_enum_drift, for the three other
    string sets the CLI emits into schema-constrained fields. The C side
    is compile-time pinned (-Wswitch on the no-default switches), but
    nothing ties the SCHEMA enums to those switches — this check does.
      - error codes:   cli/common.c mos_error_to_code() (minus the
                       MOS_OK row: "ok" is unreachable in an error
                       envelope and excluded from the schema enums)
                       vs mos.error.v1 + mos.event.v1 error.code
      - event kinds:   cli/watch.c event_kind_string() case arms
                       (the post-switch "unknown" fallback is not a
                       schema value) vs mos.event.v1 event
      - media classes: src/mos_strings.c mos_profile_class()
                       vs mos.state.v1 + mos.event.v1 media_class
    Returns a failure count."""
    root = here.parent

    def switch_returns(path: Path, func: str, case_prefix: str) -> set:
        src = path.read_text()
        m = re.search(re.escape(func) + r'\b.*?\{(.*?)\n\}', src, re.DOTALL)
        if not m:
            return set()
        return set(re.findall(
            r'case\s+' + case_prefix + r'\w*\s*:\s*(?:case\s+\w+\s*:\s*)*'
            r'return\s+"([a-z_]+)"', m.group(1)))

    def media_class_returns(path: Path) -> set:
        src = path.read_text()
        m = re.search(r'mos_profile_class\b.*?\{(.*?)\n\}', src, re.DOTALL)
        if not m:
            return set()
        return set(re.findall(r'return\s+"([a-z_]+)"', m.group(1)))

    def enum_at(path: Path, *keys: str) -> set:
        node = json.loads(path.read_text())
        for key in keys:
            node = node["properties"][key]
        return set(node["enum"])

    checks = []

    c_codes = switch_returns(root / "cli" / "common.c",
                             "mos_error_to_code", "MOS_") - {"ok"}
    checks.append(("error.code", c_codes, "cli/common.c mos_error_to_code()",
                   (("mos.error.v1.error.code",
                     enum_at(here / "mos.error.v1.json", "error", "code")),
                    ("mos.event.v1.error.code",
                     enum_at(here / "mos.event.v1.json", "error", "code")))))

    c_events = switch_returns(root / "cli" / "watch.c",
                              "event_kind_string", "MOS_EVENT_")
    checks.append(("event", c_events, "cli/watch.c event_kind_string()",
                   (("mos.event.v1.event",
                     enum_at(here / "mos.event.v1.json", "event")),)))

    c_classes = media_class_returns(root / "src" / "mos_strings.c")
    checks.append(("media_class", c_classes,
                   "src/mos_strings.c mos_profile_class()",
                   (("mos.state.v1.media_class",
                     enum_at(here / "mos.state.v1.json", "media_class")),
                    ("mos.event.v1.media_class",
                     enum_at(here / "mos.event.v1.json", "media_class")),
                    ("mos.metadata.v1.disc.class",
                     enum_at(here / "mos.metadata.v1.json",
                             "disc", "class") - {None}))))

    # switch_returns' case-arm regex cannot see this function's
    # `case MOS_DISC_OTHER: default:` fallback arm, so harvest every
    # string return in the function body instead (the function is a
    # pure string table, same shape as mos_profile_class).
    def fn_returns(path: Path, func: str) -> set:
        src = path.read_text()
        m = re.search(re.escape(func) + r'\b.*?\{(.*?)\n\}', src, re.DOTALL)
        return set(re.findall(r'return\s+"([a-z_]+)"', m.group(1))) if m else set()

    c_disc_status = fn_returns(root / "src" / "mos_strings.c",
                               "mos_disc_status_description")
    checks.append(("disc_status", c_disc_status,
                   "src/mos_strings.c mos_disc_status_description()",
                   (("mos.metadata.v1.disc.disc_info.status",
                     enum_at(here / "mos.metadata.v1.json",
                             "disc", "disc_info", "status")),)))

    # Physical-structure (DVD/HD-DVD) token tables vs the schema enums on
    # mos.metadata.v1.disc.disc_structure.{physical,copyright}. Same
    # pure-string-table shape; the schema enums carry an extra null
    # (the token fns return NULL for unrecognized codes / unmounted), so
    # compare against the enum set minus None.
    c_book_type = fn_returns(root / "src" / "mos_strings.c",
                             "mos_book_type_name")
    checks.append(("book_type_name", c_book_type,
                   "src/mos_strings.c mos_book_type_name()",
                   (("mos.metadata.v1.disc.disc_structure.physical.book_type_name",
                     enum_at(here / "mos.metadata.v1.json", "disc",
                             "disc_structure", "physical",
                             "book_type_name") - {None}),)))

    c_track_path = fn_returns(root / "src" / "mos_strings.c",
                              "mos_track_path_name")
    checks.append(("track_path", c_track_path,
                   "src/mos_strings.c mos_track_path_name()",
                   (("mos.metadata.v1.disc.disc_structure.physical.track_path",
                     enum_at(here / "mos.metadata.v1.json", "disc",
                             "disc_structure", "physical",
                             "track_path") - {None}),)))

    c_protection = fn_returns(root / "src" / "mos_strings.c",
                              "mos_protection_name")
    checks.append(("protection_name", c_protection,
                   "src/mos_strings.c mos_protection_name()",
                   (("mos.metadata.v1.disc.disc_structure.copyright.protection_name",
                     enum_at(here / "mos.metadata.v1.json", "disc",
                             "disc_structure", "copyright",
                             "protection_name") - {None}),)))

    print("\nCLI-enum drift (C emit tables vs schema enums):")
    failures = 0
    for field, c_set, c_src, schema_sets in checks:
        if not c_set:
            print(f"  FAIL drift-check: could not extract {field} strings from {c_src}")
            failures += 1
            continue
        for label, s in schema_sets:
            if s != c_set:
                print(f"  FAIL {label} != {c_src}:")
                print(f"    only in C table: {sorted(c_set - s)}")
                print(f"    only in {label}: {sorted(s - c_set)}")
                failures += 1
            else:
                print(f"  ok   {label} matches {c_src}")
    return failures


def main() -> int:
    here = Path(__file__).parent
    schemas: dict[str, Draft202012Validator] = {}
    for schema_file in sorted(here.glob("mos.*.v*.json")):
        body = json.loads(schema_file.read_text())
        # Validate the schema document itself (catches a malformed schema
        # that would otherwise silently accept/reject fixtures by accident).
        Draft202012Validator.check_schema(body)
        schemas[schema_file.stem] = Draft202012Validator(body)
    if not schemas:
        print(f"No schemas found under {here}", file=sys.stderr)
        return 2
    print(f"Loaded {len(schemas)} schemas: {sorted(schemas)}")

    failures = 0

    # Positive: every fixture under schemas/examples must validate.
    examples_dir = here / "examples"
    print(f"\nPositive (must validate, from {examples_dir.relative_to(here.parent)}):")
    for fixture in sorted(examples_dir.glob("*.json")):
        sname = schema_name_from_filename(fixture.name)
        if sname not in schemas:
            print(f"  FAIL {fixture.name}: unknown schema family '{sname}'")
            failures += 1
            continue
        payload = json.loads(fixture.read_text())
        errors = sorted(schemas[sname].iter_errors(payload),
                        key=lambda e: list(e.path))
        if errors:
            print(f"  FAIL {fixture.name} vs {sname}:")
            for err in errors:
                print(f"    {err.message} (at {list(err.path)})")
            failures += 1
        else:
            print(f"  ok   {fixture.name}")

    # Negative: every fixture under schemas/negative must be rejected.
    negative_dir = here / "negative"
    if negative_dir.is_dir():
        print(f"\nNegative (must be rejected, from {negative_dir.relative_to(here.parent)}):")
        for fixture in sorted(negative_dir.glob("*.json")):
            sname = schema_name_from_filename(fixture.name)
            if sname not in schemas:
                print(f"  FAIL {fixture.name}: unknown schema family '{sname}'")
                failures += 1
                continue
            payload = json.loads(fixture.read_text())
            errors = list(schemas[sname].iter_errors(payload))
            if errors:
                print(f"  ok   {fixture.name} (correctly rejected)")
            else:
                print(f"  FAIL {fixture.name}: should have been rejected by {sname}")
                failures += 1

    print()
    failures += check_state_enum_drift(here)
    failures += check_cli_enum_drift(here)

    print()
    if failures:
        print(f"{failures} fixture(s) had unexpected outcomes")
        return 1
    print("All fixtures behaved as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
