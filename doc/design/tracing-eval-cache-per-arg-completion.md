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

## What's still broken (2 red tests)

### `cb-385` deep-indep test 4

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

### `cb-sibling-discrimination-via-observation`

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

What's missing for this to actually work in tree is that the
walker has to reproduce the cb_arg root's evolved CID at each
fact's lookup point. Today the walker's `runningWalk` evolution
is misaligned with the writer's per-flush evolution — same root
cause as cb-385.

So **both red tests are blocked by one fix.**

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
  Terminal — every fact's `from` resolves correctly.
- `cb-sibling-discrimination-via-observation` discriminates the
  two siblings — divergent `.whatever` responses evolve `cur`
  differently per call (corollary to principle 8), and each
  Terminal lookup lands at its own sibling's position.

## Sequencing

Single commit. Drop the `d1CidasksWalk` evolution machinery; the
edits are localized to `tracing-writer.cc`/`.hh` and the walker
sites that consumed the edgeIndex parameter (`resolveCdiId`).

Run the full cb-* suite after; expected delta is 23/25 → 25/25.

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
  query emission). This was the wrong tree. Child queries on
  apply-result wrappers use the `from = triePos.queryHashStr`
  encoding consistently on writer and walker; both sides hash
  the same payload; the lookup hits. The per-arg encoding lives
  in the *ambient flush* path (= which is symmetric on both
  sides already).
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
