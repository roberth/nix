# Outer-change fall-through for cb-higher-order family

Diagnostic reference for the `cb-higher-order` step-3 and
`cb-higher-order-nested` step-2 failures — both cases where the
outer (cached-call's argument) has changed since the recording,
and the walker's miss-and-fall-through machinery cascaded into
nested cache lookups that re-threw stale-data errors rather than
letting the outer cache miss cleanly and re-run the inner cached
body fresh.

Companion to
[`tracing-eval-cache-higher-order-replay.md`](./tracing-eval-cache-higher-order-replay.md),
which handled warm replay (step-2 of cb-higher-order). Both that
fix and the outer-change variant this memo describes have shipped.

## The failure mode

`cb-higher-order.sh` step 3: after cold + warm-replay populated
the cache for `f = g: g 5`, the outer changes to `f = g: g 10`.
Expected: cache invalidates correctly, returns 11. The visible
error at the time was:

```
… while calling the '<cached-fn>' builtin
… while calling anonymous lambda  ({ f }: f (x: x + 1))
… while calling the '<cached-fn>' builtin
… while calling 'f'  (g: g 10)
… while calling the '<cached-fn>' builtin

error: ReplayCallbackArg::defeatCache: cannot bypass the cache
  on a frozen callback arg — use toValueOrProxy to obtain a primop
  standin
```

Reading the trace, the sequence was:

1. Outer walker walked the outer cb-apply's getType chain:
   dispatched `getInt from=contraArg_g's state hash`, got live
   response 10 vs recorded 5 → divergent `cur` → no recorded
   Terminal → clean walker miss. ✓
2. Replay fallback: `getType` → `ensureInner` triggered fresh
   `Interpreter::apply` on the outer cb-apply. Inner re-parsed the
   cached body. New `openApplyBoundary` entries pushed; new Ambient
   chains computed.
3. Inside the fresh re-eval, outer's body ran `g 10`. `g` was
   bound to `<cached-fn>(TCA)` — the bridged inner-lambda via
   `runOn`. Outer applied the primop to 10. Its impl called
   `innerEval->apply(TCA, contraArg_g10)` — another walk.
4. That nested walk's chain inherited the recorded `envAsksEdges`
   from cold's `logResult` (all Queries at the same `logResult`
   share the per-query chain). Walker dispatched the chain.
5. Chain contained the recorded `(x:x+1) 5` apply Fact whose
   arg state hash was `contraArg_g5`'s structural state hash.
   Dispatch routed to `dispatchApplyLive`.
6. `dispatchApplyLive` looked up the localArg sidecar:

   ```cpp
   auto sidecarPayload = decisionGraph.getRequestPayload(argHash);
   if (!sidecarPayload)
       throw Error("dispatchApplyLive: no localArg sidecar at argHash=%s", ...);
   ```

   For the recursive apply's arg (`contraArg_g5`) there was **no
   sidecar** — sidecars are only written by `runOn` for cb-apply
   boundaries, not by `<cached-fn>.impl` for its own contraArg.
   This wasn't an issue at warm-replay (step 2) because the
   walker only dispatched the outer cb-apply Fact whose arg
   (`innerLam`) *did* have a sidecar. At outer-change, the cascade
   of walks during fresh re-eval made the walker reach this
   no-sidecar apply Fact via the inherited chain.
7. `dispatchApplyLive` threw. The throw propagated out of
   `dispatch`, out of the walk, out of the TracingReplayObject
   method, out of `Interpreter::apply`.
8. The original `<replay-callback-arg-lambda>` primop —
   materialised in some earlier `dispatchApplyLive` setup before
   the throw — was still in a captured closure. Stack unwinding
   accessed the primop's underlying `ReplayCallbackArg` via
   `defeatCache`, which threw the visible error message.

The visible error was the `ReplayCallbackArg::defeatCache` throw;
the precipitating throw was `dispatchApplyLive`'s no-sidecar
assertion.

