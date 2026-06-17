# `builtins.cache` on top of the v13 tracing eval cache

This document is a design plan for restoring the `builtins.cache`
primop on the `eval-cache-v13` branch (under the working name
`eval-cache-v13-primop`). It is a companion to
[`tracing-eval-cache.md`](./tracing-eval-cache.md), which describes the
v13 data model the primop will sit on top of.

Read those first. The terminology — Query/Result vs Request/Response,
FactSet, RequestSet trie, the `Asks` / `Terminals` edge tables, and
the `walk(Q, dispatch)` replay primitive — is assumed below.

## Goal and non-goals

`builtins.cache` evaluates a Nix expression in a separate inner
`EvalState` whose tracing trie persists across CLI invocations. Its
practical purpose: a project that imports Nixpkgs can wrap the
`import` in `builtins.cache { import = ./nixpkgs; }`, so that
re-evaluations of the project do not re-evaluate Nixpkgs when its
source files are unchanged. The cache is enabled by the
`tracing-eval-cache` experimental feature flag (which gates the primop
itself) and is independent of `--option tracing-eval-cache true`
(which gates the CLI-level cache stack on the outer evaluator).

The two compose. With both enabled, the outer expression sits behind a
`TracingReplayEvaluator` and so does each `builtins.cache` call.

This document covers:

- restoring the primop with at least the v12 feature coverage
  (`import`, `expr`+`baseDir`, function values, covariant callbacks,
  multiple cache calls in one expression, nested `builtins.cache`,
  laziness through attrset returns, cache invalidation on file edits);
- making the inner evaluator's "d=2 ambient" interactions (the
  function-argument Query/Result conversation between the inner
  evaluator and the outer-provided Object) fit the v13 Fact / FactSet
  algebra rather than v12's temporal afterHash chain;
- sharing a single `TracingDecisionGraph` instance between
  `EvalCommand` and `builtins.cache` when both are wired up in the
  same process.

Out of scope here (deferred follow-ups):

- The future-extensions interface (`lookupPath`, `env`,
  cached-function-call roots as their own cache key).
- Replacing the per-call `NullTraceSink` workaround with a proper
  `TeeTracingWriter`.
- Wiring `nix-env -qa` through the cache (separate piece of work
  called out in the v13 doc).
- Cache eviction / compaction.
- Plugin interaction.

## Status of the in-tree pieces

The v12→v13 migration commit `3db54dcff` stripped the v12 SQLite trie
(`tracing-index.{cc,hh}`, ~1.9k LOC), gutted v12's
`TracingReplayEvaluator::lookup` cascade, and replaced the
`builtins.cache` primop with a stub that throws
`"builtins.cache: not implemented in v13 (d=2 ambient layer pending)"`.

What it kept is most of what the primop actually needs. Reading the
v13 tree:

- `src/libexpr/include/nix/expr/ambient-object.hh` —
  `AmbientObject` wraps an outer Object behind ambient-query callbacks.
  Each Object method (`maybeGetAttr`, `getString`, `getInt`, …) issues
  a `trace::QueryVariant` via the `AmbientQueryFn` and interprets the
  `trace::ResultVariant` it gets back. `queryApply` covers the
  function-application case via `AmbientApplyFn`.
- `src/libexpr/include/nix/expr/expr-from-object.hh` —
  `ExprFromObject` bridges an inner `ref<Object>` into an outer
  `Value` and creates `<cached-fn>` PrimOps (when `innerEvaluator` is
  set) or `<ambient-fn>` PrimOps (when only `ambientResolver` is set)
  for `nFunction` values. `ExprFromObjectAttr` is the lazy thunk used
  for attrset children. `makeAmbientResolver(outerState,
  innerEvaluator)` constructs the shared resolver.
- `src/libexpr/include/nix/expr/interpreter.hh` — `Interpreter` already
  carries `std::shared_ptr<struct AmbientResolver> ambientResolver`,
  the slot the primop sets before delegating evaluation.
- `src/libexpr/include/nix/expr/tracing-environment.hh` —
  `TracingEnvironment::ambientQuery(query, resolve)` calls the supplied
  `resolve` to compute a result and then calls
  `writer.logAmbientInteraction(query, result)`. That's the recording
  side wired up: ambient queries become d>0 Facts in the inner
  `v13FactSet`.
- `src/libexpr/include/nix/expr/tracing-writer.hh` —
  `TracingWriter::logAmbientInteraction(QueryVariant, ResultVariant)`
  XOR-folds the `(queryHash, responseHash)` Fact into
  `v13FactSetHash`, inserts the hashed Request payload into
  `Requests`, and tracks the Request in `allRequestsTrie`. The
  recording machinery does not distinguish ambient interactions from
  file reads at the storage layer — they are just Facts whose
  Request payload happens to deserialise as a `QueryGetAttr` /
  `QueryGetString` / `QueryApply` JSON rather than a
  `FileReadRequest` / `GetEnvRequest`.
- `src/libexpr/tracing-replay-evaluator.cc` — `apply(fn, arg)` still
  sets up an `AmbientReplayState` whose `unresolvedRoots` holds the
  raw Objects for `fn` and `arg` before calling
  `lookup(trace::QueryApply{...})`. `getCurrentResponse` already
  routes Requests whose payload contains `"query"` through
  `dispatchAmbientQuery`, which resolves the recorded `from` id to a
  live Object and re-issues the query against it.
- The functional test `tests/functional/builtins-cache.sh` (603
  lines) still exists in the v13 tree. It covers the full v12 surface
  including covariant callbacks, ambient paths, the
  `callPackageWith` self-referential pattern, function-result
  laziness, and `_NIX_DISALLOW_PARSE` replay-completeness checks. We
  inherit it.

What is missing or wrong, and so must change:

1. `src/libexpr/primops/cache.cc` is a stub. We need to restore its
   body atop the v13 types (`TracingDecisionGraph` instead of
   `TracingIndex`).

2. There is no path from `EvalState` to the EvalCommand-owned
   `TracingDecisionGraph`. `EvalState::cacheState` exists but the
   pointer-to-root that v12 used (`state.rootTracingIndex`) is gone.
   The v13 doc explicitly calls out the EvalCommand-owned graph: we
   need to expose it on `EvalState` so the primop can attach.

