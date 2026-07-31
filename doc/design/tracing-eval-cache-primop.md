# `builtins.cache` primop design

Design doc for the `builtins.cache` primop. Companion to
[`tracing-eval-cache.md`](./tracing-eval-cache.md) (the base cache
model),
[`tracing-eval-cache-vocabulary.md`](./tracing-eval-cache-vocabulary.md)
(term glossary), and
[`tracing-cache-callback-model.md`](./tracing-cache-callback-model.md)
(callback tracking). Terminology from those docs — Query/Result
vs Request/Response, FactSet, RequestSet trie, Query and Env
message pairings, Selector chains, cells, the walker,
`SelectorCallbackApply` and its ObservationSet — is assumed below.

## Goal and non-goals

`builtins.cache` evaluates a Nix expression in a separate inner
`EvalState` whose tracing decision graph persists across CLI
invocations. Practical purpose: a project that imports Nixpkgs can
wrap the `import` in `builtins.cache { import = ./nixpkgs; }`, so
re-evaluating the project does not re-evaluate Nixpkgs when the
Nixpkgs source is unchanged. The primop is gated by the
`tracing-eval-cache` experimental feature, independent of
`--option tracing-eval-cache true` (which gates the CLI-level
cache stack on the outer evaluator).

The two compose. With both enabled, the outer expression sits
behind a `TracingReplayEvaluator` and so does each `builtins.cache`
call.

Feature coverage:

- `import` and `expr` + `baseDir` forms.
- Function values crossing the cache boundary.
- Covariant callbacks (inner-supplied lambdas that the outer runs).
- Multiple cache calls in one expression, nested `builtins.cache`.
- Laziness through attrset returns.
- Cache invalidation on file edits.

Out of scope:

