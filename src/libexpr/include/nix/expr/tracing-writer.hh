#pragma once
/**
 * @file
 * Trace writer that logs evaluation events to a JSON sink and the
 * v13 decision-graph index.
 */

#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/ref.hh"

#include <map>
#include <optional>
#include <unordered_set>
#include <vector>

namespace nix {

class Object;

/**
 * Serialize a JSON value to CBOR as a std::string (for v13 payload storage).
 */
inline std::string jsonToCborString(const nlohmann::json & j)
{
    auto cbor = nlohmann::json::to_cbor(j);
    return std::string(reinterpret_cast<const char *>(cbor.data()), cbor.size());
}

/**
 * Parse a CBOR blob (stored as std::string) back to JSON.
 */
inline nlohmann::json cborStringToJson(const std::string & s)
{
    auto bytes = reinterpret_cast<const uint8_t *>(s.data());
    return nlohmann::json::from_cbor(bytes, bytes + s.size());
}

/**
 * A handle identifying a recorded d=0 Result, kept for back-compat with
 * a hex string of the queryHash that produced it (used by child queries
 * to compute their own queryHash with Merkle provenance).
 */
struct TriePosition
{
    Hash resultNodeHash;          // v13 ResultHash for this result
    std::string queryHashStr; // hex of the queryHash that produced it
};

/**
 * Trace writer: logs evaluation events to a JSON sink and records
 * them in the v13 decision graph.
 */
class TracingWriter
{
    TraceSink & sink;
    /* v13 decision-graph index. nullptr disables decision-graph
       recording (sink-only mode). */
    TracingDecisionGraph * decisionGraph;
    /* v13 global factSet, accumulating monotonically across the
       session per the design doc. Sampled at each logResult and
       fed into decisionGraph->record(). Only d>0 (Request, Response)
       Facts are added; d=0 Q→R pairs are not (the walk dispatch
       can't fetch them).

       The factSet hash is maintained incrementally via XOR-fold on
       each new Fact, and a seenRequests set dedupes per request.
       This makes the per-logResult cost O(1) instead of O(|factSet|)
       for the hash computation: insertFactSet (which would re-sort
       and re-fold all members) is bypassed via primeFactSetCache. */
    std::vector<TracingDecisionGraph::Fact> v13FactSet;
    TracingDecisionGraph::SetHash v13FactSetHash;
    std::unordered_set<Hash> seenRequests;
    /* request → response lookup, maintained as facts arrive.
       Handed to record() by reference so it doesn't rebuild
       O(N) per call. */
    std::unordered_map<Hash, Hash> responseFor;
    /* Incremental trie of allRequests; gives record() the canonical
       RequestSet hash for the whole-remaining edge in O(1). */
    TracingDecisionGraph::TrieBuilder allRequestsTrie;

    /* Ambient facts buffered during recording and flushed at
       logResult time via flushPendingAmbient. The Subject identifies
       which value the observation is about — flush uses it via
       cidasks::contentIdAt to compute the fact's `from` field
       against the relevant Asks-edge precondition factset. */
    struct PendingFact
    {
        trace::QueryVariant query;
        trace::ResultVariant result;
        cidasks::Subject subject;
        Hash inheritedScope; ///< outer-scope CDIs for contentIdAt
    };
    std::vector<PendingFact> pendingFacts;

    struct PendingRequest
    {
        nlohmann::json payload;
        std::optional<std::string> keyPlaceholder;
    };
    std::vector<PendingRequest> pendingRequests;

public:
    TracingWriter(TraceSink & sink, TracingDecisionGraph * decisionGraph = nullptr)
        : sink(sink)
        , decisionGraph(decisionGraph)
        , v13FactSetHash(TracingDecisionGraph::emptySetHash())
    {
    }

    /**
     * Opaque handle linking a query to its result.
     */
    struct QueryHandle
    {
        std::optional<Hash> queryHash;
    };

    /**
     * Log a root query (evalFile, evalExpr, apply).
     * Returns (valueHandle, queryHandle) so the caller can pass queryHandle to logResult.
     */
    template<typename Q>
    std::pair<ValueHandle, QueryHandle> logRootQuery(const Q & query)
    {
        auto valueNum = sink.logQuery(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto queryHash = TracingDecisionGraph::computeQueryHash(query);
        return {valueNum, {queryHash}};
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.).
     * The query's `from` field must contain the parent's queryHash
     * (Merkle identity).
     */
    template<typename Q>
    std::pair<ValueHandle, QueryHandle> logQuery(const Q & query, const std::optional<TriePosition> & /*parent*/)
    {
        auto valueNum = sink.logQuery(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto queryHash = TracingDecisionGraph::computeQueryHash(query);
        return {valueNum, {queryHash}};
    }

    /**
     * Log a response (file read, env lookup, etc.) — a d>0
     * Request/Response pair. Appended to v13 factSet for the next
     * Result's recording, and the Request/Response payloads land
     * in v13's atomic pools.
     */
    template<typename Req>
    void logResponse(const trace::Response<Req> & resp)
    {
        sink.log(nlohmann::json(resp));
        if (!decisionGraph)
            return;
        nlohmann::json reqJson = resp.request;
        nlohmann::json respJson = resp.response;
        auto queryHash = TracingDecisionGraph::computeQueryHash(resp.request);
        auto responsePayload = jsonToCborString(respJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
        decisionGraph->insertRequest(queryHash, jsonToCborString(reqJson));
        if (storeAllResponsePayloads)
            decisionGraph->insertResponse(queryHash, responsePayload);
        /* Dedupe by (request, response) pair, not request alone.
           Idempotent observations (same request, same response —
           e.g. file reads, env reads) collapse to one entry; sibling
           cb applies (same request, different responses) keep both
           contributions so v13FactSetHash reflects both elementHashes
           and the trie's per-(Q, factSet) terminals don't collide. */
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), queryHash, responseHash);
        if (seenRequests.insert(factHash).second) {
            v13FactSet.push_back({queryHash, responseHash});
            v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                v13FactSetHash, queryHash, responseHash);
            responseFor.emplace(queryHash, responseHash);
            allRequestsTrie.insert(queryHash);
        }
    }

