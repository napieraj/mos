#!/usr/bin/env python3
"""Guard README.md's example output against the source of truth.

The JSON-schema ADR (AGENTS.md) requires the README contract section to be
updated in the same commit as any schema shape change; nothing enforced that.
A field could be renamed in schemas/mos.*.v1.json — or a human row relabelled
in cli/<verb>.c — and the README left showing the old name. This catches both.

JSON blocks, checked against schemas/mos.*.v*.json:

  * complete documents  — a fenced block carrying a top-level "schema" key
    (e.g. the `mos status --json` example). Validated in full against
    schemas/<schema>.json: required fields, types, enums, additionalProperties.

  * abridged fragments  — the `jsonc` subtrees (disc_structure, track_info,
    cdtext, speeds, mechanical, error_recovery) and the `watch` NDJSON lines
    that use `// comments` and `...` ellipses. Not whole documents, so they are
    key-checked: every key present must exist in the schema
    (additionalProperties:false → an unknown key is drift) and match its
    type/enum/pattern; omitted keys are fine. The fragment's container key is
    located by name anywhere in any schema, so it tracks the schema's nesting.

Human blocks, checked against cli/<verb>.c emit_human (the same parse the
mos-sim self-gen kit lifts from the source):

  * the `$ mos <verb>` pair blocks (status / drive / metadata / capacity).
    The labels shown must be an in-order SUBSEQUENCE of emit_human's pairs[]
    label order — which tolerates the rows a given example suppresses while
    catching a relabelled, reordered, or deleted row.

  * the `$ mos list` table HEADER row vs the column-header arrays in
    cli/common.c (with / without the Volume column). The header must equal one
    variant exactly — columns aren't individually suppressed. Only the header
    is checked: the body is whitespace-aligned with space-bearing,
    dynamically-widthed cells, so it isn't round-trippable.

Exit: 0 if every checkable block validates, 1 on any failure, 2 on setup error.
Runnable locally with `python3 schemas/check_readme.py`; meant for CI alongside
validate.py.
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

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
README = ROOT / "README.md"

# verbs whose emit_human builds a pairs[] block (matches the self-gen kit).
PAIR_VERBS = ("status", "drive", "metadata", "capacity", "tray")


def fenced_blocks(md: str):
    """Yield (line, raw_body) for every ``` fenced block, raw (the `$ command`
    lines kept, so the human path can read the verb)."""
    lines = md.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("```"):
            start = i
            i += 1
            body = []
            while i < len(lines) and not lines[i].startswith("```"):
                body.append(lines[i])
                i += 1
            yield start + 1, "\n".join(body)
        i += 1


def json_text(raw: str):
    """Reduce a raw block to its JSON, or None if it isn't JSON. The README
    bundles the `$ command` line inside the fence with its output, and abridged
    fragments are brace-less object interiors starting with a quoted key."""
    lines = raw.splitlines()
    while lines and (not lines[0].strip()
                     or lines[0].lstrip().startswith(("$", "#"))):
        lines.pop(0)
    text = "\n".join(lines).strip()
    if text[:1] == '"':                # brace-less fragment interior
        text = "{" + text + "}"
    return text if text.startswith(("{", "[")) else None


def func_body(src: str, signature_re: str):
    """Brace-balanced body of the first function matching signature_re, or None."""
    m = re.search(signature_re, src)
    if not m:
        return None
    i = src.index("{", m.end())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
    return None


def c_human_labels(verb: str):
    """emit_human's pairs[] label order from cli/<verb>.c, consecutive dups
    collapsed (a label emitted in both arms of an if/else is one row). Same
    parse as the self-gen kit's gen_spec.py."""
    path = ROOT / "cli" / f"{verb}.c"
    if not path.exists():
        return None
    body = func_body(path.read_text(), r'static\s+void\s+emit_human\s*\(')
    if body is None:
        return None
    out = []
    for lab in re.findall(r'mos_cli_human_pair\)\s*\{\s*"([^"]+)"', body):
        if not out or out[-1] != lab:
            out.append(lab)
    return out


