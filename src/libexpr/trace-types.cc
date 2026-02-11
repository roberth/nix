#include "nix/expr/trace-types.hh"
#include "nix/util/json-utils.hh"

#include <map>

namespace nix::trace {

void to_json(nlohmann::json & j, const FileReadRequest & r)
{
    j = nlohmann::json{{"absPath", r.absPath}};
}

void from_json(const nlohmann::json & j, FileReadRequest & r)
{
    j.at("absPath").get_to(r.absPath);
}

void to_json(nlohmann::json & j, const FileReadResponse & r)
{
    j = nlohmann::json{{"contentHash", r.contentHash.to_string(HashFormat::SRI, true)}};
}

void from_json(const nlohmann::json & j, FileReadResponse & r)
{
    r.contentHash = Hash::parseSRI(j.at("contentHash").get<std::string>());
}

void to_json(nlohmann::json & j, const GetEnvRequest & r)
{
    j = nlohmann::json{{"name", r.name}};
}

void from_json(const nlohmann::json & j, GetEnvRequest & r)
{
    j.at("name").get_to(r.name);
}

void to_json(nlohmann::json & j, const GetEnvResponse & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, GetEnvResponse & r)
{
    j.at("value").get_to(r.value);
}

// ---------------------------------------------------------------------------
// Result payload types
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const ResultType & r)
{
    j = nlohmann::json{{"type", r.type}};
}

void from_json(const nlohmann::json & j, ResultType & r)
{
    j.at("type").get_to(r.type);
}

void to_json(nlohmann::json & j, const ResultMaybeType & r)
{
    j = nlohmann::json{{"attrType", r.type}};
}

void from_json(const nlohmann::json & j, ResultMaybeType & r)
{
    if (j.at("attrType").is_null())
        r.type = std::nullopt;
    else
        r.type = j.at("attrType").get<std::string>();
}

void to_json(nlohmann::json & j, const ResultString & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultString & r)
{
    j.at("value").get_to(r.value);
}

void to_json(nlohmann::json & j, const ResultInt & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultInt & r)
{
    j.at("value").get_to(r.value);
}

void to_json(nlohmann::json & j, const ResultBool & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultBool & r)
{
    j.at("value").get_to(r.value);
}

void to_json(nlohmann::json & j, const ResultPath & r)
{
    j = nlohmann::json{{"path", r.path}};
}

void from_json(const nlohmann::json & j, ResultPath & r)
{
    j.at("path").get_to(r.path);
}

void to_json(nlohmann::json & j, const ResultListOfStrings & r)
{
    j = nlohmann::json{{"values", r.values}};
}

void from_json(const nlohmann::json & j, ResultListOfStrings & r)
{
    j.at("values").get_to(r.values);
}

void to_json(nlohmann::json & j, const ResultStringWithContext & r)
{
    j = nlohmann::json{{"value", r.value}, {"context", r.context}};
}

void from_json(const nlohmann::json & j, ResultStringWithContext & r)
{
    j.at("value").get_to(r.value);
    j.at("context").get_to(r.context);
}

// ---------------------------------------------------------------------------
// Query payload types
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const QueryExpr & q)
{
    j = nlohmann::json{{"query", QueryExpr::tag}, {"params", {{"expr", q.expr}, {"baseDir", q.baseDir}}}};
}

void from_json(const nlohmann::json & j, QueryExpr & q)
{
    j.at("params").at("expr").get_to(q.expr);
    j.at("params").at("baseDir").get_to(q.baseDir);
}

void to_json(nlohmann::json & j, const QueryImport & q)
{
    j = nlohmann::json{{"query", QueryImport::tag}, {"params", {{"path", q.path}}}};
}

void from_json(const nlohmann::json & j, QueryImport & q)
{
    j.at("params").at("path").get_to(q.path);
}

