#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-environment.hh"
#include "nix/expr/trace-file.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-writer.hh"

#include "nix/util/environment-variables.hh"

namespace nix {

/**
 * TracingWriter requires a TraceSink reference but builtins.cache only
 * needs the v13 decision graph for persistence. Refactoring so trie
 * recording works without a TraceSink would let this go away.
 */
class NullTraceSink final : public TraceSink
{
public:
    void log(const nlohmann::json &) override;
};

void NullTraceSink::log(const nlohmann::json &) {}

static void prim_cache(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    /* Arm the stats sidecar writer (no-op unless NIX_CACHE_STATS_FILE
       is set; idempotent across calls). Tests that assert argStateId-layer
       hit/miss counts rely on this. */
    armTracingCacheStatsExitWriter();

    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.cache");

    std::optional<SourcePath> importPath;
    std::optional<std::string> expr;
    std::optional<SourcePath> baseDir;

    for (auto & attr : *args[0]->attrs()) {
        auto n = state.symbols[attr.name];
        if (n == "import") {
            NixStringContext ctx;
            importPath.emplace(state.coerceToPath(
                attr.pos, *attr.value, ctx,
                "while evaluating the 'import' attribute passed to builtins.cache"));
        } else if (n == "expr") {
            expr.emplace(state.forceStringNoCtx(
                *attr.value, attr.pos, "while evaluating the 'expr' attribute passed to builtins.cache"));
        } else if (n == "baseDir") {
            NixStringContext ctx;
            baseDir.emplace(state.coerceToPath(
                attr.pos, *attr.value, ctx,
                "while evaluating the 'baseDir' attribute passed to builtins.cache"));
        } else {
            state.error<EvalError>("unsupported argument '%1%' to builtins.cache", n).atPos(attr.pos).debugThrow();
        }
    }

    if (importPath && expr)
        state.error<EvalError>("builtins.cache: 'import' and 'expr' are mutually exclusive").atPos(pos).debugThrow();
    if (!importPath && !expr)
        state.error<EvalError>("builtins.cache: either 'import' or 'expr' is required").atPos(pos).debugThrow();
    if (expr && !baseDir)
        state.error<EvalError>("builtins.cache: 'baseDir' is required when using 'expr'").atPos(pos).debugThrow();

    auto & cache = state.cacheState;

    // Share the EvalCommand-owned graph if present; otherwise lazily
    // construct an owned one and reuse it across subsequent cache calls
    // in this process.
    TracingDecisionGraph * decisionGraph;
    if (state.rootDecisionGraph) {
        decisionGraph = state.rootDecisionGraph;
    } else {
        if (!cache.ownedDecisionGraph)
            cache.ownedDecisionGraph = std::make_unique<TracingDecisionGraph>();
        decisionGraph = cache.ownedDecisionGraph.get();
    }

    // Per-call tracing infrastructure: TraceSink + TracingWriter →
    // decisionGraph. The writer records both queries/results and
    // environment responses (file reads, env lookups, ambient
    // interactions) into the graph for dependency tracking.
    //
    // When NIX_TRACE_CACHE_DIR is set, each prim_cache call also
    // writes a JSON-line trace into that directory (a fresh file per
    // call). Useful for debugging cache behaviour. Otherwise a
    // NullTraceSink discards events at the sink level (the decision
    // graph still records).
    std::shared_ptr<TraceSink> sink;
    if (auto traceDir = getEnv("NIX_TRACE_CACHE_DIR")) {
        static std::atomic<int> seq{0};
        auto fileName = fmt("cache-%d-%d.jsonl", getpid(), seq.fetch_add(1));
        std::filesystem::create_directories(*traceDir);
        sink = std::make_shared<TraceFile>(std::filesystem::path(*traceDir) / fileName);
    } else {
        sink = std::make_shared<NullTraceSink>();
    }
    auto writer = std::make_shared<TracingWriter>(*sink, decisionGraph);

    // Wrap the *outer* environment, not a fresh SystemEnvironment, so
    // file reads bubble up through the outer accessor chain
    // (input-traced nesting; required for nested builtins.cache
    // correctness).
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

    // Nested-builtins.cache propagation: a nested call running inside
    // this inner evaluator should share the same decisionGraph.
    innerState->rootDecisionGraph = decisionGraph;

    auto interpreter = make_ref<Interpreter>(innerState);

    // Evaluator stack: TracingReplayEvaluator → TracingEvaluator →
    // Interpreter. On cache hit, replay serves results from the graph.
    // On miss, recording falls through and writes new entries.
    ref<Evaluator> recordingEval = make_ref<TracingEvaluator>(*writer, interpreter);
    ref<Evaluator> replayEval = make_ref<TracingReplayEvaluator>(
        recordingEval, *state.environment, *writer, *decisionGraph);

    // Per-call state must outlive any lazy thunks the result attrset/list
    // is wrapped in (forced after prim_cache returns).
    cache.calls.push_back({
        .sink = sink,
        .writer = writer,
        .recordingEval = recordingEval.get_ptr(),
        .replayEval = replayEval.get_ptr(),
        .innerState = innerState,
    });

    // Shared resolver for ambient interactions; threads through every
    // <cached-fn>/<ambient-fn> PrimOp this call produces.
    auto resolver = makeAmbientResolver(&state, replayEval.get_ptr(), writer.get());
    interpreter->ambientResolver = resolver;
    /* Inherited scope for cidasks: uniquely identifies this cached
       call so sibling cached calls (different import / expr) get
       distinct scope state ids throughout the cb-apply boundary.
       XOR-fold with `state.inheritedCallScope` to accumulate
       across enclosing cached calls (= per via-asks
       `scopeStateId(LocalObject) = ... ⊕ argStateId(Q) ⊕ argStateId(Q_outer) ⊕
       ...`). For a top-level cached call, `inheritedCallScope`
       is 0 and this reduces to the own contribution. The inner
       EvalState (= cached body's evaluator) carries the combined
       value forward so deeper-nested cached calls accumulate
       further. */
    auto ownContribution = importPath
        ? hashString(HashAlgorithm::SHA256, "cache-import:" + importPath->path.abs())
        : hashString(HashAlgorithm::SHA256, "cache-expr:" + *expr + ":" + baseDir->path.abs());
    auto effectiveCallScope = TracingDecisionGraph::xorHashes(
        state.inheritedCallScope, ownContribution);
    setAmbientResolverCallScope(*resolver, effectiveCallScope);
    innerState->inheritedCallScope = effectiveCallScope;

    // Convert paths to use the inner accessor (TracingSourceAccessor)
    // so file reads are recorded as dependencies for invalidation.
    auto toInnerPath = [&](const SourcePath & p) { return RootedPath{innerState->rootFSRoot, p.path}; };

    ref<Object> result = importPath
        ? replayEval->evalFile(toInnerPath(*importPath), importPath->path.abs())
        : replayEval->evalExpr(*expr, toInnerPath(*baseDir));

    // Bridge the inner Object back to the outer Value via
    // ExprFromObject. Eager top-level eval (primops must produce a
    // concrete Value); attrset/list children become lazy
    // ExprFromObjectAttr / ExprFromObject thunks.
    ExprFromObject(result.get_ptr(), replayEval.get_ptr(), resolver).eval(state, state.baseEnv, v);
}

static RegisterPrimOp primop_cache({
    .name = "__cache",
    .args = {"args"},
    .doc = R"(
      Evaluate an expression in a separate evaluator with persistent caching.

      The argument is an attribute set with either:

      - `import` — a path to evaluate (equivalent to `import <path>`)
      - `expr` and `baseDir` — a Nix expression string and base directory

      The expression is evaluated in a fresh evaluator. Results are
      cached based on file content hashes and the inner evaluator's
      observed interactions; if nothing in the inner's transitive
      input set has changed, subsequent calls return cached results
      without re-evaluation.

      Each call creates a separate evaluator that parses files
      independently and performs cache I/O, so there is a per-call
      cost. This is intended for architectural boundaries where the
      cached expression is large relative to the overhead — e.g.
      wrapping a Nixpkgs import so project changes don't trigger
      Nixpkgs re-evaluation:

      ```nix
      let pkgs = builtins.cache { import = ./nixpkgs; };
      in pkgs.hello
      ```

      **Identity is not preserved across the cache boundary.**
      Direct comparison of two function values in Nix (`f == g`)
      always returns `false` regardless of how the functions were
      constructed; that's a language-level rule, not a cache
      concern. The wart is that when functions sit inside attrsets
      or lists being compared, the recursive equality falls back
      to pointer identity for the function-valued elements — so
      `{f = h;} == {f = h;}` is `true` because both `h` references
      resolve to the same closure pointer, but `{f = (x: x);} ==
      {f = (x: x);}` is `false` because each `x: x` evaluates to a
      fresh closure. Values bridged in or out of `builtins.cache`
      are reconstructed on each crossing, so attrset / list
      equalities that depend on a function reaching both sides of
      the comparison with the same pointer can flip from `true` to
      `false` when one side passes through the cache. Cached code
      that relies on this transitive pointer identity will
      misbehave.

      Values aren't eagerly deep-copied. Both directions of
      crossing are on-demand: what the inner observes gets
      recorded into the index, what the outer accesses gets
      materialised back through the bridge. A cached expression
      that returns a large value and is asked for only a few attrs
      pays only for those attrs. But: passing the same large value
      *into* a cached call and asking for it back forces both
      copies — one into the index for the observations the inner
      makes, one back across the bridge for the outer's accesses.
      The cache is designed for results consumed by content, not
      for threading large values through unchanged.
    )",
    .impl = prim_cache,
    .experimentalFeature = Xp::TracingEvalCache,
});

} // namespace nix
