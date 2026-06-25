# Per-arg CDI completion

Implementation status and remaining work for argument-level
content-defined identity, as specified in
[`tracing-eval-cache-content-identity-via-asks.md`](./tracing-eval-cache-content-identity-via-asks.md).

This doc tracks the gap between the current implementation and the
design's end state, with a concrete fix plan for the two failing
functional tests.

## What's in tree

The cidasks math is implemented and argument-level only:

- `cidasks::contentIdAt(DerivedSubject, ...)` traps via
  `nix::unreachable()`. Only argument-bearing subjects
  (`PositionalSeed`, `ApplyResultSubject`, `OpaqueContentSubject`)
  have CDIs.
- `cidasks::structuralAddress(subject, scope, walk, edgeIndex)` /
  `structuralAddressAfter` exposes a content-addressed identifier
  for *any* subject — for derived subjects it returns the producer
  query's hash, computed without going through `contentIdAt`'s
  trap. Callers that need a `Hash` handle for any proxy
  (`AmbientObject::getCdi`, `TracingLocalObject::localId`,
  `ReplayLocalObject` construction, `resolveCdiId`'s cell-chain
  matching) route through this.
- `ApplyResultSubject`'s `contentIdAt` arm composes via
  `structuralAddress` on its constituents, so a Derived `fn` or
  `arg` participates correctly without re-entering the trap.
- `QueryApply` carries optional `fromCIDs`/`fnPath`/`argPath`/
  `fnRootIndex`/`argRootIndex` fields alongside the legacy
  `fn`/`arg` (mode-tagged via `fromCIDs` population). The legacy
  fields stay populated for `resolveApplyId`'s backward
  compatibility.
- `cidasks::makeApplyResultQuery(applyResult, scope, walk, edgeIndex)`
  builds the per-arg-encoded `QueryApply` payload — available for
  any future caller wanting Request-pool key alignment with the
  apply-result's CID.
- `cidasks::subjectFromObjectIdentity({obj.getSubject(),
  obj.getCdiHex()})` bridges `Object` identity to `Subject` — used
  at apply boundaries to compose `ApplyResultSubject` from the
  fn/arg constituents.

The writer's per-arg flush
(`tracing-writer.cc:flushPendingAmbient`) collapses derived chains
to the cb_arg root via `pathAndRootsFromSubject` and stamps every
fact with `from = root_cdi` plus `path`/`fromCIDs`. Depth-1 facts
feed `v13FactSet`; depth-2 facts group per cb-apply into
`AmbientAsks` chains.

The depth-2 local objects (`TracingLocalObject` /
`ReplayLocalObject`) emit queries per-arg via `stampPerArgFields`
— depth-2 dispatches inside `AmbientAsks` chains work end-to-end.

## Test status

After Fix A: 24/25 cb-* tests pass. `cb-385` and
`cb-local-descendants` both green; `cb-sibling-discrimination-via-observation`
remains red — see the post-mortem under [Fix A] below.

Pre-Fix-A diagnosis of the two red tests (kept for context;
cb-385 is now fixed):

### `cb-385` deep-indep test 4 *(was red, now green)*

Test 3 records `{a=1; b=99}` in its own nix invocation; test 4
warm-replays in a fresh invocation. `a = args.x.val` hits; `b =
args.y.val` falls through to inner and trips `DISALLOW_PARSE`.

