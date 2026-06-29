# Walker-side walk alignment with writer flushes

Follow-up to [`tracing-eval-cache-per-arg-completion.md`](./tracing-eval-cache-per-arg-completion.md):
how to close the remaining cb-sibling warm-DISALLOW_PARSE gap.

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
- cb-sibling warm DISALLOW_PARSE should pass (= return 1100,
  no fallback).
- cb-higher-order should not regress (= still handles outer-fn
  invalidation correctly via the AmbientAsks chain).
- cb-385 deep-indep test 4 is independent and won't be fixed
  by this change; investigate separately.
