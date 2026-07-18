#include "nix/expr/trace-sink.hh"
#include "nix/expr/trace-types.hh"

#include "nix/util/logging.hh"

#include <map>

namespace nix {

/* Out-of-line virtual destructor so the abstract base gets a key
   function — without it, clang's `-Wweak-vtables` reports the vtable
   as emitted in every TU. */
TraceSink::~TraceSink() = default;

} // namespace nix

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
// Ambient message pairing serialization
// ---------------------------------------------------------------------------

// Shared helpers: both outgoing and incoming wrap QueryVariant/ResultVariant.

static void queryVariantToJson(nlohmann::json & j, const QueryVariant & query)
{
    nlohmann::json queryJson;
    std::visit([&](const auto & q) { queryJson = nlohmann::json{{"tag", q.tag}, {"payload", q}}; }, query);
    j = nlohmann::json{{"query", queryJson}};
}

static void queryVariantFromJson(const nlohmann::json & j, QueryVariant & query)
{
    auto & q = j.at("query");
    auto tag = q.at("tag").get<std::string_view>();
    auto & payload = q.at("payload");

    auto tryParse = [&]<typename T>() -> bool {
        if (tag == T::tag) {
            T val;
            from_json(payload, val);
            query = val;
            return true;
        }
        return false;
    };

    if (tryParse.template operator()<QueryExpr>() || tryParse.template operator()<QueryImport>()
        || tryParse.template operator()<QueryGetAttr>()
        || tryParse.template operator()<QueryGetListOfStrings>()
        || tryParse.template operator()<QueryGetListElem>()
        || tryParse.template operator()<QueryGetFunctionInfo>()
        || tryParse.template operator()<QueryGetWHNF>() || tryParse.template operator()<QueryApply>()
        || tryParse.template operator()<QueryCallbackApply>())
        return;

    throw nlohmann::json::parse_error::create(302, 0, "unknown ambient query tag: " + std::string(tag), &j);
}

static void resultVariantToJson(nlohmann::json & j, const ResultVariant & result)
{
    nlohmann::json resultJson;
    std::visit([&](const auto & res) { resultJson = res; }, result);
    j = nlohmann::json{{"result", resultJson}};
}

static void resultVariantFromJson(const nlohmann::json & j, ResultVariant & result)
{
    auto & res = j.at("result");

    auto tryParse = [&]<typename T>(T *) -> bool {
        try {
            T val;
            from_json(res, val);
            result = val;
            return true;
        } catch (...) {
            return false;
        }
    };

    if (tryParse((ResultType *) nullptr))
        return;
    if (tryParse((ResultMaybeType *) nullptr))
        return;
    if (tryParse((ResultListOfStrings *) nullptr))
        return;
    throw nlohmann::json::parse_error::create(302, 0, "could not parse ambient result", &j);
}

void to_json(nlohmann::json & j, const OuterValueRequest & r)
{
    queryVariantToJson(j, r.query);
}

void from_json(const nlohmann::json & j, OuterValueRequest & r)
{
    queryVariantFromJson(j, r.query);
}

void to_json(nlohmann::json & j, const OuterValueResponse & r)
{
    resultVariantToJson(j, r.result);
}

void from_json(const nlohmann::json & j, OuterValueResponse & r)
{
    resultVariantFromJson(j, r.result);
}

void to_json(nlohmann::json & j, const InnerValueRequestPayload & r)
{
    queryVariantToJson(j, r.query);
}

void from_json(const nlohmann::json & j, InnerValueRequestPayload & r)
{
    queryVariantFromJson(j, r.query);
}

void to_json(nlohmann::json & j, const InnerValueResponsePayload & r)
{
    resultVariantToJson(j, r.result);
}

void from_json(const nlohmann::json & j, InnerValueResponsePayload & r)
{
    resultVariantFromJson(j, r.result);
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

void to_json(nlohmann::json & j, const ResultListOfStrings & r)
{
    j = nlohmann::json{{"values", r.values}};
}

void from_json(const nlohmann::json & j, ResultListOfStrings & r)
{
    j.at("values").get_to(r.values);
}

/* ResultWHNF: `type` is the discriminator; `payload`'s variant
   alternative is selected by it. The JSON shape is a flat object
   with `type` plus the payload fields appropriate for that type — no
   wrapper object around the payload, for readability and for
   matching the rest of the trace's flat JSON style. */
void to_json(nlohmann::json & j, const ResultWHNF & r)
{
    j = nlohmann::json{{"type", r.type}};
    std::visit([&](const auto & p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, WHNFInt>) {
            j["value"] = p.value;
        } else if constexpr (std::is_same_v<T, WHNFBool>) {
            j["value"] = p.value;
        } else if constexpr (std::is_same_v<T, WHNFFloat>) {
            j["value"] = p.value;
        } else if constexpr (std::is_same_v<T, WHNFPath>) {
            j["path"] = p.path;
        } else if constexpr (std::is_same_v<T, WHNFString>) {
            j["value"] = p.value;
            j["context"] = p.context;
        } else if constexpr (std::is_same_v<T, WHNFAttrs>) {
            j["names"] = p.names;
        } else if constexpr (std::is_same_v<T, WHNFList>) {
            j["size"] = p.size;
        }
        /* WHNFEmpty: nothing to add beyond `type`. */
    }, r.payload);
}

