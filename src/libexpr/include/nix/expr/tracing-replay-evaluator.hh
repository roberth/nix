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
     * dispatchQueryRequest → resolveStateHash. Holds the proxy
     * whose method triggered this history (so resolveStateHash can
     * history the parent / argCell chain on the proxy graph) plus a
     * per-history memo of ids already resolved. Lives only for the
     * duration of one history call — no cross-call leakage as
     * happened with the previous evaluator-global ambientState.
     */
    struct ResolutionContext
    {
        /** The proxy whose method triggered this history. Resolution
            walks this proxy's parent chain looking for matching
            argCell cells. Null for top-level entry points
            (evalFile, evalExpr) where no proxy exists yet. */
        std::shared_ptr<Object> currentProxy;
        /** Memoise id → resolved Object within this single history so
            recursive resolveStateHash calls don't redo work. */
        std::map<std::string, std::shared_ptr<Object>> memo;

    };

    /** Cumulative history across all history calls in this session.
        Each successfully committed Asks edge appends one entry,
        deduplicated by the edge's content-equal fact set so re-
        traversing a shared prefix doesn't double-fold. Mirrors the
        writer's `envWalk` — both grow per Asks edge ever
        committed, so `stateHashAt(subject, argAncestry, envWalk, K)`
        on the walker matches the writer's `stateHashAt` at the
        same edge K. This alignment is what makes per-fact `from`
        encodings reproducible at warm — without it, cell-chain
        state hash computation lands at the wrong edge index (= cb-385's
        original failure mode) and per-arg `from` lookups miss. */
    std::vector<ObservationSet> envWalk;
    /** Dedup committed edges by their elementHash-set fingerprint
        (= XOR-fold of fact element hashes within the edge). When
        a later history re-traverses an Asks edge already in
        envWalk (= shared prefix), commitEdge is a no-op. */
    std::unordered_set<Hash> committedEdgeFingerprints;

    /* Walks across the same process invocation re-dispatch the same
       Requests many times (each top-level lookup re-walks the shared
       prefix). Memoize requestHash -> responseHash so the file read +
       CBOR encode + SHA-256 happens once per request. */
    std::unordered_map<Hash, Hash> responseFor;

    /* Trace-continuing anchor: the session-cumulative cur — the
       factSet the last successful walk landed at. Combined with the
       session-scoped `envWalk` (which grows across walks under
       trace-continuing), this lets the walker follow a known trace:
       look up `getAsks(Q, envCur)` for the next Q, walk it lockstep,
       update `envCur` on hit. On miss the walker falls through to
       trace-discovering, which resets envWalk to per-walk (empty)
       scoping. See tracing-eval-cache.md §Replay strategies. */
    TracingDecisionGraph::SetHash envCur{TracingDecisionGraph::emptySetHash()};

    std::optional<std::string> dispatchQueryRequest(const nlohmann::json & reqJson, ResolutionContext & ctx);

    /** Resolve a recorded outer-value id (hex of a Hash) to a live
        Object. Arg ids are found by walking ctx.currentProxy's
        parent / argCell chain on the proxy graph; derived ids are
        looked up by their producer Request in the Requests pool and
        resolved recursively. Per-history memoisation in ctx.memo
        prevents redundant work within the same history. Returns
        nullptr if the id can't be resolved. */
    std::shared_ptr<Object> resolveStateHash(const std::string & idStr, ResolutionContext & ctx);

    std::shared_ptr<Object> resolveApplyId(const std::string & idStr, const nlohmann::json & params, ResolutionContext & ctx);

    /* Callback live invocation lives inside dispatchQueryRequest's
       callbackApply branch — materialise a ReplayCallbackArg from
       the recorded obsSet, then invoke fn->queryApply live. No
       separate live-fire method. */

    std::shared_ptr<Object> resolveProducerChild(const std::string & idStr, const std::string & tag, const nlohmann::json & params, ResolutionContext & ctx);

    template<typename Q>
    std::optional<std::pair<std::string, TriePosition>> lookup(const Q & query, std::shared_ptr<Object> currentProxy = nullptr);

public:
    TracingReplayEvaluator(
        ref<Evaluator> inner,
        Environment & validationEnv,
        TracingWriter & writer,
        TracingDecisionGraph & decisionGraph);

    /** Cumulative subject-id history on the walker, mirroring the writer's
        `envWalk`. Exposed so apply-result wrappers
        (TracingReplayObject with applyResultSubject) can compute
        `stateHashAt(subject, argAncestry, history, history.size())` and match the
        writer's evolved state hash at the same history index — the per-arg
        identity alignment principle #3 requires. */
    const std::vector<ObservationSet> & getCidasksWalk() const
    {
        return envWalk;
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
     * env var, outer-value probe, or QueryCallbackApply) by executing
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
    /* Task #110: if `payloadTemplate` and `fromSubject` are provided,
       the walker will re-derive Q's `from` field (via `stateHashAt`)
       after each Ask-edge commit, and lookup subsequent Ask/Terminal
       rows at the evolved Q. Matches the writer's Q-evolution protocol. */
    std::optional<WalkResult> walk(
        const Hash & queryHash,
        std::shared_ptr<Object> currentProxy = nullptr,
        std::optional<nlohmann::json> payloadTemplate = std::nullopt,
        std::optional<Subject> fromSubject = std::nullopt,
        Hash fromSubjectArgAncestry = Hash(HashAlgorithm::SHA256));

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
