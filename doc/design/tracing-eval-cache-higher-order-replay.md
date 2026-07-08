# Higher-order callback replay

Diagnostic reference for the cb-higher-order failure pattern
(`cb-higher-order`, `cb-higher-order-nested`,
`cb-stats-higher-order-baseline`) and the architectural fix that
closed it. Companion to
[`tracing-eval-cache-subject-id.md`](./tracing-eval-cache-subject-id.md)
and [`tracing-eval-cache-primop.md`](./tracing-eval-cache-primop.md).

The cb-sibling failures (`cb-sibling-discrimination-via-observation`,
`cb-sibling-b-depends-on-a`) are out of scope here — they landed
via separate work. The proposal in this doc is independent of that
work and the two compose.

The fix described below has shipped. The doc is retained as
reference for similar patterns that may surface in future work.

## The failure

`cb-higher-order` records `{f}: f (x: x+1)` against the outer
`{f = g: g 5}`. Cold returns 6 (correct). Warm replay used to fall
through to inner re-evaluation; under `_NIX_DISALLOW_PARSE=1` the
test failed.

The trigger was the Env walker dispatching an Ambient observation
`QueryGetType{from = outerApplyResult's state hash}` — "inner
observed type=int on outer's f-applied-to-innerLam result." To
serve this observation live, the walker calls the outer's apply,
which forces `getType`. Forcing runs outer's `g 5` natively where
`g` is a `<replay-callback-arg-lambda>` primop materialised from
the inner-supplied lambda's `ReplayCallbackArg`.

The primop's impl materialises the synthetic apply-result as a
`ReplayCallbackArg` that reads from the `InnerValueResponse` table.
That table is keyed by `requestHash = SHA-256(query{from = a
composed state hash})` where the composed hash is
`queryHash(QueryApply{fn = RCA subject's state hash, arg =
Arg{depth+1}'s state hash})` — the subject-id formula for
`ApplyResultSubject{RCA, contraArg}`. The writer never recorded
responses under that key; the lookup missed; the standin's
`getType` threw; the surrounding walker fell through.

## What the writer actually records

At cold, the inner observes the apply-result *twice over two
separate writer-side recording paths*. Both observations are real;
both go into the trie; they live under different subjects.

**Path 1 — Ambient observation via the outer's apply-result
`OuterObject`.** Inner forces `f x` through the `<cached-fn>`
primop, which calls `f_outer.queryApply(argObj)`. The returned
`OuterObject` carries subject
`ApplyResultSubject{f_outer.subject, innerLam.subject}` — outer's
`f` applied to inner's lambda. When inner subsequently forces
`getType` on it via the bridge, the ambient query records an
observation with `from = outerApplyResult's state hash`. This is
what the walker's Env chain dispatches.

**Path 2 — sub-Query terminals on the local apply's
`TracingObject`.** Outer's `g 5`, evaluated under outer's `f`
body, triggers `<cached-fn>.impl → innerEval->apply(TCA,
contraArg_g5)`. This goes through `TracingEvaluator::apply`, which:

- emits `logAmbientApplyFact` (the recursive apply Fact itself);
- calls `openApplyBoundary`;
- delegates to `inner->apply(TCA, contraArg)` which returns an
  `InterpreterObject` wrapping the applied result;
- wraps the result in a `TracingObject` with
  `applyResultSubject = ApplyResultSubject{TCA.subject,
  contraArg_g5.subject}` — the **local-synthetic subject**, matching
  what the walker's primop computes.

Back in `<cached-fn>.impl`, the bridging line calls
`ExprFromObject(result.get_ptr(), innerEval, resolver).eval(state,
state.baseEnv, v)`. `ExprFromObject::eval` calls `tracing_obj.getType()`,
`.getInt()`, etc. Each call routes through
`TracingObject::<method>` → `writer.logQuery + logResult`, which
inserts:

- a `Request` payload for
  `QueryGetType{from = local-synthetic subject's state hash}`
  (and its `getInt` / `getAttrNames` / … counterparts);
- a `Terminal(queryHash, envFactSetHash)` row at the current
  cumulative factset.

These are **sub-Query terminals in the main trie**, indexed by the
local-synthetic subject's state hash — exactly the key the walker's
primop computes for its synthetic standin. The data the warm
walker needs was already in the cache; the missing piece was the
*lookup path*, since `ReplayCallbackArg` read from
`InnerValueResponse`, not from the main trie.

## Where the walker was going wrong

Cold recording put the diagnostic data under the right key. The
walker was already trying to look it up. The failure was a
re-entrancy issue in the Ambient dispatcher, not a missing-data
issue.

Mechanism: all Queries at the same `logResult` inherit the same
`envAsksEdges` chain — the cumulative subject-id chain at
`logResult` time. So the getType Query's recorded chain included
the apply Fact (the cb-apply Fact, recorded as a synthetic Env
Fact whose response hash was the cold Ambient result).

When the getType's nested walker dispatched that apply Fact,
`dispatchApplyLive` was invoked **recursively** — already in
flight from the outer walk. The cycle break short-circuited to
`applyRequestHash` instead of the cold-recorded Ambient result.
The walker's `cur` diverged from cold; no recorded Terminal at
the divergent `cur`; miss; fall through to inner re-evaluation.

The cycle break is **load-bearing** — without it, the recursion is
unbounded — but the value it was returning was wrong.