**Root cause.** The writer's per-arg flush stamps `b`'s fact at an
*evolved* root cdi (= `from = 68810828a5ee`, the seed cdi at the
walk's edge 1+, not the static `from = ac1373e34ace` at edge 0).
By the time `b` is observed, several prior facts have advanced
`d1CidasksWalk` and the seed cdi has evolved through its own-loop
(per Foundational #9: factSetHash is cumulative).

The warm walker's `ResolutionContext::runningWalk` advances
per Asks-edge commit, which doesn't align in lockstep with the
writer's per-logResult `d1EdgeIndex`. When walker tries to
resolve `from=d48af43bcc44` (= seed cdi at writer's edge 1+), it
computes seed cdi at its own current edge (= edge 0, static) and
gets `ac1373e34ace`. Mismatch → falls through to `materialiseLocalStandin`
→ no matching LocalResponseMap entry → walk fails.

### `cb-sibling-discrimination-via-observation` *(still red)*

Already covered by the design (= principle 8's discrimination
corollary). Two sibling cb-applies of the same cached fn:

- Cumulative observations evolve the cb_arg root's CID per
  Foundational #9 + Design #3.
- Within the same warm invocation (cold's sequence reproduces),
  sibling A's `.whatever` query lands at one Asks/Terminal
  position; the divergent response (100) folds into `cur` and the
  apply-result's CID; sibling B's subsequent `.whatever` query
  uses the post-A `cur` and so lands at a *different* trie
  position. Both terminals coexist; both warm calls hit.

The pre-Fix-A hypothesis was that the walker's `runningWalk`
needed to align with the writer's per-flush evolution — same
root cause as cb-385. Fix A removed that misalignment (= both
sides static) and unblocked cb-385, but cb-sibling has a
different root cause that Fix A doesn't address. See the
post-mortem in the Fix A section below.

## The fix

### Fix A — Stable root cdi (no evolution across logResults)

**Decision.** The writer's per-arg flush stamps facts at the
*static* (edgeIndex=0) root cdi, not the evolved per-logResult
cdi. This removes the writer/walker reconstruction asymmetry —
no edgeIndex bookkeeping for either side to keep in lockstep.

Does this break Foundational #9 (cumulative)? No. Cumulative is
about *which facts are in the dependency set*, not about how the
facts' `from` field is encoded. Stamping `from = static root cdi`
on every fact still records the cumulative set; only the per-fact
addressing changes. The principle-8 discrimination corollary
still applies (= via the next Asks/Terminal lookup at the
post-divergence `cur`), and per-arg centralization still
collapses derived chains to the root.

**Changes.**

- `tracing-writer.cc:flushPendingAmbient` — drop the
  `d1EdgeIndex` bookkeeping. For each pending fact, compute
  `root_cdi = cidasks::contentIdAfter(root, scope, {})`
  regardless of how many flushes preceded.
- Strip `d1CidasksWalk` member from `TracingWriter`; it's dead
  state under this decision.
- `TracingReplayEvaluator::apply`'s pre-populate path
  (`getRequestsWithFrom(argCdiHex)`) matches by definition:
  every fact's `from` is the static root cdi, and the lookup
  returns the full set without edgeIndex acrobatics.
- Walker's `resolveCdiId` cell-chain matching uses
  `cidasks::structuralAddress(subject, scope, {}, 0)` — static
  edge — uniformly. No `ctx.runningWalk` / `ctx.edgeIndex`
  threading needed for this path.

**Acceptance.**

- `cb-385 deep-indep test 4` warm replay reaches the right
  Terminal — every fact's `from` resolves correctly. **(Landed.)**
- `cb-local-descendants` keeps passing. Walker's d2
  `stampPerArgFields` aligned to static-cdi to match writer.
  **(Landed.)**
- `cb-sibling-discrimination-via-observation` still red — see
  below; Fix A made it red→still-red, not red→green. 24/25.

## What Fix A did *not* fix: cb-sibling

The cur-based discrimination corollary (principle 8) is correct
in the abstract but doesn't activate for this test under the
current implementation. Mechanics:

- Cold writes two Terminals at the failing query's queryHash,
  at different `factSetHash` positions (one per sibling).
- Warm walker's `v13Walk` (`tracing-replay-evaluator.cc:35`)
  computes `candidateCur = lastQFactsHash + onlyInEdge -
  onlyInDispatched` for the fast path. For sibling B's lookups,
  all the relevant requests are already in `dispatchedTrie`
  (= dispatched during sibling A's processing) and the edge's
  requestSet is the same → `onlyInEdge = onlyInDispatched = ∅` →
  `candidateCur = lastQFactsHash`, *unchanged* between siblings.
- `lastQFactsHash` is frozen at sibling A's chain's final
  position. Lookup at this cur misses sibling B's Terminal.
- Slow `walk()` from ∅ also fails: dispatching the ∅-edge's
  requests under sibling B's live cb arg observes the *same*
  responses as sibling A (= cb arg observations don't
  distinguish the two lambdas before the body runs), so
  `nextCur` doesn't reach sibling B's chain.

The divergent observation (= `.whatever` returns 100 vs 1000)
*is* recorded in the writer's `v13FactSet`, but the walker's
`v13Walk` never folds it into `lastQFactsHash` because the apply
that produces those values goes through
`TracingReplayEvaluator::apply` (= a separate codepath), not
through `dispatch` inside `v13Walk`. The cur the walker maintains
doesn't reflect apply-result content divergence between sibling
calls.

Closing this requires either (a) routing apply-result content
back into the walker's `lastQFactsHash` so cur evolution
discriminates siblings on the next lookup, or (b) some other
mechanism that makes sibling B's lookups address sibling B's
recorded position. Both are out of scope for Fix A; needs a
follow-up design pass.

## Sequencing

Single commit landed. Edits localized to:

- `tracing-writer.cc`/`.hh` — strip `d1CidasksWalk` evolution.
- `replay-local-object.cc` — `stampPerArgFields` uses static cdi
  (no walk/edgeIndex parameters).
- `tracing-replay-evaluator.cc:resolveCdiId` — cell-chain match
  via `structuralAddressAfter` at empty walk.
- `tracing-replay-object.cc:evolvedQueryFrom` — always returns
  `triePos.queryHashStr`; no evolved-cdi branch.

Delta: 22/25 → 24/25. cb-sibling remains red pending follow-up.

## What stays out

- **Function characterization** as a separate observation-folding
  mechanism (= task #87's "apply-result observations fold back
  into fn-root's own-loop"). Under stable root cdi (Fix A),
  apply-result observations are recorded with `from = cb_arg
  root` per existing per-arg flush, and the principle-8
  discrimination corollary handles sibling differentiation
  through the Asks/Terminal lookup at post-divergence `cur` — no
  separate fold-back machinery needed.
- **Phase C** (TracingReplayObject / TracingObject per-arg child
  query emission). Child queries on apply-result wrappers use
  `from = triePos.queryHashStr` consistently on both writer and
  walker; both sides hash the same payload; the lookup hits.
  The per-arg encoding lives in the *ambient flush* path
  (= which is symmetric on both sides already). Under Fix A the
  walker's `evolvedQueryFrom` no longer branches into evolved
  cdi — it always returns `triePos.queryHashStr` for sibling
  symmetry. (The evolved-cdi branch was a partial — and
  asymmetric — attempt at the same discrimination cb-sibling
  still needs.)
- **d1↔d2 fold-back coupling** (= the design doc's old framing
  of "AmbientResult = depth-2 terminal factSetHash" as a hash
  the d1 walker XOR-folds in). The implementation doesn't and
  shouldn't do this. Outer is consulted live; d2 validates
  structure; d1 walks its own chain. The d1→d2 flip is a
  conceptual mnemonic (= a way to think about role inversion at
  cb-applies), not machinery.
- **Cross-invocation isolated replay of sibling B alone** (=
  invoking only the second sibling in a fresh nix without
  running the first). Under cumulative dependency (Foundational
  #9), B's Terminal is at a factSet position that requires A's
  prior observations to reach; fresh-B-only warm correctly
  misses. Matches the cb-sibling test's actual scope (= cold
  records both siblings in one invocation; warm replays in the
  same order).

## Source map

Files that change for Fix A:

- `src/libexpr/tracing-writer.cc` — strip d1EdgeIndex bookkeeping
  in `flushPendingAmbient`; use static root cdi for every fact.
- `src/libexpr/include/nix/expr/tracing-writer.hh` — remove
  `d1CidasksWalk` member.
- `src/libexpr/tracing-replay-evaluator.cc` — simplify
  `resolveCdiId`'s cell-chain match to use static edge 0;
  pre-populate path no longer needs runningWalk threading.

Tests already in place that should turn green:

- `tests/functional/cb-385.sh` — deep-indep test 4.
- `tests/functional/cb-sibling-discrimination-via-observation.sh`.
