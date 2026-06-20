#!/usr/bin/env python3
"""check-file-tree.py — keep CONTRIBUTING.md's "Code layout" tree honest.

The tree under CONTRIBUTING.md § Code layout drifts the moment a new
source file lands without a matching line (it did: a 2026-06 audit found
~14 src/ files, 5 CLI commands and 4 schemas missing from it). This is the
forcing function — the same regenerate-and-diff shape as the amalgamate and
gen-cli-docs gates, but for the hand-maintained tree.

Two checks, both keyed on git-tracked files (so untracked scratch files
never trip it):

  FORWARD  — every tracked file directly in a "listed individually"
             directory (src/, cli/, scripts/, schemas/ top level) must
             appear in the tree. The `foo.c / .h` shorthand counts for the
             sibling header.
  REVERSE  — every filename token in the tree (anything ending .c/.h/.sh/
             .py/.json/.txt) must name a real tracked file, so a deleted or
             renamed file can't linger in the tree.

tests/ is deliberately NOT forward-checked: the tree documents it by
convention (`test_*.c`, `test_adapter_*.c`, subdir names), not file by
file, because the per-unit test list grows constantly and an exhaustive
enumeration is itself the drift magnet this gate exists to kill.

Pure stdlib, no pip. Exit 0 when the tree matches, 1 (with a diff-style
report) otherwise. Run by scripts/preflight.sh and the doc-staleness CI job.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "CONTRIBUTING.md"

# Directories whose depth-1 files the tree lists one per line. tests/ is
# excluded on purpose (see module docstring).
FORWARD_DIRS = ["src", "cli", "scripts", "schemas"]

# Extensions a reverse token must carry to be treated as a file reference.
FILE_TOKEN = re.compile(r"[A-Za-z0-9_./\-]+\.(?:c|h|sh|py|json|txt)\b")


def tracked_files():
    out = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout
    return [line for line in out.splitlines() if line]


def extract_tree(text):
    """Return the fenced block that follows the '## Code layout' heading."""
    m = re.search(r"##\s+Code layout\b", text)
    if not m:
        sys.exit("check-file-tree: no '## Code layout' section in CONTRIBUTING.md")
    fence = re.search(r"```\n(.*?)\n```", text[m.end():], re.DOTALL)
    if not fence:
        sys.exit("check-file-tree: no fenced tree block after '## Code layout'")
    return fence.group(1)


def main():
    text = DOC.read_text()
    tree = extract_tree(text)
    tracked = tracked_files()
    basenames = {p.rsplit("/", 1)[-1] for p in tracked}

    problems = []

    # FORWARD: every listed-dir file appears in the tree.
    for d in FORWARD_DIRS:
        for path in sorted(p for p in tracked if p.startswith(d + "/")):
            rel = path[len(d) + 1:]
            if "/" in rel:  # depth-1 only; subdir files are grouped
                continue
            name = rel
            if name in tree:
                continue
            # `foo.c / .h` shorthand covers the sibling header.
            if name.endswith(".h"):
                stem = name[:-1]  # drop only the 'h', keep the dot: "common."
                if re.search(re.escape(stem) + r"c\s*/\s*\.h", tree):
                    continue
            problems.append(f"  MISSING from tree: {path}")

    # REVERSE: every filename token in the tree names a real tracked file.
    for token in FILE_TOKEN.findall(tree):
        base = token.rsplit("/", 1)[-1]
        if base not in basenames:
            problems.append(f"  STALE in tree (no such file): {token}")

    if problems:
        print("CONTRIBUTING.md § Code layout is out of sync with the repo:\n")
        for p in sorted(set(problems)):
            print(p)
        print("\nUpdate the tree in CONTRIBUTING.md to match, then re-run.")
        return 1

    print("check-file-tree: ok — CONTRIBUTING.md tree matches the repo.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