3. `TracingReplayEvaluator::dispatchAmbientQuery` uses
   "pendingChildren + unresolvedRoots" positional matching, which
   assumed v12's temporal afterHash ordering of recorded Query/Result
   pairs. v13's FactSet has no temporal order. See
   [§Ambient identity in an unordered model](#ambient-identity-in-an-unordered-model)
   below — the existing v13 code happens to work for the *single*
   `apply()` recording at the CLI root because `dispatch` only fires
   on Facts the walk needs and only ever processes one `apply` per
   `TracingReplayEvaluator` instance at a time, but as soon as
   `builtins.cache`'s cached-function PrimOps push apply boundaries
   inside the inner evaluator, the positional approach starts pairing
   ids by FactSet iteration order rather than by what the recorded
   counter labelled them — wrong in any branching access pattern.

4. The recording-side `TracingWriter` requires a `TraceSink &`
   reference. As in v12, the primop will satisfy it with a
   `NullTraceSink`. Removing this is the deferred `TeeTracingWriter`
   work.

## Mapping the v12 "d=2 ambient layer" onto v13

The v12 doc and the stub commit message both say v13 "does not yet
model the d=2 ambient-query layer that `builtins.cache`'s
explicit-scope semantics require." The phrase is v12 vocabulary, and
understanding the mapping is the entry point to most decisions below.

**v12** had `depth` as a column on every recorded node:

- `depth = 0` — the user query (`Q`) whose result is being recorded.
  In a `builtins.cache` context, the top-level `Q` was either
  `QueryImport(path)`, `QueryExpr(text, baseDir)`, or — for cached
  function calls — `QueryApply{fn, arg}`.
- `depth = 1` — environment events flowing out of the inner evaluator
  while it computed Q's result: file reads, env lookups, *and*
  ambient queries on outer-provided Objects.
- `depth = 2` — the user-doc layer when a recorded "depth=1" event was
  itself the user-query of a downstream recording: i.e. when the
  *outer* evaluator records its own Q, the inner evaluator's
  d=1 ambient queries show up to the outer cache as Q→R events at
  one more level of indirection.

In v12 the inner `TracingReplayEvaluator` could walk the recorded
depth=1 chain by `afterHash` order. The ambient queries appeared in
the chain interleaved with file reads, and `dispatchAmbientQuery`
resolved their `from` ids by stepping a positional registry whose
pop order matched the temporal record order.

**v13 has no temporal order at all.** The recording side keeps a
single per-`Q` `FactSet` — an unordered set of `(RequestHash,
ResponseHash)` pairs. The replay side walks the decision graph from
`(Q, ∅)`, taking edges whose `RequestSet`'s "useful dispatch" is
satisfied by the current environment; at each step it XOR-extends a
running `cur` hash by the per-Fact element hashes; on terminal match
it returns the recorded result.

The mapping is therefore *not* about the trie storage layer — the
v13 schema can already hold ambient Facts unchanged. `logResponse`
and `logAmbientInteraction` in v13's `TracingWriter` are deliberately
symmetric, both XOR-folding the same Fact shape into `v13FactSet`.
The mapping is about three other things:

1. **Dispatch routing during replay**: the walk's `dispatch(reqHash) →
   respHash` callback has to recognise ambient Requests and route
   them through the AmbientResolver instead of the
   filesystem/env-var Environment. v13's
   `TracingReplayEvaluator::getCurrentResponse` already keys off
   `reqJson.contains("query")`, so the route is already there; what
   isn't yet correct is the identity model `dispatchAmbientQuery`
   uses to resolve `from` ids.

2. **Ambient identity in an unordered model.** With a temporal chain
   gone, identifiers attached to ambient Requests cannot mean "the
   `from` id that flowed into position k of the recorded chain." They
   must be structurally derivable from queries the replayer has
   already resolved, plus the seed roots (`fn` and `arg`) provided
   by the apply call that triggered the walk. See
   [§Ambient identity in an unordered model](#ambient-identity-in-an-unordered-model).

3. **The `Q` for cached function calls.** When the outer evaluator
   forces a `<cached-fn>` PrimOp, the call shows up at the inner
   evaluator as `apply(fnObj, argObj)` where `fnObj` is the recorded
   inner function (or its replay-Object) and `argObj` is an
   `AmbientObject` for the outer argument. The Q hash on the inner
   side is `QueryApply{fnId, argId}`. Both ids must be stable across
   sessions for the cache to hit: `fnId` is the recorded inner
   `queryHashStr` for the function value, `argId` is the
   `getOrAllocVirtualRoot` id for the outer Object (process-local on
   recording, repopulated on replay from the live `apply()` arguments
   via lazy unification).

The original v12-era plan's term "interaction tracing" survives intact
under v13. The only thing v13 changes is the underlying storage —
from a temporal afterHash chain to a set algebra. The conversation
records (Outgoing/Incoming) and the validity conditions on them
(replay re-issues outgoing queries, replay serves recorded incoming
answers if/when the outer evaluator asks) are unchanged.

## Ambient identity

This is the design-level question that the v12 stub commit was
flagging when it said "d=2 ambient layer pending." It is the one
substantive thing the primop work has to think through.

### Background: where ids come from

Ambient ids are produced interpreter-side, not by the storage
layer. There are two id namespaces sharing the same `from` /
`argId` string slots in the JSON payloads:

- **Writer-virtual ids** — `"virtual:N"`, allocated by
  `TracingWriter::getOrAllocVirtualRoot(Object*)`. These appear in
  the `fnId` / `argId` of `QueryApply` at the d=0 level when the
  `TracingEvaluator` records an `apply` whose operands have no
  trie identity.
- **Resolver-ambient ids** — plain `"N"` (under the current code)
  or a content hash (under Step C), allocated by
  `AmbientResolver::registerOuter` / `registerLocal`. These appear
  in the `from` field of every d>0 ambient Request.

Seed allocations fire only at apply boundaries — apply forcings
where an outer-side Object becomes an input to an inner evaluator
(or vice versa). Structural-query forcings (`getAttr`, `getInt`, …)
on already-identified Objects don't allocate; under Step C they
just compute the producer query's `queryHash` as the child's id.
The CLI's apply boundaries happen to arise in a stable sequence —
auto-calling top-level functions during nix-build's traversal of
the configuration, passing the few positional arguments to the
initial `call-flake` call, and registering the cached function's
argument in the `<cached-fn>` PrimOp impl — so seed counters are
stable in practice without anything in the design enforcing it.

If that practice ever stopped holding, the general lever is
unification at replay time — match recorded nodes by their
structural children rather than by their recorded id
(Hindley-Milner sense). Applicable to any caller, potentially
expensive. See *Future work*.

### The actual gap

`builtins.cache` is disabled today, so the traces below don't
exist in any real recording. The point of this subsection is to
show what they *would* look like if the primop were restored
against the existing counter-based resolver — and where the
dispatcher would fail.

Take the `call-fn.nix` case from `builtins-cache.sh`:

```nix
(builtins.cache { import = ./call-fn.nix; }) { f = x: x + 1; x = 10; }
```

where `call-fn.nix` is `{ f, x }: f x`. If the primop were
restored as-is, `nix eval` forcing the result would drive the
inner to evaluate the body `f x`, which would force `arg.f` to
know what to apply. The outer lambda's body `x + 1` would then
force `arg.x` (bound to the lambda's `x`), and the addition would
force *that* as an int. Each forcing would fire an ambient query
on the `AmbientObject` wrapping the outer arg, and the resolver's
counter would walk: L0 = seed (the outer attrset, registered at
the apply boundary), L1 = child returned from `getAttr "f"`
(first `registerOuter` call), L2 = child returned from
`getAttr "x"` (second `registerOuter` call). The recorded
`(QueryApply, factSet)` would contain:

```
Q  = QueryApply{fnId=…, argId=L0}
Facts:
  Request(QueryGetAttr{name="f", from=L0})  ─ Response(maybe-lambda)
  Request(QueryGetAttr{name="x", from=L0})  ─ Response(maybe-int)
  Request(QueryGetInt{from=L2})              ─ Response(10)
  Request(QueryApply{fn=L1, arg=L2})         ─ Response(apply)
```

The third Fact's `from="L2"` would reference the value the
recorder labelled L2 — the child of `getAttr "x"`. The dispatcher
would need to map "L2" back to that live child Object to issue
`getInt` against it. But the recorded `from` string would carry
only the label, not the derivation: there is no way to tell from
"L2" alone that it came from `getAttr "x"` on `L0`.

The in-tree recovery mechanism — two FIFO queues
(`unresolvedRoots` for seeds, `pendingChildren` for children
produced by earlier dispatches) — would be broken even if the
walk dispatched in eval order:

```
F1 GetAttr from="L0" name="f"  → would produce child_f, queue = [child_f]
F2 GetAttr from="L0" name="x"  → would produce child_x, queue = [child_f, child_x]
F3 GetInt  from="L2"           → would pop front = child_f       ← should be child_x
```

The FIFO pop would pair unknown ids to children by insertion
order rather than by counter id. For any branching access pattern
the queues would lose the mapping. The walk's canonical-hash
dispatch order makes it worse, but even eval order wouldn't fix
it. The queue handling is effectively dead code for any non-chain
case, which is why the only consumer (`builtins.cache`) is
disabled.

### The fix: producer query as id

Counter ids are deterministic during recording but drop the
producer relationship when written down. The recorder knows that
`L2` is "child of `getAttr "x"` on `L0`" but the recorded
`from="L2"` string doesn't carry that. So we change *what id we
write down*.

A derived ambient value is fully described by its producer query:
`child_x` *is* "the result of `getAttr "x"` on `L0`." That producer
query is already recorded as a Fact, and v13 already
content-addresses it by its `queryHash` in the `Requests` pool. So
use the producer's `queryHash` as the derived value's id. No
counter, no allocation, no separate registry — the `Requests` pool
already keys what we want.

Seed roots still need an identifier (they don't have a producer
query — they're inputs from outside the trace). For now: hash a
short string formed from the seed role and an interpreter-side
counter, e.g. `hashString("seed:0")`, `hashString("seed:1")`. The
counter is stable across invocations to the extent that the CLI's
setup-phase events (above) fire in the same order — true for the
patterns we care about. It is not robust to setup reorderings;
named hints would be the upgrade, but the metadata to conjure them
isn't there for general Values (see *Future work*).

With both halves, **`AmbientId` collapses to `Hash`** everywhere.
The `from` field in query payloads is the hex form,
indistinguishable to the dispatcher between a seed and a derived
value — it only needs to know whether the id is pre-bound (seed)
or has a producer Request in the `Requests` pool (derived).

**Recording side**: in `AmbientResolver::query`, the child-producing
arms compute and register the current query's `queryHash` as the
child id instead of bumping `nextId`. `registerOuter`/`registerLocal`
become inserts into `map<Hash, ref<Object>>`. Seed allocation in the
PrimOp impl calls `registerOuter(seedObj,
hashString("seed:" + std::to_string(counter)))`.

**Replay side**: at `apply()` setup, pre-bind seed Hashes into
`idToObject`. `dispatchAmbientQuery` resolves unknown ids by
recursive lookup against the `Requests` pool:

```
resolveAmbientId(id):
    if (idToObject.contains(id)) return idToObject[id];
    # id is a queryHash; the Request whose payload hashes to id is the producer
    auto req = decisionGraph.getRequestPayload(id);
    auto parsed = parse(req);
    auto parent = resolveAmbientId(parsed.from);
    auto child = dispatchQueryOnObject(parent, parsed.query, parsed.params);
    idToObject[id] = child;
    return child;
```

Dispatch on `dispatch(reqHash)`:

```
reqJson = decisionGraph.getRequestPayload(reqHash);
fromId  = reqJson["params"]["from"];
liveObj = resolveAmbientId(fromId);
result  = dispatchQueryOnObject(liveObj, reqJson["query"], reqJson["params"]);
return computeResponseHash(serialise(result));
```

Properties:

- **Order-independent.** Any visit order over the FactSet produces
  the same `idToObject` because `resolveAmbientId` does the
  derivation walk on demand.
- **Self-pre-warming.** Resolving id `X` populates `X` and every
  ancestor along its derivation path. Subsequent dispatches whose
  `from` matches any ancestor are O(1).
- **No new index, no schema change.** The `Requests` pool already
  keys Requests by their `queryHash`; the dispatcher just reuses
  the existing `getRequestPayload(h)`. The `producerByChildId`
  sidecar earlier drafts proposed turns out to be redundant.
- **Plays with the fast path.** `TracingReplayEvaluator`'s
  `dispatchedTrie` and `lastQFactsHash` fast path still works
  because it operates on Request hashes, not on the ambient
  resolution layer.

Cost ceiling: O(depth of derivation chain) per resolved id, with
memoisation across resolves in a single walk. For typical
`builtins.cache` workloads this is negligible.

## Architecture: bringing back `cache.cc`

The shape from v12 holds. Inside `prim_cache`:

1. **Parse args.** `{import|expr,baseDir}` exactly as v12. Reject
   unknown attrs, mutual exclusion, missing required.

2. **Resolve the `TracingDecisionGraph`.** Two cases:

   - The outer EvalCommand has one: `state.rootDecisionGraph` is set.
     Share it; both inner recordings and the outer cache write to the
     same SQLite file via the same in-process connection. This is the
     v13-only correctness fix to the v12 follow-up "Should share a
     single instance".
   - No outer: lazily create `state.cacheState.ownedDecisionGraph`
     on first call, reuse across subsequent calls in the same
     process.

   This requires adding a `TracingDecisionGraph * rootDecisionGraph =
   nullptr;` field to `EvalState`, set by
   `EvalCommand::getEvalState()` when it constructs
   `tracingDecisionGraph` (right next to its existing wiring of
   `tracingWriter` and `tracingDecisionGraph`).

   For **nested `builtins.cache`** correctness, the same pointer
   must propagate through to the inner `EvalState` so a nested call
   inside the inner evaluator finds and shares the same graph. The
   inner state's constructor does not take this as a parameter, so
   the primop sets it after construction:

   ```cpp
   innerState->rootDecisionGraph = decisionGraph;
   ```

   Without this line, a nested `builtins.cache` call sees
   `innerState->rootDecisionGraph == nullptr` and constructs its own
   `cacheState.ownedDecisionGraph` — a brand-new SQLite trie in
   memory, divorced from the persistent one. Nested calls' cache
   hits would be lost across CLI invocations.

3. **Build the per-call tracing stack.** Reusing v13's existing
   types:

   - `auto sink = make_shared<NullTraceSink>()` — still needed
     until `TeeTracingWriter` lands.
   - `auto writer = make_shared<TracingWriter>(*sink, decisionGraph)`.
   - `auto tracingEnv = make_ref<TracingEnvironment>(state.environment,
     *writer)` — wrap the *outer* environment (not a fresh
     `SystemEnvironment`); this was the Step 2a ambient-capability
     fix and is mandatory for nested `builtins.cache` correctness.
   - `auto innerState = make_ref<EvalState>(LookupPath{},
     state.fetchSettings, state.settings, tracingEnv,
     state.systemEnvironment, state.getSymbolTable())`. The shared
     `SymbolTable` is required so symbols interned during inner
     parse / `AttrPath::parse` compare equal to outer state's
     symbols. This is the existing v13 constructor
     (`eval.hh:778`).
   - `auto interpreter = make_ref<Interpreter>(innerState);`
   - `auto recordingEval = make_ref<TracingEvaluator>(*writer,
     interpreter);`
   - `auto replayEval = make_ref<TracingReplayEvaluator>(
       recordingEval, *state.environment, *writer, *decisionGraph);`

   The TracingEvaluator wraps Interpreter; TracingReplayEvaluator
   wraps that, so misses fall back through recording.

4. **Set up the shared `AmbientResolver`.** `makeAmbientResolver(&state,
   replayEval)` exists in v13; assign it to
   `interpreter->ambientResolver`. The same resolver instance threads
   through every `<cached-fn>` PrimOp created by `ExprFromObject`
   inside this call.

5. **Convert paths to the inner accessor.** `RootedPath{innerState->
   rootFSRoot, p.path}` for both `importPath` and `baseDir`. (This
   is straight from v12.)

6. **Evaluate.** `importPath ? replayEval->evalFile(...) :
   replayEval->evalExpr(...)`. The replay evaluator tries the cache
   first and falls back to recording.

7. **Bridge back.** `ExprFromObject(result, replayEval, resolver)
   .eval(state, state.baseEnv, v)`. Children become lazy
   `ExprFromObjectAttr` thunks. For `nFunction` values the
   `ExprFromObject` machinery creates a `<cached-fn>` PrimOp that
   routes future applications back through the AmbientResolver.

8. **Retain state.** Push a `CacheState::CallState` onto
   `state.cacheState.calls` (the struct already exists in v13's
   `EvalState`) holding the sink, writer, evaluators, and innerState.
   This keeps them alive past the primop frame so the lazy
   `ExprFromObjectAttr` thunks can fire later.

The above is essentially the v12 `cache.cc` body re-targeted at v13's
types. The non-obvious step is #2 (sharing the
`TracingDecisionGraph`) — and that is also the only step that
requires touching `EvalState` and `EvalCommand`.

## Recording semantics for `builtins.cache`

The inner evaluator's `TracingEvaluator` records the cached
expression's Q exactly once per fresh evaluation. Whether that Q is
`QueryImport`, `QueryExpr`, or `QueryApply` depends on which path
forced the value:

- `replayEval->evalFile(...)` / `evalExpr(...)` records `QueryImport`
  or `QueryExpr` at the inner root.
- The first time an outer caller forces a `<cached-fn>` PrimOp with
  a new argument, the PrimOp's impl routes to
  `innerEvaluator->apply(fnObj, ambientArgObj)`, which records
  `QueryApply{fnId, argId}` at the inner level.

Each of these recordings produces its own `(Q, factSet, result)` row
via `decisionGraph.record(...)`. The `factSet` contains:

- file reads from the inner evaluator (already plumbed through
  `TracingEnvironment::getFileHash`);
- env var lookups (`getEnv`);
- ambient queries against the outer-provided value(s), recorded by
  `TracingEnvironment::ambientQuery` via `logAmbientInteraction`.

Because the outer environment is wrapped by the inner
`TracingEnvironment`, *file reads from the inner evaluator also flow
upward through the outer cache's `TracingEnvironment`*. The outer
trace therefore sees those reads as its own Facts — Step 2a's
"ambient capability fix" — which means an outer recording that
calls `builtins.cache` automatically has the nested call's file
dependencies in its FactSet. We don't need to do anything special at
this level for outer correctness.

For inner correctness, the ambient Facts are crucial:

- A different outer value with the same `fnId`/`argId` would otherwise
  pass the cache check despite producing different observed
  Responses. Recording the `(QueryGetAttr{from=L1}, …)` Fact ties the
  recorded result to the *observed* shape/values of the argument,
  not just to the seed ids.
- A different argument with the same structural answers would still
  legitimately hit the cache — that's the point of validating
  *responses* rather than identities.

This recording behaviour falls out of the v13 infrastructure already
in place. No new TracingWriter methods are needed.

## Replay semantics for `builtins.cache`

When `replayEval->evalFile(...)` (or `evalExpr`, or `apply`) runs:

1. `lookup(Q)` calls `v13Walk(queryHash)`.
2. `v13Walk` first tries the fast path: `dispatchedTrie.diff` against
   the `RequestSet` reachable from `(Q, ∅)`. If the delta resolves
   and `Terminals(Q, candidateCur)` matches, hit.
3. Otherwise, falls back to `decisionGraph.walk(queryHash,
   dispatch)`.
4. The `dispatch` callback in `TracingReplayEvaluator::v13Walk`
   reads each Request payload, calls `getCurrentResponse`, and
   returns the response hash. For Requests whose payload contains
   `"query"`, dispatch routes to `dispatchAmbientQuery`, which:
   - resolves the `from` id via `AmbientReplayState::idToObject`
     (and the structural-derivation path described above),
   - issues the query against the resolved outer Object,
   - serialises the result.
5. On a hit, the result payload comes back; `lookup` wraps it in a
   `TracingReplayObject`. The outer caller forces attrs/strings/…
   via this TracingReplayObject, which itself defers to its own
   `lookupResult<Q, R>` per-method (using the recorded `queryHashStr`
   as the parent in child Queries' Merkle chain).

The `apply()` path additionally seeds the seed roots into
`AmbientReplayState`:

```cpp
ambientState = AmbientReplayState{};
ambientState->idToObject[fnIdStr]  = fn.get_ptr();   // seed root
ambientState->idToObject[argIdStr] = arg.get_ptr();  // seed root
auto result = lookup(trace::QueryApply{fnIdStr, argIdStr});
```

This is the change to v13's existing `apply()`: pre-bind the seed
roots into `idToObject` so structural derivation works for their
descendants, rather than relying on the queue's pop order to assign
identities at first reference. (The queues go away.)

A successful replay still feeds the recording-side writer state
(`v13FactSet`, `allRequestsTrie`, `lastQFactsHash`) so a *later*
miss on a different Q in the same session falls into a coherent
recording chain. v13 doesn't currently sync these on a hit — there
is no equivalent of v12's `syncAfterHash`. The session-level
fast-path state (`lastQFactsHash`, `dispatchedTrie`) is maintained,
but writer-side state for any subsequent fresh recording is not.
That is fine for the CLI root because the CLI does one outer Q. The
primop's case is the same shape — recording state is per-inner-call,
each call has its own writer — so we don't need to revisit this
either.

## Lifetime and ownership

Two cases.

**Process-shared `TracingDecisionGraph`.** When the outer
`EvalCommand` has one, the same pointer is shared. SQLite WAL mode
gives correct concurrent behaviour even though both stacks are
writing through the same handle. The handle outlives the primop call
because `EvalCommand` owns it for the lifetime of the evaluation.

**Owned `TracingDecisionGraph`.** When no outer is present (e.g. a
bare `nix eval` without `--option tracing-eval-cache true`), the
first `builtins.cache` call creates one. `CacheState`'s destructor
runs at `EvalState` teardown, which is after the last cached thunk
could possibly be forced.

Per-call state goes on
`EvalState::cacheState.calls` as a `CallState`. The v13 tree already
has this struct (`sink`, `writer`, `recordingEval`, `replayEval`,
`innerState`) and notes the destruction order: `innerState` first
(because `TracingEnvironment` references the writer), then writer,
then sink. We push one entry per `builtins.cache` call and never
remove until process teardown — the alternative is reference-counting
per-thunk, which is unnecessary overhead.

Each `CallState` carries one `AmbientResolver` instance via the
`<cached-fn>` PrimOps that may have leaked into outer Values. That
resolver in turn carries:

- the per-call seed-root id counter,
- the `idToObject` registry of outer values it's been handed,
- a pointer to `outerState` and `replayEval`.

It must outlive any `<cached-fn>` PrimOp it produced. Because the
PrimOp captures `shared_ptr<AmbientResolver>` directly (via
`ExprFromObject`), the resolver is reference-counted independently of
`CallState`; `CallState` keeps everything else alive.

## Open questions and known risks

These are not blockers — they are places where the design has a
defensible choice but a future revision may want to revisit.

1. **Multiple recordings of the same Q in one factSet "session".**
   The session-level `lastQFactsHash`/`dispatchedTrie` fast path in
   `TracingReplayEvaluator` assumes a single growing factSet across
   the session. When the primop creates a fresh
   `TracingReplayEvaluator` per call, each has its own session
   state — no cross-call contamination. But within one cache call,
   if the same fn is applied twice with different args, both Qs
   share the writer's `v13FactSet`. The fast path's delta
   computation will work but per-Q hit rates depend on FactSet
   stability between Qs. This is identical to the CLI-root case;
   noting it for transparency.

2. **Cross-process concurrent recording of the same Q.** SQLite WAL
   gives correctness on insert (every row is INSERT OR IGNORE on a
   content-addressed key). Two concurrent processes can record the
   same `Q` with the same `factSet` and one of them will lose its
   `Asks` rows to the OR IGNORE. The recorded result is the same
   because the computation is (assumed) deterministic. This is the
   pre-existing v13 contract; no change.

3. **Non-deterministic ambient evaluators.** If the outer evaluator
   returns different `getAttr` responses for the same id across two
   recordings (impossible in practice for pure Nix, possible if
   plugins are involved), the inner `record()` will land at
   different `factSet` hashes; the two recordings coexist as
   independent paths through the decision graph, and which one a
   replay hits depends on what the live outer answers. This is the
   model-level "nondeterminism" the v13 doc punts to Phase 2 and
   we inherit the same punt.

4. **Outer `<cached-fn>` cache key vs inner `QueryApply` Q.** The
   outer evaluator sees a `<cached-fn>` PrimOp and records its own
   `QueryApply{outerFnId, outerArgId}` for the call. The inner
   evaluator separately records `QueryApply{innerFnId,
   innerArgId}`. These are two independent Qs at two independent
   layers, and the cache hits or misses on each layer independently.
   That's intentional — the outer cache short-circuits at the
   architectural boundary (no inner work needed), while the inner
   cache is the persistent unit that survives outer cache misses.

5. **`getOrAllocVirtualRoot` is process-local.** The counter id
   assigned to a virtual root is a `uint64_t` that resets per
   process. Across two recording sessions, the same outer Object
   gets the same id only if the recording happens in the same
   relative order. In single-shot CLI invocations that's fine
   because the recording starts fresh each time. In a long-lived
   process (daemon mode, repeated evals), if the outer has two
   different Objects show up as the `fn` of two cached calls, their
   ids will not collide *but* the canonical `(QueryApply{fnId, argId})`
   hash will differ from what a fresh process would record for the
   same Object. The fast path replays still hit because the lookup
   resolves the seed root by *Object pointer* identity at runtime
   and uses whatever id the recording wrote. This is fine. The
   open question is whether two concurrent calls within the same
   process need to coordinate counter allocation; the answer is
   that they already do because the counter is on `TracingWriter`,
   and per-call `TracingWriter`s have independent counters — which
   is also fine because each call's recordings live in a separate
   factSet (different Qs) and the ids only need to be self-consistent
   within one recording.

6. **Test for ambient-id collision after re-evaluation.** When a
   recording falls through to the inner Interpreter (cache miss) and
   the Interpreter re-issues the apply, fresh seed roots get fresh
   counter ids. If the falling-through call previously did *some*
   ambient queries off a `replayEval`'s assigned ids and then
   transitioned to inner recording, the id-namespaces could appear
   to mix. The v12-era plan flagged this; the cleanest fix is the
   one already documented: on first replay failure inside a single
   `apply()`, drop the seed-root id assignments and let the inner
   re-run start with fresh counters. The test suite
   (`builtins-cache.sh`'s "function changed" and "different
   argument value" cases) exercises the cache-miss-on-apply path
   and is the regression net for this.

7. **TrieBuilder rebuild on per-call `TracingWriter`.** Each
   `builtins.cache` call constructs a fresh `TrieBuilder`. The trie
   is in-memory and built up by `insert(requestHash)` calls during
   recording. Inserting the same Request twice in one call is
   already cheap (the trie deduplicates on insert). Across calls in
   one process, two `TracingWriter`s building two `TrieBuilder`s for
   the same set of Requests will end up persisting the same nodes
   into `RequestSetNodes` — fine, `INSERT OR IGNORE` absorbs the
   duplication. Memory cost is per-call, gone at `CacheState`
   teardown.

9. **Covariant-callback apply Requests are unresolvable on replay
   today.** When `makeCachedFnPrimOp`'s `applyFn` records the
   outer→outer apply via `innerEnv.ambientQuery`, the Request
   payload is `QueryApply{fn=fnId, arg=resultId}` with no `from`
   field — `QueryApply` doesn't have one (its operands are `fn`
   and `arg` instead). `TracingReplayEvaluator::dispatchAmbientQuery`
   explicitly returns `nullopt` for `tag == "apply"` (the in-tree
   comment reads "Apply replay not yet implemented"). When the walk
   dispatches that Request the response hash is zero, the XOR-fold
   diverges from the recorded `cur`, and the walk falls through to
   inner re-evaluation. The covariant-callback test cases in
   `builtins-cache.sh` (`call-fn.nix`, `path-fn.nix`,
   `callpkg-fn.nix`) therefore re-evaluate every time even on
   what would otherwise be a hit. The id-resolution work in Step C
   is necessary but not sufficient — Step D needs an additional
   `QueryApply` dispatcher on the replay side that re-issues the
   outer apply (using the resolved `fn` and the recorded `arg`'s
   resolved Object) and serialises `ResultType{"apply"}` so the
   Fact's response hash matches. Because the apply's recorded
   response payload is the constant `ResultType{"apply"}`, the
   re-issued dispatch trivially matches as long as the apply
   *succeeds* — the actual identity of the result is validated by
   subsequent child queries on the apply-result Object, not by this
   Fact. So the replay implementation can be minimal: resolve `fn`
   via the resolver, route through `resolver->apply(fnId, argObj)`
   to register the result Object under the recorded `arg` id, and
   return the constant response hash.

10. **`QueryApply.arg` field semantics in covariant callbacks.**
    `trace::QueryApply` documents `arg` as the "argument's queryHash
    identity," but the covariant-callback recorder writes the
    *result*'s outer-resolver id there
    (`std::to_string(resultId.value())`), not the argument's local id
    (`argId` from `registerLocal`). That choice means downstream
    Requests can reference the apply's result by that id without an
    extra naming step. It's safe — recording and replay both
    interpret the field as "the entity produced by this apply" — but
    the docstring on the struct should be amended to "argument or
    result identity, depending on the recorder" once the primop
    work lands. The replay-side dispatcher in Step D needs to know
    this convention to register the result Object under the right
    id.

8. **`builtins.cache` inside `apply()` inside `builtins.cache`.**
   The CLI sets up a `TracingReplayEvaluator` for the outer Q, the
   primop sets up a *separate* one for the inner Q, and a
   covariant callback from the inner evaluator back to an outer
   function could re-enter the outer evaluator and trigger another
   `builtins.cache`. Each level has its own factSet, its own
   AmbientResolver, and its own writer. The lifetime model already
   handles this (each `CacheState::CallState` is independent). The
   only place a leak could happen is if the inner covariant
   callback retains the outer's `<ambient-fn>` PrimOp past the
   outer's `CacheState` lifetime — but the PrimOp captures a
   `shared_ptr<AmbientResolver>` directly, so it stays alive on its
   own.

11. **Counter id collisions between inner and outer traces.**
    With one shared `decisionGraph` and one shared `Requests` pool,
    nothing currently distinguishes "`virtual:0` minted by
    `TracingWriter_inner.getOrAllocVirtualRoot`" from
    "`virtual:0` minted by `TracingWriter_outer.getOrAllocVirtualRoot`"
    — both produce the same string, so a Request payload
    referencing `from="virtual:0"` hashes to the same `RequestHash`
    in both stacks. Same story for `hashString("seed:N")` under
    Step C if both the outer's own resolver (when it has one) and
    the inner's resolver mint the same counter. The Qs themselves
    don't collide (their `fn` fields differ between inner and
    outer), so `Asks`/`Terminals` rows stay disjoint, but their
    factSets may share Request hashes across writers and the
    *meaning* differs — a `from="virtual:0"` Request resolves to
    different live Objects depending on which resolver dispatches
    it. Correctness in the worst case falls out of "wrong response
    hash → walk falls through," but this is an obvious source of
    spurious replay misses and possibly worse if the responses
    happen to match. Needs investigation: scope the id strings by
    the writer that minted them (e.g. include a writer-id prefix
    or hash the writer's address into the seed string), or
    document the cross-trace contract explicitly. Flagged for
    later — keep reading first.

12. **Do the AmbientObject closures obviate
    `resolver.outerValues`?** The `queryFn` / `applyFn` captured
    in each `AmbientObject` already close over the resolver and
    the outer Object, so in principle an `AmbientObject` could
    answer its own queries directly without round-tripping through
    `resolver.outerValues[id]`. If that's true throughout, the
    `outerValues` map is dead state. There may be reasons it
    isn't (replay-side dispatch resolving an id it didn't construct
    the AmbientObject for; sharing between sibling callbacks;
    something subtler), but worth checking — flagged for later.

## Implementation step list

The work decomposes into roughly five steps, each independently
testable:

**Step A — share the root `TracingDecisionGraph`.**
Add `TracingDecisionGraph * rootDecisionGraph = nullptr;` to
`EvalState`. Have `EvalCommand::getEvalState()` set it (alongside
`evaluatorCompat`) right after constructing `tracingDecisionGraph`.
No primop changes yet; existing CLI-level tests should still pass.

**Step B — restore `cache.cc`'s body atop v13 types.**
Reimplement `prim_cache` per [§Architecture](#architecture-bringing-back-cachecc).
Reuse the v12 logic for arg parsing, paths, and `CacheState` push.
Use `TracingDecisionGraph` everywhere `TracingIndex` was.
At this point `builtins-cache.sh`'s data-only tests
(`import = scalar / string / attrset / list`, transitive invalidation,
multiple cache calls) should pass — those don't exercise the d=2
ambient layer at all.

**Step C — switch ambient ids to producer queryHashes.**
Collapse `AmbientId` to `Hash`. In `AmbientResolver::query`, the
child-producing arms return the current query's `queryHash` as the
child id instead of bumping a counter. Seed allocation uses
`hashString("seed:" + counter)`. Strip the queue-based fallback
from `TracingReplayEvaluator::dispatchAmbientQuery`; replace with
the on-demand recursive resolution against the `Requests` pool
(described in §Ambient identity). Pre-bind seed Hashes into
`idToObject` in `apply()`. The CLI-level `tracing-eval-cache.sh`
tests should still pass — at the CLI root there is only the seed
pair, which the pre-binding handles directly.

**Step D — apply-Request dispatcher on the replay side.**
`dispatchAmbientQuery` returns `nullopt` for `tag == "apply"` today;
this is what causes recorded `QueryApply` Facts to permanently miss
on replay (open question 9). The minimum dispatcher resolves
`params.fn` via the resolver, ensures the live outer fn is callable,
and returns the canonical response hash for `ResultType{"apply"}`
without re-invoking the outer apply itself (the result value's
identity is validated by subsequent child Queries on the apply
result, not by this Fact). Stretch goal: also register the
resolved outer fn's apply-result Object under the recorded `arg`
id so downstream `getAttr`/`getString` Requests against it can
dispatch. This is enough for the `functionArgs`, simple-lambda, and
curried test cases.

**Step E — incoming-query recording for covariant callbacks.**
This is what `builtins-cache.sh`'s `call-fn`, `path-fn`, and
`callpkg-fn` test cases need. The v12 plan called this out as
unfinished work too; reading the in-tree code confirms it has
neither a recording-side TracingLocalObject decorator nor a
replay-side ReplayLocalObject. Scope:

- A wrapping Object inserted by `resolver->apply` around the
  passed-in local `argObj`. Its methods record an
  `AmbientIncomingRequest`-shaped Fact (`{query: T, params: {from:
  localId, ...}}`) into the inner FactSet via
  `innerEnv.ambientQuery` before delegating to the wrapped Object.
- A corresponding ReplayLocalObject that, during a recorded apply's
  replay, serves recorded incoming answers from the FactSet
  indexed by `(localId, queryType)`. The replay's apply path swaps
  in this proxy for the recorded `argObj`.
- Local-vs-outer discrimination falls out of the same Hash
  scheme: locals get seed hashes of the form
  `hashString("local:" + counter)`; derived locals use the
  producer Request's `queryHash` like derived outers do. The
  dispatcher doesn't need to discriminate by namespace tag — it
  just looks the id up in `idToObject` (pre-bound at apply setup)
  or in the `Requests` pool.

If schedule is pressing, Step E can be deferred. Without it, the
covariant-callback test cases fall through to inner re-evaluation
every time. The cache returns the correct value (the fall-through
path runs the inner Interpreter for real), only the speedup is
missing. The trace types (`AmbientIncomingRequest`/`AmbientIncoming
Response` at `trace-types.hh:540–551`) are already declared; only
the recorder and replayer are missing.

**Step F — clean up and document.**
Move the bulk of the design content here into the source-level
comments where appropriate, add a paragraph to `tracing-eval-cache.md`
that drops the "deferred" qualifier from the `builtins.cache` row,
and remove the v12-era TODO comments in `cache.cc` that refer to
removed types.

### Minimum-viable cut

Steps A + B + C give a correct (data-only and simple-function)
`builtins.cache`. Step D removes a permanent miss for any cached
function call. Step E is required for covariant-callback caching to
actually hit — but the result is correct either way because every
unresolved walk falls through to inner re-evaluation. The
implementation can ship A+B+C+D as the v1 cut and leave E for a
follow-up, or do A+B+C+D+E together; the correctness story is
unchanged.

## At-a-glance implementation checklist

A one-page reference for picking up the work. Each line is one
edit; rough order is top-to-bottom, but A is the only hard
prerequisite of all the rest.

**Step A — share the root TracingDecisionGraph** (~5 lines)

- [ ] `src/libexpr/include/nix/expr/eval.hh`: add
      `TracingDecisionGraph * rootDecisionGraph = nullptr;` next to
      the existing `evaluatorCompat` field.
- [ ] `src/libexpr/include/nix/expr/eval.hh`: add
      `std::unique_ptr<TracingDecisionGraph> ownedDecisionGraph;`
      inside `CacheState`.
- [ ] `src/libcmd/command.cc`: in `EvalCommand::getEvalState()`
      after `tracingDecisionGraph = std::make_unique<TracingDecisionGraph>();`
      add `evalState->rootDecisionGraph = tracingDecisionGraph.get();`.

**Step B — restore prim_cache** (~150 lines, lifts cleanly from v12 form)

- [ ] `src/libexpr/primops/cache.cc`: replace the throwing stub with
      the body in Appendix A. Use `TracingDecisionGraph` everywhere
      the v12 form used `TracingIndex`.
- [ ] `src/libexpr/primops/cache.cc`: include the `NullTraceSink`
      class definition.
- [ ] Critical line in the new body:
      `innerState->rootDecisionGraph = decisionGraph;` (nested
      builtins.cache propagation).
- [ ] Run `tests/functional/builtins-cache.sh`; expect the data-only
      assertions (scalar/string/attrset/list/nested/expression
      form/error cases/transitive invalidation/multiple calls)
      to pass.

**Step C — producer queryHash as ambient id** (~80 lines across 3 files)

- [ ] `src/libexpr/include/nix/expr/trace-ids.hh`: change
      `AmbientId` to `Hash` (or an alias `using AmbientId = Hash;`
      if the strong tag is still useful). Drop the
      seed-counter-vs-derived distinction.
- [ ] `src/libexpr/expr-from-object.cc`: in `AmbientResolver::query`,
      change `registerOuter(child)` in the child-producing arms
      (`QueryGetAttr`, `QueryGetListElem`, `QueryApply`) to return
      the current query's `queryHash` and register the child under
      that. `registerOuter(seedObj)` callers in the PrimOp impl
      pass `hashString("seed:" + std::to_string(counter))`.
- [ ] `src/libexpr/ambient-object.cc`: `AmbientObject::id` stores a
      `Hash`; methods emit it in the `from` string slot using
      `.to_string(HashFormat::Base16, false)`.
- [ ] `src/libexpr/tracing-replay-evaluator.cc`: rewrite
      `dispatchAmbientQuery` to use on-demand recursive resolution
      via `resolveAmbientId` against the existing `Requests` pool
      (described in §Ambient identity). Remove
      `pendingChildren` / `unresolvedRoots`.
- [ ] `src/libexpr/tracing-replay-evaluator.cc`: in `apply()`,
      pre-bind seed Hashes into `idToObject`. No queue setup.
- [ ] Run `builtins-cache.sh`; the `functionArgs`, simple-lambda,
      and curried cases should now exercise the new code path —
      cache misses are still expected for covariant callbacks
      until Step D.

**Step D — apply-Request replay dispatcher** (~30 lines)

- [ ] `src/libexpr/tracing-replay-evaluator.cc`:
      `dispatchAmbientQuery` adds an `if (tag == "apply")` arm that
      resolves `params.fn` via the resolver, attempts a *dummy*
      apply on the resolved outer fn (without actually invoking
      it — see open question 9), and returns the canonical
      `ResultType{"apply"}` response hash. Register the
      apply-result Object under the recorded `arg` id so
      downstream Requests against it dispatch.
- [ ] Cached function-call replay-completeness assertions
      (`_NIX_DISALLOW_PARSE=1`) in `builtins-cache.sh` should now
      pass for simple-function cases.

**Step E — incoming-query recording for covariant callbacks** (~200 lines)

- [ ] `src/libexpr/include/nix/expr/tracing-local-object.hh`,
      `src/libexpr/tracing-local-object.cc`: new `TracingLocalObject`
      per Appendix C, recording every outer access as an incoming
      Fact in the inner FactSet.
- [ ] `src/libexpr/expr-from-object.cc`: `AmbientResolver::apply`
      wraps `argObj` in `TracingLocalObject` before bridging via
      `ExprFromObject`.
- [ ] `src/libexpr/tracing-replay-evaluator.cc`: an analogous
      `ReplayLocalObject` that serves recorded incoming answers
      from the FactSet, indexed by `(localId, queryType)`.
      `dispatchAmbientQuery`'s apply arm swaps in this proxy for
      the recorded `argObj`.
- [ ] `builtins-cache.sh`'s `call-fn.nix`, `path-fn.nix`,
      `callpkg-fn.nix` cases should now produce cache hits
      (verifiable via `_NIX_DISALLOW_PARSE=1`).

**Step F — wrap-up** (~20 lines)

- [ ] `doc/design/tracing-eval-cache.md`: in the "What's deferred"
      section, drop the `builtins.cache` line.
- [ ] `src/libexpr/primops/cache.cc`: remove the "d=2 ambient layer
      pending" comment block.
- [ ] Update or remove obsolete project-doc references to
      `TracingIndex`.
- [ ] Inline-comment the structural-id encoding in
      `tracing-replay-evaluator.cc` and `expr-from-object.cc`.

### Stop conditions

- A is mandatory.
- B without C ships only the data-only test surface.
- C without D leaves a permanent miss on every apply Fact.
- D without E leaves covariant-callback hits unimplemented but
  every replay correctly falls through to inner re-evaluation.
- v1 ship = A+B+C+D. E is a follow-up.

## Future work (out of scope here)

These were in the v12-era follow-up list and remain valid:

- `TeeTracingWriter` to replace the `NullTraceSink` placeholder
  inside the primop.
- Shared in-memory AST cache across inner evaluators so each
  `builtins.cache` call doesn't re-parse from scratch (was previously
  blocked by SourcePath/accessor binding into the AST).
- A deduplicating Environment layer for overlapping file reads
  across cache calls.
- Restoring the `nix eval-cache` introspection subcommand — it was
  removed by `3db54dcff` and would help debug the
  producer-query-as-id path in Step C.
- Interaction-traced *outer→inner* nesting as an alternative to the
  current input-traced nesting (so the outer cache treats the inner
  as an oracle and benefits from per-method early cutoff). This is a
  change in cost model rather than correctness; defer until we have
  numbers.
- **Structural-replay unification — the general fallback for
  cross-invocation seed drift.** When two evaluations produce
  semantically identical ambient values via different
  apply-boundary sequences, the seed counters disagree and
  cross-invocation Q hashes drift even though the values match. A
  unification algorithm at replay time — match recorded nodes by
  their structural children rather than by their recorded id
  (Hindley-Milner sense; distinct from `AmbientId` collapsing to
  `Hash` in Step C) — would let the cache hit. Applicable to any
  caller, not just the CLI, and compatible with the
  producer-query-as-id model since derived ids are already
  structural; only seed identification is positional. Cost may be
  substantial; defer until a workload justifies it.

  *Narrow CLI complement: named hints.* The CLI specifically could
  stabilise seed identity by supplying a semantic hint string at
  `registerOuter` / `registerLocal` time — modelled on the
  unpinned fetch URL hint used for lazy-paths source-root
  identity — letting seed Hashes survive apply-boundary
  reorderings without paying for replay-time unification. General
  hints are infeasible (no method-argument metadata to conjure
  hints for arbitrary Values), so this is CLI-only. Mentioned for
  completeness; the CLI's apply-boundary sequence is stable enough
  today that this doesn't need building.

## Source map

For the implementation phase, the files we expect to touch:

- `src/libexpr/primops/cache.cc` — restore `prim_cache` body.
- `src/libexpr/include/nix/expr/eval.hh` — add
  `rootDecisionGraph` pointer and (if needed) the `CacheState`
  fields for the owned-graph case.
- `src/libcmd/command.cc` — set `evalState->rootDecisionGraph`
  during `getEvalState()`.
- `src/libexpr/tracing-replay-evaluator.cc` — rewrite
  `dispatchAmbientQuery` and `apply()` to use recursive
  producer-Request lookup instead of positional queues; add
  apply-Request dispatcher for covariant callbacks (open
  question 9).
- `src/libexpr/ambient-object.cc` + the resolver in
  `expr-from-object.cc` — emit hash-encoded derived ids.
- `tests/functional/builtins-cache.sh` — verify in place (no code
  change required if the implementation matches the test
  expectations). Wired into `tests/functional/meson.build:173`.
  Currently broken on `eval-cache-v13` — first assertion
  (`builtins.cache { import = scalar.nix }`) throws on the
  primop stub.

## Appendix A: sketch of restored `cache.cc`

This is the shape `prim_cache` will take, lifted from the v12 form
and ported to v13's types. It is not committed code; it is a
reading guide to the design decisions above.

```cpp
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-environment.hh"
#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-writer.hh"

namespace nix {

class NullTraceSink final : public TraceSink {
public:
    void log(const nlohmann::json &) override {}
};

static void prim_cache(EvalState & state, PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos,
        "while evaluating the argument passed to builtins.cache");

    std::optional<SourcePath> importPath;
    std::optional<std::string> expr;
    std::optional<SourcePath> baseDir;

    for (auto & attr : *args[0]->attrs()) {
        auto n = state.symbols[attr.name];
        if (n == "import") {
            NixStringContext ctx;
            importPath.emplace(state.coerceToPath(attr.pos, *attr.value, ctx,
                "while evaluating the 'import' attribute passed to builtins.cache"));
        } else if (n == "expr") {
            expr.emplace(state.forceStringNoCtx(*attr.value, attr.pos,
                "while evaluating the 'expr' attribute passed to builtins.cache"));
        } else if (n == "baseDir") {
            NixStringContext ctx;
            baseDir.emplace(state.coerceToPath(attr.pos, *attr.value, ctx,
                "while evaluating the 'baseDir' attribute passed to builtins.cache"));
        } else {
            state.error<EvalError>("unsupported argument '%1%' to builtins.cache", n)
                .atPos(attr.pos).debugThrow();
        }
    }

    if (importPath && expr)
        state.error<EvalError>("builtins.cache: 'import' and 'expr' are mutually exclusive")
            .atPos(pos).debugThrow();
    if (!importPath && !expr)
        state.error<EvalError>("builtins.cache: either 'import' or 'expr' is required")
            .atPos(pos).debugThrow();
    if (expr && !baseDir)
        state.error<EvalError>("builtins.cache: 'baseDir' is required when using 'expr'")
            .atPos(pos).debugThrow();

    auto & cache = state.cacheState;

    // Step A: share the EvalCommand-owned graph if present;
    // otherwise lazily construct an owned one.
    TracingDecisionGraph * decisionGraph;
    if (state.rootDecisionGraph) {
        decisionGraph = state.rootDecisionGraph;
    } else {
        if (!cache.ownedDecisionGraph)
            cache.ownedDecisionGraph = std::make_unique<TracingDecisionGraph>();
        decisionGraph = cache.ownedDecisionGraph.get();
    }

    // Per-call tracing stack.
    auto sink   = std::make_shared<NullTraceSink>();
    auto writer = std::make_shared<TracingWriter>(*sink, decisionGraph);

    // Wrap the OUTER environment (Step 2a ambient-capability fix —
    // inner file reads must flow up to outer accessors so the outer
    // cache observes them).
    auto tracingEnv = make_ref<TracingEnvironment>(state.environment, *writer);

    // Inner EvalState shares the outer's SymbolTable so symbols
    // interned during inner parse compare equal to outer symbols.
    auto innerState = make_ref<EvalState>(
        LookupPath{},
        state.fetchSettings,
        state.settings,
        tracingEnv,
        state.systemEnvironment,
        state.getSymbolTable());

    auto interpreter = make_ref<Interpreter>(innerState);

    // Evaluator stack: TracingReplay → TracingEval → Interpreter.
    // On miss, TracingReplay falls back into TracingEval, which
    // records into the shared decision graph via the writer.
    ref<Evaluator> recordingEval = make_ref<TracingEvaluator>(*writer, interpreter);
    ref<Evaluator> replayEval = make_ref<TracingReplayEvaluator>(
        recordingEval, *state.environment, *writer, *decisionGraph);

    // Propagate the shared graph into the inner EvalState so nested
    // builtins.cache calls find it through state.rootDecisionGraph.
    innerState->rootDecisionGraph = decisionGraph;

    // Persist per-call state on the outer EvalState so it outlives
    // any ExprFromObjectAttr thunks the result attrs are wrapped in.
    cache.calls.push_back({
        .sink           = sink,
        .writer         = writer,
        .recordingEval  = recordingEval.get_ptr(),
        .replayEval     = replayEval.get_ptr(),
        .innerState     = innerState,
    });

    // Shared ambient resolver — one per builtins.cache invocation,
    // threaded through every <cached-fn> PrimOp this call produces.
    auto resolver = makeAmbientResolver(&state, replayEval.get_ptr());
    interpreter->ambientResolver = resolver;

    // Re-anchor paths on the inner accessor (TracingSourceAccessor).
    auto toInnerPath = [&](const SourcePath & p) {
        return RootedPath{innerState->rootFSRoot, p.path};
    };

    // Evaluate via the replay evaluator (cache-first, recording-fallback).
    ref<Object> result = importPath
        ? replayEval->evalFile(toInnerPath(*importPath), importPath->path.abs())
        : replayEval->evalExpr(*expr, toInnerPath(*baseDir));

    // Bridge the inner Object back to the outer Value via
    // ExprFromObject. Eager top-level eval (primops must produce a
    // concrete Value), lazy children (ExprFromObjectAttr thunks).
    ExprFromObject(result.get_ptr(), replayEval.get_ptr(), resolver)
        .eval(state, state.baseEnv, v);
}

static RegisterPrimOp primop_cache({
    .name = "__cache",
    .args = {"args"},
    .doc = R"(
      Evaluate an expression in a separate evaluator with persistent caching.
      ...
    )",
    .impl = prim_cache,
    .experimentalFeature = Xp::TracingEvalCache,
});

} // namespace nix
```

The skeleton matches the v12 form. The only meaningful change is
Step A (sharing the graph via `state.rootDecisionGraph`) and the
type swap from `TracingIndex` to `TracingDecisionGraph`.

## Appendix B: sketch of `EvalState::rootDecisionGraph` plumbing

```cpp
// src/libexpr/include/nix/expr/eval.hh, near evaluatorCompat
// (~line 562, just before CacheState):