void from_json(const nlohmann::json & j, ResultWHNF & r)
{
    j.at("type").get_to(r.type);
    if (r.type == "int") {
        r.payload = WHNFInt{j.at("value").get<int64_t>()};
    } else if (r.type == "bool") {
        r.payload = WHNFBool{j.at("value").get<bool>()};
    } else if (r.type == "float") {
        r.payload = WHNFFloat{j.at("value").get<double>()};
    } else if (r.type == "path") {
        r.payload = WHNFPath{j.at("path").get<std::string>()};
    } else if (r.type == "string") {
        WHNFString s;
        j.at("value").get_to(s.value);
        if (j.contains("context"))
            j.at("context").get_to(s.context);
        r.payload = std::move(s);
    } else if (r.type == "set") {  /* objectTypeToString(nAttrs) = "set" */
        WHNFAttrs a;
        j.at("names").get_to(a.names);
        r.payload = std::move(a);
    } else if (r.type == "list") {
        r.payload = WHNFList{j.at("size").get<size_t>()};
    } else {
        r.payload = WHNFEmpty{};
    }
}

// ---------------------------------------------------------------------------
// QueryLeaf serialization
// ---------------------------------------------------------------------------

/* StateHashLeaf encodes as the bare hex string (wire-format compatible with
   the previous std::string `from` field). OuterLeaf encodes as an
   object so a parser can distinguish the two on the rare cases where it
   matters during transition; OuterLeafs should not appear in recorded
   artifacts. */
void to_json(nlohmann::json & j, const QueryLeaf & leaf)
{
    if (leaf.isStateHash())
        j = leaf.stateHash();
    else
        j = nlohmann::json{{"ambient", leaf.outerIndex()}};
}

void from_json(const nlohmann::json & j, QueryLeaf & leaf)
{
    if (j.is_string())
        leaf = QueryLeaf{j.get<std::string>()};
    else if (j.is_object() && j.contains("ambient"))
        leaf = QueryLeaf{OuterLeaf{j.at("ambient").get<int>()}};
    else
        throw nlohmann::json::type_error::create(
            302, "QueryLeaf JSON must be a hex string or {\"ambient\": N}", &j);
}

// ---------------------------------------------------------------------------
// PathExpr serialization
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const PathStep & s)
{
    if (s.kind == PathStep::Kind::GetAttr) {
        j = nlohmann::json{{"kind", "attr"}, {"name", s.name}};
    } else if (s.kind == PathStep::Kind::GetListElem) {
        j = nlohmann::json{{"kind", "listElem"}, {"index", s.index}};
    } else {
        /* Apply: persist both sub-paths inline. fromStateHashes indices live
           alongside so the walker knows which cb_arg each side
           resolves against (= multi-root applies are supported even
           though today's recorder only emits single-root). */
        j = nlohmann::json{
            {"kind", "apply"},
            {"fnRootIndex", s.fnRootIndex},
            {"argRootIndex", s.argRootIndex},
            {"fnPath", s.fnPath ? *s.fnPath : PathExpr{}},
            {"argPath", s.argPath ? *s.argPath : PathExpr{}},
        };
    }
}

void from_json(const nlohmann::json & j, PathStep & s)
{
    auto kindStr = j.at("kind").get<std::string>();
    if (kindStr == "attr") {
        s.kind = PathStep::Kind::GetAttr;
        j.at("name").get_to(s.name);
    } else if (kindStr == "listElem") {
        s.kind = PathStep::Kind::GetListElem;
        j.at("index").get_to(s.index);
    } else if (kindStr == "apply") {
        s.kind = PathStep::Kind::Apply;
        s.fnRootIndex = j.at("fnRootIndex").get<size_t>();
        s.argRootIndex = j.at("argRootIndex").get<size_t>();
        s.fnPath = std::make_shared<PathExpr>(j.at("fnPath").get<PathExpr>());
        s.argPath = std::make_shared<PathExpr>(j.at("argPath").get<PathExpr>());
    } else {
        throw nlohmann::json::type_error::create(
            302, "PathStep JSON: unknown kind \"" + kindStr + "\"", &j);
    }
}

