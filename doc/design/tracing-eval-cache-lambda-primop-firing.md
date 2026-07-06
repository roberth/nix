# Lambda-primop firing — principled fix for cb-higher-order

Design memo for making the standin lambda's primop actually fire
at warm — implementing the design's lambda-LO intent rather than
the d=1 main-trie shortcut the current code falls into.

Closes the architectural gap diagnosed in:
- [`tracing-eval-cache-higher-order-replay.md`](./tracing-eval-cache-higher-order-replay.md)
- [`tracing-eval-cache-outer-change-fallthrough.md`](./tracing-eval-cache-outer-change-fallthrough.md)

Targets:
- **cb-higher-order step 2** (warm replay): currently red.
- **cb-higher-order step 3** (outer change): currently red, downstream
  of the same bypass.
- **cb-higher-order-nested step 2** (outer change): same root cause.

## The design's lambda-LO intent

From
[`tracing-eval-cache-subject-id.md`](./tracing-eval-cache-subject-id.md#atom-storage):

> Lambda LocalObjects don't need their body stored. A lambda's
> atom is just `(localId, kind=lambda)`; the walker reconstructs
> it as a primop Value whose `impl`, when applied, consults the
> `AmbientAsks` trie for a recorded edge matching the live arg's
> evolved content id, and either reproduces the recorded apply
> result from CAS atoms or throws a depth-2 divergence signal.

And on storage:

> `AmbientResponse` payloads live in `LocalResponseMap`, which is
> keyed by `requestHash` rather than `responseHash`. […] the
> depth-2 walker is the only consumer.

Three structural commitments:
1. The standin lambda IS a primop at warm.
2. When applied, the primop consults `AmbientAsks` (= per-probe
   validation) and reads from `LocalResponseMap` (= per-probe
   responses).
3. Failure = throws a divergence signal; the surrounding walker
   catches it as a miss.

The current code violates (1) at every level: the standin's primop
gets wrapped in `TLO` by `runOn` and never fires; the `<cached-fn>`
chain takes over and routes through main-trie `Asks`/`Terminals`
instead. And it violates (2) because the writer-side `tracing_obj_g5`
records under main-trie sub-Q `Terminals` (= d=1 storage), not under
`LocalResponseMap` (= the design's d=2 storage).

## The three changes

Numbered by where they live.

### Change A — `dispatchApplyLive` uses Object-level apply

**Location**: `src/libexpr/tracing-replay-evaluator.cc`,
`dispatchApplyLive`.

**Today** (lines 696-706):

```cpp
auto fnValue = fnObj->toValueOrProxy(evalState, /*resolver=*/ nullptr);
auto argValue = replayLocal->toValueOrProxy(evalState, /*resolver=*/ nullptr);
auto * resultVal = evalState.allocValue();
resultVal->mkApp(*fnValue, *argValue);
auto resultObj = std::make_shared<InterpreterObject>(
    evalState, allocRootValue(resultVal));
(void) resultObj->getType();
```

The mkApp constructs at the Value level. `argValue` is the
standin's primop Value, but by the time it's wrapped in a Value
and then in an `InterpreterObject` (= via `<ambient-fn>.impl`
later on the outer side), its Object-ness is lost. `runOn` sees
an `InterpreterObject`, not the RLO.

**Proposed**:

```cpp
std::shared_ptr<Object> resultObj;
try {
    resultObj = fnObj->queryApply(replayLocal);
} catch (const std::exception & e) {
    tracingCacheLog(
        "dispatchApplyLive: divergence at apply for applyReqHash=%s: %s",
        applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        e.what());
    return std::nullopt;
}
try {
    (void) resultObj->getType();
} catch (const std::exception & e) {
    tracingCacheLog("dispatchApplyLive: divergence during force: %s", e.what());
    return std::nullopt;
}
```

Object-level apply preserves the RLO through the bridging chain.
`fnObj.queryApply(replayLocal)` routes through `AmbientObject::
queryApply` → `applyFn` → `resolver->apply` → `runOn` with
`argObj = replayLocal` (= the RLO directly).

The try/catch wraps the divergence-signal flow: anything thrown
during the apply or force is treated as a clean cache miss.

### Change B — `runOn` skips TLO wrap when argObj is an RLO

**Location**: `src/libexpr/expr-from-object.cc`, `AmbientApply::runOn`,
around line 320-324.

**Today**:

```cpp
auto wrappedArg = (innerWriter && outerRootFSRoot)
    ? std::shared_ptr<Object>(std::make_shared<TracingLocalObject>(
          argObj, argSubject, *innerWriter, ref<SourceRoot>(outerRootFSRoot), localCell,
          resolverHandle->callScope, resultId))
    : argObj;
```

**Proposed**:

```cpp
auto wrappedArg = (innerWriter && outerRootFSRoot
                   && !dynamic_cast<ReplayLocalObject *>(argObj.get()))
    ? std::shared_ptr<Object>(std::make_shared<TracingLocalObject>(
          argObj, argSubject, *innerWriter, ref<SourceRoot>(outerRootFSRoot), localCell,
          resolverHandle->callScope, resultId))
    : argObj;
```

Justification: TLO's purpose is recording outer's probes on
inner-supplied locals. At warm, the standin (= RLO) already
encapsulates the recorded contract; outer's probes on it go
through the standin's own validation logic
(`advanceChainAndAppendFact` against `AmbientAsks`). Wrapping
the standin in TLO would add a second recording layer that's
both redundant (= no new information to capture) and corrupting
(= it converts the standin's primop into a `<cached-fn>(TLO)`
that bypasses the design's lambda-LO mechanism).

At cold, `argObj` is `InterpreterObject` of a real inner Value;
`dynamic_cast<RLO>` returns null; TLO wraps as before. Cold flow
unchanged.

### Change C — Writer records lambda apply-results in `LocalResponseMap`

**Location**: `src/libexpr/tracing-evaluator.cc`, `IT::apply` (around
line 289 where it detects `dynamic_cast<TracingLocalObject *>(fn)`),
and corresponding paths in `tracing-object.cc` / `tracing-writer.hh`.

**Today**:

The result of `inner.apply(TLO, contraArg)` is wrapped in a
generic `TracingObject` (via `TracingObject::create`, line 359).
That `TracingObject`'s method calls (= getType, getInt, etc.)
record sub-Q `Terminals` rows in the main trie via
`writer.logQuery + logResult`.

**Proposed**:

When `IT::apply` detects `fn` is a `TLO` (= same branch as the
existing `logDepth2ApplyFact` call), it wraps the result in a
**lambda-apply-result recorder** — a TracingObject variant whose
method calls store responses in `LocalResponseMap` (= d=2
storage) instead of main-trie `Terminals` (= d=1 storage). The
`from` field for these recordings is the local-synthetic CDI
(= `ApplyResultSubject{PostulatedIdempotentRead{TLO.cdi}, contraArg.subject}`).

Two implementation shapes:

**B.C.1 — New class `TracingLocalApplyResult`** mirroring
`TracingObject` but with method-level recording redirected to
`writer.logDepth2Observation` (or a new equivalent that writes
`insertLocalResponse` directly). Cleanest separation.

**B.C.2 — Flag on `TracingObject`**: add a boolean
`recordToLocalResponseMap` field; method implementations branch
on it. Smaller diff but mixes two recorders in one class.

Recommend C.1. Naming: `LambdaApplyResultObject` perhaps —
narrower than "TracingLocalApplyResult" and explicit about the
specific use.

The recordings under the local-synthetic CDI must match what the
walker's synthetic RLO reads at warm: `reqHash = qH(query{from=hex(
contentIdAt(syntheticSubject, scope, walkFacts, walkFacts.size()))})`.
The writer's stamp of the same query with the same evolved-CDI from
field produces the same reqHash; the response payload is stored in
LocalResponseMap under that reqHash. Lookup-on-write key matches
lookup-on-read key by the same evolution formula on both sides.

The chain-advance the standin's primop performs (= for the
recursive apply Fact itself) is already in tree via
`logDepth2ApplyFact`. That's the apply Fact's recording; Change C
adds the apply-result observations alongside it, so the chain has
both the apply Fact and its result probes recorded in d=2.

## Walk-through: cb-higher-order step 2 (warm replay, expect 6)

After all three changes:

1. Outer applies `<cached-fn>(TO)` primop to `{f = g: g 5}`.
   Routes through `IR.apply(TO, contraArg_outer)` → returns
   `obj_outer_replay` (= TRO with `applyCdi_outer` triePos).

2. `ExprFromObject(obj_outer_replay).eval` triggers
   `obj_outer_replay.getType` → `v13Walk(Q=4ed6c1c8bdac)`.

3. v13Walk dispatches chain. ε for boundary #1 dispatches via
   `dispatchApplyLive(applyReqHash=ce25f821df1e)`. Constructs
   `RLO_innerLam` standin. **(Change A)** Calls
   `fnObj.queryApply(RLO_innerLam)`.

4. `fnObj` (= resolved live `f_amb`) `.queryApply(RLO)` →
   `AmbientObject::queryApply` → `applyFn` → `resolver->apply`
   → `runOn(argObj=RLO)`.

5. `runOn` **(Change B)**: `dynamic_cast<RLO>(argObj)` → yes →
   `wrappedArg = argObj` (no TLO). `argThunk = ExprFromObject(RLO,
   innerEval, resolver)`. `mkApp(outer.f.defeatCache=`g: g 5`,
   argThunk)`. Registers resultObj.

6. Back in `<ambient-fn>.impl`: `ExprFromObject(result_amb,
   nullptr, resolver).eval`. result_amb.getType → ambient query
   → resolver query → InterpreterObject(mkApp Value).getType
   → forces mkApp.

7. Force mkApp: substitutes g = argThunk. Outer body `g: g 5`.
   Force g: `ExprFromObject(RLO).eval`. nFunction case (line
   678): `dynamic_cast<ReplayLocalObject *>(obj.get())` → yes →
   `obj->toValueOrProxy(state, resolver)` → returns standin's
   `<replay-local-lambda>` primop Value. `v = **val`. **g IS the
   standin's primop, NOT a `<cached-fn>` wrapper.**

8. Outer applies primop to 5. **`<replay-local-lambda>.impl`
   fires.** Impl:
   - Composes synthetic apply-result subject `ApplyResultSubject{
     RLO.subject, PositionalSeed{depth+1}}`.
   - Chain-advance: computes recursive apply Fact's elementHash
     against the recorded `AmbientAsks` chain at `chainCursor`.
     Validates the apply happened. Updates chainCursor.
   - Constructs synthetic RLO (= for the apply-result).
   - `ExprFromObject(synthetic).eval`.

9. `synthetic.getType` → `readResponse` looks up `reqHash =
   qH(QueryGetType{from=hex(syntheticEvolvedCdi)})` in
   `LocalResponseMap`. **(Change C)** ensured the writer recorded
   exactly this entry. Returns "int".

10. `synthetic.getInt` → same path → returns 6. ExprFromObject
    sets `v = mkInt(6)`. Outer's `g 5` = 6. Outer's `(g: g 5)
    (x:x+1)` = 6.

11. Back to step 6: result_amb's mkApp forces to 6. getType
    returns nInt. Ambient query records d=1 fact.
    `dispatchApplyLive` returns `replayLocal->getChainCursor()`
    — which now equals cold's AmbientResult because the
    primop's chain-advance fired (= principle 6 satisfied).

12. Outer walker's apply Fact dispatch matches recorded
    response. Chain advances to next edge. Eventually
    `Terminals(Q=4ed6c1c8bdac, factSet=correct)` found.
    `obj_outer_replay.getType` returns nInt. Outer
    interpreter sees nInt; forces getInt → walks → 6.

**Final value: 6. ✓**

## Walk-through: cb-higher-order step 3 (outer change, expect 11)

After steps 1+2 populated the cache, change outer to `f = g: g 10`.

1. Outer applies `<cached-fn>(TO)` to `{f = g: g 10}`. Same
   IR.apply chain as before.

2. `obj_outer_replay.getType` → v13Walk dispatches chain.
   `dispatchApplyLive(applyReqHash=ce25f821df1e)` → constructs
   standin → `fnObj.queryApply(replayLocal)` → outer body force.

3. Outer's `g 10` fires the standin's primop. Primop's impl:
   - Composes synthetic apply-result subject.
   - Chain-advance: `getAmbientAsks(chainCursor)` looks for an
     edge whose request matches the synthetic apply's `reqHash`.
     But the live evolved CDI (= contributed by contraArg_g10
     returning 10 vs cold's 5) **doesn't match the recorded
     AmbientAsks edge**. `advanceChainAndAppendFact` throws
     "depth-2 divergence: no AmbientAsks edge from cur".

4. The throw propagates out of `<replay-local-lambda>.impl`,
   out of outer body force, out of `dispatchApplyLive`'s try/
   catch **(Change A)** which catches and returns nullopt.

5. Walker treats apply Fact dispatch as failed. Edge fails.
   `obj_outer_replay.getType` v13Walk eventually misses (=
   no terminal reachable through any chain).

6. Falls through to OUTER `obj_outer_replay.ensureInner` =
   `inner.apply(TO, contraArg_outer)` = `Interpreter.apply`.

7. `Interpreter.apply`: `fnValue = TO.toValueOrProxy →
   TO.defeatCache → TO.ensureInner.defeatCache → inner.evalFile`.
   **Cached body re-parsed fresh.** Returns the actual cb-body
   Value.

8. `argValue = contraArg_outer.toValueOrProxy` (= ExprFromObject
   thunk). `mkApp(fresh_body, args_thunk)`. Force.

9. Fresh body `{f}: f (x:x+1)` runs. f.queryApply((x:x+1)) →
   `<ambient-fn>.impl` → `runOn(argObj=InterpreterObject((x:x+1)
   Value))`. **(Change B)** `dynamic_cast<RLO>(InterpreterObject)`
   → no → TLO wrap as cold. wrappedArg = TLO(real_lambda).

10. Outer's `g 10` runs natively (= via `<cached-fn>(TLO)` →
    `IR.apply` → fall-through to `Interpreter.apply` → mkApp
    (real_lambda, contraArg_g10_thunk) → force → 10 + 1 = 11).

**Final value: 11. ✓**

Critically: step 9 takes the TLO-wrap branch because argObj at
fresh re-eval IS InterpreterObject of a real lambda (not an
RLO). The standin's primop never gets re-introduced into the
cascade. The cascade terminates at the fresh re-eval.

## Walk-through: cb-higher-order-nested

`{apply2}: apply2 (innerLambda: innerLambda 5)` with `apply2 =
midfn: midfn (n: n + 100)`.

At warm step 2 (= outer change to `(n + 200)`):

Same shape — at each nesting level, the standin's primop fires
(per Changes A+B), runs the recursive apply, validates against
AmbientAsks. At divergence (= outer changed), throws → caught
by dispatchApplyLive → walker miss → OUTER fall-through to
fresh parse.

Three nesting levels means three potential primop firings, each
with its own chain-advance. Each is independent. As long as
each level's primop fires cleanly, the cascade terminates.

The Change C recording covers each level: each `IT::apply(TLO,
contraArg)` at cold records its result wrapper into
LocalResponseMap. Multi-level nesting just means multiple
LocalResponseMap entries, each keyed under its level's local-
synthetic CDI.

## Validation against principles

Per `tracing-eval-cache-subject-id.md`:

**Foundational #6 (no deep hashing).** All lookups by query
hash (= queries hashed via SHA-256). No value-content hashing.
✓

**Foundational #7 (laziness end-to-end).** The standin's
primop fires only when outer applies it (= consumer-triggered).
Synthetic RLO's getType/getInt fire only when ExprFromObject's
type-driven evaluation needs them. ✓

**Foundational #8 (forcedness-independence).** The recording
side and replay side use the same `IT::apply` / `IR::apply`
dispatch for the recursive apply. Whether the apply-result is
forced eagerly or lazily doesn't change what's recorded or what
the walker looks up. ✓

**Foundational #9 (cumulative dependency).** No factSet
pruning. The recursive apply's d=2 chain is appended to the
enclosing boundary's chain at flush, just as today. ✓

**Design principle 1 (CDIs are pure functions of (subject,
factset)).** Local-synthetic CDI computed identically on writer
and walker side via the same `cidasks::contentIdAfter` formula.
✓

**Design principle 2 (subjects are static structural
identifiers).** `ApplyResultSubject{PostulatedIdempotentRead{TLO/RLO.cdi},
PositionalSeed{depth+1}}` is structural. ✓

**Design principle 3 (apply-result formula).** Both sides
compose the apply-result subject from the same constituents.
The local-synthetic CDI = `qH(QueryApply{fn=hex(constituent.cdi),
arg=hex(...)})` matches on both sides. ✓

**Design principle 4 (per-Asks-edge membership).** The
recorded apply-result observations belong to boundary #1's
d=2 chain (= the cb-apply boundary's `AmbientAsks` edges,
sequenced through cumulativeFactSet evolution at flush). ✓

**Design principle 5 (recording flush rewrites `from` per
edge).** The lambda-apply-result recorder uses the same
flush-time CDI computation as existing d=2 recordings. The
`from` evolves through prior probes in the same chain. ✓

**Design principle 6 (walker advances in lockstep).** The
standin's primop's chain-advance is the walker-side mirror of
the writer's `logDepth2ApplyFact` + d=2 fact recording. The
standin's chainCursor naturally reaches the recorded
AmbientResult terminal. **This is what the workarounds I
reverted violated.** ✓

**Design principle 7 (XOR commutativity within edge).**
Single-observation edges in AmbientAsks chain; commutativity
within edge holds trivially. ✓

**Design principle 8 (same-shape collapse and discrimination).**
Two cb-apply invocations with identical recorded probes
collapse via content-addressed `AmbientAsks` rows.
Observation-driven discrimination via chain divergence works
because the standin's chain-advance evolves CDIs through
probes per the cidasks formula. ✓

**Polarity-validation memory.** Inner produces `+` values (=
the inner-lambda's apply-result). The standin's primop
materialises that `+` value from recorded atoms in
LocalResponseMap. Validation = `AmbientAsks` lookup; divergence
= miss + throw. The standin's role is exactly what the polarity
memory describes. ✓

**Fix A / Fix B avoidance.** Fix A removed CDI evolution to
make cb-385 green; Fix B froze CDI via PostulatedIdempotentRead for
apply-result observations. This proposal does neither — CDIs
evolve via cidasks per the principles; subjects are structural
ApplyResultSubject (not PostulatedIdempotentRead for observation
subjects). ✓

## Risks

1. **Change C is the largest piece.** Writer-side changes to
   how lambda apply-results are recorded touch
   `tracing-evaluator.cc`, `tracing-object.cc` (or a new sibling
   class), and possibly `tracing-writer.hh`. Risk of regression
   in non-higher-order cb-* tests if the discriminator (= `fn`
   is TLO) misclassifies any case. Mitigation: the
   discriminator is the SAME as the existing `logDepth2ApplyFact`
   branch (line 289 of `tracing-evaluator.cc`), which has been
   in tree and tested.

2. **Object-level apply in `dispatchApplyLive` (Change A)**
   changes the resultObj's type from `InterpreterObject(mkApp
   Value)` to whatever `fnObj.queryApply` returns (= AmbientObject
   for the apply-result, per `AmbientObject::queryApply` line
   284-289). Callers of `dispatchApplyLive` that depended on the
   InterpreterObject form would need updating. Trace: the only
   caller is the dispatch lambda in v13Walk; it only uses
   `resultObj->getType()` to force, which AmbientObject
   supports. Safe.

3. **`runOn` skipping TLO for RLO (Change B)** affects
   the `bridgedLocals` cache keyed by `argObj.get()` (= line
   329). With argObj == RLO, the cache key is the RLO pointer.
   The thunk wraps `ExprFromObject(RLO)`. Subsequent runOn
   calls with the same RLO would hit the cache and return the
   same thunk. Correct for sibling cb-apply invocations of the
   same standin (= same standin pointer; same thunk). The
   sidecar `deferRequest` at the end of runOn still fires, so
   sidecar registration is unchanged. The argId computed from
   localCell.depth still matches cold's argId (= same
   depth+scope at writer recording time). ✓

4. **Cold-side `logDepth2ApplyFact` (already in tree)** vs
   the proposed Change C apply-result recording: both go into
   boundary #1's d=2 chain. The chain has one fact for the
   apply itself + N facts for the apply-result observations.
   Today only the first is recorded (= chain has 1 fact); under
   Change C the chain has 1+N facts. Walker's primop chain-
   advance handles 1 fact; it needs to handle N+1. Requires
   the primop's impl to update chainCursor for each probe (=
   already done via `advanceChainAndAppendFact` inside
   synthetic RLO methods); just ensure the primop's
   chain-advance for the recursive apply Fact happens AT THE
   RIGHT POSITION in the per-probe sequence (= before the
   synthetic's own probes).

5. **cb-stats-higher-order-baseline hit-count drift**. The
   change shifts dispatch patterns; hit count will change.
   Update assertion empirically (= same as for prior fixes).
   Not a correctness concern.

## Open questions

1. **Result of `fnObj.queryApply(replayLocal)` vs current
   `mkApp + InterpreterObject`**: are there any callers of
   dispatchApplyLive that need the InterpreterObject
   specifically? Verify the only consumer is `dispatch` in
   v13Walk, which only uses `getType` to force.

2. **`LambdaApplyResultObject` (Change C.1)** — what's the
   minimal interface? Should it inherit from `TracingObject`
   to reuse method implementations, or be a sibling that
   delegates to `inner` for the actual result computation and
   only differs in WHERE the recording goes? Sibling is
   cleaner per single-responsibility.

3. **Per-probe sequence in chain**: the standin's chain has:
   (1) the recursive apply Fact (via `logDepth2ApplyFact`),
   (2) the result wrapper's getType, (3) the result wrapper's
   getInt (or other accessors). Order matters because
   cumulativeFactSet evolves through the chain. The writer's
   `flushPendingAmbient` line 254-267 processes facts in
   `boundary.facts` insertion order. Need to verify the
   insertion order: `logDepth2ApplyFact` adds (1) before
   `LambdaApplyResultObject`'s methods add (2), (3) — which
   matches the cold execution order (= recursive apply
   happens first, then accessors). Same chronology at warm
   (= primop's chain-advance for (1) happens before the
   synthetic's methods are probed, per the impl structure).

4. **Outer-arg's CDI evolution within the recursive apply's
   chain**: at warm step 3 (outer change), `contraArg_g10`'s
   structural CDI is the same as cold's `contraArg_g5`. The
   divergence shows up at the observation level (= `getInt
   from=contraArg_g.cdi` returns 10 vs recorded 5). Does the
   primop's chain-advance detect this? The chain-advance
   matches against AmbientAsks; if the recorded chain has
   `(getInt from=contraArg, ResultInt{5})` and live response
   is 10, the elementHash differs, the AmbientAsks edge for
   the new elementHash doesn't exist, throws divergence. ✓

5. **Atom storage details for nested cases**: cb-higher-
   order-nested has multiple `IT::apply(TLO, ...)` calls.
   Each gets its own `LambdaApplyResultObject` recording. Each
   level's local-synthetic CDI is distinct. LocalResponseMap
   entries are keyed by reqHash which includes the level's
   CDI. No key collisions. ✓

## Verification plan

Staged. Each step has measurable success criteria.

**Step 1 — Read `<cached-fn>.impl` and confirm that an RLO
arriving at runOn IS reachable.** Trace cb-higher-order step 2
with logging to confirm the current code path: outer's `g 5`
goes through `<cached-fn>(TLO)` → `IR.apply` → `tre.getType`,
and TLO's inner is the standin's `<replay-local-lambda>` primop
Value. Already-done diagnostic; restated for completeness.

**Step 2 — Implement Change A in isolation.** Modify
`dispatchApplyLive` to use `fnObj.queryApply(replayLocal)`. Run
`builtins-cache cb-higher-order nix-expr-tests`. Expect no
behaviour change (= without Change B, runOn still wraps in TLO
which bypasses the primop). Verify the trace shows `runOn`
being called with `argObj = RLO` (= log line; add temporary
debug print if needed).

**Step 3 — Implement Change B.** Add the `dynamic_cast<RLO>`
check in `runOn`. Run cb-higher-order. Expect the trace to
show outer's `g 5` firing the standin's primop (= log line
inside primop's impl) instead of going through
`<cached-fn>(TLO)`. cb-higher-order step 2 might still fail
at the synthetic's readResponse (= because LocalResponseMap
doesn't have the data yet — Change C is required).

**Step 4 — Implement Change C.** Add the
`LambdaApplyResultObject` (or equivalent) and route IT::apply's
TLO-fn branch through it. Verify with sqlite inspection that
`LocalResponseMap` now has entries keyed under local-synthetic
CDI for `QueryGetType` and `QueryGetInt`. Run cb-higher-order
step 2. Expect green.

**Step 5 — Run cb-higher-order step 3.** Expect green. The
divergence chain throws cleanly via the standin's primop;
dispatchApplyLive catches; walker miss; OUTER fall-through to
fresh parse; produces 11.

**Step 6 — Run cb-higher-order-nested.** Expect step 2 (outer
change) green. Multi-level nesting exercises Change C's
multiple LocalResponseMap entries.

**Step 7 — Full `nix build` regression.** Expect:
- cb-higher-order: green.
- cb-higher-order-nested: green.
- cb-stats-higher-order-baseline: green (hit-count assertions
  updated in prior commits).
- cb-385: green (closed under separate work — see task #92).
- cb-sibling-discrimination-via-observation: green.
- cb-sibling-b-depends-on-a: green as of `9184b703e` (snapshot-
  padded retry).
- Other cb-* and non-cb: unchanged, all green (30/30 cb-* +
  builtins-cache).

**Step 8 — Performance check (perf harness).** Multi-level
LocalResponseMap entries add storage. cb-higher-order family
is minimal; full nixpkgs flake with cached higher-order
patterns would stress this. Perf measurement runs post-merge.

**Rollback policy.** Any test failure outside the expected
list (= cb-385, cb-sibling, cb-stats hit-count) means
regression. Revert the step that introduced it and diagnose.

## What this proposal does NOT include

- **cb-sibling discrimination via observation**: separate fix
  per [`tracing-eval-cache-per-arg-completion.md`](./tracing-eval-cache-per-arg-completion.md#how-cb-sibling-was-closed).
  Landed and orthogonal.
- **cb-385 regression**: separate work; root cause may or may
  not interact with these changes.
- **cb-stats hit-count updates**: mechanical follow-up.
- **Cleaning up the supplementary-objects / offline AmbientResult
  workarounds** — those were reverted in this session; nothing
  to clean. The two doc memos (`tracing-eval-cache-higher-order-
  replay.md` and `tracing-eval-cache-outer-change-fallthrough.md`)
  already note what was reverted under "What I am NOT proposing".

## Implementation status (= what landed)

All three changes shipped:
- **Change A** (`dispatchApplyLive` Object-level apply): in
  `src/libexpr/tracing-replay-evaluator.cc`.
- **Change B** (`runOn` skips TLO wrap for RLO): in
  `src/libexpr/expr-from-object.cc`.
- **Change C** (writer routes lambda apply-results to
  `LocalResponseMap`): new `LambdaApplyResultObject` sibling in
  `src/libexpr/{include/nix/expr/,}lambda-apply-result-object.{hh,cc}`,
  wired through `TracingEvaluator::apply`'s TLO-fn branch.

Walker-side support landed alongside Change C:
- Synthetic apply-result is constructed with
  `withAmbientAsksValidation()` so each post-apply probe advances
  `chainCursor` through the recorded d=2 chain.
- The `<replay-local-lambda>` primop uses LOCAL copies of
  `walkFacts`/`chainCursor` per firing — same standin re-fired
  across multiple walker dispatches stays aligned with the cold
  recording. The recursive apply Fact's stamped fn/arg now use
  `contentIdAt(...,0)` so the walker's stampedReqHash matches what
  `logDepth2ApplyFact` records.
- `resolveApplyId`'s standin path reads the `localArg` sidecar's
  `depth`/`scope` and sets `applyContext` so the synthetic's
  `PositionalSeed{depth+1}` is the right one (previously
  dereferenced an empty optional and produced garbage subjects).

### Live-proxy registration (= seed-resolution gap closer)

In addition to Changes A/B/C the implementation also closes a
downstream gap that the memo above did not anticipate. After the
three changes were in tree, cb-higher-order's DISALLOW_PARSE warm
replay still missed — but for a different reason than the
lambda-primop bypass: the OUTER walker, after correctly dispatching
the cb-apply Fact, then tries to dispatch d=1 facts whose `from`
is the inner-side cb-arg seed CDI (= `seed(applyDepth+1).initial`,
e.g., `de4269a965a5...` for cb-higher-order's contraArg_5). Cold
recorded these as the inner's observations on the AmbientObject the
`<cached-fn>(TLO).impl` constructed around outer's passed value;
the AmbientObject is transient (constructed inside the impl and
never registered, per `makeCachedFnPrimOp`'s "boundary-trace-only
discipline" comment), so at warm `resolveCdiId` has no producer
Request and no localArg sidecar to chase, and falls through
"outer-seed by elimination" returning null.

The fix is the standin's `<replay-local-lambda>` primop itself
registering args[0] under the cb-arg seed's initial CDI at fire
time. Each firing publishes an `InterpreterObject(state, args[0])`
into the shared `AmbientResolver.registry.outerValues` keyed by
`contentIdAfter(PositionalSeed{applyDepth+1}, applyScope, {})` —
the same expression `makeCachedFnPrimOp` uses for its `rootId`, so
the OUTER walker reaches us via matching CDI keys. The walker's
`resolveCdiId` calls `inner->getAmbientResolver()` (a new virtual
on `Evaluator`) and consults `outerValues` before the
"outer-seed by elimination" fall-through. Dispatch on the resolved
`InterpreterObject` is live (= capability-mediated) — outer-side
behaviour change still surfaces as a divergent live response and a
clean walker miss.

Plumbing required:
- `Evaluator::getAmbientResolver()` virtual (default null), with
  `Interpreter` returning its `ambientResolver` field and the two
  wrapping evaluators delegating to `inner`.
- `registerAmbientResolverProxy` / `tryResolveAmbientResolverProxy`
  free functions in `expr-from-object.{hh,cc}` exposing the
  resolver's outer-values registration without leaking
  `AmbientRegistry`'s definition into the header.

Boundary-trace-only discipline still applies for SIBLING cb-applies
sharing the same cb_arg seed: the registration is last-write-wins,
so cb-sibling tests (which exercise multiple cb-applies in the
same cache call against the same seed) aren't helped by this fix
alone. They were closed via separate work — see
[`per-arg-completion`](./tracing-eval-cache-per-arg-completion.md#how-cb-sibling-was-closed).
The narrow fix in this proposal is sound for cb-higher-order because
there is only one cb-apply per cache call.

### Test outcomes

- `cb-higher-order`: green (all four steps — cold, warm-replay
  DISALLOW_PARSE, outer-change, and restore DISALLOW_PARSE).
- `cb-higher-order-nested`: green (cold and outer-change steps;
  warm-replay DISALLOW_PARSE is commented out in the test by
  design).
- `cb-stats-higher-order-baseline`: green.
- `cb-385`, `cb-sibling-discrimination-via-observation`,
  `cb-sibling-b-depends-on-a`: green.
- All tests: 30/30 cb-* + builtins-cache; 324/0 full meson suite.
