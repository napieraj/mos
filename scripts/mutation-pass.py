#!/usr/bin/env python3
"""scripts/mutation-pass.py — the pre-tag mutation ritual for the
mac-optical-state pure layer (vendored 2026-06-10 from the fourth
review's harness, re-anchored to the current tree and extended with
mutants for the audit-session and CLI-batch code).

LATCH WARNING (read before trusting a clean run): mutants match exact
source substrings. On a drifted tree a mutant reports "?? NO-MATCH" /
"?? AMBIG" and counts as unappliable, NOT killed. NO-MATCH means
"could not locate", never "fixed": re-anchor before drawing
conclusions.

For each (label, file, find, replace): apply to a scratch copy,
rebuild the pure-layer test binary, run it. KILLED = suite fails
(compile rejection counts — the change was refused). SURVIVED = suite
still green: a behavioral change the tests did not notice. Survivors
are the finding.

Usage: python3 scripts/mutation-pass.py [tree-root]   (default: .)
"""
import os, shutil, subprocess, sys, tempfile

SRC = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
# Keep in lockstep with the mos_pure / mos_tests targets in CMakeLists.txt
# (a stale list compiles an incomplete binary that link-fails on the
# missing register_*_tests, reported as BASELINE NOT GREEN).
PURE = ["src/mos_pure.c", "src/mos_sense.c", "src/mos_strings.c",
        "src/mos_state_core.c", "src/mos_watch_core.c", "src/mos_config.c",
        "src/mos_discinfo.c", "src/mos_discstruct.c", "src/mos_physstruct.c",
        "src/mos_trackinfo.c", "src/mos_perf.c", "src/mos_modepage.c",
        "src/mos_result.c", "cli/human.c"]
TESTS = ["tests/test_main.c", "tests/test_sense.c", "tests/test_bsd_name.c",
         "tests/test_scsi_status.c", "tests/test_ioreturn.c",
         "tests/test_state_core.c", "tests/test_watch_core.c",
         "tests/test_render.c", "tests/test_human.c", "tests/test_config.c",
         "tests/test_discinfo.c", "tests/test_discstruct.c",
         "tests/test_physstruct.c", "tests/test_trackinfo.c",
         "tests/test_perf.c", "tests/test_modepage.c", "tests/test_tray.c",
         "tests/test_result.c"]

MUTANTS = [
    # ---- carried from the fourth-review campaign (re-anchored) ----
    ("sense: hardware-error guard dropped", "src/mos_sense.c",
     "if (sk == 0x04) return MOS_STATE_DEVICE_FAULT;",
     "if (0) return MOS_STATE_DEVICE_FAULT;"),
    ("sense: 3A presence hoist weakened (asc==0x3B)", "src/mos_sense.c",
     "if (asc == 0x3A) return MOS_STATE_EMPTY;",
     "if (asc == 0x3B) return MOS_STATE_EMPTY;"),
    ("gesn: door bit mask 0x01 -> 0x02", "src/mos_pure.c",
     "*door_open = (resp[5] & 0x01) != 0;",
     "*door_open = (resp[5] & 0x02) != 0;"),
    ("config: malformed add-length tolerated (desync)", "src/mos_config.c",
     "    if (add & 3u) return false;",
     "    if (0) return false;"),
    ("config: whole-descriptor fit check dropped", "src/mos_config.c",
     "    if (end - c < span) return false;",
     "    if (0) return false;"),
    ("discinfo: status mask 0x03 -> 0x07", "src/mos_discinfo.c",
     "    out->status             = (mos_disc_status)(b2 & 0x03u);",
     "    out->status             = (mos_disc_status)(b2 & 0x07u);"),
    ("watch: zero never overwrites id -> zero clobbers", "src/mos_watch_core.c",
     "    if (r.media_id != 0)        w->last_media_id = r.media_id;",
     "    w->last_media_id = r.media_id;"),
    ("watch: latency saturation -> raw underflow cast", "src/mos_watch_core.c",
     "    uint64_t delta = end >= start ? end - start : 0;",
     "    uint64_t delta = end - start;"),
    ("json: high-byte escape threshold 0x7f -> 0x80 (DEL leaks)", "src/mos_strings.c",
     "                    if (*p < 0x20 || *p >= 0x7f) {",
     "                    if (*p < 0x20 || *p >= 0x80) {"),
    ("bsd_name_format: domain upper-bound check removed", "src/mos_strings.c",
     "    if (unit < 0 || unit > (int64_t)UINT32_MAX) { buf[0] = 0; return false; }",
     "    if (unit < 0) { buf[0] = 0; return false; }"),

    # ---- audit-session code: nub gate, trusted_len, TOC ----
    ("nubgate: 0x03 dropped from eject set (kernel-divergent)",
     "src/mos_state_core.c",
     "         sk != 0x02 && sk != 0x03 && sk != 0x04 && sk != 0x08)) {",
     "         sk != 0x02 && sk != 0x04 && sk != 0x08)) {"),
    ("nubgate: 00/00 condition inverted to ||", "src/mos_state_core.c",
     "        (asc == 0 && ascq == 0 &&",
     "        (asc == 0 || ascq == 0 ||"),
    ("trusted_len: min -> max (transport overrun trusted)", "src/mos_pure.c",
     "    size_t trusted = allocated < transferred ? allocated : transferred;",
     "    size_t trusted = allocated > transferred ? allocated : transferred;"),
    ("toc: dup/non-ascending accepted", "src/mos_pure.c",
     "            if (track <= prev_track) return false; /* dup / non-ascending  */",
     "            if (0) return false; /* dup / non-ascending  */"),
    ("toc: trailing partial descriptor accepted", "src/mos_pure.c",
     "    if (cursor != span) return false;",
     "    if (0) return false;"),
    ("toc: header/descriptor count consistency dropped", "src/mos_pure.c",
     "    if (out->track_count != out->last_track - out->first_track + 1) return false;",
     "    if (0) return false;"),
    ("media_class: BD-ROM row reclassed dvd", "src/mos_strings.c",
     "        case 0x0040: case 0x0041: case 0x0042: case 0x0043:",
     "        case 0x0041: case 0x0042: case 0x0043:"),

    # ---- CLI-batch code: registry plumbing, bsd path, layout ----
    ("state_core: registry copy-through zeroed", "src/mos_state_core.c",
     "    out->registry_id = env->registry_id;",
     "    out->registry_id = 0;"),
    ("dev_node: negative half of the domain guard removed", "src/mos_strings.c",
     "    if (unit < 0 || unit > (int64_t)UINT32_MAX) return false;",
     "    if (unit > (int64_t)UINT32_MAX) return false;"),
    ("dev_node: domain upper-bound check removed", "src/mos_strings.c",
     "    if (unit < 0 || unit > (int64_t)UINT32_MAX) return false;",
     "    if (unit < 0) return false;"),
    ("dev_node: node prefix corrupted", "src/mos_strings.c",
     "    int n = snprintf(out, out_cap, \"/dev/disk%lld\", (long long)unit);",
     "    int n = snprintf(out, out_cap, \"/dev/dsk%lld\", (long long)unit);"),
    ("human: gutter shrunk to one space", "cli/human.c",
     "        fputs(\":  \", f);",
     "        fputs(\": \", f);"),
    ("human: NULL renders empty instead of dash", "cli/human.c",
     "    return s ? s : MOS_CLI_HUMAN_DASH;",
     "    return s ? s : \"\";"),
    ("human: key right-alignment dropped", "cli/human.c",
     "        size_t pad = keyw - l;\n        for (size_t s = 0; s < pad; s++) fputc(' ', f);",
     "        size_t pad = keyw - l; (void)pad;"),
    ("human: numeric column right-align ignored", "cli/human.c",
     "            bool   ra  = right_align && right_align[c] && !header_row;",
     "            bool   ra  = false && right_align && right_align[c] && !header_row;"),
]

