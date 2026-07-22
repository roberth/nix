# Task #110 Q-evolution status

> User instructions for this file (verbatim):
> "Could you maintain status in a file, e.g. ~/q-evolution-status.md? Don't treat it as a log: only known bugs and model-confirming fix plans. Keep it terse but clear and honest about confidence."



HEAD: `8b7be0720`. Suite: 312/16/7 — matches pre-Phase-3 baseline. No net correctness regressions from the redesign.

## Known bugs

### B1. FIXED — walker per-Q chain (`3b386adb8`)
Walker's `recomputeQ` now reads from a walk-local `perQEnvWalk` fed by `commitEdge`, matching writer's `ActiveQuery::perQEnvWalk` basis. Session envWalk retained for fingerprint dedup and walkScope. Both sides derive the same Q trajectory under matching-until-divergence.

### B2. logResult bridges Ask chain under finalQ from session-cumulative envAsksEdges — MODEL VIOLATION

`tracing-writer.hh` logResult iterates `envAsksEdges` (session-cumulative) and inserts every entry under `finalQ`. Parent Q ends up with every sub-Q's observations duplicated under it — a hack to preserve walker reachability. Violates "each observation attributes to exactly one Q".

**Fix plan (high confidence, requires walker-side change too):** at sub-Q's logResult, insert *one* composite observation into the newly-innermost Q. request = sub-Q's queryHash; response = sub-Q's resultHash; elementHash = XOR(req, resp). Parent's chain then has one entry per sub-Q completion, not per sub-Q observation. Walker's dispatch of this composite request needs to recursively `lookup(subQ)` and return sub's resultHash — a new dispatch branch. Retire the logResult bridging afterward.

### B7. Contra-arg observations not reaching CallbackCell — HIGH (blocks sibling test)

Diagnostic (2026-07-22, cb-sibling-discrimination-via-observation): cold has 4 `createCallbackCell` calls but 0 `logCallbackObservation` calls. Cells' `runningObsSet` stays empty and `argAncestryHex` stays empty → `emitCallbackApplyForApplyResult` returns early → 0 QueryCallbackApply Requests in the DB. The callback body's access to its parameter `x` isn't flowing through `TracingCallbackArg::whnf` / `getInt` / etc. — i.e., the wrappedArg produced by `OuterApply::run` isn't the Object the callback body actually probes when it reads `x`.

**Fix plan (medium confidence — needs diagnosis first):** trace where the callback body's parameter access actually resolves. Candidates: (a) `wrappedArg` is bridged through `ExprFromObject` into a Value thunk; the outer's force of `x` may unwrap back to `argObj` (the raw InterpreterObject) rather than to `wrappedArg` (the TracingCallbackArg); (b) the callback's parameter is resolved through a different path that doesn't traverse TracingCallbackArg at all. Under B7 fixed, `logCallbackObservation` fires, cells populate, QCA emissions fire, and B3/B4/sibling discrimination naturally follow.

### B3. TracingObject lacks general Subject tracking — HIGH (blocks sibling test)

`TracingObject::getSubject()` returns non-null only when `applyResultSubject` is set. All other TracingObjects (arg wrappers, navigation children, evalFile results) return null → their `logQuery` passes nullopt `fromSubject` → their Q doesn't participate in evolution. Q evolution never fires in cold for `cb-sibling-discrimination-via-observation` because of this.