    /**
     * Log an ambient interaction as a d>0 Request/Response pair.
     *
     * Under Phase 4 of content-defined identity, ambient facts are
     * buffered here rather than eagerly inserted into v13FactSet and
     * the Requests/Responses pools — the `from` field of the query
     * may be a placeholder (counter-derived local id) whose final
     * Buffered until flushPendingAmbient() at logResult time
     * inserts into the pool at the query payload's natural reqHash. */
    void logAmbientInteraction(
        const trace::QueryVariant & query,
        const trace::ResultVariant & result,
        cidasks::Subject subject,
        Hash inheritedScope = Hash(HashAlgorithm::SHA256))
    {
        if (!decisionGraph)
            return;
        pendingFacts.push_back({query, result, std::move(subject), std::move(inheritedScope)});
    }

    /**
     * Note an environment observation made by the walker during a
     * cache hit's dispatch. The walker calls dispatch live to verify
     * that recorded paths still hold against the current environment;
     * each `(request, response)` it computes is a real observation of
     * the environment, just like one made via `logResponse` or
     * `logAmbientInteraction` during interpretation. Feeding it back
     * into `v13FactSet` keeps the writer's cumulative state invariant
     * to whether facts came via interpretation or cache-hit dispatch.
     * Without this, a subsequent `logResult` for some Q that fell
     * back to inner would record at a factSetHash missing the
     * walker's prior dispatches — creating a sibling Asks chain and
     * disqualifying single-edge fast paths on future warms.
     */
    void noteEnvObservation(const Hash & request, const Hash & response)
    {
        if (!decisionGraph)
            return;
        /* Same dedupe semantics as logResponse — see commentary
           there. */
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), request, response);
        if (seenRequests.insert(factHash).second) {
            v13FactSet.push_back({request, response});
            v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                v13FactSetHash, request, response);
            responseFor.emplace(request, response);
            allRequestsTrie.insert(request);
        }
    }

    /**
     * Defer a Requests-pool insert until logResult.
     *
     * AmbientResolver::apply uses this to register the QueryApply
     * Request and the localArg sidecar. At flush: if `keyPlaceholder`
     * is set the insert key is that key (the local's content id);
     * otherwise the insert key is the hash of the payload (the apply
     * Q's own queryHash).
     */
    void deferRequest(nlohmann::json payload, std::optional<std::string> keyPlaceholder = std::nullopt)
    {
        if (!decisionGraph)
            return;
        pendingRequests.push_back({std::move(payload), std::move(keyPlaceholder)});
    }

    /**
     * Flush buffered ambient facts and Requests into the pool at
     * their natural reqHashes. Called from logResult, before
     * record(). Under the via-Asks design, facts carry positional
     * initial content ids in `from`; no per-fact substitution.
     */
    void flushPendingAmbient();

    /**
     * When true, every file-read / env-var response payload gets
     * persisted into the decisionGraph's Responses pool too. Useful
     * for offline debugging when JSON traces aren't available.
     * Default false to keep environment storage as v13 designed it.
     */
    bool storeAllResponsePayloads = false;

    /**
     * Log a d=0 Result. Records (Q, current factSet) -> Result in
     * the v13 decision graph and returns a TriePosition for use by
     * child queries.
     */
    template<typename R>
    std::optional<TriePosition> logResult(ValueHandle valueNum, const R & result, const QueryHandle & qh)
    {
        sink.logResult(valueNum, result);

        if (!decisionGraph || !qh.queryHash)
            return std::nullopt;

        /* Insert buffered ambient facts/Requests into the pool and
           populate the v13FactSet structures record() reads below. */
        flushPendingAmbient();

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph->insertResult(resultNodeHash, resultPayload);

        /* v13FactSetHash is maintained incrementally per fact; skip
           insertFactSet's O(N log N) sort + fold. primeFactSetCache
           makes the members available to record() via getFactSet
           without rebuilding the hash. responseFor + seenRequests
           are passed by reference so record() doesn't re-build its
           per-call lookup map and remaining set.

           allRequestsTrie is maintained incrementally per fact and
           gives us the canonical RequestSet root hash for the
           current allRequests in O(1). Persist any unwritten nodes
           and hand the root hash to record() as the precomputed RS
           hash for the whole-remaining edge — record() can then
           skip its insertRequestSet(remainingVec) call. */
        decisionGraph->primeFactSetCache(v13FactSetHash, v13FactSet);
        allRequestsTrie.persist(*decisionGraph);
        decisionGraph->record(*qh.queryHash, v13FactSetHash, resultNodeHash,
            responseFor, seenRequests, allRequestsTrie.rootHash());

        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = qh.queryHash->to_string(HashFormat::Base16, false),
        };
    }

    /**
     * Get underlying TraceSink for compatibility.
     */
    TraceSink & getSink()
    {
        return sink;
    }

    /**
     * used to advance the temporal cursor after a hit. v13 has no
     * temporal cursor; this is a no-op.
     */
    void syncAfterHash(const Hash & /*resultNodeHash*/)
    {
        // No-op under v13.
    }

    /**
     * Whether v13 decision-graph recording is enabled.
     */
    bool hasIndex() const
    {
        return decisionGraph != nullptr;
    }

    TracingDecisionGraph * getDecisionGraph() const
    {
        return decisionGraph;
    }
};

} // namespace nix
