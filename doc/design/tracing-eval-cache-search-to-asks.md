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

Our current hypothesis is that this pattern needs work at the
cb-apply reqhash construction site (writer-side `logAmbientApply` or
walker-side dispatch equivalent) beyond just removing the search,
because the `from` field's K value must match cold's at each of
multiple applies within a single Q walk. But we don't actually
know:

- **It could turn green** as a side effect of the alignment work,
  if the same K-precision that removes the search also happens to
  cover cb-apply's `from` field. That would be a happy accident
  worth investigating.
- **It could stay red**, in which case the cb-apply reqhash path
  specifically still uses post-observation indexing and needs its
  own alignment fix. That's the follow-up phase.
- **It could partially go green** — some variants pass, others
  don't. Each partial pattern is a data point about what alignment
  covers and what it doesn't.

The red test is data. Whichever outcome, we've learned something
about the shape of the alignment mechanism.

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
- Compile-cleanliness and full-suite green are hygiene, not scope.

The project delivers whatever combination of these turns out to be
achievable. A partial simplification with `resolveCdiId` cleaned up
and one or two mechanisms still load-bearing is still valuable if
the alignment story is clearer. Same if every simplification lands
except reentrancy. What we don't want is claiming closure on things
we haven't actually investigated.
