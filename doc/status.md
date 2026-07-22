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

### B7. QCA emission on cb-apply result — HIGH (partial, sibling test still misses)

Original description ("contra-arg observations aren't reaching cells") was wrong. Empirical trace of `cb-sibling-discrimination-via-observation` cold shows `TracingCallbackArg::whnf` fires and `logCallbackObservation` populates the cell (`obsSet=1`, `argAncestryHex` set). The break is at emission.

Mechanism:

- `<cached-fn>.impl` calls `innerEval.apply({f,x}: f x, contraArg)`. `TracingEvaluator::apply` wraps the result in a `TracingObject` with `applyResultSubject={fn=innerFn, arg=argAttrset}` and creates cell #A keyed by `fn=innerFn` state hash.
- During that TracingObject's `whnf` → `computeWHNFFromObject`, the inner body's nested `f x` fires `OuterObject::queryApply` → `OuterApply::run`, which creates cell #B keyed by outer f's state hash and wraps the arg in `TracingCallbackArg`.
- `TracingCallbackArg` records the contra-arg's `whnf` observation into cell #B (routed by `applyId`). Cell #B ends up populated correctly.
- The enclosing `TracingObject::whnf`'s `emitCallbackApplyForApplyResult` scans cells for `fn=innerFn` state hash — finds empty cell #A, skips populated cell #B on fn-hash mismatch. Emits nothing.

**Partial fix landed:** `OuterApply::run` now wraps its result in a `TracingObject` with `applyResultSubject.fn = fnSubject` (the real OuterObject's Subject, threaded through as a new `fnArgAncestry` parameter on `OuterApplyFn`), `applyArgAncestry = fnArgAncestry`. `stateHashAtSubject` at step 0 reproduces `fnStateHash`, matching cell #B's `fnStateHashHex`. The wrapper's `whnf` therefore emits QCA against the right cell.

An earlier iteration used `PostulatedIdempotentRead{fnStateHash}` for the fn root — the PIR docstring explicitly flags this as invalid ("taking an arbitrary subject id by value and using it as if it's an up-to-date id … conflates all possible future states of the argument"), which surfaces as a wrong hit in `cb-two-sibling-distinct-callbacks` where sibling A and B's `f`s share the same initial `fnStateHash` but should evolve to distinct `Q_final` values.

**Still blocks the sibling test:**

1. **Sibling-A's applyResult whnf fires before its contra-arg probe.** `.whatever` on outer forces contra-arg lazily, after applyResult.whnf has returned. At emit time cell #B has empty `argAncestryHex` (no observation yet), and the current emit code skips empty-argAncestry cells. Sibling A's QCA never emitted.
2. **Children of the wrapper don't emit QCA on their own whnf.** `TracingObject::maybeGetAttr` returns children without `applyResultSubject`, so `.whatever`'s whnf doesn't fire `emitCallbackApplyForApplyResult`. Callback-model §7 says each WHNF-producing probe on a cb-originated value emits its own QCA — the child probe should be QCA-2.

Remaining work: (a) propagate `applyResultSubject` (or an equivalent QCA-emit shape) to `maybeGetAttr`/`getListElem` children of the wrapper; (b) allow emission when observations arrive after the wrapper's `whnf` — either lazy re-emit, or accepting empty-argAncestry cells at emit time.

**Further partial fix landed** (2026-07-23): `queryFn` in `makeCachedFnPrimOp.impl` now skips the redundant `innerEnv.outerQuery` when the target is a `TracingObject` with `cbApplyOrigin=true`. Cold was previously recording two overlapping observations for the cb-apply's whnf — a QCA (the design's intended emission) and a generic getWHNF whose `fromStateHashes` referenced `Arg{depth}` (the contra-arg). Warm can't resolve `Arg{depth}` because it has no live cell chain for a contra-arg (the QCA obsSet is the design's contra-arg carrier). The redundant getWHNF caused warm to miss on cases where the QCA alone would have hit.

Suite 313/15/7 (+1 net vs baseline 312/16/7). Newly passing vs baseline: `cb-local-descendants`, `cb-with-scope-and-tryeval`. Newly failing: `cb-two-sibling-distinct-callbacks` — this test's warm was passing via the redundant getWHNF observation folding into `cur` to disambiguate sibling B's Terminal from sibling A's; that mechanism was an accident of the PIR shortcut and the same design gap that made `cb-sibling-discrimination-via-observation` fail — under proper Q evolution siblings should diverge via evolved fn state hashes, but the walker-side counterpart to reconstruct the cb-apply wrapper isn't in place yet, so cold's evolved `from` fields can't be resolved on warm and both siblings fall back.

**Walker-side gap**: `TracingReplayEvaluator::apply` builds a `TracingReplayObject` with `applyResultSubject={fn, arg}` for the inner apply's result, but there's no counterpart for the nested cb-apply that cold's `OuterApply::run` wraps. Cold's stamped `from` fields on the cb-apply chain reference evolved state hashes of `applyResultSubject.fn = fnSubject`, and warm's cell chain has no live Object at those evolved hashes.

