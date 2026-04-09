#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/expr/tracing-environment.hh"
#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-writer.hh"

namespace nix {

/**
 * Noop TraceSink — TracingWriter requires a TraceSink reference but
 * builtins.cache only needs the TracingIndex (SQLite trie) for persistence.
 */
class NullTraceSink : public TraceSink
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
            NixStringContext context;
            importPath.emplace(state.coerceToPath(
                attr.pos, *attr.value, context, "while evaluating the 'import' attribute passed to builtins.cache"));
        } else if (n == "expr") {
            expr.emplace(state.forceStringNoCtx(
                *attr.value, attr.pos, "while evaluating the 'expr' attribute passed to builtins.cache"));
        } else if (n == "baseDir") {
            NixStringContext context;
            baseDir.emplace(state.coerceToPath(
                attr.pos, *attr.value, context, "while evaluating the 'baseDir' attribute passed to builtins.cache"));
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

    // Use the root TracingIndex if available (shared with the root evaluator),
    // otherwise create one (lazy, first cache call creates it).
    TracingIndex * index;
    if (state.rootTracingIndex) {
        index = state.rootTracingIndex;
    } else {
        if (!cache.ownedTracingIndex)
            cache.ownedTracingIndex = std::make_unique<TracingIndex>();
        index = cache.ownedTracingIndex.get();
    }

    // Per-call tracing infrastructure: NullTraceSink + TracingWriter → TracingIndex.
    // The TracingWriter records both queries/results and environment responses
    // (file reads, env lookups) into the trie for dependency tracking.
    // NullTraceSink exists because TracingWriter conflates JSON tracing and trie
    // recording — see "TeeTracingWriter" in project docs for the planned fix.
    auto sink = std::make_shared<NullTraceSink>();
    auto writer = std::make_shared<TracingWriter>(*sink, index);

    // Create inner EvalState with TracingEnvironment wrapping the outer
    // environment so file reads flow through the outer accessor chain.
    // This ensures the outer trace (if any) sees inner file reads as
    // dependencies — required for correctness when the outer evaluator
    // is itself cached.
    auto tracingEnv = make_ref<TracingEnvironment>(state.environment, *writer);
    auto innerState =
        make_ref<EvalState>(LookupPath{}, state.fetchSettings, state.settings, tracingEnv, state.systemEnvironment);

    auto interpreter = make_ref<Interpreter>(innerState);

    // Evaluator stack: TracingReplayEvaluator → TracingEvaluator → Interpreter
    // On cache hit, TracingReplayEvaluator returns results from the trie.
    // On miss, TracingEvaluator records into the trie via TracingWriter.
    ref<Evaluator> recordingEval = make_ref<TracingEvaluator>(*writer, interpreter);
    ref<Evaluator> replayEval =
        make_ref<TracingReplayEvaluator>(recordingEval, *index, *state.environment);

    // Store per-call state on the outer EvalState so it outlives Object references.
    // TracingObjects and TracingReplayObjects hold raw references to these.
    cache.calls.push_back({
        .sink = sink,
        .writer = writer,
        .recordingEval = recordingEval.get_ptr(),
        .replayEval = replayEval.get_ptr(),
        .innerState = innerState,
    });

    // Convert paths to use the inner EvalState's rootFS (TracingSourceAccessor)
    // so file reads are recorded as dependencies for cache invalidation.
    // The original paths carry the outer EvalState's accessor.
    auto innerRootFS = tracingEnv->fsRoot();
    auto toInnerPath = [&](const SourcePath & p) { return SourcePath(innerRootFS, p.path); };

    // Evaluate in the replay evaluator (tries cache first, falls back to recording)
    auto displayName = importPath ? importPath->path.abs() : *expr;
    tracingCacheLog("builtins.cache: evaluating %s", displayName);

    ref<Object> result = importPath ? replayEval->evalFile(toInnerPath(*importPath), importPath->path.abs())
                                    : replayEval->evalExpr(*expr, toInnerPath(*baseDir));

    tracingCacheLog("builtins.cache: done evaluating %s", displayName);

    // Bridge back to outer evaluator via ExprFromObject.
    // Evaluate eagerly — primops must not return thunks (forceValue
    // doesn't recurse into thunks-inside-thunks). Child attrset/list
    // elements are still lazy (ExprFromObject creates child thunks).
    ExprFromObject(result.get_ptr(), replayEval.get_ptr()).eval(state, state.baseEnv, v);
}

static RegisterPrimOp primop_cache({
    .name = "__cache",
    .args = {"args"},
    .doc = R"(
      Evaluate an expression in a separate evaluator with its own caching.

      The argument is an attribute set with either:

      - `import` — a path to evaluate (equivalent to `import <path>`)
      - `expr` and `baseDir` — a Nix expression string and base directory

      The expression is evaluated in a fresh evaluator. Results are cached
      based on file content hashes — if the source files haven't changed,
      subsequent evaluations return cached results without re-evaluation.

      Each call creates a separate evaluator that parses files independently
      and performs cache I/O, so there is a per-call cost. This is intended
      for architectural boundaries where the cached expression is large
      relative to the overhead — e.g. wrapping a Nixpkgs import so that
      project changes don't trigger Nixpkgs re-evaluation:

      ```nix
      let pkgs = builtins.cache { import = ./nixpkgs; };
      in pkgs.hello
      ```
    )",
    .impl = prim_cache,
    .experimentalFeature = Xp::TracingEvalCache,
});

} // namespace nix
