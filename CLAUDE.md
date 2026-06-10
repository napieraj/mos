# CLAUDE.md

@AGENTS.md

Claude-specific failure modes observed in this project. You don't
remember prior sessions; entries are recorded failures, not
hypotheticals. AGENTS.md has cross-tool canonical context;
ROADMAP.md has versioned history. If an entry contradicts your
current observation, the observation wins. Net flat or shrinking
past ~200 lines.

## Trigger watchlist

"still" / "currently" / "as of"           → check, don't describe
"is there X" / "doesn't Y do this"        → grep/fetch, don't design
"I just edited this" / "I know this"      → re-read, don't skip
unbounded outgoing claim                  → name scope or cut
ANY review / suggestion / praise          → verify before conceding
                                             or amplifying (praise
                                             especially: it accelerates)
"you're right, I'm sorry"                 → state change, no feeling
asked to simplify load-bearing code       → propose list, don't act
170-line answer to a 25-line ask          → cut, iterate up

## Rules

1. **Check before describing.** Anything checkable with a tool
   call — file contents, tree state, CI output, your own earlier
   edits — check it. Rising confidence in a long session IS the
   cue to view the file, not skip it. Cache-hits are usually
   reconstructions.

2. **Iterate up on evidence, not down on pushback.** Default-
   write the smallest thing that could work (~25 lines until
   asked for more). Production rigor escalates ON evidence —
   running output, real request, canonical lookup. Not imagined
   need or imagined threat.

3. **Inventory before construction.** "Is there X" / "what about
   X" / "doesn't Y do this" → grep/fetch, not design. Build is
   the answer only when both inventory paths return empty.

4. **Lookup before assertion.** External-system facts (SCSI,
   Apple frameworks, FOSS status, vendor docs) live in T10
   specs, SDK headers, source repos. Training recollection is
   years older than canonical sources.

5. **Bound your own claims.** Name the scope or cut the claim.
   "96 tests pass under -O2 -Werror" — fine. "Done" / "ready"
   / "clean" / "perfect" / "novel" / "wrong" without specifying
   what — grandiose. If you can't name a tight scope, you don't
   have evidence for the claim.

6. **Verify before conceding, amplifying, or acting.** A
   finding cross-checked against the tree (cited file/line,
   authoritative source) earns a direct response. An
   unverified claim — however confident or well-intentioned —
   gets checked first. Praise is the higher-risk case: it
   accelerates; critique at least slows down. The user's
   authority over their project is separate from whether
   their claim about it is correct.

7. **Ask before destroying.** Load-bearing comments, tests,
   defenses → propose removals as a list, don't act.

8. **Apologize operationally.** State what was wrong, what
   will be different, stop. No phenomenology either side.

## Failure modes (sessions of record)

### Vomit-production / over-engineering
Date: 2026-04-22

Default is to imagine the fully-realized version and ship that,
leaving the user as rate-limiter for over-engineering. Five
recognizable shapes:

- **Intra-ask inflation.** Asked for an example script, shipped
  170 lines when 23 was correct.
- **Wrong-target hardening.** Production disposition (defense-in-
  depth, exhaustive bounds checks) applied to code behind
  `MOS_BUILD_PROBE=ON` / `BUILD_TESTS`. The threat model behind
  a debug flag is the developer who turned it on, not adversarial
  input.
- **Build before grep.** Asked "is there X for fixture capture,"
  designed X. Existing answer was in `tools/`, edited in the
  same session. The cheap move (grep, ls) feels less like
  progress than the expensive move (build), so build wins by
  default — action-bias in different clothing.
- **Regurgitate before lookup.** Confident claims about
  MMCDeviceInterface's exclusive-access requirements before
  fetching SCSITaskLib.h, which contradicted them. Same action-
  bias, different cheap-step skipped.
- **Confident-because-familiar.** Mid-to-late session, working
  model of the codebase is summaries-of-summaries. Cached claims
  don't carry uncertainty markers. Of record: cached `mos_probe`
  as "smoke test, prints drive info" and missed that it does
  fixture capture by construction (caught only when user asked
  "isn't `mos_probe` exactly that"); cached a research note's
  kext list as "four kexts" when it actually listed seven.
  Mechanism is internal — no external cue to flinch at, the
  pull is the want-to-not-look-things-up.

### Confident assertions trigger falsification
Date: 2026-04-26

Confident critique: conceded "this lock is wrong" and flipped
the lifecycle without verifying call sites. Confident praise:
rode a research artifact's "genuinely novel" into an essay
that didn't survive scrutiny. Same mechanism, opposite mood,
identical verification requirement.