/* Shared decision-graph index for builtins.cache. Set by
   EvalCommand when CLI-level tracing-eval-cache is enabled, so
   builtins.cache and the CLI cache write to the same SQLite
   file via the same in-process handle. Null otherwise; the
   primop falls back to cacheState.ownedDecisionGraph. */
TracingDecisionGraph * rootDecisionGraph = nullptr;
```

```cpp
// src/libexpr/include/nix/expr/eval.hh, inside CacheState
// (~line 567, alongside the existing CallState vector):

std::unique_ptr<TracingDecisionGraph> ownedDecisionGraph;
```

```cpp
// src/libcmd/command.cc, in EvalCommand::getEvalState()
// after `tracingDecisionGraph = std::make_unique<TracingDecisionGraph>();`
// and after `evalState->evaluatorCompat = eval.get_ptr();`:

evalState->rootDecisionGraph = tracingDecisionGraph.get();
```

That is the entire Step A patch. Three lines of header diff and one
line of command.cc.

## Appendix C: sketch of `TracingLocalObject` for Step E

When `resolver->apply(fnId, argObj)` is invoked during a covariant
callback, the outer evaluator may access `argObj`'s fields directly
— attribute lookups, getString, getInt — and those accesses must be
recorded as incoming Facts in the inner FactSet for replay
validation. The piece missing today is the decorator that does the
recording.

The shape:

```cpp
/* Object decorator that wraps a local (inner) value passed to the
   outer evaluator during a covariant callback. Records every outer
   access as an AmbientIncoming Fact in the inner FactSet, then
   delegates to the wrapped Object. */
