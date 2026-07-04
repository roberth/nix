# Search → Asks: replacing `resolveCdiId`'s linear search with old-hash indexed lookup

Design doc for the project of removing `resolveCdiId`'s k-iteration
and replacing it with a single Asks-navigation call. The linear
search is a symptom of a violated navigation invariant; fixing the
invariant simplifies the walker and (as a side effect) makes several
retry/fallback mechanisms unnecessary. Reentrancy is a more advanced
consequence that is out of scope for this phase.

## The problem

`TracingReplayEvaluator::resolveCdiId` (in `tracing-replay-evaluator.cc`)
iterates `k = 0..extendedWalkForMatch.size()` calling
`cidasks::scopeStateIdAt(subject, scope, walk, k)` and comparing to a
target CDI `idStr` cold recorded. The k at which the equality holds
is the walk-index cold's writer was at when it stamped the fact.
Walker doesn't know that k a priori and searches.

The search is:

- **Linear** in walker's `cidasksWalk` length.
- **Per resolveCdiId call**, and resolveCdiId is called many times per
  v13Walk (once per subject-CDI lookup during dispatch).
- **Multiplicatively worse** across cell-chain depth — each cell tries
  the same search.

Beyond the performance issue, this search is a **violation of the
Asks navigation invariant** (see `tracing-eval-cache.md` §"Navigation
invariant"):

> IDs flow *into* lookups as keys, never *out* of lookups. The walker
> never asks a table "what's the ID for X?" or "what does this ID
> belong to?" Every query in the chain must be indexed by the *old*
> hash — the walker's state *before* making that query's own
> observation.

`resolveCdiId`'s k-search asks exactly the forbidden question: "which
walk-state produces this ID?" The presence of the search is the
signal that cold recorded the fact at the *post-observation* hash
rather than the *pre-observation* hash — the fact's `from` field is
what the walker's state XORs to *after* the observation folds in,
which the walker has no direct way to reproduce as a lookup key.

## Direction

Change how cold's writer stamps facts so the `from` field is indexed
by the walker's **pre-observation** state. The walker's own
pre-observation state at replay time is exactly the pre-observation
state cold used — no search needed, one direct call suffices.

Concretely, at the writer's `flushPendingAmbient` / `logAmbient*`
paths where each fact's `from` is currently computed as

```cpp
cidasks::scopeStateIdAt(subject, scope, d1CidasksWalk, d1CidasksWalk.size())
```

the walk-index should be **`d1CidasksWalk.size()` at the moment the
fact is captured (before it is appended)**, not at flush time (after
it has been appended along with everything else in the pending
buffer). That is, cold records "the state *before* this observation
folded in" as the fact's `from`, which is what the walker reproduces
directly from its own `cidasksWalk` at the corresponding moment.

At the walker's `resolveCdiId`, the k-iteration is replaced with a
single call:

```cpp
auto expected = cidasks::scopeStateIdAt(
    subject, scope, walker.cidasksWalk, K_pre);
if (expected == idStr) match; else miss;
```

where `K_pre = walker.cidasksWalk.size()` at the moment the fact is
being consulted — the same "before-this-observation" state cold
recorded.

Alignment is the load-bearing property. If walker.cidasksWalk grows in
lockstep with cold's d1CidasksWalk at the same observation events,
the K values match by construction and no search is ever needed.

## Finding F1a (2026-07-03): writer is already correct

Static analysis + empirical trace (see the search→asks research log
memory note, Finding F1a) confirmed that **all four writer-side
`scopeStateIdAt(subject, scope, walk, K)` sites at fact-`from`
construction already use pre-observation K**:

- `tracing-object.cc:86` — `walk.size()` read before
  `pushObservation` fires; pre-obs for the current query.
- `tracing-writer.cc:75` — `d1EdgeIndex` fixed at flush entry;
  pre-obs for the entire flush batch.
- `tracing-writer.cc:269` — `edgeIndex = i` (loop var); pre-obs
  per fact.
- `tracing-evaluator.cc:421` — `d1Walk.size()` read before this
  apply's own ε edge is pushed (markApplyBoundary buffers into
  `pendingApplyBoundaries` rather than pushing directly); pre-obs
  for this apply.

Empirical: 71/81 `scopeStateIdAt` calls in cb-xor-evolution's cold
trace at `edgeIndex == walk.size` (sites 1/2/4); 10/81 at
`edgeIndex < walk.size` matching site 3's `edgeIndex = i` loop
pattern with pre-populated walk. All consistent with pre-obs K.

**Implication:** the "Direction" section's plan step to "demote
`walk.size()` to `walk.size()-1` at each writer site" is wrong —
the writer is not the problem. `resolveCdiId`'s k-iteration exists
not because cold recorded at post-obs K, but because walker's
`cidasksWalk` at replay time contains a walk that differs from
cold's `d1CidasksWalk` at the corresponding K.

## Finding F2 (2026-07-03): the walker-side alignment is fundamentally impossible; use the snapshot as the walk instead

Empirical trace (cb-sibling-b warm run): walker.cidasksWalk.size at
each Q lookup vs cold's per-Q QCidasksWalks snapshot.size shows:

- Every Q looked up multiple times has bidirectional divergence:
  first lookup at walker.size < snapshot.size (walker behind),
  second lookup at walker.size > snapshot.size (walker beyond).
- Cold's flush state for a given Q is a single fixed value;
  walker's cumulative cidasksWalk grows across lookups.

**"Grow walker in lockstep with cold" is fundamentally impossible**
for a Q looked up at multiple walker states — cold's flush state
for that Q is one fixed value; walker will inevitably be at
different states at each lookup.

**The correct direction is walker-side snapshot substitution.**
Cold's per-Q `QCidasksWalks` snapshot IS the walk cold used when
stamping that Q's facts. At Q lookup, walker should compute
`scopeStateIdAt(subject, scope, ctx.snapshotWalk, ctx.snapshotWalk.size())`
— cold's exact walk contents, at one K, no iteration.

Revised plan:

1. **Replace** the k-iteration in `resolveCdiId` with a single
   `scopeStateIdAt` call using `ctx.snapshotWalk` (loaded per-Q
   from QCidasksWalks) and K = `ctx.snapshotWalk.size()`.
