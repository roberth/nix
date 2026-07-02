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

## Reentrancy is downstream

`cb-repeated-cb-apply-diff-args` (the test committed as `debb52e83`)
exercises `(cb 10) + (cb 20)` — multiple cb-applies of the same
callback with different arguments in a single cached body. This
pattern is closely related to but distinct from the resolveCdiId
search:

- The search→Asks fix ensures walker computes fact-`from` fields at
  the correct K without iteration.
- Reentrancy specifically exercises **cb-apply reqhash** construction,
  where the `from` field's K value must match cold's at each of
  multiple applies within a single Q walk.

If alignment is correct throughout, multi-cb-apply-different-args
should follow. But it may need additional work at the cb-apply
reqhash construction site (writer-side `logAmbientApply` or the
walker-side dispatch equivalent) that is beyond the scope of just
removing the search. That work is tracked as a separate follow-up
once the alignment mechanism is in place.

If after the search→Asks work lands the reentrancy test is still
red, that is the signal that the cb-apply reqhash path specifically
still uses post-observation indexing and needs its own alignment fix.

## Anticipated simplifications

After alignment is correct, several existing mechanisms may become
unnecessary:

- **Snapshot-padded retry** in v13Walk. The retry aligns walker's
  cidasksWalk to cold's snapshot on primary miss. If primary walks
  succeed by construction (because K values match), the retry is
  dead code.
- **XOR-coincidence guard** in resolveCdiId. The guard rejects
  matches whose k happens to coincide by XOR-fold-coincidence
  rather than by semantic k-alignment. Once search is eliminated,
  there is no k-iteration and hence no coincidence to guard against.
- **QCidasksWalks snapshot table**. Cold serialises d1CidasksWalk at
  each logResult so the walker can align to cold's flush-time state.
  If growth is lockstep, this table is redundant.
- **Iterative pending-edge extension** and **iterative multi-round
  fold** in resolveCdiId. Both are alternative-k search variants
  layered on top of the base linear iteration. Removing the base
  search removes their reason to exist.
- **Progressive cross-Q pool pull**. Compensates for missing
  observations in walker.cidasksWalk at the moment of resolveCdiId
  by pulling observations from other Q chains. If alignment is
  correct, walker.cidasksWalk should have the needed observations
  at the right K without cross-Q pulling.

These simplifications are conditional on the alignment mechanism
being genuinely correct — not just "correct enough to pass the
existing tests." If some are still needed after search removal,
that's a data point about what edge cases the alignment mechanism
still misses.

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

## Success looks like

- `resolveCdiId` down from ~200 lines of k-iteration + fallbacks
  to a handful of lines with a single `scopeStateIdAt` call.
- Snapshot-padded retry deleted.
- XOR-coincidence guard deleted.
- Progressive cross-Q pool pull evaluated for deletion.
- 30/1 baseline preserved (reentrancy still red).
- Compile-clean, no compiler warnings.
- Full meson suite at 324/N (where N is the pre-existing skip count).