def c_list_headers():
    """The list table's column-header arrays from cli/common.c — two variants
    (with / without the Volume column), each a plain string-literal array. Only
    the HEADER ROW is contract: the body is whitespace-aligned with space-
    bearing, dynamically-widthed cells, so it isn't round-trippable and isn't
    checked."""
    src = (ROOT / "cli" / "common.c").read_text()
    arrs = re.findall(r'headers_(?:v|nv)\[[^\]]*\]\s*=\s*\{([^}]*)\}', src)
    return [re.findall(r'"([^"]+)"', a) for a in arrs] or None


def list_block(raw: str):
    """If raw is the `$ mos list` table (not --json), return its header row as
    a column-label list; else None. The header tokens are single words, so the
    first output line splits cleanly on runs of 2+ spaces."""
    lines = raw.splitlines()
    if not any(re.match(r'\$\s*mos\s+list\b', ln) for ln in lines) or "--json" in raw:
        return None
    for ln in lines:
        if ln.lstrip().startswith("$"):
            continue
        if ln.strip():
            return re.split(r"\s{2,}", ln.strip())
    return None


def human_block(raw: str):
    """If raw is a `$ mos <verb>` pair block (not --json), return
    (verb, [labels shown]); else None."""
    m = re.search(r'\$\s*mos\s+(\w+)', raw)
    if not m or m.group(1) not in PAIR_VERBS or "--json" in raw:
        return None
    labels = []
    for ln in raw.splitlines():
        lm = re.match(r'\s*([A-Za-z][\w /-]*?):\s{2,}\S', ln)
        if lm:
            labels.append(lm.group(1))
    return (m.group(1), labels) if labels else None


def subsequence_problems(shown, c_labels, verb):
    """Every shown label must exist in the C labels, and the shown labels must
    appear in C order (suppression drops rows; it never reorders or renames)."""
    problems = [f"{lab!r}: label not in cli/{verb}.c emit_human {c_labels}"
                for lab in shown if lab not in c_labels]
    it = iter(c_labels)
    if not all(lab in it for lab in shown):
        problems.append(f"label order {shown} is not a subsequence of {c_labels}")
    return problems


def strip_jsonc(text: str) -> str:
    """Remove ` // …` line comments and `...` ellipses, then repair the commas
    the ellipses leave behind. Comments here are always whitespace-prefixed, so
    a `//` inside a string literal (paths use single slashes) is left alone."""
    text = re.sub(r"\s+//.*", "", text)
    text = text.replace("...", "")
    text = re.sub(r",\s*,", ",", text)
    text = re.sub(r"([{\[])\s*,", r"\1", text)
    text = re.sub(r",\s*([}\]])", r"\1", text)
    return text


def find_property_schema(prop: str, schemas: dict):
    """Locate the subschema for a property named `prop` anywhere in any schema
    (descending properties/ and array items/). Returns the subschema, or None
    if absent / ambiguous across schemas."""
    hits = []

    def walk(node):
        if not isinstance(node, dict):
            return
        props = node.get("properties")
        if isinstance(props, dict):
            if prop in props:
                hits.append(props[prop])
            for v in props.values():
                walk(v)
        items = node.get("items")
        if isinstance(items, dict):
            walk(items)

    for schema in schemas.values():
        walk(schema)
    # de-dup identical subschemas (same key defined the same way in siblings)
    uniq = []
    for h in hits:
        if h not in uniq:
            uniq.append(h)
    return uniq[0] if len(uniq) == 1 else None


def errors(validator, instance):
    return sorted(validator.iter_errors(instance), key=lambda e: e.path)


def leaf_problems(value, subschema, path):
    """Validate a non-object leaf (scalar or array) against its subschema for
    type / enum / pattern only — `required` and oneOf discrimination don't
    apply, and an abridged value must not be faulted for what it omits."""
    leaf = {k: v for k, v in subschema.items()
            if k in ("type", "enum", "const", "pattern", "minimum",
                     "minLength", "format")}
    return [f"{path}: {e.message}" for e in errors(Draft202012Validator(leaf), value)]


