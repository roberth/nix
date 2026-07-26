#pragma once

#include "nix/expr/subject-id.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/ref.hh"

#include <map>
#include <unordered_map>
#include <vector>

namespace nix {

class Environment;
struct ArgCell;

/**
 * Evaluator that replays cached results from the decision graph.
 * On cache miss, defers to the inner evaluator.
 */
class TracingReplayEvaluator : public Evaluator
{
    ref<Evaluator> inner;
    TracingDecisionGraph & decisionGraph;
    TracingWriter & writer;
    Environment & validationEnv;

    /**
     * Per-history resolution context.
     *
     * Threaded through history → dispatch → getCurrentResponse →
     * dispatchQueryRequest → resolveIdentity. Holds the proxy
     * whose method triggered this history (so resolveIdentity can
     * history the parent / argCell chain on the proxy graph) plus a
     * per-history memo of ids already resolved. Lives only for the
     * duration of one history call — no cross-call leakage from a
     * shared evaluator-scoped state.
     */
    struct ResolutionContext
    {
        /** The proxy whose method triggered this history. Resolution
            walks this proxy's parent chain looking for matching
            argCell cells. Null for top-level entry points
            (evalFile, evalExpr) where no proxy exists yet. */
        std::shared_ptr<Object> currentProxy;
        /** Phase F: the active walk's cell, whose chain includes the
            arg (or root value) the walk is about — reachable ancestrally
            through parent-cell links. Distinct from currentProxy.argCell
            because the applyResult / root value doesn't exist as a
            proxy yet at lookup time; the cell is what the caller has
            constructed to represent it. Resolution walks this chain
            FIRST, then falls back to currentProxy.argCell. Null when
            no cell is provided to walk(). */
        std::shared_ptr<const ArgCell> walkCell;
        /** Memoise id → resolved Object within this single history so
            recursive resolveIdentity calls don't redo work. */
        std::map<std::string, std::shared_ptr<Object>> memo;

    };

    /* Phase F: envWalk / envCur / responseFor / committedEdgeFingerprints
       migrated to `SessionState` (defined in q-state.hh) held via
       shared_ptr on `QState::session`. Cells within an active tree
       share the same SessionState by inheritance through parent-cell
       qState at walk-start. Switching active trees = switching qState =
       switching SessionState; no shared TRE state to trample under the
       concurrency invariant. See task #168. */

    std::optional<std::string> dispatchQueryRequest(const nlohmann::json & reqJson, ResolutionContext & ctx);

    /** Resolve a recorded outer-value id (hex of a Hash) to a live
        Object. Arg ids are found by walking ctx.currentProxy's
        parent / argCell chain on the proxy graph; derived ids are
        looked up by their producer Request in the Requests pool and
        resolved recursively. Per-history memoisation in ctx.memo
        prevents redundant work within the same history. Returns
        nullptr if the id can't be resolved. */
    std::shared_ptr<Object> resolveIdentity(const std::string & idStr, ResolutionContext & ctx);

    std::shared_ptr<Object> resolveApplyId(const std::string & idStr, const nlohmann::json & params, ResolutionContext & ctx);

    /* Callback live invocation lives inside dispatchQueryRequest's
       callbackApply branch — materialise a ReplayCallbackArg from
       the recorded obsSet, then invoke fn->queryApply live. No
       separate live-fire method. */

    std::shared_ptr<Object> resolveProducerChild(const std::string & idStr, const trace::SelectorVariant & qv, const nlohmann::json & params, ResolutionContext & ctx);

    template<typename Q>
    std::optional<std::pair<std::string, TriePosition>> lookup(
        const Q & query,
        std::shared_ptr<Object> currentProxy = nullptr,
        std::shared_ptr<const ArgCell> cell = nullptr);

public:
    TracingReplayEvaluator(
        ref<Evaluator> inner,
        Environment & validationEnv,
        TracingWriter & writer,
        TracingDecisionGraph & decisionGraph);

    /** Phase D2: getters do direct Terminal lookups (no walk), so
        TRO needs access to the decision graph. */
    TracingDecisionGraph & getDecisionGraph() const
    {
        return decisionGraph;
    }

    /** Access the shared TracingWriter. Used by TracingReplayObject's
        `evolvedQueryFrom` to read the writer's `envWalk`
        directly — single source of truth for the cumulative history on
        both writer and walker sides, so option-2 encoding can't drift
        between the two. */
    TracingWriter & getWriter() const
    {
        return writer;
    }

    /**
     * Compute the current response for a recorded request (file hash,
     * env var, outer-value probe, or SelectorCallbackApply) by executing
     * against the current validation environment. Query-carrying
     * requests route through proxy-graph resolution using `ctx`.
     */
    std::optional<std::string> getCurrentResponse(const std::string & requestCbor, ResolutionContext & ctx);

    /**
     * history lookup. Returns (resultPayload, resultHash) on hit,
     * nullopt on miss. `currentProxy` is the cache-boundary proxy
     * whose method triggered this history — its parent/argCell chain
     * grounds outer-value id resolution during dispatch. Null for
     * top-level entry points (evalFile/evalExpr) that have no
     * proxy yet.
     */
    /** Returned (payload, resultNodeHash, terminalCur). `terminalCur`
        is the factSet the walker landed on when committing the
        Terminal — child Q lookups thread it through their TracingReplayObject's
        TriePosition.factSetHash and use it as their structural-anchor
        candidate startCur. */
    struct WalkResult { std::string payload; Hash resultNodeHash; Hash terminalCur; };
    /* #178: Q evolution retired. Q hashes are stable per operation;
       walker doesn't re-derive `from` fields during traversal. */
    std::optional<WalkResult> walk(
        const Hash & selectorHash,
        std::shared_ptr<Object> currentProxy = nullptr,
        std::shared_ptr<const ArgCell> cell = nullptr);

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;
    EvalState & getEvalState() override;

    ref<Object> evalFile(const RootedPath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const RootedPath & basePath) override;
    ref<Object> evalExprLazy(const std::string & expr, const RootedPath & basePath) override;
    ref<Object> mkString(const std::string & s) override;
    ref<Object> mkInt(NixInt i) override;
    ref<Object> mkBool(bool b) override;
    ref<Object> mkPath(const RootedPath & path) override;
    ref<Object> mkAttrs(const std::map<std::string, ref<Object>> & attrs) override;
    ref<Object> getInternalPrimOp(const std::string & name) override;
    ref<Object> apply(ref<Object> fn, ref<Object> arg) override;
    std::shared_ptr<struct OuterResolver> getOuterResolver() override
    {
        return inner->getOuterResolver();
    }
};

} // namespace nix
