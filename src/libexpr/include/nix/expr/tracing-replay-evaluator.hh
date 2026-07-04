#pragma once

#include "nix/expr/content-identity-via-asks.hh"
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
 * Evaluator that replays cached results from the v13 decision graph.
 * On cache miss, defers to the inner evaluator.
 */
class TracingReplayEvaluator : public Evaluator
{
    ref<Evaluator> inner;
    TracingDecisionGraph & decisionGraph;
    TracingWriter & writer;
    Environment & validationEnv;

    /**
     * Per-walk resolution context.
     *
     * Threaded through v13Walk → dispatch → getCurrentResponse →
     * dispatchAmbientQuery → resolveCdiId. Holds the proxy
     * whose method triggered this walk (so resolveCdiId can
     * walk the parent / argScope chain on the proxy graph) plus a
     * per-walk memo of ids already resolved. Lives only for the
     * duration of one v13Walk call — no cross-call leakage as
     * happened with the previous evaluator-global ambientState.
     */
    struct ResolutionContext
    {
        /** The proxy whose method triggered this walk. Resolution
            walks this proxy's parent chain looking for matching
            argScope cells. Null for top-level entry points
            (evalFile, evalExpr) where no proxy exists yet. */
        std::shared_ptr<Object> currentProxy;
        /** Memoise id → resolved Object within this single walk so
            recursive resolveCdiId calls don't redo work. */
        std::map<std::string, std::shared_ptr<Object>> memo;

        /** Current v13Walk's queryHash. */
        Hash currentQueryHash{HashAlgorithm::SHA256};

        /** Per-applyReqHash dispatch counter within this walk. Used
            to compute the LocalResponseMap context: same applyReqHash
            dispatched multiple times (cb-repeated's PositionalSeed-
            abstracted (cb 10)/(cb 20) sharing one applyReqHash) gets
            distinct LRM lookup keys via seq=0, seq=1, ... Symmetric
            with cold's per-boundary counter in the finalize pass. */
        std::unordered_map<Hash, size_t> perApplyReqDispatchCount;

        /** Per-(applyReqHash, walkerCur) → assigned seq. Fixes
            speculative-retry-inflation: walker's walk() can dispatch
            the same apply Request MULTIPLE times at the SAME cur
            (via apply-bypass fallback trying alternate branches).
            Under bare `perApplyReqDispatchCount++`, each retry gets
            a new seq, blowing past cold's actual per-boundary count.
            Assigning seq based on the UNIQUE walkerCur observed keeps
            walker's seq aligned with cold's per-boundary count:
            first unique cur → seq 0 (matches cold's boundary 0), etc.
            Retries at the same cur reuse the previously-assigned seq.
            Keyed as hex-concatenation of the two hashes for stable
            hashing. */
        std::unordered_map<std::string, size_t> assignedApplySeq;

        /** Pointer to walk()'s pendingEdgeObservations vector, or
            nullptr if resolveCdiId is called outside a walk. When set,
            resolveCdiId's extendedWalkForMatch includes these
            not-yet-committed observations as a virtual final edge.
            Rationale: cold's writer accumulates observations
            in-order during its flush chain — seed(1) evolved via
            (getAttr cb, getWHNF cb) before Q's applyResult-CID request
            was recorded. Warm's walker dispatches these facts during
            walk() but stores them in pendingEdgeObservations until
            commit; if resolveCdiId is invoked mid-walk (which it is
            for `from`-CID resolution of subsequent dispatches within
            the same Asks edge), reading only committed cidasksWalk
            misses these in-flight folds and can't resolve cold's
            evolved-state CIDs. Threading pending obs through
            extendedWalkForMatch closes that gap. */
        const std::vector<cidasks::Observation> * pendingEdgeObservations = nullptr;
    };