def key_problems(obj, schema, path=""):
    """Recursively check that every key PRESENT in an abridged object exists in
    the schema (additionalProperties:false → an unknown key is drift) and that
    its value matches the schema's type/enum/pattern. Omitted keys are fine —
    abridgement is expected — so `required` and oneOf are deliberately ignored.
    This is what catches a README key the schema renamed, dropped, or retyped
    without fighting the event schema's per-kind oneOf."""
    problems = []
    if not isinstance(obj, dict):
        return leaf_problems(obj, schema, path or "<root>")
    props = schema.get("properties", {})
    closed = schema.get("additionalProperties", True) is False
    for key, val in obj.items():
        p = f"{path}/{key}" if path else key
        if key not in props:
            if closed:
                problems.append(f"{p}: key not in schema "
                                f"{schema.get('title', '(untitled)')}")
            continue
        sub = props[key]
        if isinstance(val, dict) and "properties" in sub:
            problems += key_problems(val, sub, p)
        elif isinstance(val, list) and isinstance(sub.get("items"), dict):
            for idx, item in enumerate(val):
                problems += key_problems(item, sub["items"], f"{p}[{idx}]")
        else:
            problems += leaf_problems(val, sub, p)
    return problems


def main() -> int:
    if not README.exists():
        print(f"error: {README} not found", file=sys.stderr)
        return 2
    schemas = {p.name: json.loads(p.read_text())
               for p in HERE.glob("mos.*.v*.json")}

    md = README.read_text()
    checked = skipped = failures = 0

    for line, raw in fenced_blocks(md):
        loc = f"README.md:{line}"

        def report(label, probs):
            nonlocal checked, failures
            if probs:
                print(f"  FAIL {loc}: {label}")
                for pr in probs:
                    print(f"         {pr}")
                failures += 1
            else:
                print(f"  ok   {loc}: {label}")
                checked += 1

        cols = list_block(raw)
        if cols:
            variants = c_list_headers()
            if not variants:
                print(f"  SKIP {loc}: list headers not found in cli/common.c")
                skipped += 1
            else:
                report("list table header",
                       [] if cols in variants else
                       [f"header {cols} matches no cli/common.c variant {variants}"])
            continue

        hb = human_block(raw)
        if hb:
            verb, shown = hb
            c_labels = c_human_labels(verb)
            if c_labels is None:
                print(f"  SKIP {loc}: cli/{verb}.c emit_human not found")
                skipped += 1
            else:
                report(f"human {verb} rows",
                       subsequence_problems(shown, c_labels, verb))
            continue

        raw = json_text(raw)
        if raw is None:
            continue
        abridged = "..." in raw or "//" in raw
        # an NDJSON block (mos watch) is several whole-line objects, not one
        # document; a pretty-printed doc spans lines so none is self-contained.
        records = [ln.strip() for ln in strip_jsonc(raw).splitlines()
                   if ln.strip().startswith("{") and ln.strip().endswith("}")]
        try:
            if len(records) > 1:
                docs = [json.loads(r) for r in records]
            else:
                docs = [json.loads(strip_jsonc(raw))]
        except json.JSONDecodeError as e:
            print(f"  SKIP {loc}: not parseable JSON ({e})")
            skipped += 1
            continue

        for doc in docs:
            if isinstance(doc, dict) and "schema" in doc:
                name = doc["schema"] + ".json"
                if name not in schemas:
                    print(f"  FAIL {loc}: unknown schema {doc['schema']!r}")
                    failures += 1
                    continue
                schema = schemas[name]
                if abridged:
                    report(f"{doc['schema']} (abridged)",
                           key_problems(doc, schema))
                else:
                    report(f"{doc['schema']} (full)",
                           [f"{'/'.join(map(str, e.path)) or '<root>'}: {e.message}"
                            for e in errors(Draft202012Validator(schema), doc)])
            elif isinstance(doc, dict) and doc:
                # abridged fragment: each top-level key is a container property
                # locatable by name in some schema.
                for key, val in doc.items():
                    sub = find_property_schema(key, schemas)
                    if sub is None:
                        print(f"  SKIP {loc}: fragment {key!r} not locatable "
                              f"in a single schema")
                        skipped += 1
                        continue
                    report(f"fragment {key!r}", key_problems(val, sub, key))
            else:
                print(f"  SKIP {loc}: not a JSON object")
                skipped += 1

    print(f"\n{checked} checked, {skipped} skipped, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
