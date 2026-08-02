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

/* to_json emits {tag, absPath}; from_json reads absPath. Register
   via NIX_SELECTOR_STR_SERDE below so the shape matches the Request
   variant's tag-based decode. */

void to_json(nlohmann::json & j, const FileReadResponse & r)
{
    j = nlohmann::json{{"contentHash", r.contentHash.to_string(HashFormat::SRI, true)}};
}

void from_json(const nlohmann::json & j, FileReadResponse & r)
{
    r.contentHash = Hash::parseSRI(j.at("contentHash").get<std::string>());
}

/* to_json emits {tag, name}; from_json reads name. Registered via
   NIX_SELECTOR_STR_SERDE below alongside FileReadRequest. */

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

/* Resolved-phase OuterValueRequest to_json lives below the
   NIX_SELECTOR_STR_SERDE definitions — it delegates to
   StringOuterValueRequest's macro-generated to_json.
   No from_json for OuterValueRequest — needs SelectorPool.
   Use `resolve(StringOuterValueRequest, pool)`. */

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
// Query payload serialization
// ---------------------------------------------------------------------------

/* Flat envelope: each Query type emits `tag` alongside its fields.
   Same JSON regardless of whether the caller went through
   per-type to_json directly or via SelectorNode's std::visit. */

/* Tag-tagged serde for String-phase alternatives and phase-independent
   leaves. `to_json` emits `{"tag": <Type::tag>, <fields...>}`;
   `from_json` reads the listed fields via NLOHMANN_JSON_FROM (throws
   on missing — no defensive substitution).

   Macro is codebase-local because nlohmann's own NLOHMANN_DEFINE_TYPE_*
   family emits fields only, no tag prefix. */
#define NIX_SELECTOR_STR_SERDE(Type, ...) \
    void to_json(nlohmann::json & nlohmann_json_j, const Type & nlohmann_json_t) \
    { \
        nlohmann_json_j = nlohmann::json{{"tag", Type::tag}}; \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__)) \
    } \
    void from_json(const nlohmann::json & nlohmann_json_j, Type & nlohmann_json_t) \
    { \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, __VA_ARGS__)) \
    }

NIX_SELECTOR_STR_SERDE(SelectorExpr, expr, baseDir)
NIX_SELECTOR_STR_SERDE(SelectorImport, path)
NIX_SELECTOR_STR_SERDE(SelectorArg, depth)
NIX_SELECTOR_STR_SERDE(StringSelectorGetAttr, name, parent)
NIX_SELECTOR_STR_SERDE(StringSelectorGetListElem, index, parent)
NIX_SELECTOR_STR_SERDE(StringSelectorGetFunctionInfo, parent)
NIX_SELECTOR_STR_SERDE(StringSelectorApply, parent)
NIX_SELECTOR_STR_SERDE(StringSelectorCallbackApply, argObsSet, parent)
NIX_SELECTOR_STR_SERDE(StringOuterValueRequest, query)
NIX_SELECTOR_STR_SERDE(FileReadRequest, absPath)
NIX_SELECTOR_STR_SERDE(GetEnvRequest, name)

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

/* Resolved-phase step to_json: hex-render parents via unresolve. Used
   by log paths (tracingCacheLog, describe) so the textual shape matches
   the on-wire form. Actual persistence goes through unresolve directly. */
static std::string hexOf(const ref<const Selector> & s)
{
    return s->cachedHash.to_string(HashFormat::Base16, false);
}
void to_json(nlohmann::json & j, const SelectorGetAttr & q)
{ to_json(j, StringSelectorGetAttr{q.name, hexOf(q.parent)}); }
void to_json(nlohmann::json & j, const SelectorGetListElem & q)
{ to_json(j, StringSelectorGetListElem{q.index, hexOf(q.parent)}); }
void to_json(nlohmann::json & j, const SelectorGetFunctionInfo & q)
{ to_json(j, StringSelectorGetFunctionInfo{hexOf(q.parent)}); }
void to_json(nlohmann::json & j, const SelectorApply & q)
{ to_json(j, StringSelectorApply{hexOf(q.parent)}); }
void to_json(nlohmann::json & j, const SelectorCallbackApply & q)
{ to_json(j, StringSelectorCallbackApply{q.argObsSet.to_string(HashFormat::Base16, false), hexOf(q.parent)}); }
void to_json(nlohmann::json & j, const OuterValueRequest & r)
{ to_json(j, StringOuterValueRequest{hexOf(r.query)}); }

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