class TracingLocalObject : public Object
{
    std::shared_ptr<Object> inner;
    AmbientId localId;
    Environment & innerEnv;

public:
    TracingLocalObject(std::shared_ptr<Object> inner, AmbientId localId,
                       Environment & innerEnv)
        : inner(std::move(inner)), localId(localId), innerEnv(innerEnv) {}

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override {
        auto child = inner->maybeGetAttr(name);
        // The recorded `from` is this local value's Hash id.
        auto q = trace::QueryGetAttr{name, localId.to_string(HashFormat::Base16, false)};
        auto r = child ? trace::ResultMaybeType{
                            std::optional{objectTypeToString(child->getType())}}
                       : trace::ResultMaybeType{std::nullopt};
        innerEnv.ambientQuery(q, [&](const auto &) { return r; });
        if (!child) return nullptr;
        // Derived local id is the queryHash of the producer Q,
        // mirroring the outer ambient id rule from Step C.
        return std::make_shared<TracingLocalObject>(
            std::move(child),
            TracingDecisionGraph::computeQueryHash(q),
            innerEnv);
    }

    // ...similar wrapping for getString, getInt, getListElem, etc.
    // Atomic getters: record the answer Fact, return the value
    // unchanged. Composite getters (getAttr, getListElem): record
    // and re-wrap the child for transitive recording.
};
```

Local-vs-outer is just a seed-string convention: locals use
`hashString("local:" + counter)`, outers use
`hashString("seed:" + counter)`. From the dispatcher's perspective
both are `Hash` ids resolved against the same `idToObject` (for
pre-bound seeds) or `Requests` pool (for derived).

## Appendix D: walkthrough of the call-fn.nix case

This walks through what would happen, end to end, for

```nix
(builtins.cache { import = ./call-fn.nix; }) { f = x: x + 1; x = 10; }
```

(`call-fn.nix` is `{ f, x }: f x`), assuming Steps A–C are in
place, `--option tracing-eval-cache true` enabled so the outer is
also tracing, and a cold cache. The point is to show both
evaluators' state side by side: who fires what, what each writer
records, and where the resolver ids come from.

This deliberately glosses some lower-level mechanics (the precise
`Interpreter::apply` thunk wiring, the order in which `getType` /
`getAttrNames` fire during attrset destructuring) — see *§The
actual gap* for the smaller chained-id example. Here the goal is
the broader two-evaluator picture.

### Setup

`EvalCommand::getEvalState` constructs the outer stack:

```
Interpreter_outer       (innermost)
  ↑ TracingEvaluator_outer    (writes to outerWriter)
  ↑ TracingReplayEvaluator_outer  (reads outerDecisionGraph; falls back to inner stack on miss)
