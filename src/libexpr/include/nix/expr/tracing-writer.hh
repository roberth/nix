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
       can't fetch them). */
    std::vector<TracingDecisionGraph::Fact> v13FactSet;

    uint64_t nextVirtualRoot = 0;
    std::map<Object *, VirtualRootId> virtualRootRegistry;
    std::vector<ref<Object>> virtualRootObjects; // extends Object lifetime

public:
    TracingWriter(TraceSink & sink, TracingDecisionGraph * decisionGraph = nullptr)
        : sink(sink)
        , decisionGraph(decisionGraph)
    {
    }

    /**
     * Get or allocate a virtual root id for an Object.
     *
     * Returns the same id for the same Object across calls,
     * eliminating id drift between replay and recording evaluators.
     */
    VirtualRootId getOrAllocVirtualRoot(ref<Object> obj)
    {
        auto it = virtualRootRegistry.find(&*obj);
        if (it != virtualRootRegistry.end())
            return it->second;
        auto id = VirtualRootId(nextVirtualRoot++);
        virtualRootRegistry[&*obj] = id;
        virtualRootObjects.push_back(obj);
        return id;
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
        auto responseHash = TracingDecisionGraph::computeResponseHash(jsonToCborString(respJson));
        decisionGraph->insertRequest(queryHash, jsonToCborString(reqJson));
        v13FactSet.push_back({queryHash, responseHash});
    }

    /**
     * Log an ambient interaction as a d>0 Request/Response pair.
     */
    void logAmbientInteraction(const trace::QueryVariant & query, const trace::ResultVariant & result)
    {
        if (!decisionGraph)
            return;
        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, query);
        nlohmann::json resultJson;
        std::visit([&](const auto & r) { resultJson = r; }, result);
        auto queryHash = std::visit(
            [](const auto & q) { return TracingDecisionGraph::computeQueryHash(q); }, query);
        auto responseHash = TracingDecisionGraph::computeResponseHash(jsonToCborString(resultJson));
        decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));
        v13FactSet.push_back({queryHash, responseHash});
    }

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

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph->insertResult(resultNodeHash, resultPayload);

        auto factSetHash = decisionGraph->insertFactSet(v13FactSet);
        decisionGraph->record(*qh.queryHash, factSetHash, resultNodeHash);

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
};

} // namespace nix
