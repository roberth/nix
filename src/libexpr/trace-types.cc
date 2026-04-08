#include "nix/expr/trace-types.hh"
#include "nix/util/logging.hh"

#include <map>

namespace nix::trace {

// ---------------------------------------------------------------------------
// Environment request/response serialization
// ---------------------------------------------------------------------------

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
    if (r.value)
        j = nlohmann::json{{"value", *r.value}};
    else
        j = nlohmann::json{{"value", nullptr}};
}

void from_json(const nlohmann::json & j, GetEnvResponse & r)
{
    auto & v = j.at("value");
    if (v.is_null())
        r.value = std::nullopt;
    else
        r.value = v.get<std::string>();
}

// ---------------------------------------------------------------------------
// Contra-query serialization
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const ContraQueryRequest & r)
{
    nlohmann::json queryJson;
    std::visit([&](const auto & q) { queryJson = nlohmann::json{{"tag", q.tag}, {"payload", q}}; }, r.query);
    j = nlohmann::json{{"query", queryJson}};
}

void from_json(const nlohmann::json & j, ContraQueryRequest & r)
{
    auto & q = j.at("query");
    auto tag = q.at("tag").get<std::string_view>();
    auto & payload = q.at("payload");

    auto tryParse = [&]<typename T>() -> bool {
        if (tag == T::tag) {
            T val;
            from_json(payload, val);
            r.query = val;
            return true;
        }
        return false;
    };

    if (tryParse.template operator()<QueryExpr>() || tryParse.template operator()<QueryImport>()
        || tryParse.template operator()<QueryGetAttr>() || tryParse.template operator()<QueryGetString>()
        || tryParse.template operator()<QueryGetStringWithContext>()
        || tryParse.template operator()<QueryGetAttrNames>() || tryParse.template operator()<QueryGetType>()
        || tryParse.template operator()<QueryGetBool>() || tryParse.template operator()<QueryGetInt>()
        || tryParse.template operator()<QueryGetFloat>() || tryParse.template operator()<QueryGetListOfStrings>()
        || tryParse.template operator()<QueryGetListSize>() || tryParse.template operator()<QueryGetListElem>()
        || tryParse.template operator()<QueryGetPath>()
        || tryParse.template operator()<QueryGetFunctionInfo>()
        || tryParse.template operator()<QueryApply>())
        return;

    throw nlohmann::json::parse_error::create(302, 0, "unknown contra-query tag: " + std::string(tag), &j);
}

void to_json(nlohmann::json & j, const ContraQueryResponse & r)
{
    nlohmann::json resultJson;
    std::visit([&](const auto & res) { resultJson = res; }, r.result);
    j = nlohmann::json{{"result", resultJson}};
}

void from_json(const nlohmann::json & j, ContraQueryResponse & r)
{
    // The result type is determined by context (the query determines
    // which result type to expect). For generic deserialization we
    // store the raw JSON — the caller can parse the specific type.
    // For now, try common result types.
    auto & res = j.at("result");

    auto tryParse = [&]<typename T>(T *) -> bool {
        try {
            T val;
            from_json(res, val);
            r.result = val;
            return true;
        } catch (...) {
            return false;
        }
    };

    if (tryParse((ResultType *) nullptr))
        return;
    if (tryParse((ResultMaybeType *) nullptr))
        return;
    if (tryParse((ResultString *) nullptr))
        return;
    if (tryParse((ResultInt *) nullptr))
        return;
    if (tryParse((ResultFloat *) nullptr))
        return;
    if (tryParse((ResultBool *) nullptr))
        return;
    if (tryParse((ResultPath *) nullptr))
        return;
    if (tryParse((ResultListOfStrings *) nullptr))
        return;
    if (tryParse((ResultStringWithContext *) nullptr))
        return;
    if (tryParse((ResultListSize *) nullptr))
        return;
    throw nlohmann::json::parse_error::create(302, 0, "could not parse contra-query result", &j);
}

// ---------------------------------------------------------------------------
// Result payload serialization
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
    if (r.type)
        j = nlohmann::json{{"attrType", *r.type}};
    else
        j = nlohmann::json{{"attrType", nullptr}};
}

void from_json(const nlohmann::json & j, ResultMaybeType & r)
{
    auto & v = j.at("attrType");
    if (v.is_null())
        r.type = std::nullopt;
    else
        r.type = v.get<std::string>();
}

