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

/**
 * TracingWriter requires a TraceSink reference but builtins.cache only
 * needs the v13 decision graph for persistence. Refactoring so trie
 * recording works without a TraceSink would let this go away.
 */
class NullTraceSink final : public TraceSink
{
public:
    void log(const nlohmann::json &) override {}
};

static void prim_cache(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
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

    // Per-call tracing infrastructure: NullTraceSink + TracingWriter →
    // decisionGraph. The writer records both queries/results and
    // environment responses (file reads, env lookups, ambient
    // interactions) into the graph for dependency tracking.
    auto sink = std::make_shared<NullTraceSink>();
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
    auto resolver = makeAmbientResolver(&state, replayEval.get_ptr());
    interpreter->ambientResolver = resolver;

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
    )",
    .impl = prim_cache,
    .experimentalFeature = Xp::TracingEvalCache,
});

} // namespace nix