/* Resolved-phase comparators: compare parent by cachedHash equality.
   String-phase comparators (defined below) compare parent hex strings
   directly — different semantics per phase, so free-function overloads
   per phase-specialization rather than an if constexpr fork inside a
   single template. */
bool operator==(const SelectorGetAttrF<Resolved> & a, const SelectorGetAttrF<Resolved> & b)
{ return a.name == b.name && a.parent->cachedHash == b.parent->cachedHash; }
std::strong_ordering operator<=>(const SelectorGetAttrF<Resolved> & a, const SelectorGetAttrF<Resolved> & b)
{
    if (auto c = a.name <=> b.name; c != 0) return c;
    return a.parent->cachedHash <=> b.parent->cachedHash;
}

bool operator==(const SelectorGetListElemF<Resolved> & a, const SelectorGetListElemF<Resolved> & b)
{ return a.index == b.index && a.parent->cachedHash == b.parent->cachedHash; }
std::strong_ordering operator<=>(const SelectorGetListElemF<Resolved> & a, const SelectorGetListElemF<Resolved> & b)
{
    if (auto c = a.index <=> b.index; c != 0) return c;
    return a.parent->cachedHash <=> b.parent->cachedHash;
}

bool operator==(const SelectorGetFunctionInfoF<Resolved> & a, const SelectorGetFunctionInfoF<Resolved> & b)
{ return a.parent->cachedHash == b.parent->cachedHash; }
std::strong_ordering operator<=>(const SelectorGetFunctionInfoF<Resolved> & a, const SelectorGetFunctionInfoF<Resolved> & b)
{ return a.parent->cachedHash <=> b.parent->cachedHash; }

bool operator==(const SelectorApplyF<Resolved> & a, const SelectorApplyF<Resolved> & b)
{ return a.parent->cachedHash == b.parent->cachedHash; }
std::strong_ordering operator<=>(const SelectorApplyF<Resolved> & a, const SelectorApplyF<Resolved> & b)
{ return a.parent->cachedHash <=> b.parent->cachedHash; }

bool operator==(const SelectorCallbackApplyF<Resolved> & a, const SelectorCallbackApplyF<Resolved> & b)
{ return a.argObsSet == b.argObsSet && a.parent->cachedHash == b.parent->cachedHash; }
std::strong_ordering operator<=>(const SelectorCallbackApplyF<Resolved> & a, const SelectorCallbackApplyF<Resolved> & b)
{
    if (auto c = a.argObsSet <=> b.argObsSet; c != 0) return c;
    return a.parent->cachedHash <=> b.parent->cachedHash;
}

/* String-phase comparators: default lexicographic per field. Defined so
   the friend declarations resolve; used by future tests / round-trip. */
