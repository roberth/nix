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

#include <nlohmann/json.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
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
// SelectorLeaf: typed `from` / `fn` / `arg` field of Query types
// ---------------------------------------------------------------------------

/**
 * Numbered identifier carrier. Sanctioned only at the CLI per
 * Principle #1 of the content-identity design — everything below the
 * CLI uses content-defined identifiers.
 */
struct OuterLeaf
{
    int index;
    auto operator<=>(const OuterLeaf &) const = default;
};

/**
 * SelectorLeaf: the typed `from` (and `fn`/`arg`) field of Query
 * types. Under #178 the state-hash payload retires; SelectorLeaf
 * keeps its variant wrapper (single item) so the wire format has a
 * stable envelope for future leaf kinds.
 */
struct SelectorLeaf
{
    std::variant<OuterLeaf> data;

    SelectorLeaf() : data(OuterLeaf{0}) {}
    SelectorLeaf(OuterLeaf a) : data(a) {}
    /* Legacy string-hex constructors (state-hash-flavoured, #178) —
       hex content ignored; produces the default OuterLeaf{0}. Kept
       so existing call sites continue to compile; sweep pending. */
    SelectorLeaf(const std::string &) : data(OuterLeaf{0}) {}
    SelectorLeaf(const char *) : data(OuterLeaf{0}) {}

    bool isOuter() const
    {
        return std::holds_alternative<OuterLeaf>(data);
    }
    int outerIndex() const
    {
        return std::get<OuterLeaf>(data).index;
    }

    auto operator<=>(const SelectorLeaf &) const = default;
};

void to_json(nlohmann::json & j, const SelectorLeaf & leaf);
void from_json(const nlohmann::json & j, SelectorLeaf & leaf);

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

/**
 * `PerArgFrame`: the shared "reference to parent via cb_arg roots" pair
 * that four Query types (GetAttr, GetListElem, GetWHNF,
 * GetFunctionInfo) all carry. `fromStateHashes[i]` is the state hash
 * of cb_arg root `i`; `path` describes the navigation from those roots
 * to this observation.
 *
 * Extracted into its own struct so each of those Selectors' to_json/
 * from_json embeds one field (`perArgFrame`) instead of duplicating
 * the same conditional emit/parse block.
 */
struct PerArgFrame
{
    std::vector<SelectorLeaf> fromStateHashes;
    PathExpr path;
    auto operator<=>(const PerArgFrame &) const = default;
};

void to_json(nlohmann::json & j, const PerArgFrame & f);
void from_json(const nlohmann::json & j, PerArgFrame & f);

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

/** Get an attribute by name from a value that has been shown (via
    parent WHNF) to contain it. Pure retrieval — no existence check
    is folded in; the caller must have projected membership from the
    parent's WHNFAttrs.names first. Returns the child's WHNF. */