**Walker-side partial fix landed** (2026-07-23): `dispatchQueryRequest`'s callbackApply branch resolves fn via `resolveRoots` + `navigatePath` on the QCA payload's `fromStateHashes` + `path`, instead of routing through `resolveStateHash(fnHex)` (which failed to find fnCurrent in the pool). Roots are `Arg{depth}` subjects that resolve via the cell chain's K-iteration; the recorded path navigates live from there. Warm now dispatches QCAs successfully (3 HITs on `cb-sibling-discrimination-via-observation` where 0 fired before).

Suite 315/13/7 (+3 net). Newly passing vs baseline: `cb-forcedness-independence`, `cb-local-descendants`, `cb-stats-sidecar-baseline`, `cb-with-scope-and-tryeval`. Newly failing: `cb-two-sibling-distinct-callbacks` — walker-algorithm interaction (trace-continuing hits sibling A's Terminal at shared startCur before exploring outgoing Asks), not a B7 gap.

The sibling test still misses because a *later* Ask edge in the chain has no recorded continuation at some downstream cur (`Q=... NO EDGE COMMITTED at cur=...` after the QCA hits). That's a separate chain-completeness issue past the QCA dispatch.

**Broader refactor (R3, new): resolveStateHash → resolveSubject.** `resolveStateHash(idStr, ctx)` takes a hex string and guesses (cell-chain first, then pool CAS, then live-proxy). Under design, the caller almost always knows the Subject variant — QCA's `fn` is an `ApplyResultSubject.fn`; getAttr's `from` is a `DerivedSubject` chain; etc. The dual-role property (state hash = producer query hash for `DerivedSubject`) is real math but using it as an implicit routing hint is fragile — the pool has to have exactly the right producer at exactly the right state, and if it doesn't, we can't distinguish "wrong id" from "recording incomplete." The callbackApply migration above is the first step; extending to all callers is the natural continuation. Owner: task #121 (partial).

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

### B5. FIXED — perQEnvWalk rolled back at trace-continuing→trace-discovering transition

Trace-continuing rollback now saves `perQEnvWalk->size()` alongside the other fast-path save-set and `resize()`s on miss, matching the existing envWalk/envCur/fingerprints discipline. Test suite unchanged at 312/16/7 — no visible failures relied on this, but the invariant leak is closed.

### B8. TracingCallbackApplyResult mis-routes nested-application observations — HIGH (blocks curried callbacks)

Scope: the curried / returned-closure case where a callback-originated value is itself applied. Not the ordinary contra-arg → cell → QCA-as-arg-observation channel (which is correct and load-bearing).

Concretely: `cb = k: v: k == v`; `c1 = cb "a"`; `c2 = c1 "b"`; `c3 = c1 "a"`. `c2` and `c3` are two separate applications of `c1`. Under the current `TracingCallbackApplyResult` routing, their observations feed back into `c1`'s cell — two different applications writing to the same cell. Ambiguous and contradictory.

**Model** (user, 2026-07-22): each application (including of returned closures) has its OWN cell. Nested applications compose via nested QCA structure: `QCA(QCA(cb, obs_a), obs_b)`. `c1` = `QCA(cb, obs_a)`; applying `c1` to `"b"` produces `QCA(QCA(cb, obs_a), obs_b)`. No cross-cell routing needed. The obsSet content-hash goes directly in each QCA.

The current code "has something quirky that it copied from state hashes, but there's no need for that. Just put the observation set hash in the QCA" (user).

**Fix plan (medium confidence on shape, low on scope of touch):** rework `TracingCallbackApplyResult` so applying a callback-originated value creates a fresh CallbackCell rather than routing into the enclosing one. QCA construction at each application's sample moment then embeds the enclosing QCA as the `fn` position of the outer QCA (nested composition). Writer: cell allocation moves from "one per firing" (whatever that meant) to "one per application" with correct scope. Walker: dispatch of `QCA(QCA(...), obs)` recursively resolves the inner QCA to get `f`'s current state hash, invokes the resolved callable live, backed by a `ReplayCallbackArg` for the outer obsSet. Live callables include both outer-provided fns and inner-produced closures.

### B9. Walker's TRO `applyContext` is walker-session-scoped, not per-Q-chain-scoped — HIGH (blocks sibling test)

`TracingReplayObject::applyContext` is a `shared_ptr<ApplyContext>` handed to descendants (via `withApplyContextOnly` in `lookupResult` / `lookupStructuralChild`, `tracing-replay-object.cc:211,371`) so every descendant sharing that pointer reads and writes the same observation vector. Cache-hit `pushObservation` calls append into that shared vector. When `evolvedQueryFrom` on any of them folds the vector into `applyResultSubject`'s state hash, it folds in every observation any related descendant pushed anywhere in the walker session.

Cold's writer stamped `from` at the point where it recorded that specific Q's edge — the state hash reflecting observations folded up to that moment in that Q's own recording chain. Warm's TRO folds a larger set (the whole walker-session accumulation into the shared context) and derives a different state hash. `evolvedQueryFrom`'s output disagrees with cold's stamped `from`, so warm's computed queryHash doesn't match cold's stored row and the lookup misses.

**Fix plan (medium confidence):** replace the shared-across-descendants ApplyContext with per-Q-chain state on the walker Object side — the walker's Object analogue of the writer's `ActiveQuery::perQEnvWalk` (task #110 B1 fix). Two shapes to consider: (a) each new Q-lookup opens a fresh, walk-local observation history that `evolvedQueryFrom` reads from and `pushObservation` writes into, discarded when the Q's lookup completes; (b) keep the shared_ptr shape but filter at read time to only the observations recorded within the current Q's chain. (a) is closer to the writer side.

Complements [B3](#b3-tracingobject-lacks-general-subject-tracking--high-blocks-sibling-test) (writer-side Subject exposure): together B3 + B9 align writer and walker on per-Q chain semantics. Absorbed from former task #104.

## Cosmetic / low-priority

### C4. RESOLVED — no preamble means no recursion (C3 side-effect)
The `logOuterObservation` preamble was retired when C3 moved QCA emission to `TracingObject::whnf`. No preamble → no recursive `logOuterObservation` call → no guard needed. Also removed the dead `isSuppressingCbApply()` accessor that had no callers.

### B6. FIXED — SuppressApplyBoundary narrowed to per-queryApply scope (`719a9b2bf`)
Global guard around the entire `walk()` body replaced with per-call guards around each `fnObj->queryApply(...)` invocation in the four walker paths (resolveApplyId, navigatePath's Apply step, callbackApply branch, TracingReplayEvaluator::apply outer-direction branch). Fallback triggered by leaf ops outside those narrow scopes now runs with the guard OFF — legitimate cb-applies during ensureInner activation record their cells normally.

The full architectural split of `OuterApply::run` into recording vs pure-eval variants remains as a longer-term cleanup, but the immediate correctness gap (guard leaking into fallback paths) is closed.

## Risks / architectural follow-ups

### R1. Walker `walk()` assumes only one version of state is needed at a time

`TracingReplayEvaluator::walk` maintains session state (`envWalk`, `envCur`, `fingerprints`) as mutable fields, mutated during a backtracking attempt and reset on miss. That pattern only makes sense if the walker only ever needs one version of that state — the current one — at any given moment.

That assumption is unverified. If a walker path ever needs to hold multiple versions of the state simultaneously — for example, to keep an earlier version reachable during a nested lookup, or to explore candidate traces in parallel — the mutation-restore pattern doesn't fit and existing code would be silently incorrect.

**Direction:** thread walker state through function arguments rather than mutating shared fields. Multiple versions coexist naturally as separate values; backtracking is expressed as not committing changes rather than restoring after changes.

### R2. Contra-arg observations still carry vestigial `from` fields keyed on invariant `Arg{depth}` state hashes

Under the current callback model, contra-arg values are identified by the observations stored in the `QueryCallbackApply`'s referenced `ObservationSet`. The observation set is scoped to a specific callback firing by the outer `QueryCallbackApply` payload (`fn`, `argAncestry`, `argDepth`); observations inside are all probes on the same implicit contra-arg by construction and don't need to name a Subject.

The code still computes `from` fields for contra-arg probes by running `stateHashAfter(Arg{depth}, callArgAncestry, {})` — invariant across sibling firings by design (see `replay-callback-arg.cc:30-38`, which explicitly frames the invariance as a compatibility shim). The invariant state hash contributes no discrimination; it's a leftover from when contra-args were subject-identified.

**Direction:** drop `from` from contra-arg observations inside `ObservationSet`; retire the invariant `Arg{depth}` state hash on both writer and walker sides. `Arg{depth}` stays as a positional Subject variant for `ApplyResultSubject{fn, arg=Arg{d+1}}` composition. If `ApplyResultSubject`'s own state hash formula still needs an arg-side hash, use a plain positional constant (`SHA("positional-<d+1>")`) rather than dressing it up as a "state hash of `Arg{d+1}`" — same bytes, different type-abstraction.

Not correctness-affecting; the shim works. Retiring it removes conceptual clutter (a state hash that never evolves) and a stamping/checking overhead on every contra-arg probe.

## Recovery notes

- `77db4caf8` reverted Phase 5's `evolvedQueryFrom` switch to envWalk. Phase 5's motivation was correct (session envWalk aligns writer/walker) but its implementation broke `cb-same-shape-collapse` because it lost the "inner-first applyContext preference" on TracingReplayObject. When B3 (universal Subject tracking) lands, Phase 5's switch can be re-attempted uniformly on top.