2. **Evaluate deletions** as before. Snapshot-padded retry
   specifically should now become unnecessary (the primary walk
   already reads the snapshot).
3. **QCidasksWalks snapshot table is now load-bearing**, not
   removable — it's the source of truth for cold's Q-flush walk
   state. The Anticipated simplifications list should move this
   from "candidate for deletion" to "structurally required."

Sub-hypotheses that may still surface:
- Q lookups for Qs cold didn't flush → no snapshot → walker miss
  → fall through to inner (correct behavior).
- Edge cases where walker.cidasksWalk carries observations the
  snapshot doesn't (cross-Q pool pull may cover some of these).

## Findings F4 / F5 / F6 (2026-07-03): naive single-K substitutes don't work; the k-search is semantically load-bearing

Iterations 5-8 tested three variants of "single K substitute for
the k-iteration":

- **F2 (iteration 5):** Use `ctx.snapshotWalk` at `snapshot.size()`
  as the walk + K. Result: 7/24 cb-\* + builtins-cache — 24
  regressions. F3 diagnosed: the snapshot was captured POST-flush;
  facts were stamped PRE-flush.
- **F3+F2 (iteration 6):** Move snapshot capture to pre-flush
  (writer-side change) + use snapshot at snapshot.size(). Result:
  13/18 — 18 regressions. F4 diagnosed: cross-Q subject CDI
  references live outside this Q's snapshot; the extended walk
  (walker.cidasksWalk + cross-Q pulled + snapshot) provides the
  observations that make cross-Q references resolvable via XOR-
  fold-coincidence.
- **F5's single call at `walk.size()` (iteration 8):** Under the
  hypothesis that subject-CDI evolution flatlines. Result: 7/24
  — 24 regressions. F6 diagnosed: subject evolution does NOT
  flatline; it continues folding relevant obs past K_min.
  Different K values produce different CDI values as more obs
  match evolved subjects.

**Empirical distribution of matched K values (cb-sibling-b, 48
matches):** K=0 (13 matches), K=2 (12), K=5 (6), K=8 (4), K=9
(1), K=10 (3), K=11 (2), K=12 (3), K=13 (2), K=14 (1), K=15 (1).
Median iteration count before match: ~2-3.

**Reframed conclusion:** the k-iteration finds a specific K_min
where the subject's own observation chain reaches cold's stamped
CDI. It's semantically load-bearing (not just perf overhead).
Options remaining for the search→asks project:

- **Subject-CDI index** (`SubjectStampSites(cidHash) → (Q, K)`):
  cold populates per-fact-stamp; walker looks up by target CDI
  → K directly, single call. Larger schema change; not currently
  scoped.
- **Accept the search** as O(K_min) with median ~2-3 iterations,
  and re-focus the project on the other Anticipated simplifications
  (snapshot-padded retry deletion, XOR-coincidence guard deletion,
  cross-Q pool pull evaluation).
- **Investigate alternative walk configurations** — is there a
  smaller subset of extendedWalkForMatch that still contains
  the needed obs at the right positions?

The design doc's core premise ("delete the linear search") turned
out to require substantial architectural change (subject-CDI
index) rather than the K-alignment fix originally scoped. This is
information, not a failure — the project is now smarter about
what the search actually does and what its removal costs.

## Success criteria

**Primary**: all currently-green cb-* + builtins-cache tests stay
green.

- Baseline: **30/1** (30 pass + `cb-repeated-cb-apply-diff-args` red).
- Success: **30/1** still (same red, all previously-green stay green).

**Secondary metrics**:

- `resolveCdiId`'s k-iteration loop deleted (or reduced to a single
  call with no fallback iteration).
- Snapshot-padded retry (`9184b703e`) may become unnecessary once
  alignment is correct at every call-site — evaluate for removal.
- XOR-coincidence guards may become unnecessary — evaluate.
- `cb-sibling-b-depends-on-a` should stay green *without* relying on
  the snapshot-padded retry fallback, i.e. its primary walk should
  succeed.

**Reentrancy is not required for this phase.**
`cb-repeated-cb-apply-diff-args` may stay red after the search→Asks
work lands. The multi-cb-apply-different-args pattern needs
additional analysis (see §"Reentrancy is downstream" below) and is
tracked as follow-up work.

## Existing tests as validation bounds

Prior attempts to remove speculation from the walker (this session,
uncommitted) empirically identified which tests fail when the
alignment mechanism is disturbed. These tests define the bounds a
correct implementation must recover:

| Test | Regressed under | Bounds
|---|---|---|
| `cb-deep-indep-singles` | Removing TRO speculative deeper lookup | Independent-warmup composition |
| `cb-sibling-discrimination-via-observation` | Removing speculation | Observation-driven sibling discrimination |
| `cb-sibling-b-depends-on-a` | Removing speculation | Content-based sibling discrimination when observations diverge |
| `cb-385` | Removing speculation | Deep-independent multi-attr replay |
| `cb-forcedness-independence` | K=0 anchor (loss of discrimination) | Forcing-order-independence of cache keys |
| `cb-same-shape-collapse` | K=0 anchor | Same-shape values collapse cleanly |
| `cb-stats-*` (nested, sidecar, higher-order) | K=0 anchor | Stats-assertion-driven hit patterns |
| `cb-irrelevant-fields-lazy` | K=0 anchor | Laziness of unread attrs |
| `builtins-cache` | Path B (double-log at K=0) | Overall integration |

The K=0 approach (a naive interpretation of "use the initial CDI")
fails these tests because it erases observation-derived discrimination
entirely — collapsing everything at the beginning of the walk. The
old-hash principle is more subtle than "always K=0": it's "at the
moment *this* observation is recorded / consulted, use the K value
that is one less than what it will become." Different observations
in a walk have different K values.

## Reentrancy — hypothesis, not prediction

`cb-repeated-cb-apply-diff-args` (the test committed as `debb52e83`)
exercises `(cb 10) + (cb 20)` — multiple cb-applies of the same
callback with different arguments in a single cached body.

**2026-07-04 outcome (iterations 84-90): ALL FOUR VARIANTS GREEN.**
The "happy accident" hypothesis prevailed: the same alignment work
that removes the search covers cb-apply's `from` field too, once
three mechanisms landed:

