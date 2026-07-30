#include "nix/expr/trace-sink.hh"
#include "nix/expr/trace-types.hh"

#include "nix/util/logging.hh"
#include "nix/util/util.hh"

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
// SelectorVariant serialization — discriminator lives here (flat envelope)
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const SelectorVariant & q)
{
    /* Each per-type to_json emits `tag` alongside its fields, so
       delegation suffices. */
    std::visit([&](const auto & sub) { j = sub; }, q);
}

void from_json(const nlohmann::json & j, SelectorVariant & q)
{
    /* Fold-based dispatch — see detail::fromJsonByTag. No
       per-alternative enumeration here; the variant's Ts... drive it. */
    detail::fromJsonByTag(j, q);
}

// ---------------------------------------------------------------------------
// OuterValueRequest / OuterValueResponse serialization
// ---------------------------------------------------------------------------

/* ResultVariant currently has one alternative (ResultWHNF); no
   discriminator needed. Add one if variants grow. */
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
    if (tryParse((ResultWHNF *) nullptr))
        return;
    throw nlohmann::json::parse_error::create(302, 0, "could not parse outer result", &j);
}

void to_json(nlohmann::json & j, const OuterValueRequest & r)
{
    j = nlohmann::json{{"query", r.query}};
}

void from_json(const nlohmann::json & j, OuterValueRequest & r)
{
    j.at("query").get_to(r.query);
}

void to_json(nlohmann::json & j, const OuterValueResponse & r)
{
    resultVariantToJson(j, r.result);
}

void from_json(const nlohmann::json & j, OuterValueResponse & r)
{
    resultVariantFromJson(j, r.result);
}

// ---------------------------------------------------------------------------
// Result payload serialization
// ---------------------------------------------------------------------------

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
        /* WHNFFunction, WHNFNull: tag-only, nothing to add. */
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
    } else if (r.type == "null") {
        r.payload = WHNFNull{};
    } else {
        /* Function types (objectTypeToString(nFunction) = "lambda"),
           and any unrecognised tag. */
        r.payload = WHNFFunction{};
    }
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

/* Flat envelope: each Query type emits `tag` alongside its fields.
   Same JSON regardless of whether the caller went through
   per-type to_json directly or via SelectorVariant's std::visit. */

void to_json(nlohmann::json & j, const SelectorExpr & q)
{
    j = nlohmann::json{{"tag", SelectorExpr::tag}, {"expr", q.expr}, {"baseDir", q.baseDir}};
}

void from_json(const nlohmann::json & j, SelectorExpr & q)
{
    j.at("expr").get_to(q.expr);
    j.at("baseDir").get_to(q.baseDir);
}

void to_json(nlohmann::json & j, const SelectorImport & q)
{
    j = nlohmann::json{{"tag", SelectorImport::tag}, {"path", q.path}};
}

void from_json(const nlohmann::json & j, SelectorImport & q)
{
    j.at("path").get_to(q.path);
}

void to_json(nlohmann::json & j, const SelectorGetAttr & q)
{
    j = nlohmann::json{
        {"tag", SelectorGetAttr::tag},
        {"name", q.name}, {"from", q.from}};
}

void from_json(const nlohmann::json & j, SelectorGetAttr & q)
{
    j.at("name").get_to(q.name);
    j.at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const SelectorGetListElem & q)
{
    j = nlohmann::json{
        {"tag", SelectorGetListElem::tag},
        {"from", q.from}, {"index", q.index}};
}

void from_json(const nlohmann::json & j, SelectorGetListElem & q)
{
    j.at("from").get_to(q.from);
    j.at("index").get_to(q.index);
}

void to_json(nlohmann::json & j, const SelectorGetFunctionInfo & q)
{
    j = nlohmann::json{
        {"tag", SelectorGetFunctionInfo::tag},
        {"from", q.from}};
}