```

`outerWriter` writes to a shared `TracingDecisionGraph`
(`decisionGraph`) and a JSON `TraceFile`. The outer environment is
a `TracingEnvironment` wrapping `SystemEnvironment`.

`prim_cache` first call constructs the inner stack and shares the
same graph (via `state->rootDecisionGraph`):

```
Interpreter_inner
  ↑ TracingEvaluator_inner    (writes to innerWriter, NullTraceSink)
  ↑ TracingReplayEvaluator_inner  (reads decisionGraph)
```

`innerWriter` shares `decisionGraph` with the outer. The inner
environment wraps the *outer* `TracingEnvironment` (Step 2a's
ambient-capability fix), so inner file reads bubble through both
`TracingSourceAccessor`s — this is **input-traced nesting** in
action.

A note before going further: "input-traced," "content-traced," and
"interaction-traced" name *interaction models between two
evaluators*, not properties of an evaluator on its own. The
outer↔inner relationship at this boundary uses two of them
together: file / env reads use input-traced nesting (Facts
propagate upward through the wrapped `TracingEnvironment`), and
ambient queries on the outer-provided arg use interaction-traced
nesting (recorded as d>0 Facts in the inner trace only). A given
evaluator can sit on different sides of different interactions:
the inner here is content-traced *with respect to its outer*
(its Q-hashes don't reference outer context, so it's reusable
across outers), but if this inner were itself to invoke another
nested `builtins.cache`, *that* deeper boundary would apply the
same input-traced + interaction-traced split again — the inner
would be the "outer" of the deeper interaction. The classification
is per-interaction, not per-evaluator.

A `resolver = makeAmbientResolver(&outerState, replayEval_inner)`
threads through every `<cached-fn>` PrimOp produced inside this
`builtins.cache` call, and `Interpreter_inner->ambientResolver`
is set to it.

State before any expression evaluates:

```
decisionGraph: empty pools, empty Asks, empty Terminals
outerWriter:   v13FactSet = ∅, allRequestsTrie = ∅
innerWriter:   v13FactSet = ∅, allRequestsTrie = ∅
resolver:      outerValues = ∅, localValues = ∅
```

### Step 1 — outer logs `Q_expr`

`replayEval_outer.evalExpr(exprText, baseDir)` looks up
`QueryExpr{exprText, baseDir}` — cold miss — falls through to
`tracingEval_outer.evalExpr`, which `logRootQuery(Q_expr)` and
delegates to `Interpreter_outer.evalExpr`. Parsing is in-memory;
no file Fact is recorded yet.

`outerWriter.inFlight = { Q_expr }`. No Facts yet.

The outermost expression is an application:
`(builtins.cache { import = ./call-fn.nix; }) { f = x: x+1; x = 10; }`.
Evaluating it to WHNF means forcing the function side first, then
applying. Every step below happens inside the forcing of `Q_expr`'s
body — `Q_expr` stays in flight until Step 10. That context
matters: any Fact added to `outerWriter.v13FactSet` during Steps
2–9 ends up in `Q_expr`'s recorded factSet at termination.

### Step 2 — outer forces the function side; the primop fires

To force the application, the outer Interpreter first evaluates the
function side `builtins.cache { import = ./call-fn.nix; }`. That
expression is itself a complete primop application —
`builtins.cache` is a 1-arity PrimOp and the argument attrset is
supplied — so the WHNF is not "a PrimOp waiting for its arg" but
whatever `prim_cache.impl` returns after running. The impl:

1. Parses the arg attrset, gets `importPath = ./call-fn.nix`.
2. Locates the shared graph; since `state.rootDecisionGraph` is
   set, `decisionGraph` is reused.
3. Builds the inner stack and resolver (above).
4. Calls `replayEval_inner.evalFile(./call-fn.nix)`.

What `prim_cache.impl` ultimately returns (via the `ExprFromObject`
bridge) is covered in Step 4.

### Step 3 — inner records `Q_import` (with file-read Fact)

`replayEval_inner.evalFile` cold-misses, falls through to
`tracingEval_inner.evalFile`:

- `logRootQuery(QueryImport{".../call-fn.nix"})` →
  `innerWriter.inFlight = { Q_import }`.
- `Interpreter_inner.evalFile` parses `call-fn.nix`. Reading the
  file fires `TracingSourceAccessor_inner.getFileHash`, which
  calls `innerEnv.getFileHash` → wraps to outer's
  `TracingEnvironment` → reaches the real filesystem; both writers
  log the response. This dual-recording is the input-traced
  nesting model: the inner Fact propagates upward through the
  environment chain so the outer trace counts it as one of its
  own dependencies.

  Both writers' `v13FactSet` gets:

  ```
  Fact_file = ( queryHash(FileReadRequest{".../call-fn.nix"}),
                responseHash(FileReadResponse{H_file_content}) )
  ```

- `Interpreter_inner` returns `InterpreterObject(lambda {f,x}: f x)`.
- `tracingEval_inner.logResult(ResultType{"function"}, Q_import)`:
  - factSet at this moment = `{ Fact_file }`
  - `decisionGraph.record(Q_import, factSetHash, resultHash, …)` writes
    - `Asks(Q_import_hash, ∅) → RequestSet{ R_file }`
    - `Terminals(Q_import_hash, factSetHash) → R_func`

State after Step 3:

```
decisionGraph:
  Requests:        { R_file }
  Queries:         { Q_import_hash }
  Results:         { R_func }
  RequestSetNodes: { node({R_file}) }
  Asks:            { (Q_import_hash, ∅)  → RS{R_file} }
  Terminals:       { (Q_import_hash, fH) → R_func }

