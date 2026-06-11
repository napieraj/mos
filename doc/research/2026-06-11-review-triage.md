# 2026-06-11 — review triage

Triage record for the post-DR-pivot review round. Two sources feed it:

1. An external review (uploaded-tree, Linux environment) delivered
   2026-06-11 with five findings, triaged below. The reviewer also
   reported their own check matrix (Debug + GCC ASan/UBSan + strict
   `-W… -Werror` builds, 219/219 portable tests; schema fixtures;
   doc-staleness; dist regeneration clean). Those results are
   reported-by-reviewer; locally reproduced on this branch: pure
   suite green, dist/ in sync after regeneration.
2. Four in-session review agents (general core, CLI/contracts,
   adversarial watch, memory safety) — findings to be appended to
   this file when triaged.

Every entry below was verified against the working tree
(branch `claude/watch-dedup-dr-refs-7fqt8g`) before triage, per
CLAUDE.md rule 6. Severity is the triager's, not the reviewer's.
Dispositions follow AGENTS.md Process rule 2: behavior changes need
more than a review point; comment/test alignment does not.

## External review findings

### E1. `list --json` carries display-safe identity; `status`/`watch --json` carry raw

**Verified.** `list_row` stores identity after `mos_safe_ascii`
(cli/common.c:208-210) and `emit_list_json` re-escapes the stored
form (cli/common.c:262-270) — documented as deliberate at
cli/common.c:200-207. But `status --json` and `watch --json` pass
RAW identity through the JSON escaper (cli/status.c:104→148-159,
cli/watch.c:67→121-129; their `mos_safe_ascii` call sites are the
human emitters only). For printable identity the three agree; for
hostile bytes the same device renders differently across commands
(`\\x1b…` literal text in list vs JSON-escaped raw bytes in
status/watch).

- Severity: medium (cross-command JSON contract divergence).
- Disposition: **maintainer decision required** — either (a) store
  raw in `list_row` and apply `mos_safe_ascii` only at the table
  emitter, aligning list with status/watch, or (b) keep
  stored-sanitized and document the list fields as display-safe in
  schema + README. Pre-first-tag, the schema ADR permits an in-place
  v1 change either way. Option (a) is the cleaner data contract;
  the in-tree comment defends double-escaping's safety but never
  addresses cross-command consistency, which is the actual finding.

### E2. "Profile-class change" fallback compares raw profile codes

**Verified.** `profile_class_changed_without_id`
(src/mos_watch_core.c:441-444) fires on ANY non-zero profile change
without media identity, so a same-class different-profile swap
(DVD-ROM 0x10 → DVD-R 0x11) fires too — the comment and name say
"class", and tests cover only cross-class (0x08→0x10,
tests/test_watch_core.c:345) and same-profile-no-fire
(tests/test_watch_core.c:373). The comment's own can't-see example
("DVD-R → DVD-R", src/mos_watch_core.c:435) is a same-PROFILE swap,
so the prose is internally loose, not just misaligned with code.

- Severity: low. The broader behavior is almost certainly the
  desired one: a profile flip with no identity available means a
  different disc; suppressing same-class flips would hide real swaps.
- Disposition: align name/comment/tests to the actual (any-profile)
  semantics — rename to `profile_changed_without_id`, reword the
  comment, add a same-class different-profile test pinning the fire.
  No behavior change.

### E3. `mos_cli_json_str` does unchecked `need + 1`

**Verified as written** (cli/io.c:21-27). `need + 1` could wrap only
if the escape of an in-memory string measured SIZE_MAX bytes — not
constructible on the target (identity fields are 8/16/4 bytes;
probe-path CF properties are kernel-bounded). No `mos_json_escape`
error sentinel exists to check today.

- Severity: nit (theoretical hardening).
- Disposition: cheap saturating guard, fold into a cleanup pass.

### E4. probe timestamp formatter ignores `clock_gettime`/`gmtime_r` failures

**Verified** (`format_rfc3339_utc`, cli/probe.c:104-117; reviewer's
line range matches). Failure requires libc-level breakage
unreachable on the target (64-bit time_t); the path is the
diagnostic `mos probe` subcommand.

- Severity: nit.
- Disposition: zero-init + return-check, fold into a cleanup pass.
  (CLAUDE.md's wrong-target-hardening note applies: this is
  developer-facing diagnostic tooling, not the adversarial-input
  surface.)

### E5. `mos_internal_dr_copy_identity_for_service` assumes non-NULL buffers when cap > 0

**Verified** (src/mos_dr.c:222-224). Internal helper, single in-tree
caller passes real stack arrays (src/mos_scsi.c); `NULL, nonzero`
would be a caller bug, not a reachable state.

- Severity: nit (internal precondition undocumented).
- Disposition: add `vendor && vcap`-style guards or a one-line
  precondition comment, fold into a cleanup pass.

## In-session agent findings

Pending — appended when the four agents report and their findings
survive verification against the tree.