## Why this was structural, not a bug in any one place

The cb-apply boundary's Env chain — the per-Query `envAsksEdges`
inherited by every Query recorded at the same `logResult` —
contains apply Facts whose args don't have sidecars, specifically
the recursive apply Fact recorded by `logAmbientApplyFact` when
`<cached-fn>(TCA).impl` calls the inner apply. The writer doesn't
write a sidecar for these because the recursive contraArg is an
`OuterObject` constructed by `makeCachedFnPrimOp.impl`, not a
`TracingCallbackArg` constructed by `runOn`.

At warm-replay, the walker doesn't reach the recursive apply Fact
in a dispatch context — it only dispatches the outer boundary's
ε, whose arg (`innerLam`) *has* a sidecar. The recursive apply
Fact lives inside the boundary's Ambient chain (via
`logAmbientApplyFact`'s `enclosing.facts.push_back`) and gets
folded into the Ambient result via the Ambient chain walk. It
never goes through `dispatchApplyLive` directly.

At outer-change, the fresh re-eval cascade puts the walker in a
nested walk inside the fall-through path. The nested walk
dispatches its own Query's chain, which inherits the recorded
chain and encounters the recursive apply Fact **as a direct
dispatch target**, not as part of an enclosing Ambient chain
walk. `dispatchApplyLive` is called; no sidecar; throws.

The structural mismatch: apply Facts valid as inner-chain probes
of an enclosing cb-apply boundary aren't valid as **standalone**
dispatch targets — they lack the sidecar infrastructure needed for
`dispatchApplyLive` to construct a standin.

## Diagnosis: downstream of the warm-replay bypass

The cascade at outer-change had many symptoms: no-sidecar throws
in nested `dispatchApplyLive` calls, `ReplayCallbackArg::defeatCache`
escapes through `Interpreter::apply` chains, fresh re-evals that
themselves triggered more nested walks. Local fixes for each
symptom (`dispatchApplyLive` throw → nullopt, `defeatCache`
delegation to `toValueOrProxy`) were tried and didn't make the
test pass: the cascade kept producing new failure sites as each
individual patch peeled back a layer.

The root cause was the same as
[`tracing-eval-cache-higher-order-replay.md`](./tracing-eval-cache-higher-order-replay.md)'s
diagnosis: **the standin's `<replay-callback-arg-lambda>` primop
was bypassed at warm because its primop value got wrapped in
`TracingCallbackArg` via `runOn`.** At warm-replay (no outer
change) the bypass merely produced a wrong Ambient result that
was being papered over with offline chain walks. At outer-change,
the same bypass triggered a cascade because the standin's primop
value escaped into wrappers that subsequent code paths drove
through `<cached-fn>` → replay-eval-apply → TracingReplayObject
`getType` → fall-through → another `inner.apply` → another
iteration of the same bypass.

Each cascade iteration created a new walk with its own
`ensureInner` closure, captured fresh `(TCA, contraArg)` pairs,
and the TCA wrapped the same standin's primop value. The cascade
had no termination condition other than running out of stack or
having a throw escape between two try/catch scopes.

**No principled fix to the outer-change behaviour existed without
first addressing the warm-replay bypass.** Once the lambda primop
fired at warm, the standin's primop value never got wrapped in
TracingCallbackArg, the cascade had no fuel, and outer-change
fall-through reached the outer cb-apply's `defeatCache → inner
re-parse` path cleanly.

## What NOT to reimplement

Local patches to individual symptom sites:

- `dispatchApplyLive` returns `nullopt` instead of throwing on
  missing sidecar. Local patch to one symptom site.
- `ReplayCallbackArg::defeatCache` delegates to `toValueOrProxy`.
  Local patch to another symptom site.

Both were individually defensible against the principles in
isolation but did not address the cascade's root cause. Documented
here so they aren't reimplemented if a similar cascade pattern
surfaces.