    /** Cumulative walk across all v13Walk calls in this session.
        Each successfully committed Asks edge appends one entry,
        deduplicated by the edge's content-equal fact set so re-
        traversing a shared prefix doesn't double-fold. Mirrors the
        writer's `d1CidasksWalk` — both grow per Asks edge ever
        committed, so `scopeStateIdAt(subject, scope, cidasksWalk, K)`
        on the walker matches the writer's `scopeStateIdAt` at the
        same edge K. This alignment is what makes per-fact `from`
        encodings reproducible at warm — without it, cell-chain
        scopeStateId computation lands at the wrong edge index (= cb-385's
        original failure mode) and per-arg `from` lookups miss. */
    std::vector<cidasks::Edge> cidasksWalk;
    /** Dedup committed edges by their elementHash-set fingerprint
        (= XOR-fold of fact element hashes within the edge). When
        a later v13Walk re-traverses an Asks edge already in
        cidasksWalk (= shared prefix), commitEdge is a no-op. */
    std::unordered_set<Hash> committedEdgeFingerprints;

    /* Walks across the same process invocation re-dispatch the same
       Requests many times (each top-level lookup re-walks the shared
       prefix). Memoize requestHash -> responseHash so the file read +
       CBOR encode + SHA-256 happens once per request. */
    std::unordered_map<Hash, Hash> dispatchCache;

    /* Replay-side "where we left off" — see design comment in
       v13Walk. lastQFactsHash is the cur where the last successful
       walk landed; dispatchedTrie is the cumulative set of requests
       we've dispatched (or whose responses we've taken from
       dispatchCache) in this process. Together they let the next
       walk skip the shared prefix via a trie-diff against the new
       Q's recorded RS. */
    TracingDecisionGraph::SetHash lastQFactsHash;
    TracingDecisionGraph::TrieBuilder dispatchedTrie;
    /** Flat set of dispatched request hashes that contributed to
        `lastQFactsHash`. Used as `startCurRequests` for the slow
        walk() fallback so it doesn't re-dispatch already-folded
        reqs (= which would XOR-cancel them out of cur). */
    std::unordered_set<TracingDecisionGraph::RequestHash> dispatchedRequestSet;

    /** applyReqHashes currently being driven by `dispatchApplyLive`.
        Short-circuits walker re-entry while outer's-f-invocation is
        still routed through TracingReplayEvaluator::apply. TODO:
        drop once invocation goes through a path that doesn't re-enter
        the d=1 walker (= live Interpreter::apply against the
        reconstructed value tree). */
    std::unordered_set<TracingDecisionGraph::RequestHash> inFlightApplyReqs;

    /** Dedup set for the writer.suppressedBoundaryHook. Cold's inner
        emits ONE ε obs per unique cb-apply boundary; walker's
        dispatchApplyLive fires cb-fn multiple times per session, so
        the hook may see the same applyReqHash repeatedly. */
    std::unordered_set<Hash> suppressedBoundaryEpsilonsSeen;

    std::optional<std::string> dispatchAmbientQuery(const nlohmann::json & reqJson, ResolutionContext & ctx);

    /** Resolve a recorded ambient id (hex of a Hash) to a live
        Object. Seed ids are found by walking ctx.currentProxy's
        parent / argScope chain on the proxy graph; derived ids are
        looked up by their producer Request in the Requests pool and
        resolved recursively. Per-walk memoisation in ctx.memo
        prevents redundant work within the same walk. Returns
        nullptr if the id can't be resolved. */
    std::shared_ptr<Object> resolveCdiId(const std::string & idStr, ResolutionContext & ctx);

    /* The direction-/payload-specific branches of resolveCdiId,
       extracted so each branch's discipline is named and visible.
       chaseLocalArgSidecar is Local-direction-specific (frozen, served
       from pool); resolveApplyId mixes direction (fn is Outer, arg may
       be either); resolveProducerChild is Outer-direction-specific
       (live navigation through the proxy graph). */
    std::shared_ptr<Object> chaseLocalArgSidecar(const std::string & idStr, const nlohmann::json & reqJson, ResolutionContext & ctx);

