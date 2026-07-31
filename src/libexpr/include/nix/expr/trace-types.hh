#pragma once
/**
 * @file
 * Trace message types for fine-grained evaluation caching.
 *
 * Defines request/response pairs for environment I/O operations and
 * query/result pairs for user-facing evaluator operations. These are the
 * building blocks for the tracing eval cache: a TracingEvaluator records
 * these messages during evaluation, and a replay evaluator consults them
 * to skip redundant work.
 *
 * Uses a type family pattern:
 * - ResponseTrace<Request> maps each environment request to its response
 * - ResultOf<Query> maps each user query to its result
 */

#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <nlohmann/json.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nix { class TracingDecisionGraph; }

namespace nix::trace {

// ---------------------------------------------------------------------------
// Infrastructure
// ---------------------------------------------------------------------------

/**
 * Type family mapping request types to their response types.
 * Specialize this for each request type.
 */
template<typename Request>
struct ResponseTrace;

/**
 * A traced environment operation: pairs a request with its response.
 */
template<typename T>
struct Response
{
    T request;
    typename ResponseTrace<T>::ResponseType response;
};

template<typename T>
void to_json(nlohmann::json & j, const Response<T> & r)
{
    j = nlohmann::json{{"type", T::tag}, {"request", r.request}, {"response", r.response}};
}

template<typename T>
void from_json(const nlohmann::json & j, Response<T> & r)
{
    j.at("request").get_to(r.request);
    j.at("response").get_to(r.response);
}

#define DECLARE_TRACE_PAIR(ReqType, RespType)              \
    template<>                                             \
    struct ResponseTrace<ReqType>                          \
    {                                                      \
        using ResponseType = RespType;                     \
    };                                                     \
    void to_json(nlohmann::json & j, const ReqType & r);   \
    void from_json(const nlohmann::json & j, ReqType & r); \
    void to_json(nlohmann::json & j, const RespType & r);  \
    void from_json(const nlohmann::json & j, RespType & r);

// ---------------------------------------------------------------------------
// File read operation
// ---------------------------------------------------------------------------

struct FileReadRequest
{
    static constexpr std::string_view tag = "fileRead";
    std::string absPath;
};

struct FileReadResponse
{
    Hash contentHash;
};

DECLARE_TRACE_PAIR(FileReadRequest, FileReadResponse)

// ---------------------------------------------------------------------------
// Environment variable lookup
// ---------------------------------------------------------------------------

struct GetEnvRequest
{
    static constexpr std::string_view tag = "getEnv";
    std::string name;
};

struct GetEnvResponse
{
    std::optional<std::string> value;
};

DECLARE_TRACE_PAIR(GetEnvRequest, GetEnvResponse)

// ---------------------------------------------------------------------------
// User query infrastructure
// ---------------------------------------------------------------------------

/**
 * Type family mapping query types to their result types.
 */
template<typename QueryPayload>
struct ResultOf;

/**
 * A user query: pairs a query payload with a value handle.
 * The handle is an opaque identifier linking queries to results.
 */
template<typename T>
struct Query
{
    T query;
    uint64_t v;
};

template<typename T>
void to_json(nlohmann::json & j, const Query<T> & q)
{
    j = nlohmann::json{{"query", q.query}, {"v", q.v}};
}

template<typename T>
void from_json(const nlohmann::json & j, Query<T> & q)
{
    j.at("query").get_to(q.query);
    j.at("v").get_to(q.v);
}

/**
 * A result: pairs a result payload with its value handle.
 */
template<typename T>
struct Result
{
    T result;
    uint64_t v;
};

template<typename T>
void to_json(nlohmann::json & j, const Result<T> & r)
{
    j = nlohmann::json{{"result", r.result}, {"v", r.v}};
}

template<typename T>
void from_json(const nlohmann::json & j, Result<T> & r)
{
    j.at("result").get_to(r.result);
    j.at("v").get_to(r.v);
}

#define DECLARE_SELECTOR_RESULT(QueryType, ResultType)          \
    template<>                                               \
    struct ResultOf<QueryType>                               \
    {                                                        \
        using Type = ResultType;                             \
    };                                                       \
    void to_json(nlohmann::json & j, const QueryType & q);   \
    void from_json(const nlohmann::json & j, QueryType & q); \
    void to_json(nlohmann::json & j, const ResultType & r);  \
    void from_json(const nlohmann::json & j, ResultType & r);

// ---------------------------------------------------------------------------
// Result payload types
// ---------------------------------------------------------------------------

/** Payload alternatives for `ResultWHNF`. One per Nix WHNF type.
    `WHNFFunction` / `WHNFNull` are tag-only (the value is entirely
    identified by its type — functions are represented indirectly
    through subsequent queries). Types the black-box model can't
    represent — `nThunk` (unforced), `nExternal`, `nFailed` — are
    never stored: `computeWHNFFromObject` throws so the caller
    falls back to the interpreter. */
struct WHNFInt { int64_t value; };
struct WHNFBool { bool value; };
struct WHNFFloat { double value; };
struct WHNFPath { std::string path; };
struct WHNFString { std::string value; std::vector<std::string> context; };
struct WHNFAttrs { std::vector<std::string> names; };
struct WHNFList { size_t size; };
struct WHNFFunction {};
struct WHNFNull {};

/** Result of a single WHNF (Weak Head Normal Form) force. Carries the
    type discriminator plus the type-determined payload as a tagged
    variant. A single observation instead of separate getType +
    getInt/getString/etc., so siblings that force the same value
    record symmetric chains regardless of inner-evaluator memoization
    that would otherwise skip getType on subsequent forces. */
struct ResultWHNF
{
    std::string type;
    std::variant<WHNFInt, WHNFBool, WHNFFloat, WHNFPath, WHNFString, WHNFAttrs, WHNFList, WHNFFunction, WHNFNull> payload;
};

// ---------------------------------------------------------------------------
// Query payload types and their result mappings
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Trees-that-grow: step Selectors are phase-parameterized. Two phases:
//   Resolved — parent is ref<const Selector> (in-memory recursive tree).
//   String   — parent is std::string (hex, on-wire persistence form).
// A `resolve` pass turns String into Resolved via SelectorPool; `unresolve`
// goes the other way. Serde (nlohmann NON_INTRUSIVE macros) operates on
// the String phase only — pure, no pool parameter, no defensive fallbacks.
// Runtime code uses Selector (= SelectorF<Resolved>) via the aliases below;
// callsites are alias-transparent.
// ---------------------------------------------------------------------------

/** Phase tags. */
struct Resolved {};
struct String {};

/** Forward decl so SelectorParent<Resolved> can name it. */
template<typename P> struct SelectorF;

/** Type family selecting parent-reference type per phase. */
template<typename P> struct SelectorParent;
template<> struct SelectorParent<String>   { using type = std::string; };
template<> struct SelectorParent<Resolved> { using type = ref<const SelectorF<Resolved>>; };
template<typename P> using ParentRef = typename SelectorParent<P>::type;

/** Type family selecting how a content-hash CAS reference is carried
    per phase. Resolved holds a typed Hash; String holds a hex-encoded
    payload from the wire. */
template<typename P> struct HashRef;
template<> struct HashRef<String>   { using type = std::string; };
template<> struct HashRef<Resolved> { using type = Hash; };
template<typename P> using HashRefT = typename HashRef<P>::type;

/** Evaluate an expression string. Phase-independent (no parent). */
struct SelectorExpr
{
    static constexpr std::string_view tag = "expr";
    std::string expr;
    std::string baseDir;
    auto operator<=>(const SelectorExpr &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorExpr, ResultWHNF)

/** Import/evaluate a file. Phase-independent. */
struct SelectorImport
{
    static constexpr std::string_view tag = "import";
    std::string path;
    auto operator<=>(const SelectorImport &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorImport, ResultWHNF)

/** Get an attribute by name from a value that has been shown (via
    parent WHNF) to contain it. Pure retrieval — no existence check
    is folded in; the caller must have projected membership from the
    parent's WHNFAttrs.names first. Returns the child's WHNF. */
template<typename P>
struct SelectorGetAttrF
{
    static constexpr std::string_view tag = "getAttr";
    std::string name;
    ParentRef<P> parent;
    friend bool operator==(const SelectorGetAttrF & a, const SelectorGetAttrF & b);
    friend std::strong_ordering operator<=>(const SelectorGetAttrF & a, const SelectorGetAttrF & b);
};

/** Result for getFunctionInfo: optional formals map + ellipsis. */
struct ResultFunctionInfo
{
    bool hasInfo;                        ///< false if not a function with formals
    std::map<std::string, bool> formals; ///< name -> hasDefault (empty if !hasInfo)
    bool ellipsis = false;
};
void to_json(nlohmann::json & j, const ResultFunctionInfo & r);
void from_json(const nlohmann::json & j, ResultFunctionInfo & r);

/** Get a list element by index. */
template<typename P>
struct SelectorGetListElemF
{
    static constexpr std::string_view tag = "getListElem";
    size_t index;
    ParentRef<P> parent;
    friend bool operator==(const SelectorGetListElemF & a, const SelectorGetListElemF & b);
    friend std::strong_ordering operator<=>(const SelectorGetListElemF & a, const SelectorGetListElemF & b);
};

/** Get function argument info (formals). */
template<typename P>
struct SelectorGetFunctionInfoF
{
    static constexpr std::string_view tag = "getFunctionInfo";
    ParentRef<P> parent;
    friend bool operator==(const SelectorGetFunctionInfoF & a, const SelectorGetFunctionInfoF & b);
    friend std::strong_ordering operator<=>(const SelectorGetFunctionInfoF & a, const SelectorGetFunctionInfoF & b);
};

/** Apply a function to an argument. `parent` is the fn Selector. */
template<typename P>
struct SelectorApplyF
{
    static constexpr std::string_view tag = "apply";
    ParentRef<P> parent;
    friend bool operator==(const SelectorApplyF & a, const SelectorApplyF & b);
    friend std::strong_ordering operator<=>(const SelectorApplyF & a, const SelectorApplyF & b);
};

/** Apply a callback to a contra-arg. `parent` is the fn Selector;
    `argObsSet` is a content hash referring to the outer's observation-set
    on the contra-arg. Phase-dependent: Resolved carries `Hash`, String
    carries the hex form. */
template<typename P>
struct SelectorCallbackApplyF
{
    static constexpr std::string_view tag = "callbackApply";
    HashRefT<P> argObsSet;
    ParentRef<P> parent;
    friend bool operator==(const SelectorCallbackApplyF & a, const SelectorCallbackApplyF & b);
    friend std::strong_ordering operator<=>(const SelectorCallbackApplyF & a, const SelectorCallbackApplyF & b);
};

/** Reference a positional callback arg by its reverse-De-Bruijn depth.
    Phase-independent leaf. */
struct SelectorArg
{
    static constexpr std::string_view tag = "arg";
    int depth;
    auto operator<=>(const SelectorArg &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorArg, ResultWHNF)

/** Aliases: Resolved is the in-memory form used by runtime code;
    String is the on-wire form for serde. Callsites use the Resolved
    aliases exclusively — they're transparent for construction and
    variant access. */
using SelectorGetAttr         = SelectorGetAttrF<Resolved>;
using SelectorGetListElem     = SelectorGetListElemF<Resolved>;
using SelectorGetFunctionInfo = SelectorGetFunctionInfoF<Resolved>;
using SelectorApply           = SelectorApplyF<Resolved>;
using SelectorCallbackApply   = SelectorCallbackApplyF<Resolved>;

using StringSelectorGetAttr         = SelectorGetAttrF<String>;
using StringSelectorGetListElem     = SelectorGetListElemF<String>;
using StringSelectorGetFunctionInfo = SelectorGetFunctionInfoF<String>;
using StringSelectorApply           = SelectorApplyF<String>;
using StringSelectorCallbackApply   = SelectorCallbackApplyF<String>;

template<> struct ResultOf<SelectorGetAttr>         { using Type = ResultWHNF; };
template<> struct ResultOf<SelectorGetListElem>     { using Type = ResultWHNF; };
template<> struct ResultOf<SelectorGetFunctionInfo> { using Type = ResultFunctionInfo; };
template<> struct ResultOf<SelectorApply>           { using Type = ResultWHNF; };
template<> struct ResultOf<SelectorCallbackApply>   { using Type = ResultWHNF; };

/** Resolved-phase logging helper: renders parent as hex. String-phase
    serde is via NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (trace-types.cc). */
void to_json(nlohmann::json & j, const SelectorGetAttr & q);
void to_json(nlohmann::json & j, const SelectorGetListElem & q);
void to_json(nlohmann::json & j, const SelectorGetFunctionInfo & q);
void to_json(nlohmann::json & j, const SelectorApply & q);
void to_json(nlohmann::json & j, const SelectorCallbackApply & q);

// ---------------------------------------------------------------------------
// CompletedQuery: a query correlated with its result
// ---------------------------------------------------------------------------

/**
 * A query that has been correlated with its result.
 */
template<typename T>
struct CompletedQuery : Query<T>
{
    /**
     * Index into the trace where the corresponding Result lives.
     * 0 indicates unresolved (no matching result found).
     */
    size_t resultIndex;
};

// ---------------------------------------------------------------------------
// Trace entry variant (for parsing and indexing)
// ---------------------------------------------------------------------------

/**
 * Helper to apply a wrapper template to multiple types.
 */
template<template<typename> class Wrapper, typename... Ts>
using ApplyWrapper = std::variant<Wrapper<Ts>...>;

/**
 * All environment request types.
 */
template<template<typename> class F>
using EnvRequests = ApplyWrapper<F, FileReadRequest, GetEnvRequest>;

/**
 * All query payload types.
 */
template<template<typename> class F>
using Selectors = ApplyWrapper<
    F,
    SelectorExpr,
    SelectorImport,
    SelectorGetAttr,
    SelectorGetListElem,
    SelectorGetFunctionInfo,
    SelectorApply,
    SelectorCallbackApply,
    SelectorArg>;

/**
 * All result payload types.
 */
template<template<typename> class F>
using Results = ApplyWrapper<
    F,
    ResultFunctionInfo,
    ResultWHNF>;

// ---------------------------------------------------------------------------
// Variant types for SelectorNode / ResultVariant
// ---------------------------------------------------------------------------

/** Phase-parameterized Selector node variant. Leaves are phase-independent;
    step alternatives carry their phase's parent-reference type.

    Tree-shape invariant (see tracing-eval-cache.md §"Design note: Selector
    is a sequence, not a true algebra"): every alternative embeds at most
    one Selector reference, via a single `ParentRef<P> parent` field on step
    alternatives. `SelectorCallbackApply::argObsSet` is a hex reference to
    an ObservationSet — a distinct content-addressed concept, not a
    Selector — and so does not violate the invariant. Any new alternative
    must preserve this shape or the sequence model breaks. */
template<typename P>
using SelectorNodeF = std::variant<
    SelectorExpr,
    SelectorImport,
    SelectorGetAttrF<P>,
    SelectorGetListElemF<P>,
    SelectorGetFunctionInfoF<P>,
    SelectorApplyF<P>,
    SelectorCallbackApplyF<P>,
    SelectorArg>;

using SelectorNode       = SelectorNodeF<Resolved>;
using StringSelectorNode = SelectorNodeF<String>;

using ResultVariant = std::variant<
    ResultFunctionInfo,
    ResultWHNF>;

// ---------------------------------------------------------------------------
// Recursive Selector (in-memory) — supersedes the stringly-typed
// from/fn fields on the flat SelectorNode alternatives.
//
// Each non-leaf node holds its parent as `ref<const Selector>`, so a
// chain like GetAttr(GetAttr(Apply(Import))) is represented in memory
// as a shared tree — the Apply(Import) subtree is one heap node,
// shared by every GetAttr built on top. Stack shape enforced by the
// type: exactly one `parent` field per non-leaf.
//
// Hash is computed once at construction (bottom-up) and cached in
// `cachedHash`. Both `node` and `cachedHash` are `const` — Selectors
// are immutable.
//
// DB format is unchanged: on-disk payload for a step node still emits
// the parent's hex hash as the "from" / "fn" field. Reconstruction
// from DB rows walks parent hashes via a Selector pool (see below).
// ---------------------------------------------------------------------------

/** Recursive Selector: node payload + cached content hash. Step
    alternatives carry ref<const Selector> parent (structural sharing).
    Constructed via SelectorPool::intern; both fields const.

    Only the Resolved phase is defined as a full type — the String
    phase's data lives inside SelectorNodeF<String> nodes directly and
    never needs a wrapping Selector object (used only for transient
    serde). */
template<>
struct SelectorF<Resolved>
{
    const SelectorNodeF<Resolved> node;
    const Hash cachedHash;

