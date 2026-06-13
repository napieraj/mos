#!/usr/bin/env python3
"""
validate_emitted.py — run the emit_fixtures binary for each (verb,
scenario) and validate its real stdout JSON against the schema the
document itself names (its `schema` field), exactly as a consumer
dispatches.

This is the missing guard the branch review surfaced: schemas/validate.py
checks hand-written fixtures, and the CLI contract test only covers the
verbs' error envelopes. Here the ACTUAL emit_json output of every
success path is validated, so emitter↔schema drift fails CI instead of
shipping.

Usage: validate_emitted.py <path-to-emit_fixtures> <schemas-dir>
"""
import json
import subprocess
import sys
from pathlib import Path

try:
    from jsonschema import Draft202012Validator
except ImportError:
    print("jsonschema not installed. `pip install jsonschema`", file=sys.stderr)
    sys.exit(2)

# (verb, scenario, expected schema). The expected schema is asserted
# against the document's own `schema` field too, so a verb emitting the
# wrong schema id is caught.
SCENARIOS = [
    ("metadata", "bd_mdisc",      "mos.metadata.v1"),
    ("metadata", "cd_mounted",    "mos.metadata.v1"),
    ("metadata", "not_ready",     "mos.metadata.v1"),
    ("drive",    "aacs_bd",       "mos.drive.v1"),
    ("drive",    "plain_dvd",     "mos.drive.v1"),
    ("features", "bd",            "mos.features.v1"),
    ("status",   "ready_mounted", "mos.state.v1"),
    ("list",     "one_drive",     "mos.list.v1"),
    ("tray",     "eject_done",      "mos.tray.v1"),
    ("tray",     "lock_persistent", "mos.tray.v1"),
    ("tray",     "refused_locked",  "mos.tray.v1"),
    ("tray",     "refused_other",   "mos.tray.v1"),
]


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <emit_fixtures binary> <schemas dir>",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]
    schemas = Path(sys.argv[2])

    validators: dict[str, Draft202012Validator] = {}

    def validator_for(name: str) -> Draft202012Validator:
        if name not in validators:
            schema = json.loads((schemas / f"{name}.json").read_text())
            validators[name] = Draft202012Validator(schema)
        return validators[name]

    failures = 0
    for verb, scn, expect in SCENARIOS:
        proc = subprocess.run([binary, verb, scn], capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"  FAIL {verb}/{scn}: binary exited {proc.returncode}\n"
                  f"    stderr: {proc.stderr.strip()}")
            failures += 1
            continue
        try:
            doc = json.loads(proc.stdout)
        except json.JSONDecodeError as e:
            print(f"  FAIL {verb}/{scn}: emitted invalid JSON: {e}\n"
                  f"    output: {proc.stdout[:200]!r}")
            failures += 1
            continue

        got = doc.get("schema")
        if got != expect:
            print(f"  FAIL {verb}/{scn}: schema field {got!r}, expected {expect!r}")
            failures += 1
            continue

        errs = sorted(validator_for(expect).iter_errors(doc), key=lambda e: e.path)
        if errs:
            print(f"  FAIL {verb}/{scn}: {len(errs)} schema violation(s) "
                  f"against {expect}:")
            for e in errs[:5]:
                loc = "/".join(str(p) for p in e.path) or "(root)"
                print(f"    - {loc}: {e.message}")
            failures += 1
            continue

        print(f"  ok   {verb}/{scn} -> {expect}")

    print()
    if failures:
        print(f"{failures} emitted document(s) failed validation")
        return 1
    print(f"All {len(SCENARIOS)} emitted documents validate against their schema")
    return 0


if __name__ == "__main__":
    sys.exit(main())