## Architectural diagnosis: the lambda primop was bypassed at warm

The principled gap was not in `dispatchApplyLive`'s re-entry
handling, nor in resolving inner contraArg state hashes, nor in
the shape of the Ambient result. It was that **the standin's
`<replay-callback-arg-lambda>` primop never fired when outer's
body applied its argument** — and per the design's lambda-callback
section, firing the primop is *the entire mechanism* by which warm
reproduces the cb-apply's recursive applies.

### The design's intent

From
[`tracing-eval-cache-subject-id.md`](./tracing-eval-cache-subject-id.md):

> Lambda callback-args don't need their body stored. A lambda's
> atom is just `(subjectHash, kind=lambda)`; the walker
> reconstructs it as a primop `Value` whose `impl`, when applied,
> consults the `AmbientAsk` trie for a recorded edge matching the
> live arg's evolved state hash, and either reproduces the recorded
> apply result from stored atoms or throws an ambient-interaction
> divergence exception that the surrounding walker catches as a
> miss.

So when outer's `g 5` (in cb-higher-order's `f = g: g 5` body)
encountered the standin lambda, the lambda's primop was supposed to
fire and: (1) consult AmbientAsk with the live arg's evolved state
hash, and (2) either reproduce the recorded apply-result or throw
divergence. Both branches are clean from the surrounding walker's
perspective.

### What actually happened at warm

In `dispatchApplyLive`, the standin was constructed and its
`toValueOrProxy` returned a `<replay-callback-arg-lambda>` primop
`Value`. This primop was passed to outer as the `arg` of the
mkApp. When outer's `<cached-fn>(f).queryApply` was invoked, the
ambient apply routed through `OuterResolver::apply` → `runOn`,
which wrapped `argObj` (an `InterpreterObject` of the primop
`Value`) in a `TracingCallbackArg` — the writer-side wrapper for
recording covariant-callback args.

This TCA then bridged via `ExprFromObject`. When outer's `g 5`
forced `g`, `ExprFromObject(TCA).eval` hit the `nFunction` case,
which fell through to **`makeCachedFnPrimOp(TCA, innerEval,
resolver)`** — *not* the original `<replay-callback-arg-lambda>`
primop. The `<cached-fn>` primop was applied to `5`; its impl
called `innerEval->apply(TCA, contraArg_g5)`; the replay
evaluator's apply returned a `TracingReplayObject` whose `getType`
walked the main trie via `walk()`.

So outer's body never invoked the standin's primop. Instead it
routed through the `<cached-fn>` → replay-evaluator-apply →
`TracingReplayObject` path, which is the cache-mediated path for
*recorded* values (main-trie `Ask` / `Terminal` lookups), not the
standin's AmbientAsk-driven path.

The two paths converge at the apply-result's state hash (same
`local-synthetic` value), so the sub-Query `Terminal` lookups for
synthetic.getType / synthetic.getInt *did* find the recorded data —
which is why simple cb-higher-order warm replay appeared
tantalisingly close to working. But the chain-advance the lambda
primop was supposed to do for the recursive apply Fact never
happened, so the standin's chain cursor never reached the recorded
Ambient result, and `dispatchApplyLive`'s returned value was wrong.

### The principled fix

The standin's primop `Value` must reach outer's body **without being
wrapped in a `TracingCallbackArg` first**, so outer's `g 5` fires
the primop directly. Sketched directions:

1. **`runOn` detects "argObj wraps a standin's primop"** and skips
   the callback-arg wrap, passing argObj through unchanged. Needs a
   way to identify "this argObj is from a standin."
2. **`ReplayCallbackArg::toValueOrProxy` returns something other
   than a raw primop** — an `OuterObject`-like wrapper that
   participates in the existing OuterObject-bypass path in `runOn`.
   Effectively re-uses the outer-direction machinery for the
   standin. Blurs the Env/Ambient boundary.
3. **`ExprFromObject::eval`'s nFunction case detects "TCA inner is
   a standin's primop"** and returns the primop directly rather
   than constructing `<cached-fn>(TCA)`. Symmetric to existing
   nFunction sub-cases for OuterObject and ReplayCallbackArg. The
   smallest of the three.

All three preserve the recording-side use of TracingCallbackArg —
cold recording still wraps `argObj` so the writer captures probes —
the change is purely walker-side, gating the wrap or its
consequences on whether we're dispatching a standin's primop.

## What NOT to reimplement

Rejected in earlier iterations and documented here as reference:

- **"Offline Ambient result chain walk"** in both re-entry and
  non-re-entry paths of `dispatchApplyLive`. Violates the walker's
  lockstep-with-`cur` discipline — the standin's chain cursor
  stops short of the terminal because the primop is bypassed, and
  computing the terminal offline papers over that without fixing
  it.
- **Supplementary-objects stack on the writer** for inner
  contraArg state hash resolution via try-every-step iteration.
  Adds a parallel lookup mechanism rather than fixing the proxy
  chain to extend properly. Same shape of papering-over.
- **`dispatchApplyLive` throw → nullopt conversions** and
  `ReplayCallbackArg::defeatCache` delegations to handle cascading
  fall-throughs at outer-change. Local patches to symptoms; don't
  address the cascade's root cause, which is the standin escape
  through the TracingCallbackArg wrapping.
