#include "nix/expr/trace-sink.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-decision-graph.hh"

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
// SelectorNode serialization — discriminator lives here (flat envelope)
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const SelectorNode & q)
{
    /* Each per-type to_json emits `tag` alongside its fields, so
       delegation suffices. */
    std::visit([&](const auto & sub) { j = sub; }, q);
}

/* No from_json for SelectorNode — step alternatives need SelectorPool.
   Use nodeFromJson. */

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
    nlohmann::json qJson;
    std::visit([&](const auto & sub) { qJson = sub; }, r.query->node);
    j = nlohmann::json{{"query", std::move(qJson)}};
}
/* No from_json for OuterValueRequest — needs SelectorPool. */

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
   per-type to_json directly or via SelectorNode's std::visit. */

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

static std::string hexOf(const ref<const Selector> & s)
{
    return s->cachedHash.to_string(HashFormat::Base16, false);
}

void to_json(nlohmann::json & j, const SelectorGetAttr & q)
{
    j = nlohmann::json{{"tag", SelectorGetAttr::tag}, {"name", q.name}, {"from", hexOf(q.parent)}};
}

void to_json(nlohmann::json & j, const SelectorGetListElem & q)
{
    j = nlohmann::json{{"tag", SelectorGetListElem::tag}, {"from", hexOf(q.parent)}, {"index", q.index}};
}

void to_json(nlohmann::json & j, const SelectorGetFunctionInfo & q)
{
    j = nlohmann::json{{"tag", SelectorGetFunctionInfo::tag}, {"from", hexOf(q.parent)}};
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
    j = nlohmann::json{{"tag", SelectorApply::tag}, {"fn", hexOf(q.parent)}};
}

void to_json(nlohmann::json & j, const SelectorCallbackApply & q)
{
    j = nlohmann::json{{"tag", SelectorCallbackApply::tag}, {"fn", hexOf(q.parent)}, {"argObsSet", q.argObsSet}};
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
        /* OuterValueRequest needs SelectorPool to reconstruct. Skip. */
        if (type == OuterValueRequest::tag)
            return std::nullopt;
        return std::nullopt;
    }

    // Query: has "query" and "v"
    if (j.contains("query") && j.contains("v")) {
        auto & q = j["query"];
        if (!q.contains("tag"))
            return std::nullopt;
        auto type = q["tag"].get<std::string_view>();

        /* Step Selectors have no from_json (need SelectorPool). Trace-file
           tooling only decodes leaves here. */
        if (auto r = tryParseQuery<SelectorExpr>(type, j))
            return r;
        if (auto r = tryParseQuery<SelectorImport>(type, j))
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
        std::visit(overloaded{
            /* Only Result<T> envelopes contribute to the result-index — enumerate
               by envelope template, not by field-presence, so a future variant
               alternative can't silently be absorbed as a "result-like" entry. */
            [&]<typename T>(const Result<T> & entry) {
                auto key = std::make_pair(resultTypeIndex<T>(), entry.v);
                resultIndex[key] = i;
            },
            /* Response<T> and Query<T> envelopes: no contribution here. */
            [](const auto &) {},
        }, trace[i]);
    }

    // Transform queries to completed queries
    std::vector<CorrelatedTraceEntry> result;
    result.reserve(trace.size());

    for (const auto & entry : trace) {
        std::visit(overloaded{
            /* Query<T> envelopes become CompletedQuery<T> with a resolved
               resultIndex. Enumerated by envelope template so a new
               variant alternative can't silently take this branch. */
            [&]<typename T>(const Query<T> & e) {
                /* Step Selectors have no default constructor. Skip
                   — matches parseTraceEntry's step skipping. */
                if constexpr (std::is_default_constructible_v<T>) {
                    CompletedQuery<T> completed;
                    completed.query = e.query;
                    completed.v = e.v;
                    auto key = std::make_pair(queryResultTypeIndex<T>(), e.v);
                    auto it = resultIndex.find(key);
                    completed.resultIndex = (it != resultIndex.end()) ? it->second : 0;
                    result.push_back(completed);
                }
            },
            /* Everything else — Response<T>, Result<T> — passes through
               unchanged into the CorrelatedTraceEntry variant. */
            [&](const auto & e) { result.push_back(e); },
        }, entry);
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
        std::visit(overloaded{
            [&]<typename T>(const Result<T> & entry) {
                auto key = std::make_pair(resultTypeIndex<T>(), entry.v);
                resultLookup[key] = i;
            },
            [](const auto &) {},
        }, trace[i]);
    }

    // Second pass: index queries (only those with matching results)
    for (size_t i = 0; i < trace.size(); ++i) {
        std::visit(overloaded{
            /* Only Query<T> envelopes go into the index. Enumerated by
               envelope template — no field-presence heuristic. */
            [&]<typename T>(const Query<T> & entry) {
                auto key = std::make_pair(queryResultTypeIndex<T>(), entry.v);
                auto it = resultLookup.find(key);
                if (it != resultLookup.end()) {
                    index[entry.query] = IndexEntry{i, it->second};
                }
            },
            [](const auto &) {},
        }, trace[i]);
    }
}

