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
// PathExpr: a structured access path from a cb_arg root
// ---------------------------------------------------------------------------

struct PathExpr;  // forward — PathStep::Apply nests sub-PathExprs

/** One step within an access path. `GetAttr` / `GetListElem` extend
    by an attr name or a list-elem index. `Apply` extends by an apply
    node whose `fnPath` and `argPath` are themselves sub-PathExprs;
    each sub-path resolves against an entry in the enclosing query's
    `fromStateHashes[]` (selected by `fnRootIndex` / `argRootIndex`). The
    apply form is used by function characterization so observations
    on apply-result descendants compose into a path rooted in the
    cb_args that fn and arg came from. */
struct PathStep
{
    enum class Kind {
        GetAttr,
        GetListElem,
        Apply,
    };
    Kind kind;
    std::string name;  ///< meaningful for GetAttr
    size_t index{};    ///< meaningful for GetListElem
    /* Apply sub-paths. Held by shared_ptr so PathExpr can recursively
       contain PathStep without storage cycles in the type. */
    std::shared_ptr<PathExpr> fnPath;
    std::shared_ptr<PathExpr> argPath;
    size_t fnRootIndex{0};
    size_t argRootIndex{0};

    std::strong_ordering operator<=>(const PathStep & other) const;
    bool operator==(const PathStep & other) const;
};

void to_json(nlohmann::json & j, const PathStep & s);
void from_json(const nlohmann::json & j, PathStep & s);

/** A path from a cb_arg root to the value being probed. Empty path
    means the observation is on the root itself. Used by the per-arg
    subject-id model: every probe's path identifies *which* derived
    value within the root is being probed, while the root's state hash is
    what `fromStateHashes` resolves to at flush. */
struct PathExpr
{
    std::vector<PathStep> steps;

    auto operator<=>(const PathExpr & other) const = default;
};

void to_json(nlohmann::json & j, const PathExpr & p);
void from_json(const nlohmann::json & j, PathExpr & p);

// ---------------------------------------------------------------------------
// Query payload types and their result mappings
// ---------------------------------------------------------------------------

/** Evaluate an expression string. */
struct SelectorExpr
{
    static constexpr std::string_view tag = "expr";
    std::string expr;
    std::string baseDir;
    auto operator<=>(const SelectorExpr &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorExpr, ResultWHNF)

/** Import/evaluate a file. */
struct SelectorImport
{
    static constexpr std::string_view tag = "import";
    std::string path;
    auto operator<=>(const SelectorImport &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorImport, ResultWHNF)

struct Selector;

/** Get an attribute by name from a value that has been shown (via
    parent WHNF) to contain it. Pure retrieval — no existence check
    is folded in; the caller must have projected membership from the
    parent's WHNFAttrs.names first. Returns the child's WHNF. */
struct SelectorGetAttr
{
    static constexpr std::string_view tag = "getAttr";
    std::string name;
    ref<const Selector> parent;
    bool operator==(const SelectorGetAttr & other) const;
    std::strong_ordering operator<=>(const SelectorGetAttr & other) const;
};
template<> struct ResultOf<SelectorGetAttr> { using Type = ResultWHNF; };
void to_json(nlohmann::json & j, const SelectorGetAttr & q);

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
struct SelectorGetListElem
{
    static constexpr std::string_view tag = "getListElem";
    size_t index;
    ref<const Selector> parent;
    bool operator==(const SelectorGetListElem & other) const;
    std::strong_ordering operator<=>(const SelectorGetListElem & other) const;
};
template<> struct ResultOf<SelectorGetListElem> { using Type = ResultWHNF; };
void to_json(nlohmann::json & j, const SelectorGetListElem & q);

/** Get function argument info (formals). */
struct SelectorGetFunctionInfo
{
    static constexpr std::string_view tag = "getFunctionInfo";
    ref<const Selector> parent;
    bool operator==(const SelectorGetFunctionInfo & other) const;
    std::strong_ordering operator<=>(const SelectorGetFunctionInfo & other) const;
};
template<> struct ResultOf<SelectorGetFunctionInfo> { using Type = ResultFunctionInfo; };
void to_json(nlohmann::json & j, const SelectorGetFunctionInfo & q);

/** Apply a function to an argument. `parent` is the fn Selector. */
struct SelectorApply
{
    static constexpr std::string_view tag = "apply";
    ref<const Selector> parent;
    bool operator==(const SelectorApply & other) const;
    std::strong_ordering operator<=>(const SelectorApply & other) const;
};
template<> struct ResultOf<SelectorApply> { using Type = ResultWHNF; };
void to_json(nlohmann::json & j, const SelectorApply & q);

/** Apply a callback to a contra-arg. `parent` is the fn Selector;
    `argObsSet` is a content hash referring to the outer's observation-set
    on the contra-arg. */
struct SelectorCallbackApply
{
    static constexpr std::string_view tag = "callbackApply";
    std::string argObsSet;
    ref<const Selector> parent;
    bool operator==(const SelectorCallbackApply & other) const;
    std::strong_ordering operator<=>(const SelectorCallbackApply & other) const;
};
template<> struct ResultOf<SelectorCallbackApply> { using Type = ResultWHNF; };
void to_json(nlohmann::json & j, const SelectorCallbackApply & q);

/** Reference a positional callback arg by its reverse-De-Bruijn depth.
    A Selector like any other: its Q hash falls out of its shape, and
    other Selectors reference it through their `from`/`fn` fields the
    same way they reference any Q. Used when the outer supplies an arg
    whose only identity is its position in the callback-apply stack. */
struct SelectorArg
{
    static constexpr std::string_view tag = "arg";
    int depth;
    auto operator<=>(const SelectorArg &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorArg, ResultWHNF)

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

using SelectorNode = std::variant<
    SelectorExpr,
    SelectorImport,
    SelectorGetAttr,
    SelectorGetListElem,
    SelectorGetFunctionInfo,
    SelectorApply,
    SelectorCallbackApply,
    SelectorArg>;

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
    alternatives carry ref<const Selector> parent (structural
    sharing). Constructed via SelectorPool::intern; both fields const. */
struct Selector
{
    const SelectorNode node;
    const Hash cachedHash;

    explicit Selector(SelectorNode n);
};

class SelectorPool
{
    std::unordered_map<Hash, ref<const Selector>> pool;

public:
    ref<const Selector> intern(SelectorNode node);
    std::optional<ref<const Selector>> find(const Hash & h) const;
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

/** to_json on the flat SelectorNode — dispatches to per-alternative
    to_json. No from_json companion (step Selectors need SelectorPool
    to resolve parent references; use nodeFromJson). */
void to_json(nlohmann::json & j, const SelectorNode & q);

/** Reconstruct a Selector from JSON — parents resolved via pool. */
std::optional<ref<const Selector>> nodeFromJson(
    const nlohmann::json & j, SelectorPool & pool);

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
 */
struct OuterValueRequest
{
    static constexpr std::string_view tag = "outerValue";
    ref<const Selector> query;
};

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

/* parseSelectorNode / fromHashOf / rewriteFrom retired — under
   the recursive Selector, parsing needs a SelectorPool (use
   nodeFromJson), fromHashOf is a `parent->cachedHash` access, and
   rewriteFrom is meaningless since Selectors are immutable. */

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