void to_json(nlohmann::json & j, const ResultString & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultString & r)
{
    j.at("value").get_to(r.value);
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

void to_json(nlohmann::json & j, const ResultInt & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultInt & r)
{
    j.at("value").get_to(r.value);
}

void to_json(nlohmann::json & j, const ResultFloat & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultFloat & r)
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

void to_json(nlohmann::json & j, const ResultListSize & r)
{
    j = nlohmann::json{{"size", r.size}};
}

void from_json(const nlohmann::json & j, ResultListSize & r)
{
    j.at("size").get_to(r.size);
}

// ---------------------------------------------------------------------------
// Query payload serialization
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

void to_json(nlohmann::json & j, const QueryGetFloat & q)
{
    j = nlohmann::json{{"query", QueryGetFloat::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetFloat & q)
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

void to_json(nlohmann::json & j, const QueryGetListSize & q)
{
    j = nlohmann::json{{"query", QueryGetListSize::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetListSize & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetListElem & q)
{
    j = nlohmann::json{{"query", QueryGetListElem::tag}, {"params", {{"from", q.from}, {"index", q.index}}}};
}

void from_json(const nlohmann::json & j, QueryGetListElem & q)
{
    j.at("params").at("from").get_to(q.from);
    j.at("params").at("index").get_to(q.index);
}

void to_json(nlohmann::json & j, const QueryGetPath & q)
{
    j = nlohmann::json{{"query", QueryGetPath::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetPath & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetFunctionInfo & q)
{
    j = nlohmann::json{{"query", QueryGetFunctionInfo::tag}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetFunctionInfo & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const ResultFunctionInfo & r)
{
    j = nlohmann::json{{"hasInfo", r.hasInfo}, {"formals", r.formals}, {"ellipsis", r.ellipsis}};
}

void from_json(const nlohmann::json & j, ResultFunctionInfo & r)
{
    j.at("hasInfo").get_to(r.hasInfo);
    j.at("formals").get_to(r.formals);
    j.at("ellipsis").get_to(r.ellipsis);
}

void to_json(nlohmann::json & j, const QueryApply & q)
{
    j = nlohmann::json{{"query", QueryApply::tag}, {"params", {{"fn", q.fn}, {"arg", q.arg}}}};
}

void from_json(const nlohmann::json & j, QueryApply & q)
{
    j.at("params").at("fn").get_to(q.fn);
    j.at("params").at("arg").get_to(q.arg);
}

// ---------------------------------------------------------------------------
// parseTraceEntry
// ---------------------------------------------------------------------------

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
        if (type == ContraQueryRequest::tag) {
            ContraQueryRequest req;
            from_json(j["request"], req);
            ContraQueryResponse resp;
            from_json(j["response"], resp);
            return Response<ContraQueryRequest>{req, resp};
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
        if (auto r = tryParseQuery<QueryGetFloat>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetListOfStrings>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetListSize>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetListElem>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetPath>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetFunctionInfo>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryApply>(type, j))
            return r;
        return std::nullopt;
    }

    // Result: has "result" and "v"
    if (j.contains("result") && j.contains("v")) {
        auto & r = j["result"];
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
        if (r.contains("size")) {
            Result<ResultListSize> e;
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
        // String with context before plain string
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
            if (val.is_number_float()) {
                Result<ResultFloat> e;
                from_json(j, e);
                return e;
            }
        }
        if (r.contains("hasInfo")) {
            Result<ResultFunctionInfo> e;
            from_json(j, e);
            return e;
        }
        return std::nullopt;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// correlateTrace
// ---------------------------------------------------------------------------

namespace {

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
    else if constexpr (std::is_same_v<T, ResultFloat>)
        return 4;
    else if constexpr (std::is_same_v<T, ResultBool>)
        return 5;
    else if constexpr (std::is_same_v<T, ResultPath>)
        return 6;
    else if constexpr (std::is_same_v<T, ResultListOfStrings>)
        return 7;
    else if constexpr (std::is_same_v<T, ResultStringWithContext>)
        return 8;
    else if constexpr (std::is_same_v<T, ResultListSize>)
        return 9;
    else
        return ~size_t(0);
}

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
                    using QueryPayload = std::decay_t<decltype(e.query)>;
                    CompletedQuery<QueryPayload> completed;
                    completed.query = e.query;
                    completed.v = e.v;
                    auto key = std::make_pair(queryResultTypeIndex<QueryPayload>(), e.v);
                    auto it = resultIndex.find(key);
                    completed.resultIndex = (it != resultIndex.end()) ? it->second : 0;
                    result.push_back(completed);
                } else {
                    result.push_back(e);
                }
            },
            entry);
    }

    return result;
}

// ---------------------------------------------------------------------------
// QueryIndex
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
                    }
                }
            },
            trace[i]);
    }
}

} // namespace nix::trace
