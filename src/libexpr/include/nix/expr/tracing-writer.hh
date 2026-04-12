#pragma once
/**
 * @file
 * Combined trace writer that logs to both JSON (TraceFile) and trie (TracingIndex).
 */

#include "nix/expr/trace-sink.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/util/ref.hh"

#include <map>
#include <optional>
#include <vector>

namespace nix {

class Object;

/**
 * Serialize a JSON value to CBOR as a std::string (for trie storage).
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

} // namespace nix

namespace nix {

/**
 * Tracks trie position for a single value being traced.
 * Each TracingObject holds one of these to record its operations.
 *
 * Note: the temporal cursor (afterHash) is NOT stored here.
 * During recording, TracingWriter owns it. During replay,
 * TracingReplayEvaluator owns it. TriePosition only holds the
 * structural identity of a value in the trie.
 */
struct TriePosition
{
    NodeHash resultNodeHash;  // The Result node for this value (structural parent)
    std::string queryHashStr; // The queryHash of the query that produced this result,
                              // as a hex string. Used by child queries to compute
                              // their queryHash (Merkle identity: child includes parent hash).
};

/**
 * Combined writer that logs to JSON and trie simultaneously.
 * Tracks temporal position (afterHash) across operations.
 */
class TracingWriter
{
    TraceSink & sink;
    TracingIndex * index; // nullptr if trie recording disabled
    std::optional<NodeHash> afterHash;
    uint64_t nextVirtualRoot = 0;
    std::map<Object *, VirtualRootId> virtualRootRegistry;
    std::vector<ref<Object>> virtualRootObjects; // extends Object lifetime

public:
    TracingWriter(TraceSink & sink, TracingIndex * index = nullptr)
        : sink(sink)
        , index(index)
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
     * Opaque handle linking a query to its result in the trie.
     */
    struct QueryHandle
    {
        std::optional<QueryHash> queryHash;
        std::optional<NodeHash> queryNodeHash;
    };

    /**
     * Log a root query (evalFile, evalExpr, apply).
     * Returns (valueHandle, queryHandle) so the caller can pass queryHandle to logResult.
     */
    template<typename Q>
    std::pair<ValueHandle, QueryHandle> logRootQuery(const Q & query)
    {
        auto valueNum = sink.logQuery(query);

        if (!index)
            return {valueNum, {}};

        auto queryHash = TracingIndex::computeQueryHash(query);
        nlohmann::json j = query;
        auto queryNodeHash = index->insertQuery(afterHash, queryHash, jsonToCborString(j));
        afterHash = queryNodeHash;

        return {valueNum, {queryHash, queryNodeHash}};
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.).
     * structuralParent is the Result nodeHash of the parent object.
     * The query's `from` field must contain the parent's queryHash.
     * Returns (valueNum, queryHash).
     */
    template<typename Q>
    std::pair<ValueHandle, QueryHandle>
    logQuery(const Q & query, const std::optional<TriePosition> & parent)
    {
        auto valueNum = sink.logQuery(query);

        if (!index)
            return {valueNum, {}};

        auto queryHash = TracingIndex::computeQueryHash(query);
        nlohmann::json j = query;
        auto structuralParent = parent ? std::optional{parent->resultNodeHash} : std::nullopt;
        auto queryNodeHash = index->insertQuery(afterHash, queryHash, jsonToCborString(j), structuralParent);
        afterHash = queryNodeHash;

        return {valueNum, {queryHash, queryNodeHash}};
    }

    /**
     * Log a response (file read, env lookup) as a depth=1 Query/Result pair.
     */
    template<typename Req>
    void logResponse(const trace::Response<Req> & resp)
    {
        sink.log(nlohmann::json(resp));

        if (!index || !afterHash)
            return;

        nlohmann::json reqJson = resp.request;
        nlohmann::json respJson = resp.response;
        auto queryHash = TracingIndex::computeQueryHash(resp.request);
        auto queryNodeHash = index->insertQuery(afterHash, queryHash, jsonToCborString(reqJson), std::nullopt, /*depth=*/1);
        auto resultNodeHash = index->insertResult(queryNodeHash, jsonToCborString(respJson), queryNodeHash);
        afterHash = resultNodeHash;
    }

    /**
     * Log an ambient interaction as a depth=1 Query/Result pair.
     */
    void logAmbientInteraction(const trace::QueryVariant & query, const trace::ResultVariant & result)
    {
        if (!index || !afterHash)
            return;

        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, query);
        nlohmann::json resultJson;
        std::visit([&](const auto & r) { resultJson = r; }, result);

        auto queryHash = std::visit([](const auto & q) { return TracingIndex::computeQueryHash(q); }, query);
        auto queryNodeHash = index->insertQuery(afterHash, queryHash, jsonToCborString(queryJson), std::nullopt, /*depth=*/1);
        auto resultNodeHash = index->insertResult(queryNodeHash, jsonToCborString(resultJson), queryNodeHash);
        afterHash = resultNodeHash;
    }

    /**
     * Log a result and return the TriePosition for use in child queries.
     * @param queryHash The queryHash from logRootQuery or logQuery.
     */
    template<typename R>
    std::optional<TriePosition> logResult(ValueHandle valueNum, const R & result, const QueryHandle & qh)
    {
        sink.logResult(valueNum, result);

        if (!index || !afterHash || !qh.queryHash)
            return std::nullopt;

        nlohmann::json j = result;
        auto resultNodeHash = index->insertResult(*afterHash, jsonToCborString(j), qh.queryNodeHash);
        afterHash = resultNodeHash;

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
     * Advance the temporal cursor to a replayed position.
     * Called when the replay evaluator hits — ensures that
     * subsequent misses (which fall through to recording) start
     * from the correct temporal position in the trie.
     */
    void syncAfterHash(const NodeHash & nodeHash)
    {
        afterHash = nodeHash;
    }

    /**
     * Check if trie recording is enabled.
     */
    bool hasIndex() const
    {
        return index != nullptr;
    }
};

} // namespace nix