- **Identity preservation across the boundary.** The cache
  reconstructs values on each crossing, so attrset / list
  equalities that rely on function-pointer identity reaching both
  sides of `==` (Nix's pointer-comparison-inside-structures wart)
  can flip when one side passes through the cache. Both directions
  of crossing are on-demand — observations get indexed lazily as
  the inner makes them; bridged values get materialised lazily as
  the outer accesses them — so threading a large value through and
  asking for it back pays the cost twice but only for what's
  actually touched. Users who need pointer identity through
  caching need a different tool.
- Wiring `nix-env -qa` through the cache.
- Cache eviction / compaction.
- Plugin interaction.
- `TeeTracingWriter` replacing the per-call `NullTraceSink`
  workaround.

## Tracing model: input tracing over the outer↔inner boundary

Of the two evaluator-evaluator interaction models defined in the
[vocabulary](./tracing-eval-cache-vocabulary.md), `builtins.cache`
uses **input tracing** for its inner evaluator, not content tracing.

From a high-level perspective, with all evaluation taken together,
all environment-level inputs need to be tracked and checked
regardless of tracing model. Content tracing could achieve
early cut-off (as in Build Systems à la Carte), and while we
believe this could be moderately useful, it requires value-level
interactions to be traced not just by the inner evaluator's Query
layer but also by the outer evaluator's Env layer, each time the
outer produces a trace. Input tracing avoids that per-outer-trace
cost.

## Anatomy: the pieces the primop is made of

The primop's body is thin — most of the machinery lives in shared
libexpr components that the primop wires together. Reading tour:

- `src/libexpr/include/nix/expr/outer-object.hh` — `OuterObject`
  wraps an outer-owned value behind outer-query callbacks. Each
  Object method (`maybeGetAttr`, `getString`, `getInt`, …) issues a
  `trace::SelectorVariant` via the `OuterQueryFn` and interprets the
  `trace::ResultVariant` it gets back. `queryApply` covers the
  function-application case via `OuterApplyFn`.
- `src/libexpr/include/nix/expr/expr-from-object.hh` —
  `ExprFromObject` bridges an inner `ref<Object>` into an outer
  `Value` and creates `<cached-fn>` PrimOps for `nFunction` values.
  `ExprFromObjectAttr` is the lazy thunk used for attrset children.
  `makeOuterResolver(outerState, replayEval, writer)` constructs
  the shared `OuterResolver`.
- `src/libexpr/include/nix/expr/interpreter.hh` — `Interpreter`
  carries `std::shared_ptr<OuterResolver> outerResolver`, the slot
  the primop sets before delegating evaluation.
- `src/libexpr/include/nix/expr/tracing-environment.hh` —
  `TracingEnvironment::outerQuery` calls `resolve` to compute a
  result and then calls `writer.logOuterObservation(...)`.
  Outer-value queries record as Env-layer Facts on the inner writer.
- `src/libexpr/include/nix/expr/tracing-writer.hh` — `TracingWriter`
  records Facts uniformly at the Env layer: file reads, env-var
  lookups, and outer-value probes all land in the attribution
  cell's facts (the session-root cell for env facts, or the
  probed arg's own cell for outer-value probes) and get inserted
  into `sessionRequestsTrie`. Observations the outer makes on
  inner-supplied callback args during a callback application
  accumulate in the enclosing callback application cell's running
  observation set.
- `src/libexpr/tracing-callback-arg.cc` / `replay-callback-arg.cc` —
  `TracingCallbackArg` and `ReplayCallbackArg` handle the covariant
  callback case: the writer records the outer's probes on an
  inner-supplied callback arg into the cell's obsSet; at replay
  the arg is reconstructed from the obsSet carried inside the
  recorded `SelectorCallbackApply` request.
- `src/libexpr/tracing-replay-evaluator.cc` — `dispatchQueryRequest`
  (name is legacy) routes recorded outer-value and
  `SelectorCallbackApply` Requests through the walker, resolving
  each Request's target via the current cell chain
  (`resolveIdentity` — matches recorded Q hashes against live
  proxies' `getSelectorHashHex`).
- The functional test `tests/functional/builtins-cache.sh` covers
  the full feature surface: covariant callbacks, ambient paths, the
  `callPackageWith` self-referential pattern, function-result
  laziness, and `_NIX_DISALLOW_PARSE` replay-completeness checks.

## Architecture

Inside `prim_cache`:

1. **Parse args.** `{import|expr, baseDir}`. Reject unknown attrs,
   enforce mutual exclusion, require `baseDir` with `expr`.

2. **Resolve the `TracingDecisionGraph`.** Two cases:

   - The outer `EvalCommand` has one: `state.rootDecisionGraph` is
     set. Share it — both inner recordings and the outer cache
     write to the same SQLite file via the same in-process
     connection.
   - No outer: lazily create `state.cacheState.ownedDecisionGraph`
     on first call, reuse across subsequent calls in the same
     process.

   For **nested `builtins.cache`**, the same pointer must propagate
   through to the inner `EvalState` so a nested call inside the
   inner evaluator finds and shares the same graph. The primop sets
   `innerState->rootDecisionGraph = decisionGraph` after
   construction; without this a nested call sees `nullptr` and
   constructs its own in-memory graph, and nested cache hits are
   lost across CLI invocations.

3. **Build the per-call tracing stack.**

   - `TraceSink`: `NullTraceSink` by default, `TraceFile` when
     `NIX_TRACE_CACHE_DIR` is set (for debugging).
   - `TracingWriter(*sink, decisionGraph)`.
   - `TracingEnvironment(state.environment, *writer)` — wraps the
     *outer* environment (not a fresh `SystemEnvironment`); this is
     the "ambient capability fix" and is mandatory for nested
     `builtins.cache` correctness. File reads from the inner
     evaluator bubble up through the outer accessor chain.
   - `EvalState(LookupPath{}, state.fetchSettings, state.settings,
     tracingEnv, state.systemEnvironment, state.getSymbolTable())`.
     The shared `SymbolTable` is required so symbols interned
     during inner parse compare equal to outer state's symbols.
   - `Interpreter(innerState)`.
   - `TracingEvaluator(*writer, interpreter)` wraps `Interpreter`.
   - `TracingReplayEvaluator(recordingEval, *state.environment,
     *writer, *decisionGraph)` wraps the recording evaluator, so
     misses fall through to fresh recording.

4. **Set up the shared `OuterResolver`.**
   `makeOuterResolver(&state, replayEval, writer.get())` and
   assign to `interpreter->outerResolver`. The writer arg is what
   the callback-arg objects use to wrap covariant-callback args;
   `nullptr` would keep the wrap off. The same resolver instance
   threads through every `<cached-fn>` PrimOp created by
   `ExprFromObject` inside this call.

5. **Rewrite paths to the inner accessor.** `RootedPath{innerState->
   rootFSRoot, p.path}` for both `importPath` and `baseDir`.

6. **Evaluate.** `importPath ? replayEval->evalFile(...) :
   replayEval->evalExpr(...)`. Replay tries the cache first;
   falls back to recording.

7. **Bridge back.** `ExprFromObject(result.get_ptr(), replayEval,
   resolver).eval(state, state.baseEnv, v)`. Children become lazy
   `ExprFromObjectAttr` thunks. For `nFunction` values,
   `ExprFromObject` creates a `<cached-fn>` PrimOp that routes
   future applications back through the `OuterResolver`.

8. **Retain state.** Push a `CacheState::CallState` onto
   `state.cacheState.calls` holding the sink, writer, evaluators,
   and innerState. Keeps them alive past the primop frame so the
   lazy `ExprFromObjectAttr` thunks can fire later.

## Recording semantics

The inner evaluator's `TracingEvaluator` records the cached
expression's selectorHash exactly once per fresh evaluation. Which selectorHash depends
on which path forced the value:

- `replayEval->evalFile(...)` / `evalExpr(...)` records
  `SelectorImport` or `SelectorExpr` at the inner root.
- The first time an outer caller forces a `<cached-fn>` PrimOp with
  a new argument, the PrimOp routes to
  `innerEvaluator->apply(fnObj, outerArgObj)`, which records
  `SelectorApply{fn, arg}` at the inner level.

Each recording produces its own `(selectorHash, factSet, result)` row via
`decisionGraph.record(...)`. The `factSet` contains:

- File reads from the inner evaluator (via
  `TracingEnvironment::getFileHash`).
- Env-var lookups (`getEnv`).
- Outer-value queries against outer-provided values, recorded via
  `TracingEnvironment::outerQuery` / `writer.logOuterObservation`.

Because the outer environment is wrapped by the inner
`TracingEnvironment`, *file reads from the inner evaluator also flow
upward through the outer cache's `TracingEnvironment`* — the
"outer-capability fix" (historically called "ambient capability
fix"). An outer recording that calls `builtins.cache` automatically
has the nested call's file dependencies in its FactSet; no special
handling required at that level.

For inner correctness, the outer-value Facts are load-bearing:

- A different outer value with the same fn/arg identifiers would
  otherwise pass the cache check despite producing different
  observed Responses. Recording `(SelectorGetAttr{from=parent}, …)`
  Facts ties the recorded Result to the *observed* shape/values of
  the argument, not just to the input identifiers.
- A different argument with the same structural answers still
  legitimately hits the cache — the point is to validate
  *responses*, not identities.

This recording behaviour falls out of the existing infrastructure.
No new `TracingWriter` methods are needed.

## Replay semantics

When `replayEval->evalFile(...)` (or `evalExpr`, or `apply`) runs:

1. `lookup(selectorHash)` calls `walk(selectorHash)`.
2. Trace-continuing attempt (task #106): if the walker's session
   state already reaches this Query's entry point, walk the trace
   chain from there. On hit, return the Result.
3. Otherwise trace-discovering: start at ∅ or a structural parent
   anchor and walk landing chains and the trace chain per
   [`tracing-eval-cache.md`](./tracing-eval-cache.md)'s replay
   strategies section.
4. The `dispatch` callback in `TracingReplayEvaluator` reads each
   Request payload, calls `getCurrentResponse`, and returns the
   response hash. For Requests whose payload contains a Selector,
   dispatch routes to `dispatchQueryRequest`, which:
   - resolves the Selector's target Object via `resolveIdentity`
     against the current cell chain — each wrapper along the
     chain exposes its identity as a Q hash (`getSelectorHashHex`),
     matched against the recorded Q hash;
   - issues the query against the resolved outer Object;
   - serialises the result.
5. On a hit, the result payload comes back; `lookup` wraps it in a
   `TracingReplayObject`. The outer caller forces attrs/strings/…
   via that TracingReplayObject, which defers to its own
   `lookupResult<selectorHash, R>` per-method (using the recorded
   selectorHash as the parent in child Queries' Merkle chain).

Covariant callback replay uses the callback-arg proxies —
`ReplayCallbackArg` is the frozen image reconstructed from the
observation set carried inside the recorded `SelectorCallbackApply`
request (see the callback-tracking model doc). When the outer's
callback body probes the inner-supplied arg, `ReplayCallbackArg`
serves the recorded response from the obsSet instead of dispatching
live.

**The callback-arg-lambda primop must fire when the outer applies
it.** A `ReplayCallbackArg` for an inner-supplied lambda
materialises via `toValueOrProxy` as a primop `Value`. Its `impl`
looks up the recorded response and either reproduces the result or
throws divergence. Three sites cooperate to keep that primop
reachable through the wrapping chain:

- The walker's callbackApply-dispatch path invokes the apply at
  Object level (`fnObj->queryApply(replayLocal)`) rather than
  constructing an Interpreter-level `mkApp` — the latter loses the
  `ReplayCallbackArg`'s Object identity through the Value wrapper.
- `runOn` skips the `TracingCallbackArg` wrap when `argObj` is
  already a `ReplayCallbackArg`. The wrap's purpose is recording
  outer's probes on inner-supplied locals at cold; a
  `ReplayCallbackArg` at warm already encapsulates the recorded
  contract, and re-wrapping it would route the outer's `g arg`
  through `<cached-fn>(TracingCallbackArg)` — bypassing the
  `ReplayCallbackArg`'s primop.
- `ExprFromObject::eval`'s `nFunction` case detects
  `ReplayCallbackArg` and returns its primop `Value` directly.

Without any one of these, the primop never fires and the walker
either serves a wrong callback result (warm-replay bug) or
cascades into repeated fresh re-evaluations (outer-change bug).

### Outer responses are capability-mediated, not cached

The decisive design point, easy to get wrong: every outer-value
response must be **live-validated**, the same way file reads and
env vars
are. Serving a recorded response from a Responses pool would let
the dispatcher always return the recorded hash, which matches the
recorded hash by construction, and the walk would succeed every
time regardless of whether the outer's behaviour still produces
that response. That's how caching a stale `f x = 11` when the outer
`f` has been edited to `x: x + 100` would silently pass.

The validation surface decomposes by the Selector kind carried
in the Request payload:

- **`SelectorArg{depth}`.** The primop's apply setup binds the
  live `OuterObject` at the corresponding cell. Live dispatch
  calls the method on that OuterObject, which forwards through
  the resolver to the live outer.
- **`SelectorGetAttr` / `SelectorGetListElem`.** The walker
  resolves the parent through the cell chain (matching the
  Selector's `from` Q hash against `getSelectorHashHex` on live
  wrappers), re-dispatches the producer query against it, and
  gets the same derived `OuterObject`. The dispatched method
  then goes live.
- **`SelectorApply`.** The walker resolves fn through the chain
  above and invokes `fn->queryApply(arg)` live. The arg is
  either chain-resolved (relay case) or served by a
  `ReplayCallbackArg` reading recorded content from the
  `SelectorCallbackApply`'s referenced observation set (local
  case). If the outer fn's behaviour changed, the live response
  differs from the recorded one, the walk's response-hash
  compare fails, and the cache correctly misses.
- **`SelectorImport` / `SelectorExpr`.** Re-reads the source
  (file/expression) and resolves the value fresh.

`ReplayCallbackArg` reads payloads from the recorded
`SelectorCallbackApply`'s observation set because the inner isn't
running on replay — there's no live source for the callback-arg's
content. That payload is the *content* of the frozen image; it's
not the dispatcher's response. The dispatcher computes the response
by calling `ReplayCallbackArg`'s method (`.getInt()`, etc.) and
serialising the answer.

A successful replay lets the recording-side writer state stay
coherent — the writer's session-root cell facts,
`sessionRequestsTrie`, and dispatch memo grow along with any
fallback runs, so a *later* miss on a different selectorHash in
the same session lands in a coherent recording chain.

### Live validation generalises; trace scope follows

Live validation of outer-value responses (above) is the specific
case of a broader property: every observation the cache walks
re-dispatches live at replay. Env-layer probes (OuterValueRequests
where the inner probes the outer via `OuterObject`, file reads,
env-var lookups) all re-dispatch live at replay. The recording
stores *what* was observed; each observation's validity depends on
the live environment producing the same response now.

For observations whose live re-dispatch invokes an outer callback (the
result of applying an outer-supplied function, and any subsequent
Env-layer probe that reaches into the apply-result through
`OuterObject`), the validation cost is one outer callback invocation
per observation. That's the cost the cache is designed to amortise
across warm replays, not multiply.

Consequence for trace scope: **the cache supports same-trace hits.**
Replay's trace at the point it reaches a recorded Terminal matches
the writer's trace at the point it recorded that Terminal, each
observation gets one live re-dispatch, factSetHash trajectory matches,
the Terminal hits.

A replay whose accumulated trace is a *subset* of a recording cannot
cheaply match that recording — acquiring the missing facts requires
new Env requests, and for observations that were originally callback
results, that means invoking outer callbacks the user never asked
for at those points in the evaluation. Those unprompted invocations
surface as user-facing logs, errors, and other observable outer
behaviour the user cannot correlate with the expression they wrote.
The cache is meant to be invisible; unprompted outer evaluations
aren't.

Cross-invocation cache reuse across independent recordings works when
replay's trace reproduces the recording's trace (or a superset — the
recording's facts are all present and just need finding and
validating).

## Lifetime and ownership

Two cases:

**Process-shared `TracingDecisionGraph`.** When the outer
`EvalCommand` has one, the same pointer is shared. SQLite WAL mode
gives correct concurrent behaviour even though both stacks are
writing through the same handle. The handle outlives the primop
call because `EvalCommand` owns it for the lifetime of the
evaluation.

**Owned `TracingDecisionGraph`.** When no outer is present (e.g. a
bare `nix eval` without `--option tracing-eval-cache true`), the
first `builtins.cache` call creates one. `CacheState`'s destructor
runs at `EvalState` teardown, which is after the last cached thunk
could possibly be forced.

Per-call state goes on `EvalState::cacheState.calls` as a
`CallState`. The destruction order is: `innerState` first (because
`TracingEnvironment` references the writer), then writer, then sink.
The `EvalState` itself holds a `unique_ptr<CacheState>`; `CacheState`
keeps everything else alive.

## Open questions and known risks

Items that were closed by the shipped implementation
(cross-process concurrent recording, `SelectorApply` semantics,
positional-queue replacement, apply-selectorHash teardown) are no longer
listed. What remains:

1. **Positional-depth collisions across writers, and the general
    fallback.** `SelectorArg{depth}`'s content hash is a pure
    function of `depth` — the same positional value hashes
    identically across every resolver that shares the
    `decisionGraph`. Request payloads referring to the same
    `SelectorArg{depth}` (or a `SelectorGetAttr` / `SelectorApply`
    that bottoms out at such an Arg) hash identically across
    writers, which risks pairing them to different live Objects
    at replay depending on which resolver dispatches.

    Correctness in the worst case falls out of "wrong response
    hash → walk falls through," but this is a source of spurious
    replay misses and, if responses happen to match, silent
    wrong hits.

    **Observed in practice (nixpkgs).** A flake that caches the
    nixpkgs *function* — `cachedNixpkgs = builtins.cache { import
    = nixpkgs; }` — and applies it to a config attrset containing
    a `rewriteURL` callback: cold record produces the right
    answer, warm replay too, but a probe on the apply-result
    falls through to inner re-evaluation because a
    `getStringWithContext` Selector received different live
    responses on the replay and record sides. Its `from` chain
    bottoms out at a positional depth, and nixpkgs's lazy-forcing
    order shifts enough between record and replay that the same
    depth ends up labelling a different attrset element in each.

    Mitigations, in roughly increasing scope: tighten dispatch
    validity checks so any wrong-Object resolution reliably fails
    (no false hits); namespace positional depth so trace-side and
    interpreter-side handles can't collide; or, the general
    answer, **observation-driven unification** — treat identities
    minted by different sources as implicitly namespaced, then
    build an equality table from recorded Facts. Each Fact
    referencing a positional identity is an *observation* that
    constrains the table; the replayer accepts a hit when every
    recorded observation matches a live one. (Roughly the inverse
    of Hindley-Milner: identities start equal-to-anything and
    observations narrow them.)

2. **Retired.** The `outerValues` map is gone — `OuterObject`'s
    closures answer their own queries directly via `queryFn` /
    `applyFn`, which close over the resolver and the outer
    Object. No id map round-trip remains.

3. **`Interpreter::apply`'s arg handling is a try/catch.**
   `arg->defeatCache()` for concrete Objects; on throw (the
   `OuterObject` case), manually wrap via `mkThunk(ExprFromObject(
   arg, nullptr, outerResolver))`. A new virtual — say
   `toValueOrProxy(EvalState &)` — could absorb the fallback and
   let each subclass pick its representation. Deferred; would
   ripple through the Object interface.

4. **Trace coverage: do both evaluators observe all relevant
   changes?** Any outer-provided value the inner uses must flow
   through `OuterObject` (so each inspection logs a Fact), and any
   input the user can change must flow through the outer
   `TracingEnvironment` or appear in `Q_expr`. Both hold by
   construction today, but plugin primops and any native path that
   bypasses `TracingEnvironment` would be gaps to watch.

5. **Storage-layer leverage.** The base cache's design goals — no
   linear search, no unbounded backtracking, session-cumulative
   work proportional to observed change — apply to the primop's
   new touchpoints too. `resolveIdentity` memoises within a walk;
   the apply-Request dispatcher invokes the outer apply once and
   reuses the result across all child-query dispatches. Audit each
   new touchpoint against these goals before landing.

## Future work

- **`TeeTracingWriter`** to replace the `NullTraceSink` placeholder
  inside the primop.
- **Shared in-memory AST cache** across inner evaluators so each
  `builtins.cache` call doesn't re-parse from scratch (previously
  blocked by SourcePath/accessor binding into the AST).
- **Deduplicating Environment layer** for overlapping file reads
  across cache calls.
- **Restore the `nix eval-cache` introspection subcommand** to help
  debug getter Selector chains (`SelectorGetAttr` /
  `SelectorGetListElem` produce-child paths).
- **Interaction-traced outer→inner nesting** as an alternative to
  the current input-traced nesting (so the outer cache treats the
  inner as an oracle and benefits from per-method early cutoff).
  Change in cost model, not correctness; wait for numbers.
- **Observation-driven unification** — the general fallback for
  cross-invocation positional-depth drift (see open question 1).
  When two evaluations produce semantically identical ambient
  values via different apply-boundary sequences, the same
  positional depth labels different values and cross-invocation
  selectorHashes drift even though the values match. A unification
  algorithm at replay time would let the cache hit anyway. Caught
  case: unconditionally lazy functions (`\arg: e` where `e`'s
  evaluation never reaches into `arg`) — such a function is
  constant in `arg` under referential transparency, and a
  subsequent call with any argument gets the recorded result.
  Cost may be substantial; wait for a workload that justifies it.
- **CLI-specific complement: named hints.** The CLI could
  stabilise positional-depth identity by supplying a semantic
  hint string at cache-call registration (modelled on the
  unpinned-fetch URL hint used for lazy-paths source-root
  identity) — letting identities survive apply-boundary
  reorderings without paying for replay-time unification.
  General hints aren't feasible (no method-argument metadata to
  conjure hints for arbitrary Values); CLI-only. Mentioned for
  completeness — the current apply-boundary sequence is stable
  enough that this doesn't need building.