    explicit SelectorF(SelectorNodeF<Resolved> n);
};

using Selector = SelectorF<Resolved>;

/** DB facade over the Selectors table: interning writes an
    `INSERT OR IGNORE` row, lookup falls back to reconstructing from
    a stored payload on memory-miss. Pool membership tracks DB
    membership; cross-session sharing falls out. The pool must be
    bound to a TracingDecisionGraph (via `bind`) before any intern/find
    call; TracingDecisionGraph's constructor does this automatically. */
class SelectorPool
{
    std::unordered_map<Hash, ref<const Selector>> pool;
    ::nix::TracingDecisionGraph * backing = nullptr;

public:
    /** Bind the pool to the graph that provides DB backing. Called
        once by TracingDecisionGraph during construction; after this,
        every intern/find touches the DB transparently. */
    void bind(::nix::TracingDecisionGraph & graph);

    ref<const Selector> intern(SelectorNode node);

    /** Memory cache first, DB reconstruction on miss. Returns
        nullopt only if the hash is absent from both. */
    std::optional<ref<const Selector>> find(const Hash & h);

    /** find() with hex-string input: parses to Hash, returns nullopt
        on parse failure. Encapsulates the parseNonSRIUnprefixed +
        find pattern common at sites decoding DB payloads and
        callback state. */
    std::optional<ref<const Selector>> findByHex(std::string_view hex);
};

/* Selector adapters — most consumers hold Selector / ref<const Selector>. */
std::string describe(const Selector & s);
std::string describe(const SelectorNode & q);
nlohmann::json toJson(const Selector & s);
nlohmann::json toJson(const SelectorNode & q);
inline const Hash & computeSelectorHash(const Selector & s) { return s.cachedHash; }
Hash computeSelectorHash(const SelectorNode & q);
bool willMoveStateHash(const Selector & s);
bool willMoveStateHash(const SelectorNode & q);

/** to_json on the Resolved SelectorNode — hex-renders parents so
    log paths (tracingCacheLog, describe) get the same textual shape
    as the on-wire form. Delegates to unresolve + String-phase serde. */
void to_json(nlohmann::json & j, const SelectorNode & q);

/** Convert a String-phase node to Resolved by looking up each parent
    hex in the pool. Returns nullopt if any parent hex isn't present
    (pool + DB). */
std::optional<ref<const Selector>> resolve(
    const StringSelectorNode & raw, SelectorPool & pool);

/** Convert a Resolved node to String-phase by hex-rendering parents.
    Total; never fails. */
StringSelectorNode unresolve(const SelectorNode & node);

/** Convenience: decode a JSON payload as StringSelectorNode via nlohmann
    serde, then resolve. Returns nullopt if either the decode throws or
    the resolve misses a parent hex. */
std::optional<ref<const Selector>> resolveFromJson(
    const nlohmann::json & j, SelectorPool & pool);

namespace detail {

/** Fold-based `from_json` for any `std::variant<Ts...>` whose
    alternatives carry a `static constexpr std::string_view tag`
    discriminator. `Ts...` unpacks straight from the variant type — no
    per-variant hand-enumeration. Historically written for QueryVariant
    (commit 055a8c9e5); restored under the trees-that-grow refactor to
    dispatch on StringSelectorNode without hand-listing 8 alternatives. */
template<typename V, typename T>
inline bool tryVariantAlternative(const nlohmann::json & j, V & v, std::string_view tag)
{
    if (tag != T::tag) return false;
    T val;
    from_json(j, val);
    v = std::move(val);
    return true;
}

template<typename... Ts>
inline void fromJsonByTag(const nlohmann::json & j, std::variant<Ts...> & v)
{
    auto tag = j.at("tag").template get<std::string_view>();
    bool matched = (tryVariantAlternative<std::variant<Ts...>, Ts>(j, v, tag) || ...);
    if (!matched)
        throw nlohmann::json::parse_error::create(
            302, 0, "unknown variant tag: " + std::string(tag), &j);
}

} // namespace detail

namespace detail {

template<typename... Ts>
consteval bool tagsAreDistinct()
{
    constexpr std::array tags{std::string_view{Ts::tag}...};
    for (size_t i = 0; i < tags.size(); ++i)
        for (size_t j = i + 1; j < tags.size(); ++j)
            if (tags[i] == tags[j]) return false;
    return true;
}

template<typename V> struct VariantTagsDistinct;
template<typename... Ts> struct VariantTagsDistinct<std::variant<Ts...>>
{
    static constexpr bool value = tagsAreDistinct<Ts...>();
};

} // namespace detail

static_assert(
    detail::VariantTagsDistinct<SelectorNode>::value,
    "SelectorNode alternatives must have distinct `tag` values.");

// ---------------------------------------------------------------------------
// OuterValueRequest / OuterValueResponse
//
// Wraps a Query / Result to tag it as being about an outer-owned
// value. Embedded in the Env-layer trace via the inner evaluator's
// OuterObject probes.
// ---------------------------------------------------------------------------

/**
 * Outgoing outer query: local→external.
 * Data queries (getType, getAttr, ...) and external calls (apply).
 *
 * Phase-parameterised: Resolved carries `ref<const Selector>` (the
 * in-memory recursive form); String carries the query as hex.
 */
template<typename P>
struct OuterValueRequestF
{
    static constexpr std::string_view tag = "outerValue";
    ParentRef<P> query;
};

using OuterValueRequest       = OuterValueRequestF<Resolved>;
using StringOuterValueRequest = OuterValueRequestF<String>;

struct OuterValueResponse
{
    ResultVariant result;
};

template<>
struct ResponseTrace<OuterValueRequest>
{
    using ResponseType = OuterValueResponse;
};
void to_json(nlohmann::json & j, const OuterValueRequest & r);
void to_json(nlohmann::json & j, const OuterValueResponse & r);
void from_json(const nlohmann::json & j, OuterValueResponse & r);

template<template<typename> class F>
using AllEnvRequests = ApplyWrapper<F, FileReadRequest, GetEnvRequest, OuterValueRequest>;

/** Env-layer Request variant. Flat tagged union across the three
    Env participants (filesystem, env vars, outer evaluator). This
    is the payload shape the walker and canonicalisation std::visit
    over after decoding a request-pool blob. */
using Request       = std::variant<FileReadRequest, GetEnvRequest, OuterValueRequest>;
using StringRequest = std::variant<FileReadRequest, GetEnvRequest, StringOuterValueRequest>;

/** String → Resolved for a whole Request. Fails when the embedded
    outerValue query can't be resolved (parent hex not in pool + DB). */
std::optional<Request> resolve(const StringRequest & raw, SelectorPool & pool);

/** Convenience: decode a JSON payload as StringRequest via
    `fromJsonByTag`, then resolve. Returns nullopt on decode-failure
    or resolve-miss. */
std::optional<Request> decodeRequest(const nlohmann::json & j, SelectorPool & pool);

namespace detail {

template<typename... Variants>
struct CombineVariants;

template<typename... Ts>
struct CombineVariants<std::variant<Ts...>>
{
    using type = std::variant<Ts...>;
};

template<typename... Ts, typename... Us, typename... Rest>
struct CombineVariants<std::variant<Ts...>, std::variant<Us...>, Rest...>
{
    using type = typename CombineVariants<std::variant<Ts..., Us...>, Rest...>::type;
};

} // namespace detail

/**
 * Combined trace entry type containing all Response, Query, and Result variants.
 */
using TraceEntry = detail::CombineVariants<AllEnvRequests<Response>, Selectors<Query>, Results<Result>>::type;

/**
 * Trace entry with queries correlated to their results.
 */
using CorrelatedTraceEntry =
    detail::CombineVariants<AllEnvRequests<Response>, Selectors<CompletedQuery>, Results<Result>>::type;

/**
 * Parse a JSON entry into a typed TraceEntry.
 * Returns nullopt if the entry type is not recognized.
 */
std::optional<TraceEntry> parseTraceEntry(const nlohmann::json & j);

/**
 * SHA-256 of the Query's JSON dump — the canonical selectorHash used
 * as its identity across the trace/store layer. Overload of the
 * per-Q-type `computeSelectorHash` on decision-graph, so callers
 * holding a `SelectorNode` don't have to std::visit at every site.
 */
Hash computeSelectorHash(const SelectorNode & query);

/** Serialise a Query variant to its inner JSON payload
    (`{"query": <tag>, "params": {...}}`). */
nlohmann::json toJson(const SelectorNode & query);

/**
 * Correlate queries with their results.
 * Builds a map from value handle to result index, then transforms
 * Query<T> entries into CompletedQuery<T> with the result index.
 */
std::vector<CorrelatedTraceEntry> correlateTrace(const std::vector<TraceEntry> & trace);

// ---------------------------------------------------------------------------
// Query index for O(1) lookup
// ---------------------------------------------------------------------------

/**
 * Query and result index pair.
 */
struct IndexEntry
{
    size_t queryIndex;
    size_t resultIndex;
};

/**
 * Index for fast query lookup in a trace.
 */
class SelectorIndex
{
    std::map<SelectorNode, IndexEntry> index;

public:
    explicit SelectorIndex(const std::vector<TraceEntry> & trace);

    template<typename Q>
    std::optional<IndexEntry> lookup(const Q & q) const
    {
        auto it = index.find(q);
        return it != index.end() ? std::optional{it->second} : std::nullopt;
    }
};

} // namespace nix::trace
