# Next session: close cb-sibling-b-depends-on-a (v3)

Baseline: **29 pass, 1 fail (cb-sibling-b-depends-on-a)** across
cb-* + builtins-cache.

## SUPERSEDED: Direction C from v2 is invalid

The v2 prompt file (`next-session-prompt-cb-sibling-b-v2.md`)
recommended sampling `fnObj->getInheritedScope()` or the callerScope
cell chain's inheritedScope inside `AmbientApply::runOn`, with the
premise that the CallScopeGuard leaves siblingScope on the
AmbientObject at construction time.

**That premise is empirically wrong.** See
`memory/cb-sibling-b-direction-c-invalid.md` for details. Summary:
`contraArg->withInheritedScope(callScope)` in `makeCachedFnPrimOp.impl`
samples `resolver->callScope` BEFORE `TracingEvaluator::apply`'s guard
ever fires. The guard sets siblingScope transiently only during
`inner->apply()` in `tracing-evaluator.cc:474` — by the time
`AmbientApply::run` fires (later, during outer forcing of the apply
result thunk), the guard destructor has restored callScope. So
`contraArg->inheritedScope == effectiveCallScope` (the cache-import
hash — same across siblings), and every "sample scope live" variant
returns the same value.

Do NOT re-attempt Direction C or its variants. Direction B (no
guard restore) regressed 18 cb-* tests last session — don't retry
that either.

## Step 1 — Required reading (do NOT skip)

Read fully, in this order:

1. `doc/design/tracing-eval-cache.md`
2. `doc/design/tracing-eval-cache-content-identity-via-asks.md` —
   principles 3, 6, 8, and the "Navigation invariant"
3. `doc/design/tracing-eval-cache-primop.md`
4. `doc/design/tracing-eval-cache-per-arg-completion.md` — the
   "wider edit" this issue lives in
5. `/home/sandbox/nix/CLAUDE.md`
6. Memory index (especially the four cb-sibling-b entries and
   `cb-sibling-b-direction-c-invalid.md` — the latest)

## Step 2 — Current empirical state (verified 2026-07-02)

At cold recording, sibling A and sibling B DO get distinct Qs for
`.whatever` getAttr:

- Q=0b4a583f6be4 from=7664ebb776 (sibling A; evolvedQueryFrom at
  applyContext with 1 obs)
- Q=91c35230aa6e from=27a6d20c6aaf (sibling B; evolvedQueryFrom at
  applyContext with 2 obs)

Warm walker's `TracingReplayObject::evolvedQueryFrom`
(`tracing-replay-object.cc:38-79`) computes these correctly, so
`getAttr .whatever` lookups do reach the right Q. Walk of
Q=91c35230aa6e SUCCEEDS at warm — reaches TERMINAL at
cur=800db8b8fb66 (sibling B's cold factSet).

**The failure is downstream.** After sibling B's `.whatever` getAttr
succeeds, the next lookup is Q=5b16d671c5ac
(getWHNF on `.whatever` result). Its walk at warm:

```
walk Q=5b16d671c5ac cur=000000000000..5d1752342501 (5 shared edges)
walk Q=5b16d671c5ac cur=5d1752342501 outgoing=1
walk Q=5b16d671c5ac rs=8beda43bccf9 useful=19 nextCur=daec9c53d689 NO RECORDED EDGE -> try next
walk Q=5b16d671c5ac NO EDGE COMMITTED at cur=5d1752342501 -> miss
```

The walker reaches cur=5d1752342501 (which is sibling A's factSet
terminal, NOT sibling B's), then can't advance. Cold recorded this
Q at factSet=8f1a44f1907e with 12 Asks edges — the walk stopped 5
edges in.

Under sibling B's currentProxy, the walk dispatches requests and
gets responses that XOR-fold to a cur cold never recorded.

## Step 3 — The real bug

`resolveCdiId` at `tracing-replay-evaluator.cc:508` walks
`ctx.currentProxy.argScope` cell chain and, for each cell, tries every
walk-index `k=0..extendedWalkForMatch.size()` looking for a
scope-state-id match. This is a **linear search** that the user
explicitly called out as anti-pattern:

> "CDI must be coherent with its context, e.g. the initial state of
> an Asks node, when an Asks node is the context. Avoid linear
> searches."

Under sibling B's currentProxy, sibling A's cell CAN match target
CDIs via progressive cross-Q pool pull (accumulating extra pulled
edges until the fold reaches the target). The result: routing goes
through sibling A's live proxy for facts cold stamped in sibling B's
context.

The XOR-coincidence guard (`resolveCdiId` line 587) tries to reject
mismatches via canonical LRM probe, but doesn't catch all cases —
either because `ctx.inCrossQPull` is already true (guard skipped),
or because the canonical probe happens to return matching bytes.

## Step 4 — Principled fix direction

Two intertwined problems (per
`tracing-eval-cache-per-arg-completion.md`'s
"Cold/warm flush-pattern asymmetry" section):

1. **Walker's cidasksWalk grows differently from writer's
   d1CidasksWalk.** Cold's writer flushes at every `logResult` +
   markApplyBoundary. Warm's walker only advances on Asks-edge
   commit + suppressed-boundary hooks. Count divergence is baked
   in. `scopeStateIdAt(subject, scope, walk, K)` is walk-composition-
   dependent, so cur→CDI mapping diverges at different Ks.

2. **`resolveCdiId` does linear search over k.** Should compute at
   ONE specific k derived from dispatch context (walker's current
   cur / current Asks-node position), not iterate.

**The wider edit both problems need:** walker fires synthesised
`logResult` equivalents during v13Walk so `walker.cidasksWalk.size`
advances in lockstep with cold's growth pattern. This touches the
writer/walker contract for what a "flush" means and when it fires.
See per-arg-completion.md's final section.

Then `resolveCdiId` can drop the linear search and compute at
`walker.cidasksWalk.size()` (or a projected dispatch-context walk
index). Any cell chain whose CDI at that specific k matches is a
candidate; the cell whose *cur* matches the walker's is the winner.

**Do not shortcut this.** Small piecewise fixes to `resolveCdiId`
(tighter XOR-coincidence guards, extra probe types) will not fix
the underlying misalignment. They just re-shape which sibling
happens to win.

## Step 5 — Working style

- **No Direction C variants.** siblingScope isn't accessible where
  the earlier prompt claimed.
- **Full re-verification.** Any change to writer.d1CidasksWalk
  growth touches most cb-* tests. Run the full cb-* suite after
  each change.
- **Commit incrementally.** Even structural refactors that don't
  land the whole fix should commit if they preserve baseline.
- **Small, principled commits.** Conventional Commits, focus on
  why.

## Step 6 — Committed debug infrastructure

The following debug prints have been committed to help future
diagnosis:

- `AmbientApply::run`: logs argScope, argId, callerScope cell chain
- `resolveCdiId` MATCH log: includes currentProxy, live, liveScope
- `makeCachedFnPrimOp.impl`: logs contraArg, seedCell, callScope,
  outerArg pointer
- `TracingEvaluator::apply`'s `CallScopeGuard SET`: logs sibling
  scope computation

Use them to trace routing decisions concretely. `/tmp/cbdbg/` has
the harness.

## Step 7 — Test scope

Preserve all of:
- All 29 currently-passing cb-* tests
- `builtins-cache` aggregate
- Full `meson test -C build` when the fix looks ready

## Step 8 — Landed context (do not re-implement)

Recent commits: see `git log --oneline eval-cache-v13-primop -20`.
Each is a working improvement. Their commit messages explain
what/why.