std::strong_ordering PathStep::operator<=>(const PathStep & other) const
{
    if (auto c = static_cast<int>(kind) <=> static_cast<int>(other.kind); c != 0) return c;
    if (kind == Kind::GetAttr) return name <=> other.name;
    if (kind == Kind::GetListElem) return index <=> other.index;
    if (auto c = fnRootIndex <=> other.fnRootIndex; c != 0) return c;
    if (auto c = argRootIndex <=> other.argRootIndex; c != 0) return c;
    auto cmpPtr = [](const std::shared_ptr<PathExpr> & a,
                     const std::shared_ptr<PathExpr> & b) -> std::strong_ordering {
        if (!a && !b) return std::strong_ordering::equal;
        if (!a) return std::strong_ordering::less;
        if (!b) return std::strong_ordering::greater;
        return *a <=> *b;
    };
    if (auto c = cmpPtr(fnPath, other.fnPath); c != 0) return c;
    return cmpPtr(argPath, other.argPath);
}

bool PathStep::operator==(const PathStep & other) const
{
    return (*this <=> other) == 0;
}

void to_json(nlohmann::json & j, const PathExpr & p)
{
    j = p.steps;  // serialize as a plain array of steps
}

void from_json(const nlohmann::json & j, PathExpr & p)
{
    j.get_to(p.steps);
}

// ---------------------------------------------------------------------------
// Query payload serialization
// ---------------------------------------------------------------------------

/* Conditional emission for the new path-carrying fields. Empty
   `fromStateHashes` / `path` are omitted so existing serialized forms (=
   pre-#86 fields) hash byte-identically to before. Emitters that
   wire path through populate one or both; consumers read them back
   tolerantly. */
static void emitPathAndFromStateHashes(
    nlohmann::json & params,
    const std::vector<QueryLeaf> & fromStateHashes,
    const PathExpr & path)
{
    if (!fromStateHashes.empty()) params["fromStateHashes"] = fromStateHashes;
    if (!path.steps.empty()) params["path"] = path;
}

static void parsePathAndFromStateHashes(
    const nlohmann::json & params,
    std::vector<QueryLeaf> & fromStateHashes,
    PathExpr & path)
{
    fromStateHashes.clear();
    path = {};
    if (params.contains("fromStateHashes")) params.at("fromStateHashes").get_to(fromStateHashes);
    if (params.contains("path")) params.at("path").get_to(path);
}

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
    emitPathAndFromStateHashes(j["params"], q.fromStateHashes, q.path);
    if (q.callbackApply)
        j["params"]["callbackApply"] = *q.callbackApply;
}

void from_json(const nlohmann::json & j, QueryGetAttr & q)
{
    j.at("params").at("name").get_to(q.name);
    j.at("params").at("from").get_to(q.from);
    parsePathAndFromStateHashes(j.at("params"), q.fromStateHashes, q.path);
    if (j.at("params").contains("callbackApply")) {
        CallbackApplyRef r;
        j.at("params").at("callbackApply").get_to(r);
        q.callbackApply = std::move(r);
    }
}

void to_json(nlohmann::json & j, const QueryGetListOfStrings & q)
{
    j = nlohmann::json{{"query", QueryGetListOfStrings::tag}, {"params", {{"from", q.from}}}};
    emitPathAndFromStateHashes(j["params"], q.fromStateHashes, q.path);
}

void from_json(const nlohmann::json & j, QueryGetListOfStrings & q)
{
    j.at("params").at("from").get_to(q.from);
    parsePathAndFromStateHashes(j.at("params"), q.fromStateHashes, q.path);
}

void to_json(nlohmann::json & j, const QueryGetListElem & q)
{
    j = nlohmann::json{{"query", QueryGetListElem::tag}, {"params", {{"from", q.from}, {"index", q.index}}}};
    emitPathAndFromStateHashes(j["params"], q.fromStateHashes, q.path);
}

void from_json(const nlohmann::json & j, QueryGetListElem & q)
{
    j.at("params").at("from").get_to(q.from);
    j.at("params").at("index").get_to(q.index);
}

void to_json(nlohmann::json & j, const QueryGetFunctionInfo & q)
{
    j = nlohmann::json{{"query", QueryGetFunctionInfo::tag}, {"params", {{"from", q.from}}}};
    emitPathAndFromStateHashes(j["params"], q.fromStateHashes, q.path);
}

