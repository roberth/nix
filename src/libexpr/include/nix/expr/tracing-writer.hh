#pragma once
/**
 * @file
 * Trace writer that logs evaluation events to a JSON sink and the
 * v13 decision-graph index.
 */

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

    /* Phase 4 of content-defined identity: ambient facts are buffered
       here during recording and flushed at logResult time with
       placeholder→intrinsic substitution applied. See
       logAmbientInteraction, deferRequest, and flushPendingAmbient. */
    struct PendingFact
    {
        trace::QueryVariant query;
        trace::ResultVariant result;
    };
    std::vector<PendingFact> pendingFacts;

    struct PendingRequest
    {
        nlohmann::json payload;
        std::optional<std::string> keyPlaceholder;
    };
    std::vector<PendingRequest> pendingRequests;

    /* Latest published intrinsic content-hash per local placeholder
       hex (the counter-derived id its facts carry as `from`).
       Populated by TracingLocalObject as observations land. */
    std::map<std::string, Hash> placeholderToIntrinsic;

    /* old→new hex substitutions discovered in prior flush cycles. A
       fact whose `from` is the old hash of an apply Q (or an Ambient
       chain child) may be deferred to a flush cycle after the one that
       inserted the Q into the pool at its new hash. Without
       persistence the later flush starts with an empty sub map and
       leaves the fact's `from` unsubstituted, so replay's
       resolveAmbientId can't find the producer in the pool and falls
       back to a frozen ReplayLocalObject standin (#49 root cause). */
    std::map<std::string, std::string> persistentSubstitutions;

    /* Phase 4 cascade: content-defined identities whose final hash
       can't be settled at observation time because their parent's
       identity is itself still a placeholder. The producer query
       carries the parent's placeholder hex in `from`; flush
       substitutes that to the parent's settled content-defined hash,
       then hashes the substituted producer query to get the child's
       settled hash. Used for two flavours of child:
       - Local children (TracingLocalObjects from maybeGetAttr /
         getListElem on a parent local), where the parent's settled
         hash is its observation intrinsic.
       - Ambient children (AmbientObjects from queryFn on a parent
         ambient), where the parent's settled hash is the
         substituted producer-query hash (e.g. an apply-result id
         after the apply Request's arg placeholder was substituted).
       Both cases settle the same way: substitute parent's
       placeholder in the derivation template, hash, register
       placeholder → settled in the substitution map so downstream
       facts and further cascade entries see the settled hash. */
    struct DelayedContentDefinedIdentity
    {
        std::string placeholderHex;
        std::string parentPlaceholderHex;
        nlohmann::json derivationTemplate;
    };
    std::vector<DelayedContentDefinedIdentity> delayedContentDefinedIdentities;

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
        if (seenRequests.insert(queryHash).second) {
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
     * content-defined value isn't known until the local's full
     * observation buffer is settled. flushPendingAmbient() at
     * logResult time substitutes placeholders with intrinsic hashes
     * and does the actual pool inserts + v13FactSet folding.
     *
     * The previous signature returned (queryHash, responseHash). Both
     * depend on `from` substitution, so neither is available at the
     * call site any more — callers that maintained per-fact state
     * (e.g. Phase 3's TracingLocalObject buffer) compute their
     * placeholder-independent contributions themselves now.
     */
    void logAmbientInteraction(const trace::QueryVariant & query, const trace::ResultVariant & result)
    {
        if (!decisionGraph)
            return;
        pendingFacts.push_back({query, result});
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
        if (seenRequests.insert(request).second) {
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
     * Request and the localArg sidecar without committing to an
     * insertion key while the local arg's intrinsic hash is still
     * being built up. At flush, the writer substitutes placeholder
     * hexes in the payload; if `keyPlaceholder` is set the insert
     * key is the substituted form of that placeholder (used for the
     * sidecar, keyed by the local), otherwise the insert key is the
     * hash of the substituted payload (used for the QueryApply,
     * keyed by the apply's own queryHash).
     */
    void deferRequest(nlohmann::json payload, std::optional<std::string> keyPlaceholder = std::nullopt)
    {
        if (!decisionGraph)
            return;
        pendingRequests.push_back({std::move(payload), std::move(keyPlaceholder)});
    }

    /**
     * Publish a local's current intrinsic content-hash. Called by
     * TracingLocalObject each time an observation lands, so the
     * latest value is available at flush time. Placeholder is the
     * hex of the local's counter-derived id, which is what its
     * deferred facts carry in their `from` fields during recording.
     */
    void updatePlaceholderIntrinsic(const std::string & placeholderHex, const Hash & intrinsic)
    {
        placeholderToIntrinsic.insert_or_assign(placeholderHex, intrinsic);
    }

    /**
     * Buffer a content-defined identity whose final hash can't be
     * settled at observation time because its parent's identity is
     * still a placeholder. At flush, substitute the parent's
     * placeholder in `derivationTemplate` to its settled content-
     * defined hash and hash the result; that's the child's settled
     * identity. Replay computes the same hash from the same
     * substituted producer query.
     */
    void delayContentDefinedIdentity(
        std::string placeholderHex, std::string parentHex, nlohmann::json derivationTemplate)
    {
        delayedContentDefinedIdentities.push_back(
            {std::move(placeholderHex), std::move(parentHex), std::move(derivationTemplate)});
    }

    /**
     * Flush all buffered ambient facts and Requests, substituting
     * placeholder hexes with intrinsic content-hashes per Phase 4 of
     * content-defined identity. Called at the top of logResult,
     * before record().
     */
    void flushPendingAmbient();

    /**
     * Record a Pass-1 or Pass-3 old→new substitution into the
     * persistent map with a collision-detection invariant. Throws if
     * `oldHex` is already mapped to a different value. Internal to
     * flushPendingAmbient.
     */
    void recordPersistentSubstitution(
        const std::string & oldHex, const std::string & newHex, const char * passLabel);

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

        /* Phase 4: settle ambient facts now that observations on
           every callback local in this Q are done. Substitution
           folds placeholders → intrinsic hashes and populates the
           v13FactSet structures the record() call below reads. */
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