**Fix plan (medium confidence on exact shape):** add general `Subject subject` + `Hash argAncestry` fields to TracingObject. Set at construction:
- root (evalFile): `PostulatedIdempotentRead{file-content-hash}` (leaf, doesn't evolve)
- navigation child of maybeGetAttr/getListElem: `DerivedSubject{parent.subject, .attr}` (evolves iff parent evolves)
- apply result: `ApplyResultSubject{fn, arg}` (already handled as applyResultSubject)

`getSubject()` returns the field. Every TracingObject probe passes fromSubject uniformly.

### B4. Callback observations attribute to wrong Q — HIGH (blocks sibling test)

Callback body observations happen during inner's `evalFile fn.nix` walk. At that moment, the innermost active Q on the writer's stack is the evalFile Q (no fromSubject) or an arg attrset probe (no Subject per B3). The applyResult TracingObject's Q — where these observations should attribute for sibling discrimination — isn't pushed until AFTER callback body returns.

**Fix plan (medium confidence):** structural refactor in `TracingEvaluator::apply` (non-fnIsTlo path). Push an ActiveQuery for the QueryCallbackApply/applyResult *around* the entire `inner->apply(fn, arg)` invocation, not around later probes on the wrapped result. That way callback body's observations attribute to the QueryCallbackApply's Q while its firing is in progress.

### B5. Walker fast path vs slow path Q trajectory mismatch — MEDIUM

Walker's fast path uses session envWalk for `recomputeQ`; slow path uses per-walk envWalk from ∅. Under Q evolution, the two paths would derive different Q trajectories for the same recorded chain. If cold recorded under one basis and warm's other-path retry uses the other, walker misses.

**Fix plan (low confidence — needs design thought):** unify under per-Q per-candidate envWalk everywhere on walker side, matching B1's per-Q on writer side. But this conflicts with the fast-path's session-cumulative-envWalk lockstep design (task #106). Design work needed to reconcile.

**Update (2026-07-22, user clarification on Q27):** fast and slow paths aren't contradictory — they operate on different axes. Fast path is between-Q lockstep succession; slow path writing is within-evolving-Q; slow path replay uses an Ask tree to reach a Q's starting factSetHash. The mechanical bug (walk-local `perQEnvWalk` not reset between fast-path miss and slow-path start — see `tracing-replay-evaluator.cc:64`) is separate from and simpler than the design question. Fix the mechanical bug first; vocabulary refinement can follow.

### B8. TracingCallbackApplyResult mis-routes nested-application observations — HIGH (blocks curried callbacks)

Scope: the curried / returned-closure case where a callback-originated value is itself applied. Not the ordinary contra-arg → cell → QCA-as-arg-observation channel (which is correct and load-bearing).

Concretely: `cb = k: v: k == v`; `c1 = cb "a"`; `c2 = c1 "b"`; `c3 = c1 "a"`. `c2` and `c3` are two separate applications of `c1`. Under the current `TracingCallbackApplyResult` routing, their observations feed back into `c1`'s cell — two different applications writing to the same cell. Ambiguous and contradictory.

**Model** (user, 2026-07-22): each application (including of returned closures) has its OWN cell. Nested applications compose via nested QCA structure: `QCA(QCA(cb, obs_a), obs_b)`. `c1` = `QCA(cb, obs_a)`; applying `c1` to `"b"` produces `QCA(QCA(cb, obs_a), obs_b)`. No cross-cell routing needed. The obsSet content-hash goes directly in each QCA.

The current code "has something quirky that it copied from state hashes, but there's no need for that. Just put the observation set hash in the QCA" (user).

**Fix plan (medium confidence on shape, low on scope of touch):** rework `TracingCallbackApplyResult` so applying a callback-originated value creates a fresh CallbackCell rather than routing into the enclosing one. QCA construction at each application's sample moment then embeds the enclosing QCA as the `fn` position of the outer QCA (nested composition). Writer: cell allocation moves from "one per firing" (whatever that meant) to "one per application" with correct scope. Walker: dispatch of `QCA(QCA(...), obs)` recursively resolves the inner QCA to get `f`'s current state hash, invokes the resolved callable live, backed by a `ReplayCallbackArg` for the outer obsSet. Live callables include both outer-provided fns and inner-produced closures.

## Cosmetic / low-priority

### C4. RESOLVED — no preamble means no recursion (C3 side-effect)
The `logOuterObservation` preamble was retired when C3 moved QCA emission to `TracingObject::whnf`. No preamble → no recursive `logOuterObservation` call → no guard needed. Also removed the dead `isSuppressingCbApply()` accessor that had no callers.

### B6. FIXED — SuppressApplyBoundary narrowed to per-queryApply scope (`719a9b2bf`)
Global guard around the entire `walk()` body replaced with per-call guards around each `fnObj->queryApply(...)` invocation in the four walker paths (resolveApplyId, navigatePath's Apply step, callbackApply branch, TracingReplayEvaluator::apply outer-direction branch). Fallback triggered by leaf ops outside those narrow scopes now runs with the guard OFF — legitimate cb-applies during ensureInner activation record their cells normally.

The full architectural split of `OuterApply::run` into recording vs pure-eval variants remains as a longer-term cleanup, but the immediate correctness gap (guard leaking into fallback paths) is closed.

## Recovery notes

- `77db4caf8` reverted Phase 5's `evolvedQueryFrom` switch to envWalk. Phase 5's motivation was correct (session envWalk aligns writer/walker) but its implementation broke `cb-same-shape-collapse` because it lost the "inner-first applyContext preference" on TracingReplayObject. When B3 (universal Subject tracking) lands, Phase 5's switch can be re-attempted uniformly on top.