innerWriter / outerWriter:
  v13FactSet = { Fact_file }
```

The result returned to `prim_cache` is a `TracingObject_inner`
wrapping the lambda Value, carrying `triePos.queryHashStr = Q_import_hash`.

What each trace did and did not capture at this point reflects
the dual nature of the outer↔inner interaction (recall: these are
per-interaction classifications, not properties of either
evaluator):

- The **outer side of the interaction is input-traced**: the
  outer sees the inner's file read as its own Fact via the
  `TracingEnvironment` chain, so any change to that file
  invalidates outer cache hits. It does *not* see `Q_import`,
  `ResultType{"function"}`, or the `TracingObject_inner` handoff
  at `prim_cache`'s return. The boundary sits at the environment,
  not at the Object interface.
- The **inner side of the interaction is content-traced**: the
  inner records what it itself observed (the file Fact, the
  `Q_import` query, the function result) and *nothing about which
  outer context invoked it*. This is load-bearing for
  `builtins.cache`'s reuse story — if the inner Q-hashes mentioned
  the outer's `Q_expr` hash, the outer arg's identity, or any
  other outer-context detail, they would vary per outer caller
  and the cache wouldn't hit across different outer projects
  importing the same inner expression. The whole point of the
  primop is that a Nixpkgs-shaped inner caches once and serves
  many outers; that demands inner Q-hashes that depend only on
  inner inputs.
- The inner is content-tracing *in relation to the outer*, but if
  it were to invoke a further nested `builtins.cache` the inner
  would sit on the input-tracing side of *that* deeper boundary.
  The classification rides on the boundary, not on the evaluator.

The same asymmetry will hold in Step 4 — `getType` and
`getAttrNames` on the TracingObject log queries into
`innerWriter`, never into `outerWriter`.

Concretely, splitting the snapshot above by writer makes this
explicit:

```
Inner trace state after Step 3:
  innerWriter.v13FactSet = { Fact_file }
  Asks rows written:       { (Q_import_hash, ∅)  → RS{R_file} }
  Terminals rows written:  { (Q_import_hash, fH) → R_func }