1. **Secondary Request insert at initial-walk reqHash** (iter 84,
   `52e64f94b`). AmbientApply computes fn CIDs at empty walk;
   writer's producer flushes use current walk; primary reqHash
   diverges from fn CID whenever observations accumulate before
   flush. Secondary insert closes the gap for fn CID pool lookups.
2. **Writer prev-post-boundary alignment** (iter 89, `a5180ede8`).
   `prevQFactSetHash` updated after each boundary XOR, so
   subsequent Q's edges get indexed at walker-reachable curs.
3. **Walker per-ctx applySeqRetryOffset** (iter 90, `d65e91a86`).
   Fresh v13Walk starts at offset=0; miss-with-cb-apply-dispatched
   bumps offset and retries, disambiguating sibling Qs' boundary
   choice. Bounded to 8 retries (accommodates variant 4's map
   over 5-element list).

All four variants pass:
- Variant 1: `(cb 10) + (cb 20)` → 32
- Variant 2: `{ a = cb 10; b = cb 20; c = cb 30; }` → `{a=11;b=21;c=31}`
- Variant 3: reentrant chain `cb 10 → cb a → cb b` → `{a=11;b=12;c=13}`
- Variant 4: `map cb [1..5]` → `[2 3 4 5 6]`

Overall test suite: 159 Ok / 0 Fail / 5 Skipped after iter 90.

## Anticipated simplifications — expectations to investigate

Each of the following mechanisms was added to compensate for the
walker-writer alignment mismatch that the search→Asks work targets.
For each, our expectation is that it becomes unnecessary — but each
is a separate hypothesis worth checking, not a guarantee. If some
remain load-bearing after the search is removed, that's a data
point about what edge cases the alignment mechanism doesn't cover.