void to_json(nlohmann::json & j, const QueryGetAttr & q)
{
    j = nlohmann::json{{"query", QueryGetAttr::tag}, {"params", {{"name", q.name}, {"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetAttr & q)
{
    j.at("params").at("name").get_to(q.name);
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetString & q)
{
    j = nlohmann::json{{"query", QueryGetString::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetString & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetStringWithContext & q)
{
    j = nlohmann::json{{"query", QueryGetStringWithContext::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetStringWithContext & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetAttrNames & q)
{
    j = nlohmann::json{{"query", QueryGetAttrNames::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetAttrNames & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetType & q)
{
    j = nlohmann::json{{"query", QueryGetType::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetType & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetBool & q)
{
    j = nlohmann::json{{"query", QueryGetBool::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetBool & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetInt & q)
{
    j = nlohmann::json{{"query", QueryGetInt::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetInt & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetListOfStrings & q)
{
    j = nlohmann::json{{"query", QueryGetListOfStrings::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetListOfStrings & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetPath & q)
{
    j = nlohmann::json{{"query", QueryGetPath::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetPath & q)
{
    j.at("params").at("from").get_to(q.from);
}

namespace {

template<typename T>
std::optional<TraceEntry> tryParseQuery(std::string_view type, const nlohmann::json & j)
{
    if (type == T::tag) {
        Query<T> e;
        from_json(j, e);
        return e;
    }
    return std::nullopt;
}

} // namespace

std::optional<TraceEntry> parseTraceEntry(const nlohmann::json & j)
{
    // Environment message: has "type", "request" and "response"
    if (j.contains("type") && j.contains("request") && j.contains("response")) {
        auto type = j["type"].get<std::string_view>();

        if (type == FileReadRequest::tag) {
            return Response<FileReadRequest>{
                .request = {.absPath = j["request"]["absPath"].get<std::string>()},
                .response = {.contentHash = Hash::parseSRI(j["response"]["contentHash"].get<std::string>())},
            };
        }
        if (type == GetEnvRequest::tag) {
            GetEnvRequest req;
            from_json(j["request"], req);
            GetEnvResponse resp;
            from_json(j["response"], resp);
            return Response<GetEnvRequest>{req, resp};
        }
        return std::nullopt;
    }

    // Query: has "query" and "v"
    if (j.contains("query") && j.contains("v")) {
        auto & q = j["query"];
        if (!q.contains("query"))
            return std::nullopt;
        auto type = q["query"].get<std::string_view>();

        if (auto r = tryParseQuery<QueryExpr>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryImport>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetAttr>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetString>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetStringWithContext>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetAttrNames>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetType>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetBool>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetInt>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetListOfStrings>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetPath>(type, j))
            return r;
        return std::nullopt;
    }

    // Result: has "result" and "v"
    if (j.contains("result") && j.contains("v")) {
        auto & r = j["result"];
        // Distinguish by what fields are present
        if (r.contains("type")) {
            Result<ResultType> e;
            from_json(j, e);
            return e;
        }
        if (r.contains("attrType")) {
            Result<ResultMaybeType> e;
            from_json(j, e);
            return e;
        }
        if (r.contains("values")) {
            Result<ResultListOfStrings> e;
            from_json(j, e);
            return e;
        }
        if (r.contains("path")) {
            Result<ResultPath> e;
            from_json(j, e);
            return e;
        }
        // Check for string with context before plain string
        if (r.contains("value") && r.contains("context")) {
            Result<ResultStringWithContext> e;
            from_json(j, e);
            return e;
        }
        if (r.contains("value")) {
            auto & val = r["value"];
            if (val.is_string()) {
                Result<ResultString> e;
                from_json(j, e);
                return e;
            }
            if (val.is_boolean()) {
                Result<ResultBool> e;
                from_json(j, e);
                return e;
            }
            if (val.is_number_integer()) {
                Result<ResultInt> e;
                from_json(j, e);
                return e;
            }
        }
        return std::nullopt;
    }

    return std::nullopt;
}

namespace {

/**
 * Get a type index for a result payload type.
 * Must match the expected result type for each query.
 */
template<typename T>
constexpr size_t resultTypeIndex()
{
    if constexpr (std::is_same_v<T, ResultType>)
        return 0;
    else if constexpr (std::is_same_v<T, ResultMaybeType>)
        return 1;
    else if constexpr (std::is_same_v<T, ResultString>)
        return 2;
    else if constexpr (std::is_same_v<T, ResultInt>)
        return 3;
    else if constexpr (std::is_same_v<T, ResultBool>)
        return 4;
    else if constexpr (std::is_same_v<T, ResultPath>)
        return 5;
    else if constexpr (std::is_same_v<T, ResultListOfStrings>)
        return 6;
    else if constexpr (std::is_same_v<T, ResultStringWithContext>)
        return 7;
    else
        return ~size_t(0);
}

/**
 * Get result type index for a query payload type (via ResultOf mapping).
 */
template<typename QueryPayload>
constexpr size_t queryResultTypeIndex()
{
    return resultTypeIndex<typename ResultOf<QueryPayload>::Type>();
}

} // namespace

std::vector<CorrelatedTraceEntry> correlateTrace(const std::vector<TraceEntry> & trace)
{
    // Build map from (result_type_index, v) to trace index
    std::map<std::pair<size_t, uint64_t>, size_t> resultIndex;
    for (size_t i = 0; i < trace.size(); ++i) {
        std::visit(
            [&](const auto & entry) {
                if constexpr (requires {
                                  entry.result;
                                  entry.v;
                              }) {
                    using ResultPayload = std::decay_t<decltype(entry.result)>;
                    auto key = std::make_pair(resultTypeIndex<ResultPayload>(), entry.v);
                    resultIndex[key] = i;
                }
            },
            trace[i]);
    }

    // Transform queries to completed queries
    std::vector<CorrelatedTraceEntry> result;
    result.reserve(trace.size());

    for (const auto & entry : trace) {
        std::visit(
            [&](const auto & e) {
                if constexpr (requires {
                                  e.query;
                                  e.v;
                              }) {
                    // It's a Query<X>, transform to CompletedQuery<X>
                    using QueryPayload = std::decay_t<decltype(e.query)>;
                    CompletedQuery<QueryPayload> completed;
                    completed.query = e.query;
                    completed.v = e.v;
                    auto key = std::make_pair(queryResultTypeIndex<QueryPayload>(), e.v);
                    auto it = resultIndex.find(key);
                    completed.resultIndex = (it != resultIndex.end()) ? it->second : 0;
                    result.push_back(completed);
                } else {
                    // Response or Result, pass through
                    result.push_back(e);
                }
            },
            entry);
    }

    return result;
}

// ---------------------------------------------------------------------------
// QueryIndex implementation
// ---------------------------------------------------------------------------

QueryIndex::QueryIndex(const std::vector<TraceEntry> & trace)
{
    // First pass: build result index (result_type_index, v) -> trace index
    std::map<std::pair<size_t, uint64_t>, size_t> resultLookup;
    for (size_t i = 0; i < trace.size(); ++i) {
        std::visit(
            [&](const auto & entry) {
                if constexpr (requires {
                                  entry.result;
                                  entry.v;
                              }) {
                    using ResultPayload = std::decay_t<decltype(entry.result)>;
                    auto key = std::make_pair(resultTypeIndex<ResultPayload>(), entry.v);
                    resultLookup[key] = i;
                }
            },
            trace[i]);
    }

    // Second pass: index queries (only those with matching results)
    size_t indexed = 0, skipped = 0;
    for (size_t i = 0; i < trace.size(); ++i) {
        std::visit(
            [&](const auto & entry) {
                if constexpr (requires {
                                  entry.query;
                                  entry.v;
                              }) {
                    using Q = std::decay_t<decltype(entry.query)>;
                    auto key = std::make_pair(queryResultTypeIndex<Q>(), entry.v);
                    auto it = resultLookup.find(key);
                    if (it != resultLookup.end()) {
                        index[entry.query] = IndexEntry{i, it->second};
                        ++indexed;
                        if constexpr (std::is_same_v<Q, QueryGetAttr>) {
                            debug(
                                "QueryIndex: indexed getAttr '%s' from=%d (v=%d)",
                                entry.query.name,
                                entry.query.from,
                                entry.v);
                        }
                    } else {
                        ++skipped;
                        if constexpr (std::is_same_v<Q, QueryGetAttr>) {
                            debug(
                                "QueryIndex: skipped getAttr '%s' from=%d (v=%d, no result)",
                                entry.query.name,
                                entry.query.from,
                                entry.v);
                        }
                    }
                }
            },
            trace[i]);
    }
    debug("QueryIndex: %d indexed, %d skipped (no result)", indexed, skipped);
}

} // namespace nix::trace