bool operator==(const SelectorGetAttrF<String> & a, const SelectorGetAttrF<String> & b)
{ return a.name == b.name && a.parent == b.parent; }
std::strong_ordering operator<=>(const SelectorGetAttrF<String> & a, const SelectorGetAttrF<String> & b)
{
    if (auto c = a.name <=> b.name; c != 0) return c;
    return a.parent <=> b.parent;
}
bool operator==(const SelectorGetListElemF<String> & a, const SelectorGetListElemF<String> & b)
{ return a.index == b.index && a.parent == b.parent; }
std::strong_ordering operator<=>(const SelectorGetListElemF<String> & a, const SelectorGetListElemF<String> & b)
{
    if (auto c = a.index <=> b.index; c != 0) return c;
    return a.parent <=> b.parent;
}
bool operator==(const SelectorGetFunctionInfoF<String> & a, const SelectorGetFunctionInfoF<String> & b)
{ return a.parent == b.parent; }
std::strong_ordering operator<=>(const SelectorGetFunctionInfoF<String> & a, const SelectorGetFunctionInfoF<String> & b)
{ return a.parent <=> b.parent; }
bool operator==(const SelectorApplyF<String> & a, const SelectorApplyF<String> & b)
{ return a.parent == b.parent; }
std::strong_ordering operator<=>(const SelectorApplyF<String> & a, const SelectorApplyF<String> & b)
{ return a.parent <=> b.parent; }
bool operator==(const SelectorCallbackApplyF<String> & a, const SelectorCallbackApplyF<String> & b)
{ return a.argObsSet == b.argObsSet && a.parent == b.parent; }
std::strong_ordering operator<=>(const SelectorCallbackApplyF<String> & a, const SelectorCallbackApplyF<String> & b)
{
    if (auto c = a.argObsSet <=> b.argObsSet; c != 0) return c;
    return a.parent <=> b.parent;
}

Hash computeSelectorHash(const SelectorNode & query)
{
    /* CBOR encoding rather than j.dump(): the JSON string dump ran
       nlohmann's dump_escaped (per-char escape check) on the whole
       payload including the 64-char hex parent strings. CBOR encodes
       strings without escaping and with a shorter length header, so
       hashing the CBOR bytes is materially cheaper for the same
       logical content. Existing DB rows keyed by the old dump-hash
       become orphans; the eval-cache is dev-only, so screw
       migrations. */
    nlohmann::json j;
    to_json(j, query);
    auto cbor = nlohmann::json::to_cbor(j);
    return hashString(HashAlgorithm::SHA256,
        std::string_view(reinterpret_cast<const char *>(cbor.data()), cbor.size()));
}

SelectorF<Resolved>::SelectorF(SelectorNodeF<Resolved> n)
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
    /* Decode into String phase via pure nlohmann serde, then pool-mediated
       resolve into Resolved. Two passes rather than a single hex-aware
       parser — decouples DB decode from pool identity resolution. */
    return resolveFromJson(j, *this);
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
                    + " obsSet=" + shortH(q.argObsSet);
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

/* Fold-based tag dispatch for the StringSelectorNode variant — no per-
   alternative enumeration here; the variant's Ts... drive it. Adding
   a new alternative to SelectorNodeF<P> updates this automatically. */
void from_json(const nlohmann::json & j, StringSelectorNode & node)
{
    detail::fromJsonByTag(j, node);
}

/* Delegate variant-level to_json to the specific alternative's to_json. */
void to_json(nlohmann::json & j, const StringSelectorNode & q)
{
    std::visit([&](const auto & alt) { to_json(j, alt); }, q);
}

/* Pool-mediated resolve: String phase → Resolved phase. Parent hex
   lookups may miss (pool + DB), in which case the whole resolve fails. */
