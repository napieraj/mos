#!/usr/bin/env python3
"""Guard README.md's example output against the source of truth.

The JSON-schema ADR (AGENTS.md) requires the README contract section to be
updated in the same commit as any schema shape change; nothing enforced that.
A field could be renamed in schemas/mos.*.v1.json — or a human row relabelled
in cli/<verb>.c — and the README left showing the old name. This catches both.

JSON blocks, checked against schemas/mos.*.v*.json:

  * complete documents  — a fenced block carrying a top-level "schema" key
    (e.g. the `mos state --json` example). Validated in full against
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

  * the `$ mos <verb>` pair blocks (state / drive / metadata / capacity).
    The labels shown must be an in-order SUBSEQUENCE of emit_human's pairs[]
    label order — which tolerates the rows a given example suppresses while
    catching a relabelled, reordered, or deleted row.

  * the `$ mos list` table HEADER row vs the column-header arrays in
    cli/common.c (with / without the Volume column). The header must equal one
    variant exactly — columns aren't individually suppressed. Only the header
    is checked: the body is whitespace-aligned with space-bearing,
    dynamically-widthed cells, so it isn't round-trippable.

Whole-document cross-checks (prose AND examples):

  * every `mos <verb>` invocation names a real verb (parsed from main.c).
    The per-block checks SKIP an unknown-verb block silently, so a rename
    (status -> state) left stale `mos status` mentions with no tripwire;
    this catches them in inline code spans and fenced command lines.
    Digit-bearing first tokens (`mos 2`, `mos disk4`) are drive selectors
    and `--flags` are options — both exempt, per the CLI's own digit-gate.

  * every `mos.<name>.v<n>` token resolves to a real schema file. The
    `schema` field inside an example doc is already validated; this covers
    the names that appear only as prose (e.g. the JSON-output section).

Exit: 0 if every checkable block validates, 1 on any failure, 2 on setup error.
Runnable locally with `python3 schemas/check_readme.py` (or `--selftest` for
the synthetic cross-check cases); meant for CI alongside validate.py.
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


def c_verbs():
    """The CLI's verb set, parsed from main.c's dispatch table (the
    `strcmp(cmd, "<verb>")` chain — the file's own header calls a dispatch
    line the way you add a verb). Derived, not hardcoded, so a new verb is
    picked up automatically; which of these is a pair verb is then decided by
    c_human_labels() returning labels vs None (no emit_human pairs[])."""
    src = (ROOT / "cli" / "main.c").read_text()
    return set(re.findall(r'strcmp\(cmd,\s*"([a-z]+)"\)', src))


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


def human_block(raw: str, verbs):
    """If raw is a `$ mos <verb>` pair block (not --json) for a known verb,
    return (verb, [labels shown]); else None. Pair-ness is confirmed later by
    c_human_labels()."""
    m = re.search(r'\$\s*mos\s+(\w+)', raw)
    if not m or m.group(1) not in verbs or "--json" in raw:
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


def mos_verb_problems(md, verbs):
    """Every `mos <verb>` invocation in the README must name a real verb —
    in prose `mos …` inline code spans and in `$ mos …` / `mos …` command
    lines inside fenced blocks alike. A digit-bearing first token is a drive
    selector (the CLI's own digit-gate in main.c) and a `--flag` is an
    option; both are exempt. This catches a verb renamed in main.c but left
    stale in the README — the per-block checks above SKIP an unknown-verb
    block silently, so a rename (status -> state) had no tripwire."""
    bad = {}  # verb -> first location, deduped

    def consider(word, loc):
        if not word or word[0] == "-" or any(c.isdigit() for c in word):
            return  # a --flag or a digit-bearing selector, not a verb
        if word not in verbs and word not in bad:
            bad[word] = loc

    for m in re.finditer(r"`mos\s+([A-Za-z][\w-]*)", md):     # prose code spans
        consider(m.group(1), "inline `mos …`")
    for line0, raw in fenced_blocks(md):                      # fenced commands
        for ln in raw.splitlines():
            cm = re.match(r"\s*\$?\s*mos\s+([A-Za-z][\w-]*)", ln)
            if cm:
                consider(cm.group(1), f"README.md:{line0}")

    return [f"{loc}: `mos {w}` is not a verb (have {sorted(verbs)})"
            for w, loc in sorted(bad.items())]


def schema_name_problems(md, schemas):
    """Every `mos.<name>.v<n>` token in the README must resolve to a real
    schema file. The `schema` field INSIDE an example document is already
    validated; this covers the names that appear only as prose (the JSON-
    output section lists all of them), where a renamed schema would slip
    through."""
    problems, seen = [], set()
    for m in re.finditer(r"\bmos\.[a-z_]+\.v\d+\b", md):
        name = m.group(0)
        if name in seen:
            continue
        seen.add(name)
        if name + ".json" not in schemas:
            problems.append(f"`{name}` names no schema in schemas/")
    return problems


def selftest() -> int:
    """Synthetic cases proving the cross-checks fire. `check_readme.py --selftest`."""
    verbs = {"state", "list", "watch", "tray"}
    schemas = {"mos.state.v1.json": {}, "mos.list.v1.json": {}}
    cases = [
        ("stale verb in prose",   mos_verb_problems,    ("Run `mos status 1`.", verbs),        True),
        ("good verb in prose",    mos_verb_problems,    ("Run `mos state 1`.", verbs),         False),
        ("stale verb in fence",   mos_verb_problems,    ("```\n$ mos status 1\n```", verbs),   True),
        ("good verb in fence",    mos_verb_problems,    ("```\n$ mos state 1\n```", verbs),    False),
        ("piped verb in fence",   mos_verb_problems,    ("```\nmos watch | jq .\n```", verbs), False),
        ("digit selector exempt", mos_verb_problems,    ("Use `mos 2` or `mos disk4`.", verbs),False),
        ("bare tool name exempt", mos_verb_problems,    ("`mos` reads the drive.", verbs),     False),
        ("flag exempt",           mos_verb_problems,    ("Pass `mos --json`.", verbs),         False),
        ("bad schema name",       schema_name_problems, ("Emits `mos.bogus.v9`.", schemas),    True),
        ("good schema name",      schema_name_problems, ("Emits `mos.state.v1`.", schemas),    False),
        ("schema in a path",      schema_name_problems, ("see schemas/mos.list.v1.json", schemas), False),
    ]
    failed = 0
    for name, fn, args, expect in cases:
        probs = fn(*args)
        ok = bool(probs) == expect
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: "
              f"{probs if probs else 'clean'}")
        failed += not ok
    print(f"\nselftest: {len(cases) - failed}/{len(cases)} passed")
    return 1 if failed else 0


def main() -> int:
    if not README.exists():
        print(f"error: {README} not found", file=sys.stderr)
        return 2
    schemas = {p.name: json.loads(p.read_text())
               for p in HERE.glob("mos.*.v*.json")}
    verbs = c_verbs()

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
                # A missing C anchor means the README<->C contract can no
                # longer be verified (the header arrays were renamed/moved) —
                # fail-closed, don't SKIP-mask the drift this gate exists for.
                print(f"  FAIL {loc}: list headers not found in cli/common.c "
                      f"(C anchor renamed? contract unverifiable)")
                failures += 1
            else:
                report("list table header",
                       [] if cols in variants else
                       [f"header {cols} matches no cli/common.c variant {variants}"])
            continue

        hb = human_block(raw, verbs)
        if hb:
            verb, shown = hb
            c_labels = c_human_labels(verb)
            if c_labels is None:
                # Same fail-closed reasoning as the list-header anchor above:
                # a human pair block in the README means the verb HAS an
                # emit_human, so a missing anchor is a rename, not a no-op.
                print(f"  FAIL {loc}: cli/{verb}.c emit_human not found "
                      f"(C anchor renamed/moved? contract unverifiable)")
                failures += 1
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

    # Document-level cross-checks (prose + examples): every `mos <verb>`
    # names a real verb, every `mos.*.v<n>` names a real schema.
    for label, probs in (("mos verb names", mos_verb_problems(md, verbs)),
                         ("schema names", schema_name_problems(md, schemas))):
        if probs:
            print(f"  FAIL README.md: {label}")
            for pr in probs:
                print(f"         {pr}")
            failures += 1
        else:
            print(f"  ok   README.md: {label}")
            checked += 1

    print(f"\n{checked} checked, {skipped} skipped, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(selftest() if "--selftest" in sys.argv else main())