void from_json(const nlohmann::json & j, QueryGetFunctionInfo & q)
{
    j.at("params").at("from").get_to(q.from);
    parsePathAndFromStateHashes(j.at("params"), q.fromStateHashes, q.path);
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

void to_json(nlohmann::json & j, const QueryGetWHNF & q)
{
    j = nlohmann::json{{"query", QueryGetWHNF::tag}, {"params", {{"from", q.from}}}};
    emitPathAndFromStateHashes(j["params"], q.fromStateHashes, q.path);
    if (q.callbackApply)
        j["params"]["callbackApply"] = *q.callbackApply;
}

void from_json(const nlohmann::json & j, QueryGetWHNF & q)
{
    j.at("params").at("from").get_to(q.from);
    parsePathAndFromStateHashes(j.at("params"), q.fromStateHashes, q.path);
    if (j.at("params").contains("callbackApply")) {
        CallbackApplyRef r;
        j.at("params").at("callbackApply").get_to(r);
        q.callbackApply = std::move(r);
    }
}

void to_json(nlohmann::json & j, const QueryApply & q)
{
    j = nlohmann::json{{"query", QueryApply::tag}, {"params", {{"fn", q.fn}, {"arg", q.arg}}}};
    /* Per-arg mode (= ApplyResultSubject state hash computation under
       per-arg centralization) emits fromStateHashes + fn/argPath + root
       indices. Legacy direct mode leaves them empty. */
    if (!q.fromStateHashes.empty())
        j["params"]["fromStateHashes"] = q.fromStateHashes;
    if (!q.fnPath.steps.empty())
        j["params"]["fnPath"] = q.fnPath;
    if (!q.argPath.steps.empty())
        j["params"]["argPath"] = q.argPath;
    if (q.fnRootIndex != 0)
        j["params"]["fnRootIndex"] = q.fnRootIndex;
    if (q.argRootIndex != 0)
        j["params"]["argRootIndex"] = q.argRootIndex;
}

void from_json(const nlohmann::json & j, QueryApply & q)
{
    j.at("params").at("fn").get_to(q.fn);
    j.at("params").at("arg").get_to(q.arg);
    const auto & params = j.at("params");
    if (params.contains("fromStateHashes"))
        params.at("fromStateHashes").get_to(q.fromStateHashes);
    if (params.contains("fnPath"))
        params.at("fnPath").get_to(q.fnPath);
    if (params.contains("argPath"))
        params.at("argPath").get_to(q.argPath);
    if (params.contains("fnRootIndex"))
        params.at("fnRootIndex").get_to(q.fnRootIndex);
    if (params.contains("argRootIndex"))
        params.at("argRootIndex").get_to(q.argRootIndex);
}

void to_json(nlohmann::json & j, const QueryCallbackApply & q)
{
    j = nlohmann::json{
        {"query", QueryCallbackApply::tag},
        {"params", {
            {"fn", q.fn},
            {"argObsSet", q.argObsSet},
            {"argAncestry", q.argAncestry},
            {"argDepth", q.argDepth},
        }}};
}

void from_json(const nlohmann::json & j, QueryCallbackApply & q)
{
    j.at("params").at("fn").get_to(q.fn);
    j.at("params").at("argObsSet").get_to(q.argObsSet);
    if (j.at("params").contains("argAncestry"))
        j.at("params").at("argAncestry").get_to(q.argAncestry);
    if (j.at("params").contains("argDepth"))
        j.at("params").at("argDepth").get_to(q.argDepth);
}

void to_json(nlohmann::json & j, const CallbackApplyRef & r)
{
    j = nlohmann::json{
        {"fn", r.fn},
        {"argObsSet", r.argObsSet},
        {"argAncestry", r.argAncestry},
        {"argDepth", r.argDepth},
    };
}

void from_json(const nlohmann::json & j, CallbackApplyRef & r)
{
    j.at("fn").get_to(r.fn);
    j.at("argObsSet").get_to(r.argObsSet);
    if (j.contains("argAncestry"))
        j.at("argAncestry").get_to(r.argAncestry);
    if (j.contains("argDepth"))
        j.at("argDepth").get_to(r.argDepth);
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
        if (type == OuterValueRequest::tag) {
            OuterValueRequest req;
            from_json(j["request"], req);
            OuterValueResponse resp;
            from_json(j["response"], resp);
            return Response<OuterValueRequest>{req, resp};
        }
        if (type == InnerValueRequestPayload::tag) {
            InnerValueRequestPayload req;
            from_json(j["request"], req);
            InnerValueResponsePayload resp;
            from_json(j["response"], resp);
            return Response<InnerValueRequestPayload>{req, resp};
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
        if (auto r = tryParseQuery<QueryGetListOfStrings>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetListElem>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetWHNF>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryGetFunctionInfo>(type, j))
            return r;
        if (auto r = tryParseQuery<QueryCallbackApply>(type, j))
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
        if (r.contains("values")) {
            Result<ResultListOfStrings> e;
            from_json(j, e);
            return e;
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
    else if constexpr (std::is_same_v<T, ResultListOfStrings>)
        return 2;
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