    /** True iff the id resolves as a Local — either it has no
        producer Request in the pool (a TracingLocalObject's content
        hash whose id isn't itself a recorded query), or its pool
        payload is a localArg sidecar, or the payload fails to parse
        (defensive fallback). False for any Outer-direction id with a
        parseable producer query payload. */
    bool isLocalArgId(const Hash & idHash);
    std::shared_ptr<Object> resolveApplyId(const std::string & idStr, const nlohmann::json & params, ResolutionContext & ctx);

    /** d=2 live AmbientResult computation for a cb-apply Fact
        dispatch. Materialises a fresh standin rooted at
        `applyReqHash`, invokes `fn->queryApply(standin)` live, then
        FORCES the apply result (= via `getType()`) so outer's `f`
        actually evaluates and drives probes against the standin
        through `ExprFromObject`'s bridge thunk. Per-probe
        `validateAgainstAmbientAsks` walks the recorded chain;
        divergence throws and is caught here. Returns
        `std::nullopt` on divergence so the d=1 dispatch fails.

        Returns the standin's terminal `chainCursor` — the
        AmbientResult to fold into d=1 cur as the cb-apply
        Request's respHash. No memoisation: per via-Asks principle
        9, each dispatch re-invokes fn fresh. */
    std::optional<Hash> dispatchApplyLive(
        const Hash & applyReqHash,
        const nlohmann::json & params,
        const Hash & walkerCur,
        ResolutionContext & ctx);

    std::shared_ptr<Object> resolveProducerChild(const std::string & idStr, const std::string & tag, const nlohmann::json & params, ResolutionContext & ctx);

    template<typename Q>
    std::optional<std::pair<std::string, TriePosition>> lookup(const Q & query, std::shared_ptr<Object> currentProxy = nullptr);

public:
    TracingReplayEvaluator(
        ref<Evaluator> inner,
        Environment & validationEnv,
        TracingWriter & writer,
        TracingDecisionGraph & decisionGraph);

    /** Cumulative cidasks walk on the walker, mirroring the writer's
        `d1CidasksWalk`. Exposed so apply-result wrappers
        (TracingReplayObject with applyResultSubject) can compute
        `scopeStateIdAt(subject, scope, walk, walk.size())` and match the
        writer's evolved scopeStateId at the same walk index — the per-arg
        identity alignment principle #3 requires. */
    const std::vector<cidasks::Edge> & getCidasksWalk() const
    {
        return cidasksWalk;
    }

    /** Access the shared TracingWriter. Used by TracingReplayObject's
        `evolvedQueryFrom` to read the writer's `d1CidasksWalk`
        directly — single source of truth for the cumulative walk on
        both writer and walker sides, so option-2 encoding can't drift
        between the two. */
    TracingWriter & getWriter() const
    {
        return writer;
    }

    /**
     * Compute the current response for a recorded request (file hash,
     * env var, or ambient interaction) by executing against the
     * current validation environment. Ambient queries route through
     * proxy-graph resolution using `ctx`.
     */
    std::optional<std::string> getCurrentResponse(const std::string & requestCbor, ResolutionContext & ctx);

    /**
     * v13 walk lookup. Returns (resultPayload, resultHash) on hit,
     * nullopt on miss. `currentProxy` is the cache-boundary proxy
     * whose method triggered this walk — its parent/argScope chain
     * grounds ambient id resolution during dispatch. Null for
     * top-level entry points (evalFile/evalExpr) that have no
     * proxy yet.
     */
    /** Returned (payload, resultNodeHash, terminalCur). `terminalCur`
        is the factSet the walker landed on when committing the
        Terminal — child Q lookups thread it through their TracingReplayObject's
        TriePosition.factSetHash and use it as their structural-anchor
        candidate startCur. */
    struct V13WalkResult { std::string payload; Hash resultNodeHash; Hash terminalCur; };
    std::optional<V13WalkResult> v13Walk(const Hash & queryHash, std::shared_ptr<Object> currentProxy = nullptr);

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
    std::shared_ptr<struct AmbientResolver> getAmbientResolver() override
    {
        return inner->getAmbientResolver();
    }
};

} // namespace nix
