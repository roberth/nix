# Outer-change fall-through for cb-higher-order family

Design memo for closing the **step-3** failure of cb-higher-order
and the **step-2** failure of cb-higher-order-nested. Both share
the same root cause: when the outer (cached-call's argument) has
changed since the recording, the walker's miss-and-fall-through
machinery cascades into nested cache lookups that re-throw stale-
data errors rather than letting the OUTER cache miss cleanly and
re-run the inner cached body fresh.

Companion to
[`tracing-eval-cache-higher-order-replay.md`](./tracing-eval-cache-higher-order-replay.md)
which handled warm replay (= step-2 of cb-higher-order). That fix
landed; the open work is the outer-change variant.

## The failure mode

`cb-higher-order.sh` step 3: after cold + warm-replay populated
the cache for `f = g: g 5`, the outer changes to `f = g: g 10`.
Expected: cache invalidates correctly, returns 11.

Live error chain:

```
… while calling the '<cached-fn>' builtin
… while calling anonymous lambda  ({ f }: f (x: x + 1))
… while calling the '<ambient-fn>' builtin
… while calling 'f'  (g: g 10)
… while calling the '<cached-fn>' builtin

error: ReplayLocalObject::defeatCache: cannot bypass the cache
  on a frozen local — use toValueOrProxy to obtain a primop standin
```

Trace evidence (`_NIX_TRACING_CACHE_LOGGING=1`):

1. Outer v13Walk for `Q=4ed6c1c8bdac` (= outer cb-apply's
   getType): walks chain, dispatches `getInt from=contraArg_g.cdi`,
   gets live response 10 vs recorded 5 → divergent cur → no
   recorded Terminal → walker miss. ✓ (clean miss)

2. `replay fallback: getType` → `ensureInner` triggers fresh
   Interpreter.apply on the outer cb-apply. Inner re-parses the
   cached body. New `markApplyBoundary` entries get pushed; new
   d=2 chains computed.

3. Inside the fresh re-eval, outer's body runs `g 10`. `g` is
   bound to `<cached-fn>(TLO)` (= the bridged inner-lambda via
   `runOn`). Outer applies primop to 10. Impl calls
   `innerEval->apply(TLO, contraArg_g10)` = `IR.apply` (=
   **another v13Walk**).

4. That nested v13Walk's chain inherits the recorded `perQAsksEdges`
   from cold's logResult (= same per-Q chain across all Q's at
   the same logResult). Walker dispatches the chain.

