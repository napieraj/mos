#!/usr/bin/env python3
"""
validate_emitted.py — run the emit_fixtures binary for each (verb,
scenario) and validate its real stdout JSON against the schema the
document itself names (its `schema` field), exactly as a consumer
dispatches.

This closes a gap left by the other checks: schemas/validate.py
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
    from jsonschema import Draft202012Validator, FormatChecker
except ImportError:
    print("jsonschema not installed. `pip install jsonschema`", file=sys.stderr)
    sys.exit(2)

# Enforce `format` (date-time) as a hard assertion, matching schemas/validate.py.
# Needs rfc3339-validator (schemas/requirements-ci.txt); assert it is registered
# so a missing dep fails loudly instead of silently skipping the check.
_FORMAT_CHECKER = FormatChecker()
if "date-time" not in _FORMAT_CHECKER.checkers:
    print("date-time format checker not registered — install rfc3339-validator "
          "(see schemas/requirements-ci.txt)", file=sys.stderr)
    sys.exit(2)

# (verb, scenario, expected schema). The expected schema is checked
# against the document's own `schema` field, catching a verb that emits
# the wrong schema id.
SCENARIOS = [
    ("metadata", "bd_mdisc",      "mos.metadata.v1"),
    ("metadata", "cd_mounted",    "mos.metadata.v1"),
    ("metadata", "not_ready",     "mos.metadata.v1"),
    ("drive",    "aacs_bd",       "mos.drive.v1"),
    ("drive",    "plain_dvd",     "mos.drive.v1"),
    ("features", "bd",            "mos.features.v1"),
    ("state",    "ready_mounted", "mos.state.v1"),
    ("list",     "one_drive",     "mos.list.v1"),
    ("error",    "no_drive",        "mos.error.v1"),
    ("tray",     "eject_done",      "mos.tray.v1"),
    ("tray",     "lock",            "mos.tray.v1"),
    ("tray",     "already_locked",  "mos.tray.v1"),
    ("tray",     "refused_locked",  "mos.tray.v1"),
    ("tray",     "refused_other",   "mos.tray.v1"),
    ("capacity", "pressed_bd",      "mos.capacity.v1"),
    ("capacity", "blank_bdr",       "mos.capacity.v1"),
    ("capacity", "empty",           "mos.capacity.v1"),
    # mos.event.v1 is an NDJSON stream: validated line by line below.
    ("watch",    "stream",          "mos.event.v1"),
]

# Schemas emitted as NDJSON (one JSON object per line) rather than a
# single pretty document. Validated per-line against the same schema.
NDJSON_SCHEMAS = {"mos.event.v1"}


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
            validators[name] = Draft202012Validator(
                schema, format_checker=_FORMAT_CHECKER)
        return validators[name]

    def validate_doc(doc: object, expect: str) -> list[str]:
        """Return a list of failure messages for one emitted document
        (empty == ok): the document must name the expected schema in its
        own `schema` field and validate against it."""
        msgs: list[str] = []
        got = doc.get("schema") if isinstance(doc, dict) else None
        if got != expect:
            return [f"schema field {got!r}, expected {expect!r}"]
        for e in sorted(validator_for(expect).iter_errors(doc),
                        key=lambda e: list(e.path))[:5]:
            loc = "/".join(str(p) for p in e.path) or "(root)"
            msgs.append(f"{loc}: {e.message}")
        return msgs

    failures = 0
    for verb, scn, expect in SCENARIOS:
        proc = subprocess.run([binary, verb, scn], capture_output=True, text=True)
        # The harness masks expected sysexit codes to 0, so any non-zero
        # exit here is a real crash/abort (e.g. ASan) regardless of stdout.
        if proc.returncode != 0:
            print(f"  FAIL {verb}/{scn}: binary exited {proc.returncode}\n"
                  f"    stderr: {proc.stderr.strip()}")
            failures += 1
            continue

        # NDJSON schemas emit one object per line; every line is validated.
        # Single-document schemas emit one pretty object.
        if expect in NDJSON_SCHEMAS:
            lines = [ln for ln in proc.stdout.splitlines() if ln.strip()]
            if not lines:
                print(f"  FAIL {verb}/{scn}: no NDJSON lines emitted")
                failures += 1
                continue
        else:
            lines = [proc.stdout]

        scn_failed = False
        for i, raw in enumerate(lines, 1):
            tag = f"{verb}/{scn} line {i}" if expect in NDJSON_SCHEMAS else f"{verb}/{scn}"
            try:
                doc = json.loads(raw)
            except json.JSONDecodeError as e:
                print(f"  FAIL {tag}: emitted invalid JSON: {e}\n"
                      f"    output: {raw[:200]!r}")
                scn_failed = True
                continue
            msgs = validate_doc(doc, expect)
            if msgs:
                print(f"  FAIL {tag}: {len(msgs)} schema problem(s) against {expect}:")
                for m in msgs:
                    print(f"    - {m}")
                scn_failed = True

        if scn_failed:
            failures += 1
        elif expect in NDJSON_SCHEMAS:
            print(f"  ok   {verb}/{scn} -> {expect} ({len(lines)} NDJSON line(s))")
        else:
            print(f"  ok   {verb}/{scn} -> {expect}")

    print()
    if failures:
        print(f"{failures} emitted document(s) failed validation")
        return 1
    print(f"All {len(SCENARIOS)} emitted documents validate against their schema")
    return 0


if __name__ == "__main__":
    sys.exit(main())
