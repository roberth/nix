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
  fn=hex(PostulatedIdempotentRead{TLO.cdi}.structural), arg=hex(PositionalSeed{3}.cdi)})`).
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
`applyResultSubject = ApplyResultSubject{PostulatedIdempotentRead{TLO.cdi},
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

## Architectural diagnosis: the lambda primop is bypassed at warm

The principled gap is not in `dispatchApplyLive`'s re-entry
handling, nor in resolving inner contraArg CDIs, nor in the
shape of AmbientResult. It's that **the standin's
`<replay-local-lambda>` primop never fires when outer's body
applies its argument** — and per the design's lambda-LO section,
firing the primop is *the entire mechanism* by which warm
reproduces the cb-apply's recursive applies.

### The design's intent

From [`tracing-eval-cache-content-identity-via-asks.md`](./tracing-eval-cache-content-identity-via-asks.md#atom-storage):

> Lambda LocalObjects don't need their body stored. A lambda's
> atom is just `(localId, kind=lambda)`; the walker reconstructs
> it as a primop Value whose `impl`, when applied, consults the
> `AmbientAsks` trie for a recorded edge matching the live arg's
> evolved content id, and either reproduces the recorded apply
> result from CAS atoms or throws a depth-2 divergence signal
> that the surrounding walker catches as a miss.

So when outer's `g 5` (in cb-higher-order's `f = g: g 5` body)
encounters the standin lambda, the lambda's primop is supposed
to fire and:
1. Consult AmbientAsks with the live arg's evolved CDI.
2. Either reproduce the recorded apply-result, or throw
   divergence.

Both branches are clean from the surrounding walker's
perspective: a match = the apply produces the cold value; a
divergence = the apply Fact's dispatch throws and
`dispatchApplyLive` catches it.

### What actually happens at warm

In `dispatchApplyLive`, the standin is constructed and its
`toValueOrProxy` returns a `<replay-local-lambda>` primop Value.
This primop Value is passed to outer as the `arg` of the mkApp.
When outer's `<ambient-fn>(f).queryApply` is invoked, the
ambient apply routes through `AmbientResolver::apply` → `runOn`,
which wraps `argObj` (= `InterpreterObject` of the primop
Value) in a `TracingLocalObject` (= the writer-side wrapper for
recording covariant-callback args).

This `TLO` then bridges via `ExprFromObject`. When outer's
`g 5` forces `g`, `ExprFromObject(TLO).eval` hits the nFunction
case, which falls through to **`makeCachedFnPrimOp(TLO,
innerEval, resolver)`** — *not* the original
`<replay-local-lambda>` primop. The `<cached-fn>` primop is
applied to `5`; its impl calls `innerEval->apply(TLO,
contraArg_g5)` = `IR.apply`; `IR.apply` returns a
`TracingReplayObject` whose `getType` walks the main trie via
`v13Walk`.

So outer's body never invokes the standin's primop. Instead it
routes through the `<cached-fn>` → `IR.apply` → `TracingReplayObject`
path, which is the cache-mediated path for *recorded* values
(= main trie `Asks`/`Terminals` lookups), not the
standin's d=2 AmbientAsks-driven path.

The two paths converge at the apply-result CDI (= same
`localSyntheticCdi`), so the sub-Q `Terminals` lookups for
synthetic.getType / synthetic.getInt *do* find the recorded
data — that's why simple cb-higher-order warm replay (= the
no-divergence case) appears tantalisingly close to working.
But the chain-advance the lambda primop was supposed to do for
the recursive apply Fact never happens, so the standin's
`chainCursor` doesn't reach the recorded AmbientResult, and
the `dispatchApplyLive`'s returned value is wrong. **All the
"offline AmbientResult" and "supplementary objects" workarounds
I sketched in this memo's prior versions were trying to paper
over this single architectural mismatch.**

### What a principled fix looks like

The standin's primop value should reach outer's body **without
being wrapped in `TLO` first**, so that outer's `g 5` fires the
primop directly. Sketched directions, none of them small:

1. **`runOn` detects "argObj wraps a standin's primop"** and
   skips the TLO wrap, passing argObj through unchanged. Needs
   a way to identify "this argObj is from a standin" — possibly
   a marker on `ReplayLocalObject` or a virtual on Object.

2. **`RLO::toValueOrProxy` returns something other than a raw
   primop** — e.g., an `AmbientObject`-like wrapper that
   participates in the existing AmbientObject-bypass path in
   `runOn`. Effectively re-using the outer-direction machinery
   for the standin. Plausible but blurs the d=1/d=2 boundary.

3. **`ExprFromObject(TLO).eval`'s nFunction case detects "TLO
   inner is a standin's primop"** and returns the primop
   directly rather than constructing `<cached-fn>(TLO)`.
   Symmetric to existing nFunction sub-cases for AmbientObject
   and RLO. Probably the smallest of the three.

All three preserve the recording-side use of TLO (= cold
recording still wraps `argObj` so the writer captures probes);
the change is purely walker-side, gating the wrap or its
consequences on whether we're dispatching a standin's primop.

### Why this isn't covered by the existing TLO checks

`ExprFromObject::eval`'s nFunction case already special-cases
`ReplayLocalObject`:

```cpp
if (dynamic_cast<ReplayLocalObject *>(obj.get())) {
    auto val = obj->toValueOrProxy(state, ambientResolver);
    v = **val;
    break;
}
```

But this only triggers when `obj` IS an `RLO`. At warm, by the
time we get to `ExprFromObject(TLO).eval`, `obj` is a `TLO`
wrapping an `InterpreterObject` wrapping the standin's primop
Value. The `dynamic_cast<RLO>` fails because we're three layers
out from the RLO. Either the TLO needs to forward the cast (=
ugly), or the wrapping needs to not happen in the first place.

## What I am NOT proposing

Earlier drafts of this memo proposed:
- "Offline AmbientResult chain walk" in both re-entry and
  non-re-entry paths of `dispatchApplyLive`. **Violates
  principle 6** (= walker advances in lockstep with cur); the
  standin's chainCursor stops short of the terminal because the
  primop is bypassed, and computing the terminal offline papers
  over that without fixing it.
- "Supplementary-objects stack" on the writer for inner
  contraArg CDI resolution via try-every-k iteration. Adds a
  parallel lookup mechanism rather than fixing the proxy chain
  to extend properly. Same shape of papering-over.
- Various `dispatchApplyLive` throw → nullopt conversions and
  `RLO::defeatCache` delegations to handle cascading
  fall-throughs at outer-change. Local patches to symptoms;
  don't address the cascade's root cause (= the standin escape
  through TLO wrapping).

These workarounds were implemented and reverted in this
session's commits. The reverts are documented; the workarounds
are what to NOT re-implement.

## Status

**Green.** cb-higher-order step 2 (warm replay) passes at HEAD.
The architectural change described above landed. The diagnostic
content of this memo (= what the writer records, where the walker
reaches, why the chains diverge) is retained as reference for
similar patterns that may surface in future work.