void from_json(const nlohmann::json & j, SelectorGetFunctionInfo & q)
{
    j.at("from").get_to(q.from);
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

void to_json(nlohmann::json & j, const SelectorApply & q)
{
    j = nlohmann::json{{"tag", SelectorApply::tag}, {"fn", q.fn}};
}

void from_json(const nlohmann::json & j, SelectorApply & q)
{
    j.at("fn").get_to(q.fn);
}

void to_json(nlohmann::json & j, const SelectorCallbackApply & q)
{
    j = nlohmann::json{
        {"tag", SelectorCallbackApply::tag},
        {"fn", q.fn},
        {"argObsSet", q.argObsSet},
    };
}

void from_json(const nlohmann::json & j, SelectorCallbackApply & q)
{
    j.at("fn").get_to(q.fn);
    j.at("argObsSet").get_to(q.argObsSet);
}

void to_json(nlohmann::json & j, const SelectorArg & q)
{
    j = nlohmann::json{{"tag", SelectorArg::tag}, {"depth", q.depth}};
}

void from_json(const nlohmann::json & j, SelectorArg & q)
{
    j.at("depth").get_to(q.depth);
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
        return std::nullopt;
    }

    // Query: has "query" and "v"
    if (j.contains("query") && j.contains("v")) {
        auto & q = j["query"];
        if (!q.contains("tag"))
            return std::nullopt;
        auto type = q["tag"].get<std::string_view>();

        if (auto r = tryParseQuery<SelectorExpr>(type, j))
            return r;
        if (auto r = tryParseQuery<SelectorImport>(type, j))
            return r;
        if (auto r = tryParseQuery<SelectorGetAttr>(type, j))
            return r;
        if (auto r = tryParseQuery<SelectorGetListElem>(type, j))
            return r;
        if (auto r = tryParseQuery<SelectorGetFunctionInfo>(type, j))
            return r;
        if (auto r = tryParseQuery<SelectorCallbackApply>(type, j))
            return r;
        if (auto r = tryParseQuery<SelectorApply>(type, j))
            return r;
        if (auto r = tryParseQuery<SelectorArg>(type, j))
            return r;
        return std::nullopt;
    }

    // Result: has "result" and "v"
    if (j.contains("result") && j.contains("v")) {
        auto & r = j["result"];
        if (r.contains("type")) {
            Result<ResultWHNF> e;
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
    if constexpr (std::is_same_v<T, ResultWHNF>)
        return 0;
    else if constexpr (std::is_same_v<T, ResultFunctionInfo>)
        return 1;
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
// SelectorIndex
// ---------------------------------------------------------------------------

SelectorIndex::SelectorIndex(const std::vector<TraceEntry> & trace)
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

// ---------------------------------------------------------------------------
// parseSelectorVariant / describe / fromHashOf
// ---------------------------------------------------------------------------

std::optional<SelectorVariant> parseSelectorVariant(const nlohmann::json & j)
{
    /* Delegate to SelectorVariant's from_json — the discriminator
       lives there. */
    if (!j.is_object() || !j.contains("tag"))
        return std::nullopt;
    try {
        SelectorVariant qv;
        from_json(j, qv);
        return qv;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

static std::string shortHex(const std::string & hex)
{
    return hex.size() > 12 ? hex.substr(0, 12) : hex;
}

std::string describe(const SelectorVariant & query)
{
    return std::visit(
        [](const auto & q) -> std::string {
            using Q = std::decay_t<decltype(q)>;
            std::string out{Q::tag};
            if constexpr (std::is_same_v<Q, SelectorGetAttr>) {
                out += " name=\"" + q.name + "\"";
            } else if constexpr (std::is_same_v<Q, SelectorGetListElem>) {
                out += " index=" + std::to_string(q.index);
            } else if constexpr (std::is_same_v<Q, SelectorApply>) {
                out += " fn=" + shortHex(q.fn);
            } else if constexpr (std::is_same_v<Q, SelectorCallbackApply>) {
                out += " fn=" + shortHex(q.fn) + " obsSet=" + shortHex(q.argObsSet);
            } else if constexpr (std::is_same_v<Q, SelectorExpr>) {
                out += " expr=\"" + q.expr + "\"";
            } else if constexpr (std::is_same_v<Q, SelectorImport>) {
                out += " path=" + q.path;
            } else if constexpr (std::is_same_v<Q, SelectorArg>) {
                out += " depth=" + std::to_string(q.depth);
            }
            return out;
        },
        query);
}

/**
 * True iff a probe's response depends on the referenced Subject's
 * state — i.e. this Selector carries a `from`/`fn`/`arg` state hash
 * that a dispatcher resolves against the caller's cell chain. The
 * response bytes may then differ across callers with the same
 * requestHash at the moment of divergence (see the walker
 * dispatcher's comment on memoization). Root queries without a
 * from-style reference (`SelectorExpr`, `SelectorImport`) never
 * move state and are always safe to memoize.
 */
bool willMoveStateHash(const SelectorVariant & query)
{
    return std::visit(
        overloaded{
            [](const SelectorExpr &) { return false; },
            [](const SelectorImport &) { return false; },
            [](const SelectorGetAttr &) { return true; },
            [](const SelectorGetListElem &) { return true; },
            [](const SelectorGetFunctionInfo &) { return true; },
            [](const SelectorApply &) { return true; },
            [](const SelectorCallbackApply &) { return true; },
            [](const SelectorArg &) { return false; },
        },
        query);
}

std::optional<Hash> fromHashOf(const SelectorVariant & query)
{
    return std::visit(
        [](const auto & q) -> std::optional<Hash> {
            using Q = std::decay_t<decltype(q)>;
            if constexpr (requires { q.from; }) {
                if (!true || std::string{}.empty())
                    return std::nullopt;
                try {
                    return Hash::parseNonSRIUnprefixed(std::string{}, HashAlgorithm::SHA256);
                } catch (...) {
                    return std::nullopt;
                }
            } else {
                return std::nullopt;
            }
        },
        query);
}

void rewriteFrom(SelectorVariant & query, const std::string & newFromHex)
{
    /* #183: `from` fields now carry query-space Q hash as std::string.
       Directly assign newFromHex. */
    std::visit(
        [&](auto & q) {
            using Q = std::decay_t<decltype(q)>;
            if constexpr (requires { q.from; })
                q.from = newFromHex;
        },
        query);
}

nlohmann::json toJson(const SelectorVariant & query)
{
    nlohmann::json j;
    std::visit([&](const auto & q) { j = q; }, query);
    return j;
}

Hash computeSelectorHash(const SelectorVariant & query)
{
    return hashString(HashAlgorithm::SHA256, toJson(query).dump());
}

// ---------------------------------------------------------------------------
// Recursive Selector: comparisons, constructor, pool, converters
// ---------------------------------------------------------------------------

/* Step-type comparisons compare by cachedHash of parent — cheap and
   correct given hashes are content-addressed. */

bool SelectorGetAttrStep::operator==(const SelectorGetAttrStep & other) const
{
    return name == other.name && parent->cachedHash == other.parent->cachedHash;
}
auto SelectorGetAttrStep::operator<=>(const SelectorGetAttrStep & other) const
{
    if (auto c = name <=> other.name; c != 0) return c;
    return parent->cachedHash <=> other.parent->cachedHash;
}

bool SelectorGetListElemStep::operator==(const SelectorGetListElemStep & other) const
{
    return index == other.index && parent->cachedHash == other.parent->cachedHash;
}
auto SelectorGetListElemStep::operator<=>(const SelectorGetListElemStep & other) const
{
    if (auto c = index <=> other.index; c != 0) return c;
    return parent->cachedHash <=> other.parent->cachedHash;
}

bool SelectorGetFunctionInfoStep::operator==(const SelectorGetFunctionInfoStep & other) const
{
    return parent->cachedHash == other.parent->cachedHash;
}
auto SelectorGetFunctionInfoStep::operator<=>(const SelectorGetFunctionInfoStep & other) const
{
    return parent->cachedHash <=> other.parent->cachedHash;
}

bool SelectorApplyStep::operator==(const SelectorApplyStep & other) const
{
    return parent->cachedHash == other.parent->cachedHash;
}
auto SelectorApplyStep::operator<=>(const SelectorApplyStep & other) const
{
    return parent->cachedHash <=> other.parent->cachedHash;
}

bool SelectorCallbackApplyStep::operator==(const SelectorCallbackApplyStep & other) const
{
    return argObsSet == other.argObsSet && parent->cachedHash == other.parent->cachedHash;
}
auto SelectorCallbackApplyStep::operator<=>(const SelectorCallbackApplyStep & other) const
{
    if (auto c = argObsSet <=> other.argObsSet; c != 0) return c;
    return parent->cachedHash <=> other.parent->cachedHash;
}

/* Convert a step Node to the flat SelectorVariant form by emitting the
   parent's hex hash into the from/fn field. Leaves pass through. */
SelectorVariant toVariant(const Selector & s)
{
    return std::visit(
        overloaded{
            [](const SelectorExpr & q) -> SelectorVariant { return q; },
            [](const SelectorImport & q) -> SelectorVariant { return q; },
            [](const SelectorArg & q) -> SelectorVariant { return q; },
            [](const SelectorGetAttrStep & q) -> SelectorVariant {
                return SelectorGetAttr{q.name, q.parent->cachedHash.to_string(HashFormat::Base16, false)};
            },
            [](const SelectorGetListElemStep & q) -> SelectorVariant {
                return SelectorGetListElem{q.parent->cachedHash.to_string(HashFormat::Base16, false), q.index};
            },
            [](const SelectorGetFunctionInfoStep & q) -> SelectorVariant {
                return SelectorGetFunctionInfo{q.parent->cachedHash.to_string(HashFormat::Base16, false)};
            },
            [](const SelectorApplyStep & q) -> SelectorVariant {
                return SelectorApply{q.parent->cachedHash.to_string(HashFormat::Base16, false)};
            },
            [](const SelectorCallbackApplyStep & q) -> SelectorVariant {
                return SelectorCallbackApply{
                    q.parent->cachedHash.to_string(HashFormat::Base16, false), q.argObsSet};
            },
        },
        s.node);
}

/* Compute a Selector's cachedHash at construction: flatten to
   SelectorVariant, dump JSON, SHA-256. Same bytes as before the
   refactor — the DB compatibility invariant. */
static Hash hashOfNode(const SelectorNode & node)
{
    /* We can't call toVariant() here without a full Selector to wrap,
       so inline the flatten step. Leaves already carry no parent so
       they hash directly; step types stringify parent->cachedHash. */
    SelectorVariant v = std::visit(
        overloaded{
            [](const SelectorExpr & q) -> SelectorVariant { return q; },
            [](const SelectorImport & q) -> SelectorVariant { return q; },
            [](const SelectorArg & q) -> SelectorVariant { return q; },
            [](const SelectorGetAttrStep & q) -> SelectorVariant {
                return SelectorGetAttr{q.name, q.parent->cachedHash.to_string(HashFormat::Base16, false)};
            },
            [](const SelectorGetListElemStep & q) -> SelectorVariant {
                return SelectorGetListElem{q.parent->cachedHash.to_string(HashFormat::Base16, false), q.index};
            },
            [](const SelectorGetFunctionInfoStep & q) -> SelectorVariant {
                return SelectorGetFunctionInfo{q.parent->cachedHash.to_string(HashFormat::Base16, false)};
            },
            [](const SelectorApplyStep & q) -> SelectorVariant {
                return SelectorApply{q.parent->cachedHash.to_string(HashFormat::Base16, false)};
            },
            [](const SelectorCallbackApplyStep & q) -> SelectorVariant {
                return SelectorCallbackApply{
                    q.parent->cachedHash.to_string(HashFormat::Base16, false), q.argObsSet};
            },
        },
        node);
    return computeSelectorHash(v);
}

Selector::Selector(SelectorNode n)
    : node(std::move(n))
    , cachedHash(hashOfNode(node))
{
}

ref<const Selector> SelectorPool::intern(SelectorNode node)
{
    auto h = hashOfNode(node);
    if (auto it = pool.find(h); it != pool.end())
        return it->second;
    auto s = make_ref<const Selector>(std::move(node));
    pool.emplace(h, s);
    return s;
}

std::optional<ref<const Selector>> SelectorPool::find(const Hash & h) const
{
    if (auto it = pool.find(h); it != pool.end())
        return it->second;
    return std::nullopt;
}

std::optional<ref<const Selector>> fromVariant(
    const SelectorVariant & v, SelectorPool & pool)
{
    /* Resolve a hex parent hash to an already-interned Selector.
       Returns nullopt if the hex doesn't parse or the pool lacks the
       referenced parent — callers reconstruct bottom-up, so the
       parent should already be in the pool by the time this fires. */
    auto lookupParent = [&](const std::string & hex) -> std::optional<ref<const Selector>> {
        try {
            auto h = Hash::parseNonSRIUnprefixed(hex, HashAlgorithm::SHA256);
            return pool.find(h);
        } catch (...) {
            return std::nullopt;
        }
    };

    return std::visit(
        overloaded{
            [&](const SelectorExpr & q) -> std::optional<ref<const Selector>> {
                return pool.intern(q);
            },
            [&](const SelectorImport & q) -> std::optional<ref<const Selector>> {
                return pool.intern(q);
            },
            [&](const SelectorArg & q) -> std::optional<ref<const Selector>> {
                return pool.intern(q);
            },
            [&](const SelectorGetAttr & q) -> std::optional<ref<const Selector>> {
                auto p = lookupParent(q.from);
                if (!p) return std::nullopt;
                return pool.intern(SelectorGetAttrStep{q.name, *p});
            },
            [&](const SelectorGetListElem & q) -> std::optional<ref<const Selector>> {
                auto p = lookupParent(q.from);
                if (!p) return std::nullopt;
                return pool.intern(SelectorGetListElemStep{q.index, *p});
            },
            [&](const SelectorGetFunctionInfo & q) -> std::optional<ref<const Selector>> {
                auto p = lookupParent(q.from);
                if (!p) return std::nullopt;
                return pool.intern(SelectorGetFunctionInfoStep{*p});
            },
            [&](const SelectorApply & q) -> std::optional<ref<const Selector>> {
                auto p = lookupParent(q.fn);
                if (!p) return std::nullopt;
                return pool.intern(SelectorApplyStep{*p});
            },
            [&](const SelectorCallbackApply & q) -> std::optional<ref<const Selector>> {
                auto p = lookupParent(q.fn);
                if (!p) return std::nullopt;
                return pool.intern(SelectorCallbackApplyStep{q.argObsSet, *p});
            },
        },
        v);
}

} // namespace nix::trace