struct SelectorGetAttr
{
    static constexpr std::string_view tag = "getAttr";
    std::string name;
    std::string from;   ///< Parent Q identity (stable under #178).
    auto operator<=>(const SelectorGetAttr &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorGetAttr, ResultWHNF)

/** Get a list element by index from a value that has been shown (via
    parent WHNF) to have at least index+1 elements. Pure retrieval —
    no bounds check is folded in; caller must have projected size from
    the parent's WHNFList.size first. Returns the child's WHNF. */
struct SelectorGetListElem
{
    static constexpr std::string_view tag = "getListElem";
    std::string from;   ///< Parent Q identity (stable under #178).
    size_t index;
    auto operator<=>(const SelectorGetListElem &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorGetListElem, ResultWHNF)

/** Force a value to WHNF and read its type + type-determined payload
    in one shot. Used by the cache-layer Objects to combine what would
    otherwise be separate getType + getInt/getString/etc. observations
    — a single WHNF probe per value force, recorded once. The
    individual getType/getInt/etc. paths remain for callers that don't
    need WHNF semantics. */
struct SelectorGetWHNF
{
    static constexpr std::string_view tag = "getWHNF";
    std::string from;
    auto operator<=>(const SelectorGetWHNF &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorGetWHNF, ResultWHNF)

/** Get function argument info (formals). */
struct SelectorGetFunctionInfo
{
    static constexpr std::string_view tag = "getFunctionInfo";
    std::string from;   ///< Parent Q identity (stable under #178).
    auto operator<=>(const SelectorGetFunctionInfo &) const = default;
};

/** Result for getFunctionInfo: optional formals map + ellipsis. */
struct ResultFunctionInfo
{
    bool hasInfo;                        ///< false if not a function with formals
    std::map<std::string, bool> formals; ///< name -> hasDefault (empty if !hasInfo)
    bool ellipsis = false;
};

DECLARE_SELECTOR_RESULT(SelectorGetFunctionInfo, ResultFunctionInfo)

/** Apply a function to an argument. `fn` is the hex of fn's own Q hash
    — a stepping stone toward the full compositional shape (nested
    SelectorVariant). Arg is outer-supplied, observed by value; its
    discrimination flows through cur (observations on arg fold into
    cell.factSetHash via the pull model). */
struct SelectorApply
{
    static constexpr std::string_view tag = "apply";
    std::string fn;
    auto operator<=>(const SelectorApply &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorApply, ResultWHNF)

/** Apply a callback to a contra-arg, identified by the outer's
    observation-set on the contra-arg.

    Distinct from `SelectorApply` in identity semantics: a regular
    `SelectorApply` identifies the arg by its state hash (evolving via
    Env-layer observation folding), while `SelectorCallbackApply`
    identifies the arg by the SET of observations the outer's
    callback made on it during this call. Different observation sets
    → different queryHashes → distinct DB rows.

    Motivation: sibling `(cb 10) + (cb 20)` calls whose contra-args
    have identical initial state hashes but observably different
    values. State-hash-only identity collides at the first probe;
    observation-set identity discriminates by construction.

    `argObsSet` is a content hash referring to an entry in the
    ObservationSet CAS pool. The pool entry lists the (selectorHash,
    responseHash) tuples of the outer's probes on the contra-arg
    during this call.

    Payload consumers: writer emits this at callback firing end
    with the accumulated observation set. Walker constructs a proxy
    that answers exactly those observations, invokes fn live. */
struct SelectorCallbackApply
{
    static constexpr std::string_view tag = "callbackApply";
    /** Hex of the callback fn's Q hash — the query-space identity of
        the fn being applied. Under the unified selector shape (SelectorX
        = fn-that-produced-us + distinguishing path), argObsSet plays
        the same role as `name` in SelectorGetAttr: the discriminator
        that says which observation set this firing recorded. */
    std::string fn;
    std::string argObsSet;     ///< Content hash of the observation set
    auto operator<=>(const SelectorCallbackApply &) const = default;
};
DECLARE_SELECTOR_RESULT(SelectorCallbackApply, ResultWHNF)

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
    SelectorGetWHNF,
    SelectorApply,
    SelectorCallbackApply>;

/**
 * All result payload types.
 */
template<template<typename> class F>
using Results = ApplyWrapper<
    F,
    ResultFunctionInfo,
    ResultWHNF>;

// ---------------------------------------------------------------------------
// Variant types for SelectorVariant / ResultVariant
// ---------------------------------------------------------------------------

using SelectorVariant = std::variant<
    SelectorExpr,
    SelectorImport,
    SelectorGetAttr,
    SelectorGetListElem,
    SelectorGetFunctionInfo,
    SelectorGetWHNF,
    SelectorApply,
    SelectorCallbackApply>;

using ResultVariant = std::variant<
    ResultFunctionInfo,
    ResultWHNF>;

/** SelectorVariant's own to_json/from_json — visits the variant to
    emit an alternative's flat fields plus the `tag` discriminator,
    and reads them back into the right alternative via the fold
    template below. */
void to_json(nlohmann::json & j, const SelectorVariant & q);
void from_json(const nlohmann::json & j, SelectorVariant & q);

namespace detail {

/**
 * Fold-based `from_json` for any `std::variant<Ts...>` whose
 * alternatives carry a `static constexpr std::string_view tag`
 * discriminator. `Ts...` unpacks straight from the variant type —
 * no per-variant hand-enumeration of alternatives.
 */
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

/**
 * `true` if every `Ts::tag` differs from every other. A tag
 * collision would let `fromJsonByTag` silently pick whichever
 * alternative appears first in `Ts...` for the colliding tag —
 * enforce distinctness at compile time instead.
 */
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
    detail::VariantTagsDistinct<SelectorVariant>::value,
    "SelectorVariant alternatives must have distinct `tag` values — "
    "duplicated tags cause silent misroute in fromJsonByTag.");

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
    SelectorVariant query;
};

struct OuterValueResponse
{
    ResultVariant result;
};

DECLARE_TRACE_PAIR(OuterValueRequest, OuterValueResponse)

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

/**
 * Parse just an inner query object — `{"query": "<tag>", "params": {...}}` —
 * into a `SelectorVariant`. Returns nullopt if `j` doesn't have a
 * recognised tag. Used at CBOR-payload dispatch sites where the
 * wrapping `{"query", "v"}` envelope isn't present.
 */
std::optional<SelectorVariant> parseSelectorVariant(const nlohmann::json & j);

/**
 * Short human-readable rendering of a Query for log lines —
 * `"tag key=value ..."`, with hashes truncated to 12 hex chars.
 * Replaces ad-hoc `params["name"/"index"/"fn"/"arg"]` reads at
 * every log site.
 */
std::string describe(const SelectorVariant & query);

/**
 * True iff a probe's response depends on the referenced Subject's
 * state — i.e. this Selector carries a `from`/`fn`/`arg` state
 * hash that a dispatcher resolves against the caller's cell chain.
 * Two sibling probes that share their requestHash (matching state
 * at that moment) can still yield different responses at the
 * probe that introduces divergence, so caching by requestHash
 * alone would serve one sibling's bytes to the other. Root
 * queries (`SelectorExpr`, `SelectorImport`) never move state.
 */
bool willMoveStateHash(const SelectorVariant & query);

/**
 * The `from` state hash a Query stamps as its primary Merkle
 * parent, if any. Returns nullopt for queries with no `from` field
 * (roots) and for leaves whose hash string doesn't parse.
 */
std::optional<Hash> fromHashOf(const SelectorVariant & query);

/**
 * Rewrite a Query's `from` (and `fromStateHashes[0]` if present)
 * to a new state hash — preserving any existing `argAncestry`
 * attached to those leaves. Used by Q-evolution paths that update
 * Q's identity as its fromSubject state advances. No-op on Selectors
 * with neither field (roots).
 */
void rewriteFrom(SelectorVariant & query, const std::string & newFromHex);

/**
 * SHA-256 of the Query's JSON dump — the canonical selectorHash used
 * as its identity across the trace/store layer. Overload of the
 * per-Q-type `computeSelectorHash` on decision-graph, so callers
 * holding a `SelectorVariant` don't have to std::visit at every site.
 */
Hash computeSelectorHash(const SelectorVariant & query);

/** Serialise a Query variant to its inner JSON payload
    (`{"query": <tag>, "params": {...}}`). */
nlohmann::json toJson(const SelectorVariant & query);

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
    std::map<SelectorVariant, IndexEntry> index;

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
