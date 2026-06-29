# Per-arg CDI completion

Implementation status for argument-level content-defined identity,
as specified in
[`tracing-eval-cache-content-identity-via-asks.md`](./tracing-eval-cache-content-identity-via-asks.md).

## What's in tree

The cidasks math is implemented and argument-level only:

- `cidasks::contentIdAt(DerivedSubject, ...)` traps via
  `nix::unreachable()`. Only argument-bearing subjects
  (`PositionalSeed`, `ApplyResultSubject`, `PostulatedIdempotentRead`)
  have CDIs.
- `cidasks::structuralAddress(subject, scope, walk, edgeIndex)` /
  `structuralAddressAfter` exposes a content-addressed identifier
  for *any* subject — for derived subjects it returns the producer
  query's hash, computed without going through `contentIdAt`'s
  trap. Callers that need a `Hash` handle for any proxy route
  through this.
- `ApplyResultSubject`'s `contentIdAt` arm composes via
  `structuralAddress` on its constituents.
- `QueryApply` carries optional `fromCIDs`/`fnPath`/`argPath`/
  `fnRootIndex`/`argRootIndex` fields alongside the legacy
  `fn`/`arg`.
- `cidasks::makeApplyResultQuery` builds the per-arg-encoded
  `QueryApply` payload.

