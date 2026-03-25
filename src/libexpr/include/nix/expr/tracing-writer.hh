#pragma once
/**
 * @file
 * Combined trace writer that logs to both JSON (TraceFile) and trie (TracingIndex).
 */

#include "nix/expr/trace-file.hh"
#include "nix/expr/tracing-index.hh"

#include <optional>

namespace nix {

/**
 * Tracks trie position for a single value being traced.
 * Each TracingObject holds one of these to record its operations.
 */
struct TriePosition
{
    NodeHash resultNodeHash;  // The Result node for this value (structural parent)
    NodeHash afterHash;       // Current temporal position (last node written)
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
    TraceFile & traceFile;
    TracingIndex * index; // nullptr if trie recording disabled
    std::optional<NodeHash> afterHash;
    std::optional<QueryHash> currentQueryHash; // queryHash of most recent query (for logResult)

public:
    TracingWriter(TraceFile & traceFile, TracingIndex * index = nullptr)
        : traceFile(traceFile)
        , index(index)
    {
    }

    /**
     * Log a root query (evalFile, evalExpr).
     * Returns (valueNum, triePosition) for use in TracingObject.
     */
    template<typename Q>
    std::pair<uint64_t, std::optional<TriePosition>> logRootQuery(const Q & query)
    {
        auto valueNum = traceFile.logQuery(query);

        if (!index)
            return {valueNum, std::nullopt};

        auto queryHash = TracingIndex::computeQueryHash(query);
        nlohmann::json j = query;
        auto queryNodeHash = index->insertQuery(afterHash, queryHash, j.dump());
        afterHash = queryNodeHash;
        currentQueryHash = queryHash;

        return {valueNum, std::nullopt}; // Result not yet known
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.).
     * structuralParent is the Result nodeHash of the parent object.
     * The query's `from` field must contain the parent's queryHash.
     */
    template<typename Q>
    std::pair<uint64_t, std::optional<TriePosition>>
    logQuery(const Q & query, const std::optional<TriePosition> & parent)
    {
        auto valueNum = traceFile.logQuery(query);

        if (!index)
            return {valueNum, std::nullopt};

        auto queryHash = TracingIndex::computeQueryHash(query);
        nlohmann::json j = query;
        auto structuralParent = parent ? std::optional{parent->resultNodeHash} : std::nullopt;
        auto queryNodeHash = index->insertQuery(afterHash, queryHash, j.dump(), structuralParent);
        afterHash = queryNodeHash;
        currentQueryHash = queryHash;

        return {valueNum, std::nullopt};
    }

    /**
     * Log a response (file read, env lookup).
     */
    template<typename Req>
    void logResponse(const trace::Response<Req> & resp)
    {
        traceFile.log(nlohmann::json(resp));

        if (!index || !afterHash)
            return;

        nlohmann::json reqJson = resp.request;
        nlohmann::json respJson = resp.response;
        auto nodeHash = index->insertResponse(*afterHash, reqJson.dump(), respJson.dump());
        afterHash = nodeHash;
    }

    /**
     * Log a result and return the TriePosition for use in child queries.
     */
    template<typename R>
    std::optional<TriePosition> logResult(uint64_t valueNum, const R & result)
    {
        traceFile.logResult(valueNum, result);

        if (!index || !afterHash || !currentQueryHash)
            return std::nullopt;

        nlohmann::json j = result;
        auto resultNodeHash = index->insertResult(*afterHash, j.dump());
        afterHash = resultNodeHash;

        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .afterHash = resultNodeHash,
            .queryHashStr = currentQueryHash->to_string(HashFormat::Base16, false),
        };
    }

    /**
     * Get underlying TraceFile for compatibility.
     */
    TraceFile & getTraceFile()
    {
        return traceFile;
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