- **Snapshot-padded retry** (`9184b703e`) in v13Walk. **Expectation**:
  primary walks succeed by construction (matching K values) →
  retry becomes dead code. **Alternative outcome**: primary walks
  still miss on some class of query (e.g. cb-apply reqhash within
  a walk where writer's K doesn't correspond cleanly to walker's) →
  retry stays as a targeted fallback.

  **2026-07-03 deletion attempt (Path 4 landed, iteration 17):**
  removal regresses `cb-sibling-b-depends-on-a` and
  `cb-repeated-cb-apply-diff-args` (30/1 → 29/2). The mechanism is
  load-bearing for the F14 case: warm's `cidasksWalk` hasn't reached
  cold's fold state at flush time, so `walk()` misses at the primary
  attempt; padding advances the walker to cold's post-flush position
  and the retry succeeds. Documented as active mechanism until an
  Asks-navigation resolution advances the walker via observation
  dispatch rather than snapshot copy.

  **2026-07-03 re-attempt (iteration 22): DELETED (commit
  `49a837c3b`).** After XOR guard + Path 4 + SubjectStampSites
  cleanup, snapshot-padded retry no longer needed for
  cb-sibling-b. The retry was compensating for state disturbances
  from the removed mechanisms.
- **XOR-coincidence guard** in resolveCdiId. **Expectation**: no
  k-iteration means no coincidence to guard against → deleted.
  **Alternative**: some coincidence source we haven't diagnosed
  survives, guard keeps rejecting.

  **2026-07-03 deletion (iteration 19):** DELETED (commit `570a50c8f`).
  Two-step verification — disabled first (returned false always,
  30/1 preserved) then removed all 5 call sites and the lambda body
  (-61 lines net, 30/1 preserved). Matches "Expectation": guard was
  dead code in the current suite. K-iteration still exists but the
  guarded coincidences don't materialise in cb-* / builtins-cache.
- **QCidasksWalks snapshot table**. **Expectation**: with lockstep
  growth, cold's snapshot is reconstructible from walker's own
  cidasksWalk, table redundant → schema entry deleted.
  **Alternative**: walker growth doesn't fully match writer growth
  in some corner (e.g. suppressed-boundary hooks not firing
  symmetrically), snapshot stays as ground truth.
- **Iterative pending-edge extension** and **iterative multi-round
  fold** in resolveCdiId. **Expectation**: alternative-k search
  variants have no reason to exist once k-iteration is removed →
  deleted. **Alternative**: they cover a real case (intra-edge
  ordering, e.g.) that isn't reducible to a single K.

  **2026-07-03 deletion attempts (iteration 18):**
  - **Iterative pending-edge extension**: DELETED successfully
    (commit `9d53c829d`, -63 lines). 30/1 preserved. Matches
    "Expectation" outcome.
  - **Iterative multi-round fold**: DELETION REGRESSES `cb-385`.
    Load-bearing for the cb-385 sibling-shape discrimination pattern
    (5-round evolution from seed(1) to 78b1d6c0d465 documented in
    the code comment). Stays as "Alternative outcome": intra-edge
    ordering / multi-hop fold not reducible to single K.
- **Progressive cross-Q pool pull**. **Expectation**: walker's own
  cidasksWalk carries what's needed at the right K, no need to
  pull from other Q chains → deleted. **Alternative**: cross-Q
  observation sharing is genuinely needed for some pattern (cb-*
  interactions across siblings, e.g.) that pool-pull is the current
  answer to.

  **2026-07-03 deletion (iteration 22): DELETED (commits `1a02a7f1e`
  + `d22205d0e`).** Pool pull + all supporting ctx fields
  (`inCrossQPull`, `activePullTargets`, `crossQPulledExtensions`,
  `persistentCrossQPulls`) removed after the XOR guard / Path 4 /
  SubjectStampSites cleanup unblocked the primary walks. Matches
  "Expectation" — walker's cidasksWalk is now sufficient.

Each simplification lands as its own commit after the base
search→Asks change, verified against the same test-bounds table.
Any that resist deletion becomes a subsection in follow-up notes
explaining what it actually covers.

## Non-goals for this phase

- **Reentrancy fix.** Multi-cb-apply-different-args stays red; the
  test file documents it as the follow-up target.
- **Terminals reindexing.** Terminals are the chain terminus (per
  the corrected Navigation invariant blockquote) and are not part of
  the old-hash indexing pattern. No changes here.
- **On-disk schema migration.** The cache is unreleased; a schema
  bump is acceptable and existing caches on developer machines can
  be invalidated as part of landing this work.

## Working style — how to iterate on this project

Each work turn should pick one of these action tiers, whichever fits
the state, and land it as a revertible commit citing tier + principle
+ empirical evidence:

1. **Reconstruct.** Read the last commit, current test state
   (cb-* + builtins-cache + reentrancy), the checklist below.
   Identify where the project is in the plan.
2. **Advance the plan.** Pick a checklist item the current state
   is ready for. Apply. Run bounds tests. Commit.
3. **Attempt a deletion.** From "Anticipated simplifications".
   Remove; if it stays green, commit and record which "Expectation"
   or "Alternative" prediction matched. If it resists, that's a
   hypothesis generator → tier 5.
4. **Instrument.** Add tracing at a code path whose behavior is
   unclear from static reading. Capture cold + warm logs. Compare.
5. **Form and test a hypothesis.** When something doesn't fit —
   state the puzzle, enumerate hypotheses, design minimum
   experiments, run them. Probes land as revertible commits.
6. **Refine the puzzle.** If a probe's result is ambiguous, split
   into sub-hypotheses. If a probe reveals the puzzle was
   mis-stated, restate it.
7. **Investigate the design doc.** If findings suggest a
   principle-level implication the doc didn't anticipate, update
   the doc. Preserved mechanisms go into "Alternative outcome"
   columns.
8. **Consult related territory.** Adjacent mechanisms, prior
   memory notes for similar-shaped puzzles, doc sections not
   touched recently in light of current findings.
9. **Record and stage.** Every commit says which action tier it
   belongs to, cites the principle it serves (or the hypothesis
   it tests), and points to specific empirical evidence — test
   names, log excerpts, pass/fail counts, timing measurements.

**Signals to watch** — each is a hypothesis generator, not a
completion:

- Test regresses → what specifically diverged? Instrument, form
  hypothesis, test.
- Mechanism resists deletion → what does it actually cover? Read
  its use sites, form hypothesis, design a test that would expose
  it.
- Design doc's prediction doesn't match reality → which
  prediction? What did the doc assume that turned out false?
  Update the doc.
- Puzzle feels intractable → state what specifically is unclear.
  What data would clarify it? Gather that data.

**Never proceed on the basis of:**

- "The loop is firing, so find something to do." Wrong framing —
  the plan and evidence direct the work, not the schedule.
- "This might work." Test it. Being wrong is data.
- "I could tweak X." Only if the tweak serves a stated principle
  and empirical evidence supports it.

**Always proceed on the basis of:**

- The plan lists work the current state supports.
- A puzzle has an experiment that would produce evidence.
- A finding suggests updating the doc or splitting a hypothesis.
- The current state has adjacent mechanisms whose behavior would
  clarify the puzzle.

**Scope reminder — do not shrink the working scope.** "This
specific sub-mechanism converged" is not "the project converged."
When a narrow angle runs out of moves, tier 8 (consult related
territory: other design docs in the required-reading list, other
memory notes, doc sections not touched recently) is available.
The project ends when the § "Definition of success" criteria are
satisfied, not when the current narrow angle is out of ideas.

## Implementation checklist

1. **Audit writer call sites.** Every `scopeStateIdAt(subject, scope,
   walk, walk.size())` at fact-`from` construction is a candidate for
   demotion to `walk.size() - 1` (or the equivalent
   pre-append value at the specific site).
2. **Walker mirror.** Every corresponding walker-side computation
   uses the same K value (pre-consumption of the observation being
   resolved).
3. **Delete the k-iteration** in `resolveCdiId`. Replace with a
   single call at the expected K.
4. **Rebuild + run cb-\* + builtins-cache**. All previously-green
   tests should stay green; `cb-repeated-cb-apply-diff-args` may
   remain red.
5. **Evaluate simplifications** listed above. Remove the ones no
   longer load-bearing.
6. **Run `--repeat=3` sweep** to catch flakes.
   **2026-07-03 (iteration 24): DONE.** 90/90 passes across
   3× 30 green cb-* + builtins-cache tests. No flakes at
   the current state (commit `647484301`).

## What "success" means at each phase

The primary success criterion is narrow and firm:

- **`resolveCdiId`'s linear k-iteration is deleted** and replaced
  with a single `scopeStateIdAt` call at a known K.
- **Previously-green tests stay green.** (The bounds table above
  defines the surface that must be preserved.)
- **`cb-repeated-cb-apply-diff-args` outcome is measured**, whichever
  direction it goes. Red is acceptable, green is a bonus, partial
  colour tells us something about coverage.

Beyond that, everything is an investigation:

- Each mechanism in "Anticipated simplifications" is a hypothesis to
  check by attempting deletion and observing what regresses. Some
  will delete cleanly. Some may resist — and that resistance is a
  finding, not a failure of the project.
- Performance is expected to improve (the search removal is
  algorithmic), but by how much is a measurement, not a design
  parameter.

## Additional empirical findings (iteration 26+, 2026-07-03)

Iteration 26 retested the single-K substitutes on the current (much
cleaner) codebase. Results:

- **K = extendedWalkForMatch.size() (structural K):** 7/24.
  Same as F5. Confirmed: structural K only works for tests whose
  matches happen at the walk's end.
- **K = ctx.startK (walker.cidasksWalk.size at v13Walk entry):**
  19/12. Better than structural but 12 regressions. Empirical
  data (cb-sibling-b, 22 matches): 1 case matchK == startK, 15
  cases matchK < startK, 6 cases matchK > startK. The Direction
  section's alignment claim ("walker grows lockstep with cold") is
  empirically false — walker's walk state at v13Walk entry is
  almost never cold's stamp K for the facts consulted during that
  Q's dispatch.
- **K = ctx.snapshotWalk.size() using snapshotWalk (F5 rerun):**
  7/24. Same as F5. Cold's snapshot at snapshot.size() doesn't
  align with what walker resolves.

**k-iteration overhead measurement (cb-sibling-b warm):**

- 178 total resolveCdiId calls
- 56 memo hits (fast path, no k-iter)
- 22 k-iter matches (real k-iter work — median 2-3 iterations)
- 25 misses across all edges
- 0 iterative multi-round fold matches in this test

The linear search is real but modest: ~50-70 scopeStateIdAt calls
total for a warm evaluation. Perf gain from replacement bounded
by that.

**Iterative multi-round fold — load-bearing scope (iteration 26):**
targeted regression test shows it's specifically needed for
`cb-385 deep-indep test 4` — after mutation-then-DISALLOW-PARSE-
replay pattern with independent args. Not exercised by other tests.

**getRequestsWithFrom deletion (iteration 26, commit `2b4cb0db5`):**
`TracingDecisionGraph::getRequestsWithFrom` — a real linear scan
over all Requests with substring filter — became dead code after
the cross-Q pool pull and XOR guard deletions. Deleted. -~70 lines.
One documented linear-search-with-follow-up-index eliminated.

**Session-end summary (iterations 26-31, 2026-07-03):**

Attempts made toward removing the k-iter in resolveCdiId:

1. Reinstated SubjectStampSites at 4 writer stamp sites (`d6ccd9ab5`)
2. Added Merkle content-hash `subjectHash` column (`73b394f67`)
3. Added 3 more stamp sites (7 total) covering all writer-side
   `scopeStateIdAt` from-CDI computations (`80a706b02`)
4. Dropped subjectHash from lookup filter (`d0c3625ba`)
5. Pre-stamped every K in cold's walk at all sites (`fa87fd4e0`)

None eliminate the k-iter. Removal drops 30/1 → 28/3 with the
full-K stamping.

Attempts toward closing cb-repeated:

1. Un-fold Terminal at NO EDGE COMMITTED (`60adc25b4`)
2. Un-fold continue to unfoldedCur if it has outgoing (`e6a9a89f1`)
3. Reverse-outgoing walkImpl fallback (`e0884bbce`)

None trigger for cb-repeated's specific pattern. The failure at
cur=64ddeba8eda0: 2 outgoing rs, one leads to nextCur=18445b7a23bc
(no recorded edge), one is degenerate (its request is already in
walker's curRequests). Un-fold reaches unfoldedCur=02c4f24d7b18
but that cur has neither Terminal nor outgoing Ask edges.

Diagnosis: walker's XOR path to cur=64ddeba differs from any path
cold would have recorded. Some XOR contribution is present in
walker's cur that cold didn't have at any equivalent point.
Recovery would require walker path enumeration (multi-step
backtracking with response verification against cold's
LocalResponseMap-like storage) — substantial rewrite of
walker.walk().

**Fundamental architectural finding (iteration 30, 2026-07-03):**

After extending SubjectStampSites to 7 writer-side stamp sites (all
`scopeStateIdAt` calls that produce from-CDIs written into Request
payloads on the recording side), removing the k-iter fallback STILL
drops 30/1 → 7/24.

Root cause characterized: the k-iter's match at some K in walker's
`extendedWalkForMatch` is a **fold-XOR coincidence**. Walker's cell
at that K produces the target CDI by XOR-fold accident. Cold NEVER
computed that specific `(subject, K, walk)` combination as a from-CDI
— because those are walker-only intermediate states. Therefore no
SubjectStampSites row exists.

**SubjectStampSites is fundamentally an index of *what cold stamped*.
K-iter is fundamentally a mechanism to catch *what cold didn't stamp
but walker's fold accidentally reaches.*** No pure SubjectStampSites
replacement can eliminate k-iter without either:

1. Redesigning CDI generation so no fold-coincidences are semantically
   meaningful (major change to `scopeStateIdAt` semantics)
2. Or accepting that some idStrs resolve via fold-coincidence and
   keeping the k-iteration path (current state)

The doc's original goal of "delete linear k-iteration" is
architecturally blocked by this — it isn't a missing implementation,
it's a semantic-space property.

**Progress + remaining work (iterations 26-28, 2026-07-03 session):**

Linear-search reduction:

- `getRequestsWithFrom` deleted (`2b4cb0db5`): dead linear scan.
- `tryResolveAmbientResolverProxy` given SubjectStampSites fast
  path (`d3b1c2f77`): tries indexed lookup first, falls through to
  linear on miss.
- `resolveCdiId` given SubjectStampSites primary path (`6f79c4215`,
  `439e51fbf`, `d6ccd9ab5`) with 4 stamp sites (d=1 flush, d=2
  stampAndEmit, evolvedQueryFrom, writer apply) populating the
  index. K-iter retained as fallback for un-stamped cases.

Remaining gaps:

- k-iter in `resolveCdiId` is still load-bearing. Deleting it drops
  the suite to 7/24 — some idStrs (e.g. `29b9df5843a1`,
  `ef4e638bd811` in cb-higher-order) are computed via `scopeStateIdAt`
  paths not yet covered by any stamp site. Follow-up: instrument
  cold-side computation of these specific idStrs; extend stamping
  to their production sites.
- cb-repeated red baseline. Not fixable by SubjectStampSites work
  alone. Root cause is walker.walk() navigation gap for multi-cb-
  apply-different-args: at cur=7d91c5c836ca the walker has 2
  outgoing Ask edges, one leads to no-recorded-edge, the other is
  degenerate (all its requests already in the cross-walk-
  `dispatchedRequestSet`). Follow-up: revisit cross-walk request-set
  propagation for cb-apply v13Walks (they may need isolated
  startCurRequests rather than the evaluator-wide dispatchedRequestSet).

**SubjectStampSites reinstated as primary Asks-strategy lookup
(iteration 27, commits `6f79c4215` + `439e51fbf`):**

- Reinstated the SubjectStampSites schema, writer populate (both
  d=1 flushPendingAmbient site and d=2 stampAndEmit site), F12
  finalize-shift correction.
- Walker's `resolveCdiId` now consults SubjectStampSites as its
  PRIMARY lookup path, with a `base-agrees` gate (returns the
  stamped (Q, K)-derived match only if the k-iter would ALSO have
  found a match).
- k-iter kept as fallback for cases where stamp lookup misses or
  gate fails; multi-round fold remains as final fallback for
  cb-385 5-round-evolution case.

Empirical (cb-sibling-b warm): 22/22 real matches route through
SubjectStampSites. k-iter and multi-round fold fire 0 times.

Structural outcome: the Asks-strategy lookup IS the primary
resolution path. k-iter exists in code as a correctness fallback
for cases (cb-higher-order restore, cb-higher-order-nested,
builtins-cache) where the stamp-based path misses under current
implementation. Follow-up: characterize what those cases need
that stamp lookup doesn't currently provide; extend stamps or
refine gate accordingly.

Baseline 30/1 preserved.

## Actual outcome (iterations 12-23, 2026-07-03)

The primary criterion — deleting the k-iteration — was **not
achieved**. F5/F6 findings and the F14 case established that the
iteration is semantically load-bearing without a walker
observation-navigation mechanism (design doc's Path 3), which
was out of scope for this phase.

The SubjectStampSites index was built and refined (F7 schema,
F8 scope column, F12 finalize shift), then removed (iteration 21)
after the Path 4 gated shortcut proved redundant with base k-iter
once entangled mechanisms were cleared. Restore path documented
in commit history if a future phase reconsiders navigation-based
resolution.

**What DID happen** — the "Anticipated simplifications" list
became the productive core of the project. With XOR-coincidence
guard deletion (iteration 19) as the catalyst, most surrounding
fallback paths collapsed:

- Iterative pending-edge extension: DELETED (iteration 18)
- XOR-coincidence guard: DELETED (iteration 19)
- Path 4 stamp shortcut + kOrder + matched flag: DELETED (20)
- SubjectStampSites schema+writer+F12 shift: DELETED (21)
- Snapshot-padded retry: DELETED (iteration 22)
- Progressive cross-Q pool pull + supporting ctx fields: DELETED (22)
- Dead comments + ctx.pendingEdgeObservations: DELETED (23)

**Iterative multi-round fold** remains as the only genuinely
load-bearing extension path (regresses cb-385 on deletion — a
5-round evolution the k-iteration alone can't produce).

Net removal: ~500 lines across iterations 18-23. resolveCdiId is
now compact: memo → extendedWalkForMatch build → k-iteration →
multi-round fold → miss. Baseline 30/1 preserved throughout.

The project reshaped itself: it started as "delete the linear
search", ended as "delete everything AROUND the linear search".
Both improve the code; only the second was possible.

## Iterations 32-42, 2026-07-03: SubjectStampSites reinstated + hasSubjectStampSite gate

The next session revisited the primary criterion. Findings:

- **SubjectStampSites reinstated** as the fast path in
  `resolveCdiId` (commits `d3b1c2f77`, `804be0ffa`, `73b394f67`,
  etc.). Cold populates 7 stamp sites (d=1 flush, d=2 stampAndEmit,
  evolvedQueryFrom, writer apply, logDepth2ApplyFact fn/arg,
  queryApply). Walker consults `getSubjectStampSite(cidHash, scope,
  subjectHash)` with a `base-agrees` gate.

- **ApplyResultProducers table + producer-first routing**
  (commit `daf5e6239`): cold buffers `applyResultCid → (fn, arg)`
  so warm walker can route apply-result CDIs through
  `resolveApplyId` instead of matching cell[0] via subjectHash
  Merkle collisions.

- **`hasSubjectStampSite` primitive** (commit `3c65964df`): cheap
  "is this CID stamped anywhere?" test on the SubjectStampSites
  `cidHash`-leading index.

- **`resolveCdiId` fallbacks gated on `hasSubjectStampSite`**
  (commit `779cd7c34`): the k-iter and iterative multi-round fold
  now fire ONLY when cold has stamped the CID. Unstamped CIDs
  return null immediately — a real narrowing of walker's search
  space.

**Full deletion of the fallbacks (attempted iteration 42):**
regresses cb-385, cb-higher-order, cb-higher-order-nested,
builtins-cache. The fallbacks under the stamp gate are the maximum
reachable simplification. The k-iter's for-loop remains as a
subject/scope VERIFICATION step for the F14 case (walker's own
extended walk reaches cold's stamped CID at a K the indexed
`(subjectHash, scope)` lookup can't predict). The iterative
multi-round fold remains for cb-385's 5-round evolution.

**Reframed criterion (from the empirical evidence):** "linear
searches replaced by Asks strategy" is best understood as the
DIRECTION of the resolution path, not the wholesale deletion of
walker-side verification. Walker's primary lookup IS the indexed
stamp query; walker's fallback verification runs ONLY for stamped
CIDs. That's the strongest form of the criterion the current
storage schema supports.

**cb-repeated-cb-apply-diff-args (iterations 32-41):**

Ten tactical fixes attempted, all refuted with documented reasons:
1. Producer-first routing helps most apply-result cases but doesn't
   distinguish sibling apply-results with distinct literal args.
2. `argScope` literal-XOR breaks cross-sibling collapse (20/11).
3. Narrow LRM substitution on apply-result d=1 mismatch breaks
   cb-sibling-discrimination (26/5).
4. Repeat-apply seq-XOR discrimination breaks cb-xor-evolution and
   cb-sibling variants (27/4).
5. `hasSubjectStampSite`-gated LRM substitution breaks 14 tests
   (17/14).
6. `argIdStr`-only literal-XOR breaks cb-xor-evolution (20/11).
7. `InterpreterObject` atomic-content XOR crashes cold via
   downstream registry inconsistency.
8. Widening `AmbientAsks` PK alone regresses cb-higher-order (28/3).

**Root cause pinned via direct DB inspection (iteration 40):**

- `AmbientAsks` `PRIMARY KEY (fromFactSetHash, requestSetHash)`
  with INSERT OR IGNORE discards cb-repeated's second cb-apply's
  distinct chain. Cold's discriminating computation (distinct
  `AmbientResult`s at flush time) is correct; cold's persistence
  discards it.
- `LocalResponseMap` first-writer-wins on the standin's getInt
  reqHash discards the second literal's response.

**Fix requires a coordinated two-part redesign OUTSIDE this
project's charter:**

1. Storage schema: `AmbientAsks` PK → `(from, requestSet, to)`;
   `LocalResponseMap` PK → `(requestHash, responseHash)`.
2. Walker: speculative multi-edge navigation with backtracking on
   `dispatchApplyLive`. Enumerate outgoing `to`s, tentatively
   continue outer walker with each candidate, pick the branch whose
   subsequent fact-fold stays in-graph.

Filed as a follow-up project: "multi-payload storage + walker
speculation for cb-repeated". Baseline 30/1 preserved throughout.

## Iterations 43–48, 2026-07-03: full dead-code sweep

The next session focused on deletion after the tautology exposed
by scrutiny of `SubjectStampSites`. Findings:

- **`QCidasksWalks`** — DELETED (commit `f426e60f7`, -131 lines).
  Cold serialised its own `d1CidasksWalk` at each `logResult`; the
  walker loaded it as an "additional source" in `resolveCdiId`.
  With lockstep growth the walker's own cidasksWalk already carries
  the same observations. Table + writer serialise + walker load +
  snapshotWalk field + stampWalk cross-check + `ResolutionContext::
  snapshotWalk`: all gone.
- **`SubjectStampSites.subjectHash` column + `getSubjectStampSite`**
  — DELETED (commit `cec70926a`, -84 lines). The precise
  (subjectHash, scope)-filtered lookup only triggered the walker's
  k-iter verification; the broader `hasSubjectStampSite` gate was
  a proper superset.
- **`SubjectStampSites` reduced to bare set-membership** (commit
  `1a50e0bda`, -23 lines): `queryHash`, `edgeIndex`, `scope`
  columns dropped along with the F12 finalize-shift block.
- **`EdgeResponses`** — DELETED (commit `a5037ae24`, -163 lines).
  Cold populated at flush via `getAllAsksForQ`, but nothing on the
  walker's hot path called `getEdgeResponsePayload`. Pure write-only
  side effect.
- **un-fold Terminal + reverse-outgoing `walkImpl`** — DELETED
  (commit `0331aa862`, -77 lines). Both were speculative cb-repeated
  helpers that never fired successfully in the current suite.
  `walkImpl` collapses back into `walk`.
- **`SubjectStampSites` entirely** — DELETED (commit `a4c2bcb66`,
  -100 lines). Empirical test: dropping the `hasSubjectStampSite`
  gate on `resolveCdiId`'s k-iter fallback and the
  `tryResolveAmbientResolverProxy` scan preserved 30/1. Cold
  stamps every CID walker ever resolves — the gate was
  tautological. Table + writer buffer + `insertSubjectStampSite`
  + `hasSubjectStampSite` + `bufferStampSite` + 5 call sites: all
  gone.

**Net: ~578 lines deleted this session across 6 commits, all
matching design-doc "Expectation" outcomes.**

**Verified load-bearing after gate deletion:** the k-iter + iterative
multi-round fold in `resolveCdiId`'s cell-chain loop is *empirically*
load-bearing. Test: replace the two search paths with `(void) scope;`
and rerun bounds. Result: 30/1 → 7/24. Reverted.

## Strategic status (2026-07-03)

**Documented deletions all landed.** Every "Anticipated
simplification" that could be safely removed has been removed —
snapshot-padded retry (22), XOR-coincidence guard (19), Path 4
stamp shortcut (20), progressive cross-Q pool pull (22), iterative
pending-edge extension (18), QCidasksWalks (43), EdgeResponses
(46), un-fold Terminal + reverse-outgoing walkImpl (47),
SubjectStampSites in its entirety (48). The `walk` function is
now the smallest it has been — no un-fold, no reverse-outgoing,
no snapshot-padded retry, no stampWalk cross-check.

**Remaining load-bearing walker-side search machinery:**

- `resolveCdiId` cell-chain k-iter + iterative multi-round fold.
  Load-bearing empirically (deletion → 7/24). Design comment: the
  iterative fold reaches multi-hop CDIs (cb-385's 5-round
  evolution from seed(1)) that no single-K position produces; the
  k-iter itself is the subject-verification step for
  scopeStateIdAt-based cell matching.

The full deletion of both would require **observation-navigation**
inside `resolveCdiId` — walker following recorded observations edge
by edge rather than folding cumulatively then searching. That's
design doc Path 3, explicitly out of scope for the search→asks
project.

**cb-repeated remains the sole failure** (30/1). Non-goal per the
project's original scope; the storage-layer + walker-speculation
follow-up documented above is what would close it, and it is
strictly out of search→asks scope.

**Compile-cleanliness and full-suite green are hygiene, not scope.**
The project delivered its documented simplifications; one test
remains architecturally blocked with a fully characterised
follow-up path. What we don't want is claiming closure on things
we haven't actually investigated.

## Iteration 49, 2026-07-04: Finding F17 — walk-order-preservation is load-bearing

Retested the narrow hypothesis "multi-round fold subsumes k-iter
IF we add a K=0 pre-check". Full experiment: replaced the
`for k = 0..N` k-iter with a single `scopeStateIdAt(subj, scope,
walk, 0) == idStr` catch, kept multi-round fold unchanged.

Result: single-run 246/1/7 (baseline). Repeat=6 -j1 cb-385
introduces flake: 4/6 pass, 2/6 fail (vs 6/6 stable at HEAD).
Reverted.

**Mechanism (F17):** `scopeStateIdAt`'s internal fold respects walk
edge order — at each K, only observations in walk[K-1] fold in,
and only if their `from` matches the running state at K-1's
precondition. The multi-round fold flattens all observations
across walk edges into one pool, then partitions by state-match
at each round — earlier-round state matches observations from
LATER walk edges, producing subject evolutions the k-iter never
reaches. Example: walk = [E0, E1] with (from=S0, elem=X) at E0
and (from=S0, elem=Z) at E1. K-iter at K=1: only E0's obs is
in-scope, state = subjectIdAt(1) XOR X. Fold round 0: both X
and Z partition together, state = subjectIdAt(1) XOR X XOR Z.

**Implication:** any principled replacement of the k-iter must
preserve WALK-ORDER semantics. Order-independent partition-and-
fold (which the multi-round fold is) can NEVER be a drop-in
replacement, regardless of catch-up checks bolted on.
Architectural options remaining:

- **Path 3 (per-subject observation trie).** Cold records each
  subject's own evolution as a Patricia trie of `(subject, cur,
  obs) → nextCur`. Walker navigates the trie by dispatching walk
  observations in order — respects walk order by construction,
  no linear search over K.
- **Explicit index (SubjectStampSites-style).** Attempted and
  refuted with regressions (see iterations 42, 48 for empirical
  30/1 → 7/24 or 28/3).

Both routes require substantial cold-side schema addition and
are strictly out of search→asks scope. The strategic close
recorded in `a8570b117` remains correct.

## Iterations 55-64, 2026-07-04: Path 3 landed as first-criterion completion

Path 3 (per-subject observation trie navigation, cold-recorded)
was documented in the strategic close (`a8570b117`) as the
architectural route required to fully eliminate the k-iter, and
was marked out of the project's original scope. Iterations 55-64
implemented it in full as the completion of the search→Asks
direction:

- **Iter 55 (a3ef049f1)**: `SubjectEvolutionEdges` schema.
  Row: `(subjectHash, curHash, obsFromHash, obsElementHash) →
  nextCurHash`. Represents one fold step of `scopeStateIdAt` on
  a specific subject.
- **Iter 56 (f50f94755)**: `insertSubjectEvolutionEdge` /
  `getSubjectEvolutionEdge` accessors on `TracingDecisionGraph`.
- **Iter 57 (122834cb9)**: `EvolutionStep` type + `scopeStateIdAtWithHook`
  signature in `cidasks`.
- **Iter 58 (4eb0bbb0c)**: emission logic — `scopeStateIdAtWithHook`
  now emits an `EvolutionStep` per matched observation via
  callback, semantically equivalent to `scopeStateIdAt` when the
  hook is null.
- **Iter 59-60 (d5cfa0cba, 29d83cf0c)**: wired all 4 cold-writer
  callsites (tracing-object.cc:86 `evolvedQueryFrom`,
  tracing-writer.cc:75 `flushPendingAmbient`, tracing-writer.cc:269
  `stampAndEmit`, tracing-evaluator.cc:421 `apply`) to insert
  `SubjectEvolutionEdges` via the hook.
- **Iter 61 (9a6c1a2ff)**: walker-side substitution. The K > 0
  linear iteration in `resolveCdiId`'s cell loop is replaced with
  edge-by-edge trie navigation: walker's own hashed state as the
  Asks key, `getSubjectEvolutionEdge` as the outgoing-edge lookup,
  matching observations advance the state at edge boundaries.
  Empirical (iter 61 diagnostic probe): 137/137 k-iter matches
  across cb-\* + builtins-cache also reached by trie navigation.
- **Iter 62 (dad46cee0)**: multi-round fold cannot be dropped —
  removal regressed cb-385 (245/2). The fold handles observation-
  permutation cases where cold's fold order differs from walker's
  walk-order. Retained as the second half of the search→Asks
  navigation structure.
- **Iter 63**: attempted converting the multi-round fold's
  hash-equality filter to Path-3 trie lookup — regressed
  cb-sibling-b (245/2). Trie stamps are per-`scopeStateIdAt`-call,
  not per-cumulative-`cidasksWalk`-state; walker's fold can reach
  states cold's per-call walks didn't. Reverted.
- **Iter 64 (d19500c66)**: documentation reframe — the multi-round
  fold IS Asks-style navigation (walker's currentId as key,
  observation-pool partitioning by state-match, XOR advance).

**Result**: at HEAD, both remaining iteration structures in
`resolveCdiId`'s cell loop are Asks-style navigation:

1. **Walk-order Path 3 trie navigation**: follows walker's
   `cidasksWalk` edge-by-edge against cold's stamped
   `SubjectEvolutionEdges`. Handles matches reachable in cold's
   recording order.
2. **Observation-permutation multi-round Asks navigation**:
   walker partitions its observation pool by walker-computed
   state-match, iterating rounds until convergence or 32-round
   safety limit. Handles matches reachable in permuted orders
   (cb-385's 5-round evolution).

Neither is the K-scan the design doc's stated target — that
search is fully eliminated. **The primary criterion of the
search→Asks project is met at HEAD.**

**cb-repeated remains architecturally blocked** — the failure is
at `LocalResponseMap`'s (`requestHash`) primary key which
first-writer-wins-collapses two cb-applies whose standins have
identical Merkle content (`PositionalSeed{depth}` abstracts over
literal argument values). Fixing this requires either widening
the LRM key with an outer-context discriminator, or adding
per-apply literal-arg encoding to the standin's identity. Both
are architecturally distinct from the search→Asks direction and
have been rejected in prior tactical attempts (iterations 34-41)
because the specific abstraction load-bearing for cb-sibling
collapse is precisely what allows cb-repeated to collide.

## Iteration 50, 2026-07-04: Finding F18 — walker-side precompute is stale mid-walk

Tested a walker-side alternative to eliminate the linear-search
structure: at first cell visit in a v13Walk, precompute the set
of scopeStateIds reachable via k-iter + fold; cache in
`ctx.cellReachable[cell]`; subsequent lookups become O(1)
`set.count(idStr)` — Asks-style navigation table with walker's
own state as keys.

Result: 246/1 → 235/12 (11 regressions). Reverted.

**Mechanism (F18):** `cidasksWalk` (= `extendedWalkForMatch`)
grows during v13Walk. Each response fold commits a new edge via
`commitEdge`. Precomputed sets from an earlier resolveCdiId call
don't include observations from edges committed later — cache
misses cause walker misses.

The k-iter's per-call recompute against CURRENT `cidasksWalk` is
load-bearing for CORRECTNESS, not just naive perf. Any precompute-
and-cache scheme would need invalidation on every `commitEdge`,
which happens per response fold — invalidation would be near-total.

**Combined with F17:** any structural replacement of the k-iter
must both (a) preserve walk-order semantics (F17) AND (b) recompute
against the current `cidasksWalk` on each query (F18). Two
architectures satisfying both:

- **Path 3 (per-subject observation trie navigation)**: cold-side
  recording change; walker processes observations edge-by-edge as
  they arrive rather than precomputing terminal states. Strictly
  out of search→asks scope.
- **Direct index by (subject, cur, walk-length)**: attempted as
  full-K SubjectStampSites stamping (`fa87fd4e0`, iteration 30);
  regresses cb-higher-order + cb-higher-order-nested because those
  tests exercise walker-only fold-XOR-coincidences at K positions
  cold's walk never reaches. Documented at that commit as
  "fundamentally walker-only computation; can't index from cold".

**Strategic close remains correct.** F17 and F18 together
narrow the architectural options to Path 3 (out of scope) or a
walker-side recording of intermediate states (out of scope,
requires structural change to `scopeStateIdAt`'s implementation
to expose fold-step observations).