// ---------------------------------------------------------------------------
// Selector step-type comparisons + Selector adapters
// ---------------------------------------------------------------------------

bool SelectorGetAttr::operator==(const SelectorGetAttr & other) const
{ return name == other.name && parent->cachedHash == other.parent->cachedHash; }
std::strong_ordering SelectorGetAttr::operator<=>(const SelectorGetAttr & other) const
{
    if (auto c = name <=> other.name; c != 0) return c;
    return parent->cachedHash <=> other.parent->cachedHash;
}

bool SelectorGetListElem::operator==(const SelectorGetListElem & other) const
{ return index == other.index && parent->cachedHash == other.parent->cachedHash; }
std::strong_ordering SelectorGetListElem::operator<=>(const SelectorGetListElem & other) const
{
    if (auto c = index <=> other.index; c != 0) return c;
    return parent->cachedHash <=> other.parent->cachedHash;
}

bool SelectorGetFunctionInfo::operator==(const SelectorGetFunctionInfo & other) const
{ return parent->cachedHash == other.parent->cachedHash; }
std::strong_ordering SelectorGetFunctionInfo::operator<=>(const SelectorGetFunctionInfo & other) const
{ return parent->cachedHash <=> other.parent->cachedHash; }

bool SelectorApply::operator==(const SelectorApply & other) const
{ return parent->cachedHash == other.parent->cachedHash; }
std::strong_ordering SelectorApply::operator<=>(const SelectorApply & other) const
{ return parent->cachedHash <=> other.parent->cachedHash; }

bool SelectorCallbackApply::operator==(const SelectorCallbackApply & other) const
{ return argObsSet == other.argObsSet && parent->cachedHash == other.parent->cachedHash; }
std::strong_ordering SelectorCallbackApply::operator<=>(const SelectorCallbackApply & other) const
{
    if (auto c = argObsSet <=> other.argObsSet; c != 0) return c;
    return parent->cachedHash <=> other.parent->cachedHash;
}

Hash computeSelectorHash(const SelectorNode & query)
{
    nlohmann::json j;
    to_json(j, query);
    return hashString(HashAlgorithm::SHA256, j.dump());
}

Selector::Selector(SelectorNode n)
    : node(std::move(n))
    , cachedHash(computeSelectorHash(node))
{}

void SelectorPool::bind(TracingDecisionGraph & graph)
{
    backing = &graph;
}

ref<const Selector> SelectorPool::intern(SelectorNode node)
{
    auto h = computeSelectorHash(node);
    if (auto it = pool.find(h); it != pool.end())
        return it->second;
    auto s = make_ref<const Selector>(std::move(node));
    pool.emplace(h, s);
    /* Mirror into the Selectors DB row so cross-session lookups
       find this Selector without depending on the intern order of
       the reconstructing session. INSERT OR IGNORE — a concurrent
       recorder that already wrote the same row is a harmless no-op. */
    auto cbor = nlohmann::json::to_cbor(toJson(*s));
    backing->insertSelector(
        h, std::string_view(reinterpret_cast<const char *>(cbor.data()), cbor.size()));
    return s;
}