std::optional<ref<const Selector>> resolve(const StringSelectorNode & raw, SelectorPool & pool)
{
    return std::visit(overloaded{
        [&](const SelectorExpr & s)   -> std::optional<ref<const Selector>> { return pool.intern(s); },
        [&](const SelectorImport & s) -> std::optional<ref<const Selector>> { return pool.intern(s); },
        [&](const SelectorArg & s)    -> std::optional<ref<const Selector>> { return pool.intern(s); },
        [&](const StringSelectorGetAttr & s) -> std::optional<ref<const Selector>> {
            auto p = pool.findByHex(s.parent);
            if (!p) return std::nullopt;
            return pool.intern(SelectorGetAttr{s.name, *p});
        },
        [&](const StringSelectorGetListElem & s) -> std::optional<ref<const Selector>> {
            auto p = pool.findByHex(s.parent);
            if (!p) return std::nullopt;
            return pool.intern(SelectorGetListElem{s.index, *p});
        },
        [&](const StringSelectorGetFunctionInfo & s) -> std::optional<ref<const Selector>> {
            auto p = pool.findByHex(s.parent);
            if (!p) return std::nullopt;
            return pool.intern(SelectorGetFunctionInfo{*p});
        },
        [&](const StringSelectorApply & s) -> std::optional<ref<const Selector>> {
            auto p = pool.findByHex(s.parent);
            if (!p) return std::nullopt;
            return pool.intern(SelectorApply{*p});
        },
        [&](const StringSelectorCallbackApply & s) -> std::optional<ref<const Selector>> {
            auto p = pool.findByHex(s.parent);
            if (!p) return std::nullopt;
            auto obsSetHash = Hash::parseNonSRIUnprefixed(s.argObsSet, HashAlgorithm::SHA256);
            return pool.intern(SelectorCallbackApply{obsSetHash, *p});
        },
    }, raw);
}

/* JSON decode + resolve combo — convenience for callers reading
   request payloads. Wraps the String from_json in a try/catch since
   from_json is strict (per no-defensive-coding, throws on malformed);
   caller wants nullopt on any decode failure since dispatch treats
   unparseable payloads as misses. */
std::optional<ref<const Selector>> resolveFromJson(
    const nlohmann::json & j, SelectorPool & pool)
{
    StringSelectorNode raw;
    try {
        from_json(j, raw);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    return resolve(raw, pool);
}

/* Resolved → String: total, hex-renders parents. */
StringSelectorNode unresolve(const SelectorNode & node)
{
    return std::visit(overloaded{
        [](const SelectorExpr & s)   -> StringSelectorNode { return s; },
        [](const SelectorImport & s) -> StringSelectorNode { return s; },
        [](const SelectorArg & s)    -> StringSelectorNode { return s; },
        [](const SelectorGetAttr & s) -> StringSelectorNode {
            return StringSelectorGetAttr{s.name, hexOf(s.parent)};
        },
        [](const SelectorGetListElem & s) -> StringSelectorNode {
            return StringSelectorGetListElem{s.index, hexOf(s.parent)};
        },
        [](const SelectorGetFunctionInfo & s) -> StringSelectorNode {
            return StringSelectorGetFunctionInfo{hexOf(s.parent)};
        },
        [](const SelectorApply & s) -> StringSelectorNode {
            return StringSelectorApply{hexOf(s.parent)};
        },
        [](const SelectorCallbackApply & s) -> StringSelectorNode {
            return StringSelectorCallbackApply{s.argObsSet.to_string(HashFormat::Base16, false), hexOf(s.parent)};
        },
    }, node);
}

/* Request-variant resolve/decode. Env-only participants
   (FileReadRequest, GetEnvRequest) are phase-independent; the
   OuterValue arm carries a Selector reference that needs the pool. */
std::optional<Request> resolve(const StringRequest & raw, SelectorPool & pool)
{
    return std::visit(overloaded{
        [](const FileReadRequest & r) -> std::optional<Request> { return r; },
        [](const GetEnvRequest & r)   -> std::optional<Request> { return r; },
        [&](const StringOuterValueRequest & r) -> std::optional<Request> {
            auto q = pool.findByHex(r.query);
            if (!q) return std::nullopt;
            return OuterValueRequest{*q};
        },
    }, raw);
}

std::optional<Request> decodeRequest(const nlohmann::json & j, SelectorPool & pool)
{
    StringRequest raw;
    try {
        detail::fromJsonByTag(j, raw);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    return resolve(raw, pool);
}

std::optional<ResultVariant> decodeResult(const nlohmann::json & j)
{
    /* One alternative for now (ResultWHNF); if more land they get
       tag-dispatched here via the same pattern used by decodeRequest. */
    try {
        ResultWHNF whnf;
        from_json(j, whnf);
        return ResultVariant{std::move(whnf)};
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

} // namespace nix::trace
