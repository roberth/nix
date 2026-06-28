# Higher-order callback replay — apply-result observation encoding

A focused design proposal for closing the cb-higher-order family of
failures (`cb-higher-order`, `cb-higher-order-nested`,
`cb-stats-higher-order-baseline`) without violating the via-Asks
principles. Companion to
[`tracing-eval-cache-content-identity-via-asks.md`](./tracing-eval-cache-content-identity-via-asks.md)
and [`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md).

The cb-sibling failure is **out of scope** here — it has a separate
principled fix sketched in
[`tracing-eval-cache-per-arg-completion.md`](./tracing-eval-cache-per-arg-completion.md#whats-still-missing-for-cb-sibling)
(= extend cidasks-evolved encoding to child queries on apply-result
wrappers). The proposal in this doc is independent of that work
and the two compose.

## The failure

`cb-higher-order` records `{f}: f (x: x+1)` against the outer
`{f = g: g 5}`. Cold returns 6 (correct). Warm replay falls through
to inner re-evaluation; under `_NIX_DISALLOW_PARSE=1` the test fails.

The trigger is the d=1 walker dispatching the inner's ambient
observation `QueryGetType{from=hex(outerApplyResultCdi)}` (= "inner
observed type=int on outer's f-applied-to-innerLam result"). To
serve this observation live, the walker calls
`resolveApplyId` → `fn.queryApply(arg)` → `InterpreterObject(mkApp)`
and forces via `getType`. Forcing runs outer's `g 5` natively where
`g` is a `<replay-local-lambda>` primop materialised from the
innerLam standin (`ReplayLocalObject`, via `toValueOrProxy`).

The primop's impl currently materialises the synthetic apply-result
as a `ReplayLocalObject` (= reads responses from `LocalResponseMap`,
the depth-2 ambient atom store). That store is keyed by
`requestHash = SHA-256(query{from=hex(syntheticCdi)})` where
`syntheticCdi = qH(QueryApply{fn=hex(RLO.subject.cdi),
arg=hex(PositionalSeed{depth+1}.cdi)})` — the cidasks formula for
`ApplyResultSubject{RLO, contraArg}`. **The writer never recorded
responses under that key.** The lookup misses; the standin's
`getType` throws "no recorded response"; `dispatchApplyLive` catches
the divergence and returns nullopt; the walker fails.

## What the writer actually records

At cold, the inner observes the apply-result *twice over two
separate writer-side recording paths*. Both observations are real;
both go into the trie; they live under different subjects.

**Path 1 — d=1 ambient observation, via outer's apply-result
`AmbientObject`.** Inner forces `f x` through the `<ambient-fn>`
primop, which calls `f_amb.queryApply(argObj)`. The returned
`AmbientObject` carries subject
`ApplyResultSubject{f_amb.subject, innerLam.subject}` (= "outer's
f applied to inner's lambda"). When inner subsequently forces
`getType` on it via the bridge, the ambient query records a d=1
fact with `from = hex(outerApplyResultCdi)`. This is what the
walker's d=1 chain dispatches.

**Path 2 — sub-Q terminals on the local apply's `TracingObject`.**
Outer's `g 5`, evaluated under outer's `f` body, triggers
`<cached-fn>`.impl → `innerEval->apply(TLO, contraArg_g5)`. This
goes through `TracingEvaluator::apply`, which:

- emits `logDepth2ApplyFact(applyQ_g5, applyReqHash_g5)` (already
  in tree, lands in boundary #1's d=2 chain — the recursive apply
  Fact itself),
- calls `markApplyBoundary(applyQ_g5)` (pushes boundary #2),
- delegates to `inner->apply(TLO, contraArg)` which returns an
  `InterpreterObject` wrapping `mkApp(TLO.toValueOrProxy(...),
  contraArg.toValueOrProxy(...))`,
- wraps the result in a `TracingObject` (call it `tracing_obj_g5`)
  with `applyResultSubject = ApplyResultSubject{TLO.subject,
  contraArg_g5.subject}` (= the **local-synthetic subject** — what
  the walker's primop computes).

Back in `<cached-fn>`.impl, the bridging line is
`ExprFromObject(result.get_ptr(), innerEval, resolver).eval(state,
state.baseEnv, v);`. `ExprFromObject::eval` calls
`tracing_obj_g5.getType()`, `.getInt()`, etc. Each call routes
through `TracingObject::<method>` → `writer.logQuery + logResult`,
which inserts:

- a `Request` payload for `QueryGetType{from=hex(localSyntheticCdi)}`
  (and its `getInt`/`getAttrNames`/… counterparts),
- a `Terminal(Q, v13FactSetHash)` row at the current cumulative
  factset.

These are **sub-Q terminals in the main trie**, indexed by the
local-synthetic subject's CDI — exactly the key the walker's
primop computes for its synthetic standin. The data the warm
walker needs is already in the cache; the missing piece is the
*lookup path* — `ReplayLocalObject` reads from `LocalResponseMap`,
not from the main trie.

## Step 1 empirical findings (= simpler than the memo first claimed)

After running `cb-higher-order` cold with `_NIX_TRACING_CACHE_LOGGING=1`
and inspecting the resulting SQLite trie, the diagnosis is
confirmed at the recording layer:

- `localSyntheticCdi_g5 = 2ae5ce38951569df…` (= `qH(QueryApply{
  fn=hex(OpaqueContent{TLO.cdi}.structural), arg=hex(PositionalSeed{3}.cdi)})`).
- `Q=qH(QueryGetType{from=hex(localSyntheticCdi_g5)}) = 6f80070d00ef…`
  has a recorded `Terminals(Q, factSet=3ea4764803…)` row with
  result payload `ResultType{"int"}`.
- `Q=qH(QueryGetInt{...}) = e2d973c22fe2…` has a recorded
  Terminal with result payload `ResultInt{6}`.

So the writer's recording IS in the right place under the right
key. **The walker is already trying to look those Q's up.** From
the warm trace:

```
walker apply: fn=opaque(985c457c7504...) arg=seed(3) -> applyCdi=2ae5ce38951569df
walker lookup: getType Q=6f80070d00ef
```

The walker's existing `IR.apply` flow already does what the memo
was proposing — `<cached-fn>` is created when `ExprFromObject(TLO).eval`
fires for the bridged TLO (= constructed at warm by the
`<ambient-fn>` flow's `runOn` inside `dispatchApplyLive`'s force);
that `<cached-fn>`'s impl calls `innerEval->apply(TLO, contraArg)`
= `IR.apply`; `IR.apply` constructs a `TracingReplayObject` with
`applyResultSubject = ApplyResultSubject{OpaqueContent{TLO.cdi},
PositionalSeed{depth+1}}` (= the local-synthetic subject). The
proposed elaborate primop rewrite, `ctx.memo` plumbing, and
`AmbientResolver` threading are all **not actually needed** —
they would re-implement plumbing that already exists.

## The actual failure mode

The walker's `v13Walk(Q=6f80070d00ef)` for the synthetic's getType
**fails** with a re-entrancy issue, not a missing-data issue.
Trace:

```
walk Q=6f80070d00ef cur=7869c739639b outgoing=1
...
dispatchApplyLive: re-entry for applyReqHash=ce25f821df1e — return chain root
walk Q=6f80070d00ef rs=e86989d3d181 useful=1 nextCur=0099e133e5c0 NO RECORDED EDGE -> try next
walk Q=6f80070d00ef NO EDGE COMMITTED at cur=7869c739639b -> miss
```

Mechanism. All Q's at the same `logResult` inherit the same
`perQAsksEdges` chain (= the cumulative cidasks chain at logResult
time). So Q=6f80070d00ef's recorded chain includes the apply Fact
`ce25f821df1e` (= boundary #1's cb-apply Fact, recorded as a
synthetic d=1 Fact whose responseHash is the cold AmbientResult
`04160569b935`).

When the synthetic's nested `v13Walk(Q=6f80070d00ef)` dispatches
that apply Fact, `dispatchApplyLive` is invoked **recursively** —
already in flight from the outer `Q=4ed6c1c8bdac` walk. The
cycle-break short-circuits to `applyReqHash` instead of the
cold-recorded `AmbientResult`. The walker's cur diverges from
cold (= `0099e133e5c0` vs cold's `3ea476480316`); no recorded
Terminal at the divergent cur; miss; fall through to inner
re-eval.

The cycle break is **load-bearing** for cb-higher-order — without
it, the recursion is unbounded. But the value it returns is wrong.

## Proposed direction (narrowed)

**Fix `dispatchApplyLive`'s re-entry path to return the recorded
AmbientResult instead of `applyReqHash` (= chain root).**

When the cycle break fires (= `inFlightApplyReqs` already
contains this `applyReqHash`), compute the cold's AmbientResult
by walking the recorded `AmbientAsks` chain offline:

```
chainCursor = applyReqHash  // d=2 chain root
loop:
    edges = decisionGraph.getAmbientAsks(chainCursor)
    if edges empty: return chainCursor  // terminal reached
    // each edge is a singleton RS; pick the one whose request's
    // recorded response is consistent — for the re-entry case
    // there's only one path because the chain was recorded
    // deterministically. Take the first edge's toFactSet.
    chainCursor = edges[0].toFactSet
```

This is purely a lookup against `AmbientAsks` rows that were
written at cold (= no live evaluation, no further re-entry).
Returns the cold AmbientResult.

The walker's `dispatchApplyLive` for the OUTER (non-re-entry)
case continues to invoke outer's body live for validation — that
behaviour is unchanged. Only the re-entry path's response value
changes from `applyReqHash` to `AmbientResult`.

### Why this is the right shape

The outer walk dispatches the apply Fact AND drives the live
outer body. The body runs to completion, producing the
TracingReplayObject synthetic for the inner side; the synthetic's
getType triggers a nested v13Walk; the nested walk **doesn't need
to re-validate the outer apply** — the outer walk already did
that. The nested walk needs to know "what response did the outer
walk produce for the apply Fact?" so the nested cur evolution
matches cold's.

The cold AmbientResult IS that response (= per writer's
flushPendingAmbient: synthetic d=1 apply Fact has
`(applyReqHash, AmbientResult)` and AmbientResult = terminal of
the d=2 chain). At warm the outer walk's `dispatchApplyLive`
computes the same AmbientResult by driving outer's body and
returns it. The nested walk can compute the same AmbientResult
without re-driving (= via the chain-walk above) because the
chain is deterministic and recorded.

### Why this doesn't break cb-sibling or cb-385

cb-sibling discrimination relies on observations evolving CDIs.
The d=2 AmbientAsks chain at cold is per-boundary; sibling
invocations record into distinct boundary chains (= different
applyId per invocation). The offline-chain-walk in the re-entry
path keys off `applyReqHash`, which discriminates between
boundaries by construction. ✓

cb-385 is data-shaped (no higher-order callbacks); `dispatchApplyLive`
doesn't fire. Unaffected. ✓

### What about the elaborate proposal in the prior draft

The previous proposal (= make `<replay-local-lambda>` mirror
`<cached-fn>`, plumb ctx.memo via AmbientResolver, etc.) was
solving problems that don't exist. Specifically:

- "Inner contraArg cdi can't be resolved" — wrong: the warm
  trace shows `resolve 209a1cd2bb8c -> memo hit` for the OUTER
  cb_arg and `producer-child via getAttr` resolves derived
  AmbientObjects via the existing producer chain. The inner
  contraArg_g5 is created by `<cached-fn>.impl` inside outer's
  body force, and its observations on the contraArg (e.g.,
  `getInt`) flow through the queryFn → ambient_query mechanism
  whose lookup goes through `resolveCdiId` finding the producer
  chain in the Requests pool.
- "Synthetic must be a TracingReplayObject" — already is: IR.apply
  constructs one with the right applyResultSubject.
- "Sub-Q terminals are recorded under different keys than walker
  reads from" — wrong: both writer and walker use the same
  `localSyntheticCdi` formula; the warm trace confirms walker's
  computed cdi matches cold's recorded one.

The actual issue is narrower: the re-entry cycle break returns
the wrong response value. Fix THAT, and the chain falls into
place.

### v13Walk re-entrancy

The synthetic's sub-Q lookups are nested `v13Walk` calls inside
the outer `v13Walk`'s call stack. They share `cidasksWalk`,
`lastQFactsHash`, `dispatchedTrie`, `dispatchCache`, and
`committedEdgeFingerprints` on the evaluator.

Analysis per shared-state field:

- `cidasksWalk` + `committedEdgeFingerprints`: inner walks
  append per `commitEdge`, deduplicating against the fingerprint
  set. The shared chain across all Q's at the same logResult is
  identical (= same `perQAsksEdges` inserted symmetrically), so
  inner walks re-traverse the shared prefix and dedup-skip.
  Inner walks only append edges the outer hasn't yet reached,
  which the outer would eventually have appended anyway — order
  shifts forward in time but the final `cidasksWalk` content is
  the same. Subject-CDI evolution via `resolveCdiId`'s
  try-every-k cell-chain match is robust to this shift because
  it tries `k` across the full range. **Safe for cb-higher-order
  and cb-385.** For cb-sibling (= sibling discrimination via
  evolved CDIs), early commits could bind a sibling's evolution
  to an earlier `k` than expected; flag for verification when
  cb-sibling's principled fix lands.

- `lastQFactsHash`: inner updates on successful hits. Outer's
  subsequent `v13Walk` calls benefit from this (= more progressed
  cumulative position). Outer's current walk (= still in
  `dispatch` loop) doesn't re-read `lastQFactsHash`
  mid-dispatch, only at entry. Safe.

- `dispatchedTrie` / `dispatchedRequestSet`: monotonic append-only.
  Inner adds requests; outer's `dispatchedTrie.diff` sees a
  bigger "dispatched" set in subsequent fast-path attempts. Safe
  (= just better hit rate).

- `dispatchCache`: ambient requests are NOT memoised (per the
  inline comment at line 86-88 of tracing-replay-evaluator.cc);
  file/env requests are. Inner walks add file/env entries; outer
  benefits. Safe.

The one risk worth flagging: if the inner walk modifies cidasksWalk
mid-outer-dispatch and the outer's next dispatch computes a CDI
that depends on cidasksWalk's exact state at that moment, the
extra entries could produce a different CDI than expected. The
try-every-k mechanism in `resolveCdiId` makes this resilient for
*resolution* (= still finds the match); evolution-driven
discrimination (= cb-sibling) needs more care. The principled-fix
landing order matters: if this proposal ships before cb-sibling's
fix, cb-sibling stays red; the two changes are independent.

### Multi-invocation of outer body

The walker invokes outer's body multiple times per warm replay
of a cb-higher-order Q:

1. Once during `dispatchApplyLive` (= ε dispatch) → primop fires,
   synthetic constructed, chain-advance happens.
2. Once per subsequent ambient observation fact whose `from` is
   the apply-result cdi (typically getType + getInt for scalar
   results) → `resolveApplyId` → `fn.queryApply(arg)` →
   `InterpreterObject(mkApp).getMethod()` → outer body forces
   AGAIN → primop fires AGAIN → fresh synthetic.

Each invocation is independent: fresh primop firing → fresh
contraArg → fresh synthetic → fresh applyContext. All read the
same recorded sub-Q terminals (= same `localSyntheticCdi` and
same writer-side cumulative chain). Results match across
invocations as long as the recorded data is deterministic in its
content (which it is — same Q in trie returns same Terminal).

Performance: 2N+1 outer body forces per warm replay where N is
the number of recorded apply-result observations. Acceptable for
correctness; optimisation deferred.

Hit-count impact: each fresh synthetic does its own sub-Q v13Walks,
each successful one increments `tracingCacheStats().hits++`.
`cb-stats-higher-order-baseline`'s expected 12 hits was authored
under the previous (failing) code path; the actual number under
this proposal will differ. **Don't assume 12 — measure and
update the assertion to reflect what the principled
implementation produces, with a comment explaining the
breakdown.**

## Falsifier walk-through: `f = g: g 5 + 1`

The falsifier my earlier proposal (= "writer routes apply-result
observations through the local-synthetic subject") didn't survive:
`f = g: g 5 + 1` makes outer's apply-result (= 7) ≠ local
recursive apply-result (= 6). The previous proposal would record
"7" under the local-synthetic CDI and the walker would mis-compute.

This proposal **records under both subjects with their actual
distinct values** (= what the writer already does):

- d=1 observation: `QueryGetType{from=outerApplyResultCdi} → "int"`,
  `QueryGetInt{from=outerApplyResultCdi} → 7`. The "7" comes from
  inner's actual probe on outer's f-applied-to-innerLam result.
- Sub-Q terminals (under local-synthetic): `QueryGetType{from=
  localSyntheticCdi} → "int"`, `QueryGetInt{from=localSyntheticCdi}
  → 6`. The "6" comes from `tracing_obj_g5` probes during outer's
  body execution.

At warm under this proposal:

1. d=1 walker dispatches `QueryGetType{from=outerApplyResultCdi}`.
   `dispatchAmbientQuery` → `resolveApplyId` → `fn.queryApply(arg)`
   → `InterpreterObject(mkApp)` → `.getType()` forces.
2. Outer's `g: g 5 + 1` runs. `g 5` invokes the primop.
3. Primop builds `syntheticSubject = ApplyResultSubject{RLO,
   PositionalSeed{depth+1}}` with `localSyntheticCdi = qH(
   QueryApply{...})`.
4. Primop chain-advances `*chainCursor` by the recorded apply
   Fact's elementHash (unchanged from current code).
5. Primop materialises `synthetic_replay_obj = TracingReplayObject(
   triePos{queryHashStr=hex(localSyntheticCdi)},
   applyResultSubject=syntheticSubject, applyScope, fresh
   applyContext)`.
6. `ExprFromObject(synthetic_replay_obj).eval(state, baseEnv, v)`:
   - `synthetic_replay_obj.getType()` → `lookupResult<QueryGetType,
     ResultType>(QueryGetType{from=hex(evolvedQueryFrom())})`.
     evolvedQueryFrom with empty observations = localSyntheticCdi.
     `v13Walk` finds the recorded Terminal "int". Returns nInt.
     Pushes the observation into applyContext.
   - `synthetic_replay_obj.getInt()` → query
     `from=hex(evolvedQueryFrom())` where evolvedQueryFrom now
     reflects the prior getType observation. **The writer
     recorded with the same evolution semantics** (= same formula
     in `TracingObject::evolvedQueryFrom`), so the queryHashes
     coincide. `v13Walk` finds Terminal 6. Returns 6.
   - `v.mkInt(6)`.
7. Outer's body computes `6 + 1 = 7`. Outer mkApp evaluates to
   `InterpreterObject(7)`.
8. `InterpreterObject(7).getType()` returns nInt. `dispatchAmbientQuery`
   serialises `ResultType{"int"}`. responseHash matches recorded. ✓
9. Walker advances. Next d=1 fact: `QueryGetInt{from=
   outerApplyResultCdi}`. Same path. Outer body runs again,
   computes 7. `InterpreterObject(7).getInt()` returns 7.
   responseHash matches recorded ResultInt{7}. ✓

Why no value corruption: the primop's synthetic returns the
**local** apply-result value (= 6, what inner-lambda body
computes); outer's body uses that to compute its **outer**
apply-result value (= 7); the walker's d=1 dispatch validates
the outer value against the recorded outer value. Two distinct
applies, two distinct values, two distinct validation surfaces.

## Validation against principles

**Foundational #6 (no deep hashing).** The lookup keys are
queries' SHA-256 hashes, not value contents. ✓

**Foundational #7 (laziness end-to-end).** The synthetic
`TracingReplayObject`'s sub-Q lookups fire only when
`ExprFromObject::eval` probes a method, which fires only when
outer's body forces a method. Walker never traverses recorded
structure ahead of the consumer. ✓

**Foundational #8 (forcedness-independence).** The recording site
(`tracing_obj_g5` during outer's body) and the replay site
(synthetic standin during outer's body) sit at the same logical
point — the apply-result that inner's `IT.apply(TLO, contraArg)`
constructs. The recording doesn't depend on whether inner forced
the result eagerly; the replay doesn't either. ✓

**Foundational #9 (cumulative dependency).** Sub-Q terminals are
recorded at `logResult` time against the cumulative
`v13FactSetHash`. The walker walks the cumulative chain via
`v13Walk` to validate before reading. ✓

**Design principle 1 (CDIs are pure functions of (subject,
factset)).** `localSyntheticCdi` is `contentIdAfter(
ApplyResultSubject{TLO.subject, contraArg.subject}, scope, {})`
on both sides. Same inputs, same output. ✓

**Design principle 2 (subjects are static structural identifiers).**
`ApplyResultSubject{TLO, contraArg}` is structural — no value
content, just the apply's constituent subjects. ✓

**Design principle 3 (apply-result formula).** The local-synthetic
subject is the apply-result of inner-lambda applied to its
arg-positional-seed. Writer and walker use the same formula. ✓

**Design principle 4 (per-Asks-edge membership).** Sub-Q
observations carry `from = evolvedQueryFrom()` which composes
prior applyContext observations into the CDI per principle 3.
Each child query's `from` is the current edge's apply-result CDI.
`TracingObject` and `TracingReplayObject` use the *same*
formula. ✓

**Design principle 5 (recording flush rewrites `from` per edge).**
Sub-Q terminals go through `logQuery + logResult`, which respect
`v13FactSetHash` cumulativity but record `from` at the time of
the method call (= already evolved per applyContext). Walker
reproduces incrementally via applyContext push during method
calls. ✓

**Design principle 6 (walker advances in lockstep).** Each
synthetic-side method push to applyContext mirrors the writer's
push on `tracing_obj_g5`'s applyContext at the symmetric point. ✓

**Design principle 7 (XOR commutativity within edge).** Each
sub-Q is its own Terminal lookup against the cumulative
v13FactSetHash; commutativity is at the v13FactSetHash level
(unchanged). ✓

**Design principle 8 (same-shape collapse & discrimination).** Two
identical higher-order cb-applies (= same recorded probes on
the apply-result) compute identical `localSyntheticCdi`s and
land at identical sub-Q terminals — collapse by content
addressing. Two divergent ones push divergent observations into
applyContext, evolving subsequent `evolvedQueryFrom` divergently,
landing at distinct sub-Q's per the per-arg-completion
discrimination corollary. ✓

**Polarity-validation memory (= `polarity-validation-direction.md`).**
Inner's `+` values (= local-supplied lambda's apply-result =
inner-lambda body's computation against outer-supplied arg) must
be materialised by the standin. The proposal does exactly that:
the synthetic standin reads inner-recorded sub-Q terminals to
materialise the inner-produced value, which outer's body then
consumes to produce outer's `+` value (= outer's f-applied-to-
innerLam result). The synthetic *materialises* the `+` value;
outer's body *consumes* it; d=1 dispatch *validates* the outer
value at the boundary. The classification (= inner-side lookups
are sub-Q terminals; outer-side observations are d=1) stays
direction-driven. ✓

## Why this isn't Fix A or Fix B

- **Fix A pattern (= drop encoding evolution):** Fix A removed
  cidasks evolution from the `from` field. This proposal preserves
  it — `evolvedQueryFrom` uses applyContext observations
  symmetrically writer/walker, so subsequent sub-Q's discriminate
  via observation evolution.
- **Fix B pattern (= freeze subject CDI via OpaqueContent):** Fix B
  used `OpaqueContentSubject{H}` as the subject for observations.
  This proposal uses `ApplyResultSubject{TLO.subject,
  contraArg.subject}` — a structurally evolving subject whose
  CDI is computed by the principled cidasks formula. The
  applyResultSubject of the local synthetic is fully determined by
  its constituents, and observations through it evolve via
  `applyContext.observations` (= principle 3's apply-result
  formula composing with subsequent fact folds).

## Walk-through for each currently failing test

**cb-higher-order** (`{f}: f (x:x+1)` with `f = g: g 5`): cold
records both d=1 observations on `outerApplyResultCdi` and sub-Q
terminals on `localSyntheticCdi` via the existing
`tracing_obj_g5` path. Warm walker dispatches d=1 facts;
`dispatchApplyLive` invokes outer live; outer's `g 5` triggers
the primop; primop materialises `TracingReplayObject` synthetic
which reads sub-Q terminals; outer's body returns 6;
`InterpreterObject(6).getType` = nInt = recorded; getInt = 6 =
recorded. Walk succeeds. ✓

**cb-higher-order-nested** (`{apply2}: apply2 (innerLambda:
innerLambda 5)` with `apply2 = midfn: midfn (n: n + 100)`): the
walker's chain of activity at warm requires:

  - Boundary #1's apply Fact (ε) dispatches via `dispatchApplyLive`,
    which constructs the outer mkApp `apply2 (RLO_innerLambda)`
    and forces. Outer body runs `midfn (n: n + 100)`. midfn =
    `<cached-fn>(RLO_innerLambda)` (per Route B).
  - The midfn primop's impl creates contraArg_mid =
    AmbientObject(PositionalSeed{2}, queryFn, applyFn). Per the
    inner-side-resolution addition, contraArg_mid is registered
    in ctx.memo under its CDI.
  - `IR.apply(RLO_innerLambda, contraArg_mid)` returns
    synthetic_mid = TracingReplayObject(triePos=localSyntheticCdi_mid,
    applyResultSubject={OpaqueContent{RLO_cdi}, PositionalSeed{2}}).
  - synthetic_mid.getType + .getInt read the writer's
    `tracing_obj_mid` sub-Q terminals from cold → "int", 105.
  - Outer's `midfn (n: n + 100)` evaluates to 105.

  Subsequent d=1 facts in boundary #1's chain include:
  - `QueryGetType/Int{from=apply_result_apply2_innerLambda_cdi}`:
    resolved via `resolveApplyId` → another outer body force →
    returns 105. ✓
  - `QueryGetType/Int{from=contraArg_mid_cdi}`: with the
    inner-side-resolution addition, `ctx.memo` has contraArg_mid
    → live AmbientObject.getInt etc. via queryFn bridging to
    outerArgObj_mid = `(n: n+100)`. ✓
  - `QueryGetType/Int{from=apply_result_inner_amb_cdi}` (= the
    `innerLambda 5` recursive apply's result observations):
    resolveApplyId → fn=contraArg_mid (in ctx.memo), arg=PositionalSeed{3}
    local standin (= RLO_for_5 via sidecar). contraArg_mid.queryApply(RLO_for_5)
    → applyFn → applyOn(outerArgObj_mid=(n:n+100), RLO_for_5, ...)
    → runOn → mkApp((n:n+100), RLO_for_5_thunk) → forces → 105. ✓

  Caveat: the test currently has the warm path commented out
  (`cb-higher-order-nested.sh:49-59`) pending d=2 walker work.
  After this proposal lands AND the inner-side-resolution
  addition lands, re-enable and verify empirically. Multiple
  nesting levels (= deeper than 2) untested in current corpus;
  flag for follow-up.

**cb-stats-higher-order-baseline** (= cb-higher-order with explicit
hit-count assertion of 12 hits, 0 misses, 0 fallbacks at warm):
each `TracingReplayObject` sub-Q lookup that hits increments
`tracingCacheStats().hits` via the `v13Walk` hit path. The
proposed change introduces new sub-Q lookups inside the primop
(= synthetic.getType, synthetic.getInt for cb-higher-order's
scalar result). The expected count of 12 already presumes these
lookups happen — the test's authoring assumed the principled
implementation. Verify empirically after the change; if the
count differs, update the test with the actual number and a
comment explaining what the lookups are.

**cb-385** (deep-indep replay): this test exercises only
data-shaped cached calls (no higher-order callbacks). The
proposal's code change is scoped to the `<replay-local-lambda>`
primop's impl, which only fires for inner-supplied lambda
standins under cb-apply boundaries. cb-385's failures are not
in this path. cb-385's regression should be investigated
separately as the user's earlier-deferred direction.

**cb-sibling-discrimination-via-observation** (intentionally red,
out of scope): not affected by this proposal. Its principled fix
is per-arg-completion's child-query-evolved-encoding work.

## Implementation surface (sketch only — no code yet)

After the Step 1 empirical correction, the change is small:
**one file, one function, ~30 lines.**

**`src/libexpr/tracing-replay-evaluator.cc` — `dispatchApplyLive`'s
re-entry path**:

The current re-entry guard (lines 630-635) returns
`applyReqHash`:

```cpp
if (!inFlightApplyReqs.insert(applyReqHash).second) {
    tracingCacheLog(
        "dispatchApplyLive: re-entry for applyReqHash=%s — return chain root",
        applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12));
    return applyReqHash;
}
```

Replace with: walk the recorded `AmbientAsks` chain starting at
`applyReqHash`; follow the deterministic chain to its terminal;
return the terminal hash.

```cpp
if (!inFlightApplyReqs.insert(applyReqHash).second) {
    // Re-entry from nested v13Walk (= synthetic.getType
    // triggers re-dispatch of this same apply Fact). Don't
    // re-drive outer's body — that's already in flight. Return
    // the cold-recorded AmbientResult by walking AmbientAsks
    // offline.
    Hash chainCursor = applyReqHash;
    while (true) {
        auto edges = decisionGraph.getAmbientAsks(chainCursor);
        if (edges.empty())
            break;
        // The chain was recorded deterministically: each step
        // has exactly one outgoing edge in the d=2 trie under
        // this applyReqHash subtree. Take the first.
        chainCursor = edges[0].second; // {requestSetHash, toFactSet}
    }
    tracingCacheLog(
        "dispatchApplyLive: re-entry for applyReqHash=%s — AmbientResult=%s",
        applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        chainCursor.to_string(HashFormat::Base16, false).substr(0, 12));
    return chainCursor;
}
```

**No other files change.** No writer-side changes. No RLO API
additions. No `AmbientResolver` plumbing. The existing
`<replay-local-lambda>` primop stays — it's not what's failing.

### Why no other changes are needed (= correcting the prior draft)

The prior draft claimed several plumbing changes were necessary:

- "Plumb `innerEvaluator` into RLO" — **not needed.** RLO doesn't
  need to call `makeCachedFnPrimOp`; the existing flow via
  `<ambient-fn>` → `runOn` → bridged-TLO → `<cached-fn>` already
  produces the right `<cached-fn>` primop at warm. Trace evidence:
  `tlo: getType from=985c457c7504 reqHash=e404a8ad1cb4 type=lambda`
  followed by `walker apply: fn=opaque(985c457c7504...) arg=seed(3)
  -> applyCdi=2ae5ce38951569df` shows `<cached-fn>` and `IR.apply`
  firing.
- "Inner contraArg `from` resolution needs ctx.memo plumbing" —
  **not needed.** Trace shows `resolve 209a1cd2bb8c -> memo hit`
  (cb_arg) and `producer-child via getAttr` (f_amb) resolving
  through existing mechanisms. The supposed "outer-seed by
  elimination" failure for inner contraArgs doesn't actually
  fire — the only mismatch is at the apply Fact dispatch in the
  re-entry case.
- "Synthetic must be a TracingReplayObject not RLO" — **already
  is.** IR.apply constructs a `TracingReplayObject` for the
  recursive apply with `applyResultSubject =
  ApplyResultSubject{OpaqueContent{TLO.cdi}, PositionalSeed{depth+1}}`.

## Open questions

1. **Deterministic single-edge AmbientAsks assumption.** The
   re-entry's chain walk picks `edges[0]` at each step. This is
   safe iff `getAmbientAsks(chainCursor)` returns exactly one
   edge per step in the recorded chain. Per writer's
   `flushPendingAmbient` finalize loop (line 254-258 of
   `tracing-writer.cc`): each d=2 probe inserts ONE
   `AmbientAsks(cumulativeFactSet, requestSet={qH(probe)},
   toFactSet)` row, advancing `cumulativeFactSet` deterministically.
   So the chain has exactly one edge per step by construction.
   **Verify empirically with `cb-higher-order-nested` where two
   levels of nesting share an AmbientAsks subtree.**

2. **Hit-count impact on `cb-stats-higher-order-baseline`.** The
   re-entry fix returns a different value, which may shift the
   walker's stats. Likely fewer fall-throughs and more hits.
   Measure after the fix and update assertion.

3. **cb-higher-order-nested correctness.** The single-line fix
   to dispatchApplyLive's re-entry handles cb-higher-order. For
   nested case, MULTIPLE distinct re-entries may happen (= one
   per boundary in the nesting). Each re-entry walks its own
   AmbientAsks chain. As long as each chain is deterministic,
   each re-entry returns the right AmbientResult. Verify
   empirically.

4. **Stale chain handling.** If the recorded AmbientAsks chain is
   stale (= outer was edited between cold and warm), the cold
   AmbientResult would be wrong. But: the OUTER `dispatchApplyLive`
   (= non-re-entry case) drives outer's body LIVE and computes
   the current AmbientResult; if outer changed, the live and
   recorded values differ at the d=1 walk's response-hash compare,
   miss, fall through. The re-entry path returning the recorded
   value is **consistent within the same warm walk**: both the
   outer's apply Fact dispatch and the nested's apply Fact dispatch
   would return the same recorded value. If outer changed, the
   outer apply Fact's compare fails (= via the outer
   `dispatchApplyLive` getting a divergent live result, returning
   nullopt, walker fails). The nested return value never matters
   in that case because the outer already invalidated. Safe.

5. **`getAmbientAsks` edge enumeration.** Verify the SQL/return
   shape: does it return `vector<pair<requestSetHash, toFactSet>>`
   or some other shape? Skim the impl before coding to confirm
   `edges[0].second` is `toFactSet`.

## Verification plan (before any commit)

**Step 1 (DONE — diagnosis confirmed).** Cold sqlite inspection
showed `Terminals(Q=6f80070d00ef..., factSet=3ea4764803...,
result=ResultType{"int"})` and `Terminals(Q=e2d973c22fe2...,
factSet=3ea4764803..., result=ResultInt{6})` exist under the
predicted local-synthetic CDI.

**Step 2 (DONE — getAmbientAsks shape confirmed).**
`std::vector<std::pair<SetHash, SetHash>>` where `.first` =
`requestSetHash` and `.second` = `toFactSetHash`.

**Step 3 (DONE — re-entry fix landed).** `dispatchApplyLive`'s
re-entry guard now walks `AmbientAsks` offline to compute the
cold-recorded AmbientResult instead of returning the chain root.
Both re-entry AND non-re-entry paths now use the same offline
chain walk (= the non-re-entry case can't rely on the standin's
chainCursor because the recursive apply Fact's elementHash isn't
folded in: the `<replay-local-lambda>` primop is bypassed when
the standin's primop Value goes through TLO → `<cached-fn>` →
IR.apply path).

**Step 4 (DONE — `<cached-fn>.impl` registers contraArg in
ctx.memo).** Writer holds a stack of v13Walk-ctx-memos; `v13Walk`
pushes/pops; `<cached-fn>.impl` writes the freshly-created
contraArg under its CDI into all stacked memos so subsequent
`resolveCdiId` calls in the OUTER walk (post-ε fact dispatches)
find inner contraArgs that weren't otherwise reachable.

**Step 5 (DONE — partial: cb-higher-order step 1 + 2 green).**
Cold record + warm replay both return 6 ✓. cb-higher-order
step 3 (outer change `g 5` → `g 10`, expects 11) still fails
with `ReplayLocalObject: no recorded response` propagating
through `<replay-local-lambda>` primop → out of `ensureInner` →
out of the OUTER tre.getType. This is a **pre-existing failure
mode** that was masked by step 2's earlier failure; the fix
exposes it but doesn't introduce it.

**Step 6 (PARTIAL — step 3 of cb-higher-order family needs
separate design).** The outer-change fall-through requires
re-evaluating the cached inner body freshly (= the actual parsed
`(x:x+1)` lambda value, not the standin's `<replay-local-lambda>`
primop). Current `ensureInner` traversal still routes through
the standin which can't materialize for divergent live args.

  The cleanest principled fix at the OUTER level: at outer-change,
  the OUTER tre.getType's miss should propagate to the OUTER
  cb-apply's TRO ensureInner, which would call `inner.apply(TO,
  contraArg_outer)` with TO = inner cached body. Routing this
  through `TO.toValueOrProxy → TO.defeatCache → inner.evalFile`
  triggers parsing, gets the actual inner-lambda value, and the
  outer's `(g: g 10)` runs natively. Three obstacles:
  1. The standin's failure throws an exception rather than
     returning a v13Walk miss, propagating past the outer's
     opportunity to fall through.
  2. The primop machinery captures the standin in its closures;
     even if the failure is caught, the cached apply chain still
     routes through it.
  3. The `<cached-fn>(TLO).impl` deep in the call stack uses
     `innerEval->apply(TLO, contraArg)` which is the bypass that
     hides the OUTER fall-through.

  None of these is impossible to fix, but each adds plumbing.
  Defer this step (= the outer-change variant of cb-higher-order
  family); ship the step 2 (warm replay) fix as principled
  progress and document this remaining work.

**Step 7 — Full `nix build` regression check (DONE — same 5
failures as before, advanced to deeper failure modes):**
- cb-higher-order: red at step 3 (was red at step 2). Forward
  progress; step 3 fix is separate work.
- cb-higher-order-nested: same shape.
- cb-stats-higher-order-baseline: red on hit-count assertion
  (= step 2 path changed). Update assertion separately once
  measured.
- cb-385: still red (unrelated to this work).
- cb-sibling-discrimination-via-observation: still red (unrelated).
- Other cb-* and non-cb: unchanged (236 OK).