std::optional<ref<const Selector>> SelectorPool::findByHex(std::string_view hex)
{
    Hash h{HashAlgorithm::SHA256};
    try {
        h = Hash::parseNonSRIUnprefixed(std::string(hex), HashAlgorithm::SHA256);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    return find(h);
}

std::optional<ref<const Selector>> SelectorPool::find(const Hash & h)
{
    if (auto it = pool.find(h); it != pool.end()) return it->second;
    /* Memory miss — try DB. On hit, decode the stored payload and
       intern the reconstructed Selector so subsequent lookups are
       memory-only. `nodeFromJson` recurses back through this same
       pool for parent hashes, so a whole chain reconstructs
       depth-first from the leaf up. Chains are shallow (< a dozen
       even under nixpkgs attribute nesting), so recursion is fine. */
    auto payload = backing->getSelectorPayload(h);
    if (!payload) return std::nullopt;
    auto bytes = reinterpret_cast<const uint8_t *>(payload->data());
    auto j = nlohmann::json::from_cbor(bytes, bytes + payload->size());
    return nodeFromJson(j, *this);
}

nlohmann::json toJson(const SelectorNode & query)
{
    nlohmann::json j;
    to_json(j, query);
    return j;
}

nlohmann::json toJson(const Selector & s) { return toJson(s.node); }

static std::string shortH(const Hash & h) { return h.to_string(HashFormat::Base16, false).substr(0, 12); }

std::string describe(const SelectorNode & query)
{
    return std::visit(
        [](const auto & q) -> std::string {
            using Q = std::decay_t<decltype(q)>;
            std::string out{Q::tag};
            if constexpr (std::is_same_v<Q, SelectorGetAttr>) {
                out += " name=\"" + q.name + "\" from=" + shortH(q.parent->cachedHash);
            } else if constexpr (std::is_same_v<Q, SelectorGetListElem>) {
                out += " index=" + std::to_string(q.index) + " from=" + shortH(q.parent->cachedHash);
            } else if constexpr (std::is_same_v<Q, SelectorGetFunctionInfo>) {
                out += " from=" + shortH(q.parent->cachedHash);
            } else if constexpr (std::is_same_v<Q, SelectorApply>) {
                out += " fn=" + shortH(q.parent->cachedHash);
            } else if constexpr (std::is_same_v<Q, SelectorCallbackApply>) {
                out += " fn=" + shortH(q.parent->cachedHash)
                    + " obsSet=" + (q.argObsSet.size() > 12 ? q.argObsSet.substr(0, 12) : q.argObsSet);
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

std::string describe(const Selector & s) { return describe(s.node); }

bool willMoveStateHash(const SelectorNode & query)
{
    return std::visit(
        overloaded{
            [](const SelectorExpr &) { return false; },
            [](const SelectorImport &) { return false; },
            [](const SelectorArg &) { return false; },
            [](const SelectorGetAttr &) { return true; },
            [](const SelectorGetListElem &) { return true; },
            [](const SelectorGetFunctionInfo &) { return true; },
            [](const SelectorApply &) { return true; },
            [](const SelectorCallbackApply &) { return true; },
        },
        query);
}

bool willMoveStateHash(const Selector & s) { return willMoveStateHash(s.node); }

std::optional<ref<const Selector>> nodeFromJson(
    const nlohmann::json & j, SelectorPool & pool)
{
    if (!j.is_object() || !j.contains("tag"))
        return std::nullopt;
    auto tag = j.at("tag").get<std::string_view>();

    auto lookupParent = [&](const std::string & hex) { return pool.findByHex(hex); };

    try {
        if (tag == SelectorExpr::tag) {
            SelectorExpr s;
            j.at("expr").get_to(s.expr);
            j.at("baseDir").get_to(s.baseDir);
            return pool.intern(std::move(s));
        }
        if (tag == SelectorImport::tag) {
            SelectorImport s;
            j.at("path").get_to(s.path);
            return pool.intern(std::move(s));
        }
        if (tag == SelectorArg::tag) {
            SelectorArg s;
            j.at("depth").get_to(s.depth);
            return pool.intern(std::move(s));
        }
        if (tag == SelectorGetAttr::tag) {
            auto p = lookupParent(j.at("from").get<std::string>());
            if (!p) return std::nullopt;
            return pool.intern(SelectorGetAttr{j.at("name").get<std::string>(), *p});
        }
        if (tag == SelectorGetListElem::tag) {
            auto p = lookupParent(j.at("from").get<std::string>());
            if (!p) return std::nullopt;
            return pool.intern(SelectorGetListElem{j.at("index").get<size_t>(), *p});
        }
        if (tag == SelectorGetFunctionInfo::tag) {
            auto p = lookupParent(j.at("from").get<std::string>());
            if (!p) return std::nullopt;
            return pool.intern(SelectorGetFunctionInfo{*p});
        }
        if (tag == SelectorApply::tag) {
            auto p = lookupParent(j.at("fn").get<std::string>());
            if (!p) return std::nullopt;
            return pool.intern(SelectorApply{*p});
        }
        if (tag == SelectorCallbackApply::tag) {
            auto p = lookupParent(j.at("fn").get<std::string>());
            if (!p) return std::nullopt;
            return pool.intern(SelectorCallbackApply{j.at("argObsSet").get<std::string>(), *p});
        }
    } catch (const std::exception &) {
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace nix::trace