Outer trace state after Step 3:
  outerWriter.v13FactSet = { Fact_file }
  Asks / Terminals rows written by the outer: ∅
  (Q_expr is still in flight; no edge tables for it yet.)
```

Both writers share the same physical SQLite tables, but rows in
the snapshot above were all written by the inner. The outer's
contribution to `decisionGraph` happens at Step 10, when `Q_expr`
closes.

### Step 4 — bridging the inner result back to the outer

Still inside `prim_cache`, after Step 3 returned the lambda's
`TracingObject_inner`:

```
ExprFromObject(lambdaTracingObj, replayEval_inner, resolver)
    .eval(outerState, outerBaseEnv, v_outer);
```

`getType()` on the TracingObject returns `nFunction`. The
`nFunction` arm of `ExprFromObject::eval` calls
`makeCachedFnPrimOp(lambdaTracingObj, replayEval_inner, resolver)`
and stores the resulting `<cached-fn>` PrimOp in `v_outer`. That
`v_outer` is the WHNF of `builtins.cache { import = ./call-fn.nix; }`
— the function side of the outermost application — and
`prim_cache` returns it to its caller (the outer Interpreter).

`Q_expr` is still in flight. `outerWriter.v13FactSet` already
contains `Fact_file` from Step 3's input-traced propagation.

### Step 5 — outer applies `<cached-fn>` to the outer arg

The outer Interpreter, having forced the function side and gotten
the `<cached-fn>` PrimOp, now completes the outermost application
by invoking it with the outer arg `{ f = x: x+1; x = 10; }`. PrimOp
application bypasses `Evaluator::apply`, so no outer `Q_apply` is
logged at this step. The cached-fn PrimOp's impl runs:

```
outerArgObj = InterpreterObject(outerState, args[0])      (lazy outer attrset)
L0          = resolver.registerOuter(outerArgObj)
            = hashString("seed:0")