5. Chain contains the recorded `(x:x+1) 5` apply Fact (=
   `applyReqHash = 2ae5ce38951569df`, argId = `273d886b158449...`
   = contraArg_g5's structural CDI). Dispatch routes to
   `dispatchApplyLive`.

6. `dispatchApplyLive` line 656 looks up the localArg sidecar:
   ```cpp
   auto sidecarPayload = decisionGraph.getRequestPayload(argHash);
   if (!sidecarPayload)
       throw Error("dispatchApplyLive: no localArg sidecar at argHash=%s", ...);
   ```
   For the recursive apply's arg (= contraArg_g5), there is NO
   sidecar — sidecars are only written by `runOn` for cb-apply
   boundaries, not by `<cached-fn>.impl` for its own contraArg.
   This was never an issue at warm-replay (step 2) because the
   walker only dispatched the OUTER cb-apply Fact (= ε for
   boundary #1, whose arg is `innerLam`, which **does** have a
   sidecar). At outer-change, the cascade of v13Walks during
   fresh re-eval makes the walker reach this no-sidecar apply
   Fact via the inherited chain.

7. `dispatchApplyLive` throws. Throw propagates out of
   `dispatch` (the lambda inside v13Walk), out of v13Walk, out
   of the TracingReplayObject method, out of Interpreter.apply.

8. The original `<replay-local-lambda>` primop (= materialised
   in some earlier `dispatchApplyLive`'s setup before the
   throw) is still in some captured closure. The throw triggers
   stack unwinding that eventually accesses the primop's
   underlying RLO via `defeatCache` (= probably from the OUTER
   tre's `defeatCache` fallback path), which throws:
   `ReplayLocalObject::defeatCache: cannot bypass the cache on
   a frozen local`.

The visible error is the RLO `defeatCache` throw; the precipitating
throw is `dispatchApplyLive`'s no-sidecar assertion.

## Why this is structural, not a bug in any one place

The cb-apply boundary's d=1 chain (= the per-Q `perQAsksEdges`
inherited by every Q recorded at the same logResult) contains
apply Facts whose args don't have sidecars — specifically, the
recursive apply Fact recorded by `logDepth2ApplyFact` when
`<cached-fn>(TLO).impl` calls `IT.apply`. The writer doesn't
write a sidecar for these because the recursive contraArg is
an `AmbientObject` constructed by `makeCachedFnPrimOp.impl`,
not a `TracingLocalObject` constructed by `runOn`.

At warm-replay (step-2), the walker doesn't reach the recursive
apply Fact in a dispatch context — it only dispatches the OUTER
boundary's ε, whose arg (innerLam) HAS a sidecar. The recursive
apply Fact lives inside boundary #1's d=2 chain (via
`logDepth2ApplyFact`'s `enclosing.facts.push_back`) and gets
folded into AmbientResult via the offline AmbientAsks walk. It
never goes through `dispatchApplyLive` directly.

At outer-change (step-3), the fresh re-eval cascade puts the
walker in a NESTED v13Walk inside the fall-through path. The
nested walk dispatches its own Q's chain — which inherits the
recorded chain and thus encounters the recursive apply Fact
**as a direct dispatch target**, not as part of an enclosing
AmbientAsks walk. `dispatchApplyLive` is called; no sidecar;
throws.

The structural mismatch: apply Facts that are valid as inner-
chain probes of an enclosing cb-apply boundary aren't valid as
**standalone** dispatch targets — they lack the sidecar
infrastructure needed for `dispatchApplyLive` to construct a
standin.


## Diagnosis: this is downstream of the warm-replay bypass

The cascade at outer-change has many symptoms (= no-sidecar
throws in nested `dispatchApplyLive` calls, `RLO::defeatCache`
escapes through `Interpreter::apply` chains, fresh re-evals
that themselves trigger more nested v13Walks). Earlier drafts
of this memo proposed local fixes for each symptom (= throw →
nullopt for no-sidecar, `RLO::defeatCache` delegates to
`toValueOrProxy`). Those were implemented and reverted in this
session: they don't make the test pass, because the cascade
keeps producing new failure sites as each individual fix
peels back a layer.

The root cause is the same as
[`tracing-eval-cache-higher-order-replay.md`](./tracing-eval-cache-higher-order-replay.md)'s:
**the standin's `<replay-local-lambda>` primop is bypassed at
warm because the standin's primop value gets wrapped in
`TLO` via `runOn`**. At warm-replay (= no outer change), the
bypass merely produces a wrong AmbientResult that I had been
papering over with offline chain walks. At outer-change, the
same bypass triggers a cascade because the standin's primop
value escapes into wrappers that subsequent code paths drive
through `<cached-fn>` → `IR.apply` → `tre.getType` → fall-
through → another `inner.apply` → another iteration of the
same bypass.

Each cascade iteration creates a new v13Walk with its own
ensureInner closure, captures fresh `(TLO, contraArg)` pairs,
and the TLO wraps the same standin's primop value. The
cascade has no termination condition other than running out
of stack or having a throw escape between two `try/catch` scopes.

**No principled fix to the outer-change behaviour exists
without first addressing the warm-replay bypass.** Once the
lambda primop fires at warm (= per the architectural diagnosis
in the warm-replay memo), the standin's primop value never
gets wrapped in TLO, the cascade has no fuel, and outer-change
fall-through reaches the OUTER cb-apply's `TO.defeatCache →
inner.evalFile` cleanly to re-parse the cached body.

## Status

**Both green.** cb-higher-order step 3 (outer change) and
cb-higher-order-nested step 2 (outer change) pass at HEAD. The
principled fix landed downstream of the warm-replay bypass fix
per the prediction in the previous status: once warm-replay
worked principlefully, outer-change fall-through resolved
without a targeted patch to the cascade sites.

This memo's diagnostic content (= the cascade trace, the
cascade-termination analysis) is retained for historical
context — it explains the shape of the bug that turned green
via the warm-replay work, and would be the starting point if
similar cascade-through-fall-through patterns surface in other
tests.

## What I am NOT proposing

Earlier drafts of this memo proposed:
- **Change 1** — `dispatchApplyLive` returns nullopt instead of
  throwing on missing sidecar. Local patch to one symptom site.
  Implemented and reverted.
- **Change 2** — `RLO::defeatCache` delegates to `toValueOrProxy`.
  Local patch to another symptom site. Implemented and reverted.

Both were individually defensible against the principles in
isolation but did not address the cascade's root cause. The
reverts are documented; the workarounds are what to NOT
re-implement.
