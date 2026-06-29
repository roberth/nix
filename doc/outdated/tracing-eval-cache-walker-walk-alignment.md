# Walker-side walk alignment with writer flushes

Follow-up to [`tracing-eval-cache-per-arg-completion.md`](./tracing-eval-cache-per-arg-completion.md):
how to close the remaining cb-sibling warm-DISALLOW_PARSE gap.

## Required reading

Read in this order, in full — per `CLAUDE.md`'s design-doc discipline,
partial reading of these produces proposals that violate invariants
they didn't see:

1. [`tracing-eval-cache.md`](./tracing-eval-cache.md) — v13 data
   model, Asks/Terminals schema, walker fast/slow paths, FactSet
   XOR-fold semantics.
2. [`tracing-eval-cache-content-identity-via-asks.md`](./tracing-eval-cache-content-identity-via-asks.md)
   — cidasks formula, subject/scope/walk parameters, principles 3/5/7
   on per-flush evolution, XOR-audit catalogue (components F/G).
3. [`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md)
   — depth-1 vs depth-2 layering, AmbientAsks, the d1/d2 flip at
   cb-apply boundaries, dispatcher routing.
4. [`tracing-eval-cache-per-arg-completion.md`](./tracing-eval-cache-per-arg-completion.md)
   — option 1 vs option 2, the cold/warm asymmetry postmortem, the
   1:1 restructure that this doc builds on.

## The gap

With the 1:1 restructure (`5e9758253`) plus option 2 apply triePos
(`6c4250cef`) plus option (b) late-d2-obs re-open (`8d9d4c58e`), the
writer-side state evolves correctly across siblings and the late
probes' responses land in LocalResponseMap. But warm cb-sibling
still misses sibling B's Q's. Concretely, on cb-sibling at warm:

- `walker.cidasksWalk.size = 7` at sib B's `TracingReplayEvaluator::apply`.
- `writer.d1CidasksWalk.size = 27` at cold sib B's `TracingEvaluator::apply`.

Same formula, different walk size → different `applyCdi_B` (`f0f2…`
at warm vs `b626…` at cold). Every Q derived from sib B's applyCdi
hashes differently than what cold recorded; lookups MISS, fallback
fires, and `_NIX_DISALLOW_PARSE` blocks the parse.

## Why the sizes diverge

`writer.d1CidasksWalk` grows via:

1. `markApplyBoundary` → `splitFlush(false)` → +1 perQ + +1 d1 when
   `pendingDepth1Facts` is non-empty.
2. `logResult` → `splitFlush(true)` → +1 perQ + +1 d1 (trailing
   close) + N ε-edge pairs for `pendingApplyBoundaries`.

`walker.cidasksWalk` grows via `commitEdge` once per Asks edge
dispatched in a `v13Walk`. Asks edges in the trie correspond 1:1
with perQ entries by my restructure invariant.

Now at cold, sib A's apply-result accesses fire MANY `queryApply`
chains live (each ambient-apply Request the walker dispatches in
sib A's Q chains calls `AmbientApply::runOn` → `markApplyBoundary`
on the shared writer). Each fires `splitFlush(false)` and adds a
new perQ + d1 entry. By sib B's apply, writer.d1 has 27 entries —
6 from sib A's logResults plus 21 from these intermediate
markApplyBoundary side effects.

Walker.cidasksWalk only grows when the walker traverses an Asks
edge during a `v13Walk`. Sib A's Q chains contain a subset of
those 27 perQ entries (each Q's chain = perQAsksEdges
cumulative at that Q's logResult). After all of sib A's Q's
HIT at warm, walker has dispatched and committed the union of
their chains — but that union is smaller than 27 because the
intermediate markApplyBoundary perQ entries were added *between*
sib A's last logResult and sib B's apply. They're in
`perQAsksEdges` and reflected in *cold*'s writer.d1, but no
recorded Q's chain at warm references them. The walker has no
edge to dispatch for them.

So: writer.d1 includes "post-last-recorded-Q" perQ entries from
writer side effects; walker.cidasksWalk doesn't.

## The fix shape

The walker needs to mirror writer side-effect growth. Three
approaches in increasing scope:

### A. Walker pulls perQAsksEdges count at lookup time

At each `v13Walk(Q)` call, before computing parentHash, read the
current `perQAsksEdges.size()` from the writer and synthesise
`cidasksWalk.size() = N - X` empty edges into the walker's walk,
where X is the count of edges already-dispatched-but-not-counted
(or just take the writer's current `d1CidasksWalk.size()`
directly). The applyCdi computation then matches writer's.

Risk: if the walker's cidasksWalk is consulted by `resolveCdiId`
to match cell-chain cdi at edge K, "empty" synthesised edges
break the per-K probe — the cdi computation iterates `walk[0..K]`
looking for observation matches, and synthetic empties don't
contain the real observations. Probably need walker to ingest
the actual edge contents, not just count.

### B. Mirror via shared subscription

Add a `writerD1Mirror` field on `TracingReplayEvaluator` that
subscribes to `writer.d1CidasksWalk` appends. Each writer push
notifies the walker; walker copies the edge into its own
`cidasksWalk` (or just reads through to the writer's vector).
This is essentially "the walker reads d1CidasksWalk directly,"
which my third iteration tried — the catch is that during
warm dispatch the writer's d1 is *growing because of walker
activity*, so by the time the walker computes a parentHash, d1
has grown past where the writer would have been at the
equivalent cold moment.

### C. Walker advances its own walk per markApplyBoundary side effect

The walker's `dispatchApplyLive` already invokes outer apply, which
fires `AmbientApply::runOn` → `writer.markApplyBoundary` → writer
`splitFlush(false)` → +1 to writer.d1. Have the walker observe
its own dispatchApplyLive completion and `commitEdge` an extra
walker.cidasksWalk entry that contains the same elementHash
the writer just folded. Concretely: in `dispatchApplyLive`, after
the live outer apply completes, push a synthetic edge with
`{fromHash=0, elementHash=factHash}` to `cidasksWalk` matching
the ε edge the writer just added. This makes walker.cidasksWalk
match writer.d1 size at the same moments writer.d1 grows from
walker-triggered side effects.

This is option (C) — most surgical. It's just "fire commitEdge
on apply Requests *and* on dispatchApplyLive's side-effect
markApplyBoundary." The element pushed matches what writer
flushed.

## Why option C is the right shape

The 1:1 alignment invariant says: every perQAsksEdges entry has
a corresponding d1CidasksWalk entry and a corresponding walker
cidasksWalk entry. At cold, writer.markApplyBoundary adds a
perQAsksEdges + d1CidasksWalk pair via side effect; the symmetric
event for the walker is `dispatchApplyLive`, which is what
triggers the same markApplyBoundary at warm. Hooking
`commitEdge` (or equivalent) into dispatchApplyLive closes the
loop.

The other paths that grow writer.d1 via markApplyBoundary side
effects (e.g. recording's `inner.apply` for a fallback) don't
need walker mirroring — they happen on the writer side during
recording, when the walker isn't reading.

## Concrete plan

1. In `TracingReplayEvaluator::dispatchApplyLive`, after the
   live `fn->queryApply(arg)` completes and `applyRespHash` is
   computed, also push a `cidasks::Edge` with
   `observations = [{fromHash=0, elementHash=SHA(applyReqHash, applyRespHash)}]`
   to the evaluator's `cidasksWalk`. This mirrors writer's ε
   edge insertion at `markApplyBoundary` finalize-time.
2. Verify size alignment by adding an assertion in `v13Walk`:
   at apply-time the parentHash computation should observe
   `cidasksWalk.size() == writer.d1CidasksWalk.size()`.
3. For boundaries with non-empty d=2 chain probes (cb-higher-order),
   the walker already extends walkFacts via `appendFactToWalk` per
   probe — that handles the d=2 side. Option C only fixes the
   d=1 ε edges that writer adds via markApplyBoundary's
   splitFlush(false) draining preceding ambient obs into a
   new perQ + d1 entry. Those are the perQ entries with
   `rs-size > 1` and `obs > 0` between Q logResults.

Hmm wait — looking more carefully at cold's log between
`(perQ=6 d1=6)` and `(perQ=27 d1=27)`, each splitFlush has
non-zero rs-size and adds a perQ. But the corresponding
markApplyBoundary is for an applyRequestHash. Each pair is
one ε-style edge for one apply Request. Each one comes from a
walker dispatch that triggered AmbientApply::runOn.

So the walker IS the trigger for each one. The walker should
also commit a corresponding cidasksWalk entry at the same time
the writer flushes.

Implementation: in the dispatcher (`v13Walk`'s `dispatch`
closure), when handling `isAmbient && queryTag == "apply"`:

```cpp
if (isAmbient && queryTag == "apply") {
    /* ... existing applyRespHash computation ... */
    pendingEdgeObservations.push_back({
        Hash(HashAlgorithm::SHA256),
        TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), requestHash, applyRespHash),
    });
    /* New: also push a SOLO commit so writer-side ε from
       markApplyBoundary's drain is mirrored on walker side */
    cidasks::Edge soloEdge;
    soloEdge.observations.push_back({...same as above...});
    cidasksWalk.push_back(std::move(soloEdge));
    /* ... return applyRespHash ... */
}
```

But this double-commits if commitEdge later picks up the
pendingEdgeObservations. Need to be careful: either skip the
pendingEdgeObservations push for apply Requests (= commit the
solo edge directly here and don't pending-buffer), or arrange
commitEdge to dedup.

The dedup-via-fingerprint already exists; a solo edge with the
same fingerprint as a subsequent committedEdge's fingerprint
will skip. So no double-commit.

## Risks

- Edge order matters for `contentIdAt` own-loop matching.
  Inserting a synthetic edge at dispatch time puts it in a
  different position than where writer's ε edge ends up after
  `flushPendingAmbient`'s insertionIndex-driven insert. Need
  to verify the resulting walker.cidasksWalk has matching
  edges at matching indices — or relax the order requirement
  (the own-loop is XOR-fold, so order within the matching
  edges doesn't matter, but order between matching and
  non-matching edges does affect intermediate `myCidAtK`).
- The walker side effect fires `markApplyBoundary` on the
  writer, which adds a perQ + d1 entry. The walker's
  pending-edge mechanism plus the synthetic commit might
  double-count from the walker's perspective. Inspect carefully.

## Test plan

After landing option C:
- `tests/functional/cb-sibling-discrimination-via-observation.sh`
  warm DISALLOW_PARSE should pass (= return 1100, no fallback).
- `tests/functional/cb-higher-order.sh` should not regress (= still
  handles outer-fn invalidation correctly via the AmbientAsks chain).
- `tests/functional/cb-385.sh` deep-indep test 4 is independent and
  won't be fixed by this change; investigate separately.

## Source map

Writer side (= what option C must mirror):
- `src/libexpr/tracing-writer.cc:9` — `flushPendingAmbient`. Drains
  `pendingDepth1Facts` into `pendingD1Edge`; processes
  `pendingApplyBoundaries` and inserts ε perQAsksEdge +
  ε d1CidasksWalk entries at `boundary.insertionIndex + shift`.
- `src/libexpr/tracing-writer.cc:349` — `splitFlush`. After
  `flushPendingAmbient`, pairs `pendingNewRequests` → perQAsksEdge
  push with `pendingD1Edge` → d1CidasksWalk push (the 1:1
  invariant).
- `src/libexpr/tracing-writer.cc:379` — `markApplyBoundary`.
  Calls `splitFlush(false)`, inserts apply Request payload into
  the CAS pool, buffers a new `PendingApplyBoundary` with
  `insertionIndex = perQAsksEdges.size()` and
  `fromFactSetHashAtBoundary = prevQFactSetHash`.
- `src/libexpr/include/nix/expr/tracing-writer.hh:122` —
  `d1CidasksWalk` declaration with the 1:1 alignment invariant
  documented.
- `src/libexpr/include/nix/expr/tracing-writer.hh:169` —
  `PendingApplyBoundary` struct with the `finalized`/`cumulativeFactSet`/
  `factHash`/`pos`/`lastProcessedCount` fields option (b) added.

Walker side (= where option C edits land):
- `src/libexpr/tracing-replay-evaluator.cc:56` — `commitEdge` lambda
  in `v13Walk`. Currently pushes one cidasksWalk edge per Asks edge
  dispatched with non-empty observations; option C also fires for
  apply-Request-triggered side effects.
- `src/libexpr/tracing-replay-evaluator.cc:154` — the apply-tag
  branch in `dispatch`. Currently pushes
  `pendingEdgeObservations` and calls `dispatchApplyLive`. Option
  C adds a `cidasksWalk.push_back` (or pushes a solo edge that
  bypasses `pendingEdgeObservations` to avoid double-commit
  through `commitEdge`'s subsequent fire).
- `src/libexpr/tracing-replay-evaluator.cc:652` —
  `dispatchApplyLive`. Lives outside the walk loop;
  `commitEdge` integration must coordinate with whichever
  caller pushed `pendingEdgeObservations`.
- `src/libexpr/include/nix/expr/tracing-replay-evaluator.hh:62`
  — `cidasksWalk` declaration with the cumulative-across-`v13Walk`
  semantics.

Bridge between walker dispatch and writer flushes:
- `src/libexpr/expr-from-object.cc:291` — `AmbientApply::runOn`,
  called by `AmbientObject::queryApply` via `applyFn` whenever the
  outer applies a cached entity. Line 328 fires
  `innerWriter->markApplyBoundary` — that's the writer-side event
  option C mirrors.

cidasks formula:
- `src/libexpr/content-identity-via-asks.cc:200` — `contentIdAt`,
  the own-loop iteration that consumes the walk's observations.
  Understanding edge order vs `myCidAtK` matching is load-bearing
  for the "Risks" section.

Committed groundwork on this branch:
- `5e9758253 refactor(eval-cache): align d1CidasksWalk 1:1 with perQAsksEdges`
  — the writer-side invariant option C extends to walker side.
- `6c4250cef feat(eval-cache): land option 2 apply triePos via cidasks evolution`
  — the apply CDI computation that needs aligned walk sizes.
- `8d9d4c58e feat(eval-cache): re-open finalized apply boundaries for late d2 obs`
  — option (b) infrastructure that closes the LocalResponseMap
  miss, independent of this doc's gap.
