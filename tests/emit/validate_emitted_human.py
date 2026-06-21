#!/usr/bin/env python3
"""
validate_emitted_human.py — run the emit_fixtures binary in HUMAN mode for
each (verb, scenario) and compare its real stdout against a committed golden
file tests/emit/golden/<verb>.<scenario>.txt, byte for byte.

This is the human-output sibling of validate_emitted.py. That script schema-
validates the --json paths; the cli/*.c emit_human renderers had no equivalent
guard, so a layout/vocabulary drift in a human block could ship silently (it
has caused real bugs). Here the ACTUAL human stdout of each verb is pinned to
a golden, so drift fails CI.

Goldens are byte-exact, INCLUDING the trailing newline the renderers emit
(mos_cli_human_block writes a '\n' after every row; the feature table and the
list table likewise newline-terminate the last row). The golden file holds
exactly the bytes the binary writes to stdout — no added or stripped newline.

De-risking: the binary only builds/runs on macOS CI, so these goldens cannot
be generated on a Linux dev box. When a golden is MISSING or MISMATCHES, this
script prints the captured stdout verbatim between grep-friendly delimiters:

    ===BEGIN <verb>.<scenario>===
    <actual bytes, exactly>
    ===END <verb>.<scenario>===

so the correct golden can be lifted straight out of the first CI run's log and
committed. Exit non-zero on any miss/mismatch.

Usage: validate_emitted_human.py <path-to-emit_fixtures> [golden-dir]
       golden-dir defaults to <this file's dir>/golden
"""
import subprocess
import sys
from pathlib import Path

# (verb, scenario). Every (verb, scenario) emit_fixtures.c supports that
# produces a single human stdout block. `error` (stderr diagnostic only) and
# `watch` (NDJSON, no human rendering) are deliberately skipped — they have no
# emit_human path. Kept in lockstep with the if-chain in emit_fixtures.c.
HUMAN_SCENARIOS = [
    ("metadata", "bd_mdisc"),
    ("metadata", "cd_mounted"),
    ("metadata", "not_ready"),
    ("drive",    "aacs_bd"),
    ("drive",    "plain_dvd"),
    ("features", "bd"),
    ("state",    "ready_mounted"),
    ("list",     "one_drive"),
    ("tray",     "eject_done"),
    ("tray",     "lock"),
    ("tray",     "already_locked"),
    ("tray",     "refused_locked"),
    ("tray",     "refused_other"),
    ("capacity", "pressed_bd"),
    ("capacity", "blank_bdr"),
    ("capacity", "empty"),
]


def dump_actual(verb: str, scn: str, actual: str) -> None:
    """Print the captured stdout verbatim between grep-friendly delimiters so
    the golden can be lifted from the CI log. No surrounding reflow."""
    tag = f"{verb}.{scn}"
    # The actual text already ends in its own newline(s); print it raw, then
    # the END marker on its own line. We use sys.stdout.write to avoid print()
    # adding a second newline that would distort the captured bytes.
    sys.stdout.write(f"===BEGIN {tag}===\n")
    sys.stdout.write(actual)
    if not actual.endswith("\n"):
        sys.stdout.write("\n")
        sys.stdout.write("===END (note: output had no trailing newline)===\n")
    else:
        sys.stdout.write(f"===END {tag}===\n")


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(f"usage: {sys.argv[0]} <emit_fixtures binary> [golden dir]",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]
    golden_dir = (Path(sys.argv[2]) if len(sys.argv) == 3
                  else Path(__file__).resolve().parent / "golden")

    failures = 0
    for verb, scn in HUMAN_SCENARIOS:
        proc = subprocess.run([binary, verb, scn, "human"],
                              capture_output=True, text=True)
        # The harness masks expected sysexit codes to 0; any non-zero here is
        # a real crash/abort (e.g. ASan), regardless of stdout.
        if proc.returncode != 0:
            print(f"  FAIL {verb}/{scn}: binary exited {proc.returncode}\n"
                  f"    stderr: {proc.stderr.strip()}")
            failures += 1
            continue

        actual = proc.stdout
        golden_path = golden_dir / f"{verb}.{scn}.txt"
        if not golden_path.exists():
            print(f"  FAIL {verb}/{scn}: golden missing ({golden_path}); "
                  f"capture the block below into it:")
            dump_actual(verb, scn, actual)
            failures += 1
            continue

        expected = golden_path.read_text()
        if actual != expected:
            print(f"  FAIL {verb}/{scn}: stdout != golden ({golden_path}); "
                  f"actual below (replace the golden if the change is intended):")
            dump_actual(verb, scn, actual)
            failures += 1
            continue

        print(f"  ok   {verb}/{scn} -> {golden_path.name}")

    print()
    if failures:
        print(f"{failures} human output(s) failed the golden comparison")
        return 1
    print(f"All {len(HUMAN_SCENARIOS)} human outputs match their golden")
    return 0


if __name__ == "__main__":
    sys.exit(main())