The writer's per-arg flush (`tracing-writer.cc:flushPendingAmbient`)
collapses derived chains to the cb_arg root via
`pathAndRootsFromSubject` and stamps every observation's `from`
at the root's cidasks-evolved CDI — `contentIdAt(root, scope,
d1CidasksWalk, d1CidasksWalk.size())`. Each flush appends one
edge to `d1CidasksWalk`, so per-flush evolution accumulates per
principles 3, 5, 7.

The walker mirrors this:
`tracing-replay-evaluator.cc:TracingReplayEvaluator` maintains a
cumulative `cidasksWalk` that persists across `v13Walk` calls,
with `commitEdge` deduping by per-edge observation-set fingerprint
so a shared Asks-chain prefix re-traversed in a later `v13Walk`
doesn't double-fold. `resolveCdiId`'s cell-chain match tries every edge
boundary 0..`cidasksWalk.size()` so a recorded CDI lookups at
the writer's then-current edgeIndex regardless of the per-`v13Walk`
position. `TracingDecisionGraph::walk` accepts a `startCur`
parameter; `v13Walk` first walks from `lastQFactsHash` (= the
cumulative position from prior hits) to continue the chain past
earlier siblings' Terminals, falling back to a walk from ∅ for
Q's whose recorded chain is rooted at a prefix.

The depth-2 local objects (`TracingLocalObject` /
`ReplayLocalObject`) emit queries per-arg via `stampPerArgFields`.

## Test status (24/25 cb-\* pass)

- **cb-385** deep-indep test 4: green. Walker reproduces the
  writer's evolved CDI at the right edge boundary via the
  cumulative `cidasksWalk` + try-every-edge cell-chain match.
- **cb-local-descendants** and all other cb-\*: green.
- **cb-sibling-discrimination-via-observation**: still red.

## What's still missing for cb-sibling

The test records two cb-applies of the same cached fn whose
pre-apply observations match but whose apply-result observations
diverge (`.whatever = 100` vs `1000`). Under the via-asks
discrimination corollary (principle 8), divergent observations
should produce distinct CDIs that lead to distinct trie
positions. In the current implementation:

- The writer's `flushPendingAmbient` evolves the cb_arg root
  CDI per-flush, so sibling B's `.whatever` observation's `from`
  is the cb_arg CDI AFTER sibling A's `.whatever` observation
  has folded in — a distinct CDI from sibling A's `from`.
- But the *child queries on the apply-result wrapper* (= what
  `TracingReplayObject`/`TracingObject` emit) carry `from =
  triePos.queryHashStr`, a structural parentHash with no cidasks
  evolution. So sibling A and sibling B's `.whatever` getType
  queries hash to the same `queryHash`. Cold records two
  Terminals at this same `queryHash` at different `factSetHash`
  positions (= sibling A's cumulative cur vs sibling B's).
- For the walker to discriminate, it must reach sibling B's
  `factSetHash` when looking up sibling B's lookup. Currently
  `walk()` stops at the first Terminal reachable from its
  starting cur. Starting from `lastQFactsHash` doesn't help
  when `lastQFactsHash` itself is upstream of both terminals
  (= the prior hits landed at a shared-prefix cur, not at a
  sibling-specific cur).

The principled completion of cb-sibling requires extending the
cidasks-evolved encoding into the `TracingObject` /
`TracingReplayObject` path: child queries on apply-result
wrappers should carry `from = applyResultCdi(...)` evolved at
the current `d1CidasksWalk`. Then sibling A's and sibling B's
child queries hash to distinct `queryHash`es, distinct
Terminals, no discrimination-via-cur required — discrimination
falls out structurally from principle 3's apply-result formula.

That's the next piece of work, and it's a wider edit than
what's currently in tree.

### Encoding alignment is the obstacle

A direct first attempt — adding `applyResultSubject` to
`TracingObject` and computing `evolvedParentHash` via
`cidasks::contentIdAt(applyResultSubject, scope, d1CidasksWalk,
walk.size())` — regressed 19/25 tests. The root cause is an
encoding mismatch in how the apply-result's structural CDI is
computed:

- `TracingEvaluator::apply` currently records the apply's
  triePos via `computeQueryHash(QueryApply{fnId, argId})`,
  where `fnId` and `argId` are the hex `getCdiHex()` of the
  fn/arg proxies — used **as-is**, no scope XOR.
- The cidasks formula for `ApplyResultSubject::structuralAt(K)`
  builds `QueryApply{hashHex(structuralAddress(fn, scope, …)),
  hashHex(structuralAddress(arg, scope, …))}`. For an
  `PostulatedIdempotentRead{H}` constituent, `structuralAddress` is
  `xorHashes(H, scope)` — so when `scope ≠ 0` (= cb-applies,
  where `fn.getInheritedScope()` carries the cb_scope), the
  constituent CDI is double-XOR'd: once when `getCdiHex()`
  embedded the scope, again when cidasks applies it.

Result: the cidasks-evolved hash and the static `triePos.queryHashStr`
disagree for the same logical apply-result, breaking every Q
whose `from` came from `evolvedParentHash`. Reverted.

A principled fix needs to reconcile this — either:

1. Make `computeQueryHash(QueryApply{fnId, argId})` and the
   cidasks formula produce the same hash by construction (= so
   when no observations have evolved anything, the two
   encodings coincide bit-for-bit). This likely means making
   `PostulatedIdempotentRead`'s structuralAt NOT apply the scope
   (= treat the already-CDI'd hash as scope-saturated), and
   relying on the scope being baked into the constituent CDIs
   upstream.
2. Or switch the apply's triePos to *also* go through cidasks
   (= compute `triePos.queryHashStr = contentIdAt(applyResult
   Subject, scope, walk_at_apply_time, …)` at apply time). Then
   `evolvedParentHash` at later child-query time is the same
   formula at a later walk index — naturally consistent.

Option 2 is more invasive (every apply site changes) but
better aligned with the via-asks principles. Option 1 is local
to cidasks but needs careful reasoning about when scope has
already been applied vs not.

## Cautionary tale: Fix A

A prior attempt ("Fix A") sidestepped the writer/walker
edgeIndex misalignment by removing CDI evolution from both
sides — stamping facts at `contentIdAfter(root, scope, {})`
(= empty walk). It was rationalized in an earlier version of
this doc as: "Cumulative is about *which facts are in the
dependency set*, not about how the facts' `from` field is
encoded."

That framing splits the CDI principle into "the important part"
(= which facts) and "the encoding" (= the `from` field) and
discards the latter — but content-defined identity is precisely
the link between observed content and identity encoding. Drop
the encoding link and you've dropped CDI; what remains is
structural identity, the very thing CDI was designed to refine.

Fix A turned cb-385 green and left cb-sibling red. It was
reverted (838424010). Outcome: no new insights, wasted time.
The lesson is in [`../../CLAUDE.md`](../../CLAUDE.md): test
failures are not a problem until the principled design is fully
implemented; principled fix beats convenient shortcut.

## Cautionary tale: Fix B (`PostulatedIdempotentRead` for apply-result observations)

A later attempt (uncommitted, reverted in-session) tried to
reconcile the writer/walker mismatch for cb-higher-order's
apply-result observations by stamping their subject as
`PostulatedIdempotentRead{applyReqHash}` on both sides — reasoning
that `PostulatedIdempotentRead.structuralAt` returns `H` unevolved, so the
walk-grouping mismatch becomes moot.

That framing splits the CDI principle the same way Fix A did:
"the important part" (= which facts are recorded) vs. "the
encoding" (= the subject's evolved CDI). `PostulatedIdempotentRead{H}`
freezes a subject's CDI to `H` at construction time. Using it
as the subject of an *observation* discards the discrimination
that future observations would have provided through
own-loop evolution — exactly the property cb-sibling discrimination
relies on.

**Per-use rule** — superseded. The canonical contract for
`PostulatedIdempotentRead` now lives in its docstring at
`src/libexpr/include/nix/expr/content-identity-via-asks.hh`. It
postulates an *idempotent source*, not "an atom whose CDI is fully
determined at construction." Valid: filesystem reads (snapshot
semantics), expression strings hashed for parsing (referential
transparency). Invalid: values that can't be characterized
completely ahead of time (e.g. a lazy fn arg given as a `Value`);
taking an arbitrary subject id by value and using it as if it's
up-to-date. The earlier "narrow legitimate site"
(`TracingWriter::logDepth2ApplyFact` for the apply Fact's subject)
is **not** legitimate under the new framing — an apply is a
behavior, not a read; its identity is `ApplyResultSubject{fn, arg}`
with constituents that evolve. Fixing that site is open work
(task #20's remaining call-site audit).

Same outcome as Fix A: no new insights, the attempt was reverted
before commit, and the principled implementation is still owed.

## Source map

The principled fix touches:

- `src/libexpr/include/nix/expr/tracing-replay-evaluator.hh` —
  ResolutionContext loses `runningWalk`/`edgeIndex`; evaluator
  gains cumulative `cidasksWalk` + `committedEdgeFingerprints`
  + `dispatchedRequestSet`.
- `src/libexpr/tracing-replay-evaluator.cc` — `v13Walk`'s
  `commitEdge` appends to cumulative `cidasksWalk` with
  fingerprint dedup; slow walk attempts `lastQFactsHash` first
  then ∅; `resolveCdiId` tries every edge boundary.
- `src/libexpr/include/nix/expr/tracing-decision-graph.hh` /
  `tracing-decision-graph.cc` — `walk()` accepts `startCur` +
  `startCurRequests` parameters.

Writer-side `d1CidasksWalk` machinery in `tracing-writer.cc` /
`tracing-writer.hh` was originally append-only and grew
independently of `perQAsksEdges`. As part of the option-2
groundwork, it is now kept 1:1-aligned with `perQAsksEdges`:
every perQAsksEdge added is paired with a d1 edge at the same
index (ε boundaries insert into both at `boundary.insertionIndex`;
trailing closes append to both). The walker's `commitEdge`
always pushes, even for empty observation sets, so its
`cidasksWalk` matches the writer's d1 edge-for-edge under
expected dispatch order. This alignment is the precondition for
option 2 — but the cold/warm flush-pattern asymmetry is a
separate obstacle:

## Cold/warm flush-pattern asymmetry

After the 1:1 alignment restructure landed, a follow-up
attempt to land option 2's apply triePos via writer's
`d1CidasksWalk.size()` at apply time still regressed cb-same-
shape, cb-stats-derived-id-collision, and didn't close
cb-sibling. The root cause is a cold/warm asymmetry in *when*
flushes fire on the writer:

- **Cold:** `writer.d1CidasksWalk` grows at every `splitFlush`
  event — both `markApplyBoundary` (= one edge per cb-apply
  boundary opening) AND `logResult` (= 1 edge per pending d1
  chunk + N edges per processed pendingApplyBoundary). Each
  child query on the apply-result wrapper fires `logResult` →
  flushes. After sibling A's full child-query sequence
  (`getType`/`getAttrNames`/`maybeGetAttr`/`getInt` on the
  wrapper and on the resulting child) the cumulative d1 size
  is ~8–10 entries before sibling B's apply.
- **Warm:** the walker only triggers writer flushes through
  `markApplyBoundary` side effects (= when dispatching apply
  Request reqHashes via `dispatchApplyLive` → outer apply →
  `AmbientApply::runOn` → `writer.markApplyBoundary`). No
  `logResult`-equivalent fires at warm — the recordings are
  already in the trie, so the walker just dispatches them.
  Cumulative d1 size at warm sibling B's apply is just ~2 (one
  per ε apply Request in sibling A's body).

Result: `writer.d1.size` at warm sibling B's apply ≠
`writer.d1.size` at cold sibling B's apply. The cidasks
evolution formula evaluated on the two sizes produces different
applyCdis. Sibling B's recorded child queries (at cold's
applyCdi_B) and the walker's looked-up child queries (at warm's
applyCdi_B) hash to different Q values → MISS → fallback re-
recording at yet another inconsistent applyCdi.

Closing this gap requires the walker to fire writer flushes at
the same rate cold did — essentially synthesising `logResult`
equivalents during `v13Walk` so `writer.d1.size` advances in
lockstep with cold's growth pattern. That's a wider edit than
option 2 alone: it touches the writer/walker contract for what
"a flush" means and when it fires. Deferred as follow-up work.