resolver.outerValues[L0] = outerArgObj
contraArg   = AmbientObject(L0, queryFn, applyFn)
appResult   = replayEval_inner.apply(lambdaTracingObj, contraArg)
```

`queryFn` and `applyFn` are closures the PrimOp impl builds
inline. `queryFn(id, q)` dispatches `q` through
`resolver.query(id, q)` to get the response from the live outer
Object, then calls `innerEnv.ambientQuery(q, …)` to log the Fact
in `innerWriter`. `applyFn(fnId, argObj)` does the same shape for
apply: routes through `resolver.apply` and logs a `QueryApply`
Fact. They're how each `AmbientObject` method call ends up both
answered (via the resolver) and recorded (in the inner trace).

Resolver state after this step:

```
outerValues: { L0 → outerArgObj }
localValues: ∅
```

### Step 6 — inner records `Q_apply` (still only `Fact_file` in flight)

`replayEval_inner.apply(lambdaTracingObj, contraArg)`:

- `getId(lambdaTracingObj)` = its `queryHashStr` = `Q_import_hash`.
- `getId(contraArg)` = nullptr → `argId = "virtual:0"` (inner
  writer's first `getOrAllocVirtualRoot`).
- Pre-bind `idToObject["virtual:0"] = contraArg`.
- `lookup(QueryApply{fn=Q_import_hash, arg="virtual:0"})` →
  cold miss → fall through.

`tracingEval_inner.apply`:

- `logRootQuery(Q_apply)`. `innerWriter.inFlight = { Q_apply }`.
  Note: `Q_apply.fn = Q_import_hash` ties the apply's identity to
  the file-read terminal recorded above. A future hit on the same
  source contents reuses the same `Q_apply` hash.
- `Interpreter_inner.apply(lambda, contraArg)`:
  - `lambda.defeatCache()` succeeds (it's an `InterpreterObject`).
  - `contraArg.defeatCache()` throws (it's an `AmbientObject`);
    fallback wraps as `mkThunk(ExprFromObject(contraArg, nullptr, resolver))`.
  - `mkApp(lambdaValue, argThunk)` → app thunk Value.
  - Returns `InterpreterObject` for the app thunk.
- `logResult(ResultType{"apply"}, Q_apply)`:
  - factSet at this moment = `{ Fact_file }` — *still no ambient
    Facts*. The app thunk hasn't been forced yet, so ambient
    queries haven't fired.
  - `decisionGraph.record(Q_apply, factSetHash, R_apply_type, …)` writes
    - `Asks(Q_apply, ∅) → RS{ R_file }`
    - `Terminals(Q_apply, factSetHash) → R_apply_type`

State after Step 6:

```
decisionGraph (added):
  Queries:   { …, Q_apply_hash }
  Results:   { …, R_apply_type }
  Asks:      { …, (Q_apply_hash, ∅)  → RS{R_file} }
  Terminals: { …, (Q_apply_hash, fH) → R_apply_type }

innerWriter:
  v13FactSet = { Fact_file }   (unchanged)
```

The recorded result for `Q_apply` is just `ResultType{"apply"}` —
a placeholder. The *actual* return values of the application are
captured by subsequent Queries against this result's
`TracingObject` (via its `queryHashStr`).

`tracingEval_inner.apply` returns
`TracingObject_inner(appResult, …, triePos{R_apply_type, Q_apply_hash})`.

### Step 7 — outer forces the apply result, driving inner body evaluation

Back in `prim_cache`:

```
ExprFromObject(appResultTracingObj, replayEval_inner, resolver)
    .eval(outerState, outerBaseEnv, v_outer);
```

`appResultTracingObj.getType()` is what triggers the inner body
to actually run. This call goes through the `TracingObject_inner`
wrapper: it logs a child Query against `Q_apply_hash`
(`QueryGetType{from=Q_apply_hash}`) into `innerWriter.inFlight`,
then forces the underlying app thunk.

Forcing the app thunk runs the inner application:

1. **Force `argValue` (the `ExprFromObject` thunk for `contraArg`)
   to destructure the formal `{f, x}` pattern.** `ExprFromObject::eval`
   on `AmbientObject(L0)` switches on type:
   - `contraArg.getType()` → `AmbientObject` issues
     `queryFn(L0, QueryGetType{from=L0})`. The resolver answers
     `ResultType{"set"}` from `outerArgObj.getType()`; the queryFn
     closure routes through `innerEnv.ambientQuery` which calls
     `innerWriter.logAmbientInteraction(QueryGetType, ResultType{"set"})`,
     adding a Fact to `v13FactSet`.
   - `contraArg.getAttrNames()` → same shape, returns `["f", "x"]`,
     adds a `(QueryGetAttrNames{from=L0}, ResultListOfStrings{["f","x"]})`
     Fact.
   - Builds a Value attrset with `ExprFromObjectAttr` thunks for
     each name.

2. **Matcher binds the inner formals.** `f` → thunk for
   `ExprFromObjectAttr("f", contraArg, …)`. `x` likewise.

3. **Evaluate body `f x`.** Forcing inner `f`:
   - `ExprFromObjectAttr::eval` for `"f"` calls
     `contraArg.maybeGetAttr("f")` → `queryFn(L0, QueryGetAttr{name="f", from=L0})`.
   - Resolver computes `outerArgObj.maybeGetAttr("f")` = the outer
     lambda `x: x+1`, and under Step C registers it as
     `L1 = queryHash(QueryGetAttr{name="f", from=L0})`.
     `resolver.outerValues[L1] = outerLambda`.
   - `innerWriter.logAmbientInteraction(QueryGetAttr{…}, ResultMaybeType{"lambda"})`
     adds a Fact whose Request payload encodes `from=L0, name="f"`.
   - `AmbientObject::maybeGetAttr` returns `AmbientObject(L1, …)`.
   - `ExprFromObject::eval` on `AmbientObject(L1)` sees `nFunction`
     and constructs a `<cached-fn>` PrimOp wrapping `AmbientObject(L1)`.
     Inner `f` is now this PrimOp.

4. **Apply inner `f` to inner `x`.** Inner Nix interpreter
   evaluates `f x` as a PrimOp call. The PrimOp's impl is
   `makeCachedFnPrimOp`'s closure; when invoked with `args[0] =
   x_thunk` (the ExprFromObjectAttr for "x" on contraArg) the impl
   sets up a fresh ambient sub-apply: it registers `x_thunk` as a
   local, calls `replayEval_inner.apply(AmbientObject_L1, AmbientObject_for_x_thunk)`,
   which fires `getAttr "x"` on `L0` (registering `L2 =
   queryHash(QueryGetAttr{name="x", from=L0})` as the child),
   then dispatches the apply through `AmbientObject::queryApply`,
   which fires `QueryApply{fn=L1, arg=L2}` as an ambient Fact.

5. **Outer lambda body `x + 1` runs in the outer Interpreter
   (because L1 is the outer lambda).** Forcing `x + 1` forces `x`
   (bound to the `ExprFromObjectAttr` thunk for `arg.x`); that
   thunk resolves to `getInt` on `L2`, which fires
   `QueryGetInt{from=L2}` as an ambient Fact and returns 10. The
   addition produces 11.

By the end of forcing the app thunk, `innerWriter.v13FactSet` has
accumulated, in addition to `Fact_file`:

```
Fact_getType_L0     = ( H(QueryGetType{from=L0}),       H(ResultType{"set"}) )
Fact_getAttrNames_L0 = ( H(QueryGetAttrNames{from=L0}), H(ResultListOfStrings{["f","x"]}) )
Fact_getAttr_f      = ( H(QueryGetAttr{name="f", from=L0}), H(ResultMaybeType{"lambda"}) )
Fact_getAttr_x      = ( H(QueryGetAttr{name="x", from=L0}), H(ResultMaybeType{"int"}) )
Fact_apply_L1_L2    = ( H(QueryApply{fn=L1, arg=L2}),   H(ResultType{"apply"}) )
Fact_getInt_L2      = ( H(QueryGetInt{from=L2}),        H(ResultInt{10}) )
```

The outer's `v13FactSet` got `Fact_file` only (the inner ambient
chatter doesn't propagate to outer env reads; those Facts stay in
`innerWriter`).

### Step 8 — close out `QueryGetType` on the apply result

The in-flight `QueryGetType{from=Q_apply_hash}` on
`innerWriter.inFlight` is closed by `logResult(R_int_type, …)`.
factSet at this moment = `Fact_file + the six ambient Facts above`.
This records:

```
Asks(QueryGetType_hash, ∅) → RS{ R_file, all six ambient Request hashes }
Terminals(QueryGetType_hash, factSetHash) → R_int_type
```

So the Q-getType-of-apply terminal pins down the ambient
interactions that were observed while computing it. *This* is the
recorded edge whose RequestSet a replay walks against the live
ambient context.

### Step 9 — `ExprFromObject::eval` extracts the int

`type = nInt` → `obj.getInt()`. This is another query against the
inner TracingObject; same shape as Step 8 but the in-flight
`QueryGetInt` terminates with `ResultInt{11}`. The outer Value
`v_outer` ends up as `mkInt(11)`.

### Step 10 — outer closes `Q_expr`

The whole expression evaluated to `11`. `tracingEval_outer.logResult(R_int_11, Q_expr)`:

- factSet at this moment for the outer = `{ Fact_file }` (the inner
  ambient stayed inner; the outer never saw it).
- Outer's `decisionGraph.record(Q_expr_hash, factSetHash, R_int_11_hash)`:
  - `Asks(Q_expr_hash, ∅) → RS{R_file}`
  - `Terminals(Q_expr_hash, fH) → R_int_11`

`nix eval` prints `11`. Done.

### What replay does on a warm cache

Second invocation, same expression text and same file contents.
The outer's `v13Walk(Q_expr_hash)`:

1. Fast path: `dispatchedTrie.diff(decisionGraph, RS{R_file}, …)`.
   `onlyInEdge = {R_file}`, `onlyInDispatched = {}`. Dispatches
   `R_file` (the file read), `xorFactIntoHash`, lands at
   `factSetHash` matching the recorded `Terminals(Q_expr_hash, …)`.
2. Returns `R_int_11` directly. `prim_cache` does *not* run.

The inner stack is never even constructed. The shared
`decisionGraph` carries enough state for the outer terminal to hit.

If, instead, the outer cache is cold but the inner is warm (e.g.
after `clearCache` on the outer JSON traces but the SQLite DB
persists), the outer falls into `prim_cache`, the inner walks each
of `Q_import`, `Q_apply`, `QueryGetType` against the live
filesystem and ambient resolver, and hits without re-evaluating
the lambda body. The ambient Fact resolution from §The fix:
producer query as id is what makes the apply-chain Facts
dispatchable when the recorded `from` refers to derived ids.

On the replay side, `ReplayLocalObject` reverses this: each
`maybeGetAttr` call queries the FactSet (or the in-walk producer
index) for a matching incoming Request and serves the recorded
response.

The integration point in `AmbientResolver::apply` becomes:

```cpp
auto argId = registerLocal(argObj);
auto wrappedArg = std::make_shared<TracingLocalObject>(
    argObj, argId, innerEvaluator->getEvalState().environment);
// argThunk wraps `wrappedArg`, not raw argObj, so outer accesses
// flow through the recording decorator.
auto * argExpr = new ExprFromObject(wrappedArg, innerEvaluator, shared_from_this());
```

Step E ships when `TracingLocalObject` and `ReplayLocalObject` are
both in place. Until then, the trace types are declared but no
producer/consumer exists, and covariant callbacks fall through to
inner each time — correct results, no cache speedup.
