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

The remaining project work is entirely walker-side alignment.
Sub-hypotheses to investigate (see research log Angle 9):
- H9.1 (growth pattern): walker's walk length at Q lookup ≠ cold's
  walk length at Q flush.
- H9.3 (content): sizes match but observation contents differ.

The Anticipated simplifications and Success criteria below still
apply — the target `resolveCdiId` cleanup and mechanism-deletion
attempts remain the same. Only the writer-side audit item is
retired.

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
- **XOR-coincidence guard** in resolveCdiId. **Expectation**: no
  k-iteration means no coincidence to guard against → deleted.
  **Alternative**: some coincidence source we haven't diagnosed
  survives, guard keeps rejecting.
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
- **Progressive cross-Q pool pull**. **Expectation**: walker's own
  cidasksWalk carries what's needed at the right K, no need to
  pull from other Q chains → deleted. **Alternative**: cross-Q
  observation sharing is genuinely needed for some pattern (cb-*
  interactions across siblings, e.g.) that pool-pull is the current
  answer to.

Each simplification lands as its own commit after the base
search→Asks change, verified against the same test-bounds table.
Any that resist deletion becomes a subsection in follow-up notes
explaining what it actually covers.

## Non-goals for this phase

- **Reentrancy fix.** Multi-cb-apply-different-args stays red; the
  test file documents it as the follow-up target.
- **Function characterization (task #87).** Content-based lambda
  CIDs are orthogonal to walk-index alignment. Sibling discrimination
  by observation still works as-is; sibling discrimination by
  structurally-distinct-but-observationally-equivalent lambdas is a
  separate design axis.
- **Terminals reindexing.** Terminals are the chain terminus (per
  the corrected Navigation invariant blockquote) and are not part of
  the old-hash indexing pattern. No changes here.
- **On-disk schema migration.** The cache is unreleased; a schema
  bump is acceptable and existing caches on developer machines can
  be invalidated as part of landing this work.

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
- Compile-cleanliness and full-suite green are hygiene, not scope.

The project delivers whatever combination of these turns out to be
achievable. A partial simplification with `resolveCdiId` cleaned up
and one or two mechanisms still load-bearing is still valuable if
the alignment story is clearer. Same if every simplification lands
except reentrancy. What we don't want is claiming closure on things
we haven't actually investigated.