def build_and_test(tree):
    srcs = [os.path.join(tree, p) for p in PURE + TESTS]
    binp = os.path.join(tree, "mt_tests")
    cc = ["cc", "-std=c11", "-O1", "-D_POSIX_C_SOURCE=200809L",
          "-I", os.path.join(tree, "include"),
          "-I", os.path.join(tree, "src"),
          "-I", os.path.join(tree, "tests"),
          *srcs, "-o", binp]
    r = subprocess.run(cc, capture_output=True, text=True)
    if r.returncode != 0:
        return "compile-rejected"
    r = subprocess.run([binp], capture_output=True, text=True)
    return "pass" if r.returncode == 0 else "fail"

def main():
    # Baseline builds in a tempdir like every mutant (fifth review, F6:
    # building it at SRC/mt_tests left a stray ELF in the live tree
    # that .gitignore and preflight both missed).
    with tempfile.TemporaryDirectory() as td:
        btree = os.path.join(td, "b")
        shutil.copytree(SRC, btree, ignore=shutil.ignore_patterns(
            "build*", ".git", "*.o", "dist", "mt_tests"))
        base = build_and_test(btree)
    if base != "pass":
        print(f"BASELINE NOT GREEN: {base}", file=sys.stderr); sys.exit(2)
    print(f"baseline: {base}\n")
    survivors, killed, broken = [], 0, []
    for label, relfile, find, repl in MUTANTS:
        with tempfile.TemporaryDirectory() as td:
            tree = os.path.join(td, "t")
            shutil.copytree(SRC, tree, ignore=shutil.ignore_patterns(
                "build*", ".git", "*.o", "dist", "mt_tests"))
            target = os.path.join(tree, relfile)
            with open(target) as f: txt = f.read()
            if find not in txt:
                broken.append(label); print(f"  ?? NO-MATCH  {label}"); continue
            if txt.count(find) > 1:
                broken.append(label); print(f"  ?? AMBIG     {label}"); continue
            with open(target, "w") as f: f.write(txt.replace(find, repl))
            res = build_and_test(tree)
            if res == "pass":
                survivors.append(label); print(f"  SURVIVED     {label}")
            else:
                killed += 1; print(f"  killed ({res:16}) {label}")
    print(f"\n{killed}/{killed+len(survivors)} killed; "
          f"{len(survivors)} survived; {len(broken)} unappliable")
    if survivors:
        print("\nSURVIVORS (untested behavior):")
        for s in survivors: print("  -", s)
        sys.exit(1)

if __name__ == "__main__":
    main()