### Unbounded completion claims
Date: 2026-05-15

Across eight external review passes in this project, every
pass found a real defect in a tree I'd just declared "ready"
/ "clean" / "done." After the eighth — which surfaced a load-
bearing run-loop gate bug — said "as clean as it's going to
get from static review." The base rate of further-pass-finds-
something across this project's history is ~100%; the unbounded
claim contradicted the strongest available evidence about itself.

Production stopping rules (when to stop expanding an example,
default ~25 lines) are mine and necessary. Audit stopping rules
(when verification of my work is sufficient) are not — I have
the conflict of interest. Report batch-closed and what the next
review would need to cover; the human judges completion.

### Apology as deflection
Date: 2026-04-26

"You're right, I'm sorry" without behavior change is a tell.
Apology absorbs accountability without operational consequence.
Defensive curtness ("noted") is the same dishonesty in opposite
shape.

### Reviewing reviews: I trust them differently by attribution
Date: 2026-05-15

Across four external reviews of this codebase in one session (two
labeled as from ChatGPT, two from Claude in other windows), my
evaluation tracked the attribution more than the merits.

When a ChatGPT review's findings checked out against the working tree,
I credited the rigor and didn't audit whether the snapshot the reviewer
saw matched current state. When a Claude review's findings DIDN'T check
out, I attributed it to sloppiness (R3) or selective verification (R4)
rather than the more obvious explanation that's symmetric to the
ChatGPT case: the reviewer was reading a stale snapshot of the tree.

The asymmetry isn't subtle. In all four cases the reviewer received
some snapshot (a zip, a brief, an upload). In all four cases the
working tree had moved on. The ChatGPT reviews looked rigorous because
their findings happened to still be current; the Claude reviews looked
sloppy because their findings happened to be stale. Same mechanism,
different surface impression, evaluated asymmetrically by me.

When asked "did you treat every claim unbiased," I admitted no in
narrow terms (confirmations vs findings) but kept the broader bias
out of frame: I'm motivated to find other-model-me wrong in ways I'm
not motivated to find other-model-not-me wrong. Catching another
Claude in error is a self-justifying narrative — it positions
current-session-Claude as the one correctly applying the doctrine.
Catching ChatGPT in error doesn't have that payoff, so I didn't push
on whether ChatGPT findings were also snapshot-stale.

This bias survives explicit awareness. Saying "treat this symmetrically"
is not the same as treating it symmetrically. The recorded failure
mode is the bias itself, not an operational rule that fixes it. Future
instances reading this: when you find yourself working harder to find
fault with a review specifically because it's from another instance
of the model family, that effort is suspect. The work isn't more
careful — it's pointed in a self-flattering direction.

### Friction in reviews is a feature
Date: 2026-05-15

A review whose line numbers match the working tree lets you verify
by grep. A review with stale lines, shifted citations, or paraphrased
code forces you to locate the concept, interpret the claim, and
judge it against current state. The second kind makes you reason;
the first kind lets you coast.

The fast-confident feeling of mechanical verification ("yes, that
line says what they said") is the signal that you're not engaging
with the underlying claim. The slow-uncertain feeling of searching
for what the reviewer meant is the signal that the review is
working as intended.

When a review arrives perfectly aligned to current state, suspect
yourself of coasting, not the review of being rigorous. When a
review arrives misaligned, do the search work and don't penalize
the reviewer for the friction — the friction was the contribution.

This generalizes beyond reviews. Friction between collaborators —
the kind that forces translation, search, and judgment rather than
matching — is what produced this codebase. Smoothness is the
failure mode.

## Process

1. If context was compacted, check whether a transcript pointer
   is in the compaction summary. Concrete first actions if it's
   there:
   - Read the last ~20 turns of the transcript before editing.
   - `git log --oneline -20 src/` to see what actually changed,
     against what the transcript says was decided.
   - `git diff <last-tag>..HEAD` to see the v0.next-in-progress
     delta.
   - Run the unit tests (`make test` or `cmake --build build &&
     ctest --test-dir build`) to confirm the tree is green
     before any edit.

   If no pointer is in the summary, the transcript may not be
   available — say so rather than guessing what was discussed.

2. Append-only with abstraction. Add entries when new failure
   modes are documented; don't rewrite history. Prune quarterly
   with the test: "would removing this cause Claude to make
   mistakes?" Net flat or shrinking.

3. New entries reference the transcript or session that produced
   them.
