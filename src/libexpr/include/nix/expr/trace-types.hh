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

#define DECLARE_QUERY_RESULT(QueryType, ResultType)          \
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

/** Result containing an ObjectType as string. */
struct ResultType
{
    std::string type;
};

/** Result for getAttr: either a type (attribute exists) or nullopt (missing). */
struct ResultMaybeType
{
    std::optional<std::string> type;
};

/** Result containing a list of strings. */
struct ResultListOfStrings
{
    std::vector<std::string> values;
};

/** Payload alternatives for `ResultWHNF`. One per Nix object type
    that carries something at WHNF. nFunction/nNull/nExternal/nThunk
    use `WHNFEmpty` (nothing to record beyond the type). */
struct WHNFEmpty {};
struct WHNFInt { int64_t value; };
struct WHNFBool { bool value; };
struct WHNFFloat { double value; };
struct WHNFPath { std::string path; };
struct WHNFString { std::string value; std::vector<std::string> context; };
struct WHNFAttrs { std::vector<std::string> names; };
struct WHNFList { size_t size; };

/** Result of a single WHNF (Weak Head Normal Form) force. Carries the
    type discriminator plus the type-determined payload as a tagged
    variant. A single observation instead of separate getType +
    getInt/getString/etc., so siblings that force the same value
    record symmetric chains regardless of inner-evaluator memoization
    that would otherwise skip getType on subsequent forces. */
struct ResultWHNF
{
    std::string type;
    std::variant<WHNFEmpty, WHNFInt, WHNFBool, WHNFFloat, WHNFPath, WHNFString, WHNFAttrs, WHNFList> payload;
};

// ---------------------------------------------------------------------------
// QueryLeaf: typed `from` / `fn` / `arg` field of Query types
// ---------------------------------------------------------------------------

/**
 * Numbered identifier carrier. Sanctioned only at the CLI per
 * Principle #1 of the content-identity design — everything below the
 * CLI uses content-defined hashes via `StateHashLeaf`. Currently
 * unused: the eval-cache path constructs `StateHashLeaf` directly, and
 * CLI integration through this carrier hasn't landed.
 * See doc/design/tracing-eval-cache-subject-identity.md §Foundational principles.
 */
struct OuterLeaf
{
    int index;
    auto operator<=>(const OuterLeaf &) const = default;
};

/**
 * Content-defined factset hash: identifies a value by the hash of its
 * accumulated observation factset.
 *
 * Stored as a hex string for wire-format compatibility with the previous
 * std::string `from` field — recorded ContentLeafs are interchangeable
 * with the strings today's code already produces.
 */
struct StateHashLeaf
{
    std::string hash;
    auto operator<=>(const StateHashLeaf &) const = default;
};

/**
 * QueryLeaf: the typed `from` (and `fn`/`arg`) field of Query types.
 *
 * Implicit construction from std::string / const char* produces a
 * StateHashLeaf — most construction sites today pass a hex string for
 * `from`, and this lets them keep working without source changes during
 * the typed-leaf rollout. JSON serialisation also preserves the wire
 * format (StateHashLeaf encodes as a plain string).
 */
struct QueryLeaf
{
    std::variant<OuterLeaf, StateHashLeaf> data;

    QueryLeaf() = default;
    QueryLeaf(std::string hex) : data(StateHashLeaf{std::move(hex)}) {}
    QueryLeaf(const char * hex) : data(StateHashLeaf{hex}) {}
    QueryLeaf(OuterLeaf a) : data(a) {}
    QueryLeaf(StateHashLeaf c) : data(std::move(c)) {}

    bool isStateHash() const
    {
        return std::holds_alternative<StateHashLeaf>(data);
    }
    bool isAmbient() const
    {
        return std::holds_alternative<OuterLeaf>(data);
    }
    const std::string & stateHash() const
    {
        return std::get<StateHashLeaf>(data).hash;
    }
    int outerIndex() const
    {
        return std::get<OuterLeaf>(data).index;
    }

    auto operator<=>(const QueryLeaf &) const = default;
};

void to_json(nlohmann::json & j, const QueryLeaf & leaf);
void from_json(const nlohmann::json & j, QueryLeaf & leaf);

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
struct QueryExpr
{
    static constexpr std::string_view tag = "expr";
    std::string expr;
    std::string baseDir;
    auto operator<=>(const QueryExpr &) const = default;
};
DECLARE_QUERY_RESULT(QueryExpr, ResultType)

/** Import/evaluate a file. */
struct QueryImport
{
    static constexpr std::string_view tag = "import";
    std::string path;
    auto operator<=>(const QueryImport &) const = default;
};
DECLARE_QUERY_RESULT(QueryImport, ResultType)

/** Get an attribute from a value. */
struct QueryGetAttr
{
    static constexpr std::string_view tag = "getAttr";
    std::string name;
    QueryLeaf from;   ///< Parent object identity (legacy single-`from`; superseded by `fromStateHashes`)
    std::vector<QueryLeaf> fromStateHashes;  ///< Root cb_arg state hashes (one entry per hole in `path`)
    PathExpr path;    ///< Path from each root to this observation
    auto operator<=>(const QueryGetAttr &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetAttr, ResultMaybeType)

/** Get a list of strings (no context). */
struct QueryGetListOfStrings
{
    static constexpr std::string_view tag = "getListOfStrings";
    QueryLeaf from;   ///< Parent object identity (legacy single-`from`; superseded by `fromStateHashes`)
    std::vector<QueryLeaf> fromStateHashes;  ///< Root cb_arg state hashes (one entry per hole in `path`)
    PathExpr path;    ///< Path from each root to this observation
    auto operator<=>(const QueryGetListOfStrings &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetListOfStrings, ResultListOfStrings)

/** Get a list element by index. */
struct QueryGetListElem
{
    static constexpr std::string_view tag = "getListElem";
    QueryLeaf from;   ///< Parent object identity (legacy single-`from`; superseded by `fromStateHashes`)
    size_t index;
    std::vector<QueryLeaf> fromStateHashes;  ///< Root cb_arg state hashes (one entry per hole in `path`)
    PathExpr path;    ///< Path from each root to this observation
    auto operator<=>(const QueryGetListElem &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetListElem, ResultType)

/** Force a value to WHNF and read its type + type-determined payload
    in one shot. Used by the cache-layer Objects to combine what would
    otherwise be separate getType + getInt/getString/etc. observations
    — a single WHNF probe per value force, recorded once. The
    individual getType/getInt/etc. paths remain for callers that don't
    need WHNF semantics. */
struct QueryGetWHNF
{
    static constexpr std::string_view tag = "getWHNF";
    QueryLeaf from;
    std::vector<QueryLeaf> fromStateHashes;
    PathExpr path;
    auto operator<=>(const QueryGetWHNF &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetWHNF, ResultWHNF)

/** Get function argument info (formals). */
struct QueryGetFunctionInfo
{
    static constexpr std::string_view tag = "getFunctionInfo";
    QueryLeaf from;   ///< Parent object identity (legacy single-`from`; superseded by `fromStateHashes`)
    std::vector<QueryLeaf> fromStateHashes;  ///< Root cb_arg state hashes (one entry per hole in `path`)
    PathExpr path;    ///< Path from each root to this observation
    auto operator<=>(const QueryGetFunctionInfo &) const = default;
};

/** Result for getFunctionInfo: optional formals map + ellipsis. */
struct ResultFunctionInfo
{
    bool hasInfo;                        ///< false if not a function with formals
    std::map<std::string, bool> formals; ///< name -> hasDefault (empty if !hasInfo)
    bool ellipsis = false;
};

DECLARE_QUERY_RESULT(QueryGetFunctionInfo, ResultFunctionInfo)

/** Apply a function to an argument.

    Two construction modes, with the same content-addressed semantics:

    - **Legacy direct mode** populates `fn`/`arg` with the constituents'
      scope state ids. Used by the cb-apply recording on the writer
      side, where the apply's `fn` and `arg` are already content-addressed
      leaf-form Objects (TracingObject, OuterObject).

    - **Per-arg path-encoded mode** populates `fromStateHashes` with the root
      cb_args' state hashes and uses `fnPath`/`argPath`+`fnRootIndex`/`argRootIndex`
      to encode how fn and arg are reached from those roots. Used by
      subject-id to compute an ApplyResultSubject's state hash without needing
      standalone derived-subject state hashes — see
      `content-identity-via-asks.md` §Principle 3 (per-arg
      centralization). `fn`/`arg` stay empty in this mode.

    Both modes share the same JSON envelope; consumers distinguish by
    whether `fromStateHashes` is populated. */
struct QueryApply
{
    static constexpr std::string_view tag = "apply";
    QueryLeaf fn;  ///< Function identity (legacy direct mode)
    QueryLeaf arg; ///< Argument identity (legacy direct mode)
    std::vector<QueryLeaf> fromStateHashes;  ///< Root cb_arg state hashes (per-arg mode)
    PathExpr fnPath;                  ///< Path from `fromStateHashes[fnRootIndex]` to fn
    PathExpr argPath;                 ///< Path from `fromStateHashes[argRootIndex]` to arg
    size_t fnRootIndex{0};
    size_t argRootIndex{0};
    auto operator<=>(const QueryApply &) const = default;
};
DECLARE_QUERY_RESULT(QueryApply, ResultType)

/** Apply a callback to a contra-arg, identified by the outer's
    observation-set on the contra-arg.

    Distinct from `QueryApply` in identity semantics: a regular
    `QueryApply` identifies the arg by its state hash (evolving via
    Env-layer observation folding), while `QueryCallbackApply`
    identifies the arg by the SET of observations the outer's
    callback made on it during this call. Different observation sets
    → different queryHashes → distinct DB rows.

    Motivation: sibling `(cb 10) + (cb 20)` calls whose contra-args
    have identical initial state hashes but observably different
    values. State-hash-only identity collides at the first probe;
    observation-set identity discriminates by construction.

    `argObsSet` is a content hash referring to an entry in the
    ObservationSet CAS pool. The pool entry lists the (queryHash,
    responseHash) tuples of the outer's probes on the contra-arg
    during this call.

    Payload consumers: writer emits this at callback firing end
    with the accumulated observation set. Walker constructs a proxy
    that answers exactly those observations, invokes fn live. */
struct QueryCallbackApply
{
    static constexpr std::string_view tag = "callbackApply";
    QueryLeaf fn;              ///< Function identity (state hash of the callback)
    std::string argObsSet;     ///< Content hash of the observation set
    std::string argAncestry;   ///< callArgAncestry of the cached call — walker needs this to reconstruct the arg's subject state hashes when firing fn live with a proxy backed by obsSet.
    int argDepth = 0;          ///< Reverse-De-Bruijn depth of the contra-arg. Combined with argAncestry lets the walker rebuild the arg's Subject and compute matching state hashes.
    auto operator<=>(const QueryCallbackApply &) const = default;
};
DECLARE_QUERY_RESULT(QueryCallbackApply, ResultWHNF)

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
using Queries = ApplyWrapper<
    F,
    QueryExpr,
    QueryImport,
    QueryGetAttr,
    QueryGetListOfStrings,
    QueryGetListElem,
    QueryGetFunctionInfo,
    QueryGetWHNF,
    QueryApply,
    QueryCallbackApply>;

/**
 * All result payload types.
 */
template<template<typename> class F>
using Results = ApplyWrapper<
    F,
    ResultType,
    ResultMaybeType,
    ResultListOfStrings,
    ResultFunctionInfo,
    ResultWHNF>;

// ---------------------------------------------------------------------------
// Variant types for QueryVariant / ResultVariant
// ---------------------------------------------------------------------------

using QueryVariant = std::variant<
    QueryExpr,
    QueryImport,
    QueryGetAttr,
    QueryGetListOfStrings,
    QueryGetListElem,
    QueryGetFunctionInfo,
    QueryGetWHNF,
    QueryApply,
    QueryCallbackApply>;

using ResultVariant = std::variant<
    ResultType,
    ResultMaybeType,
    ResultListOfStrings,
    ResultFunctionInfo,
    ResultWHNF>;

// ---------------------------------------------------------------------------
// Ambient message pairing trace types
//
// These embed content tracing events into the Environment trace.
// Outgoing: local evaluator queries the ambient (outer) evaluator.
// Incoming: ambient evaluator accesses local values during a callback.
// ---------------------------------------------------------------------------

/**
 * Outgoing ambient query: local→external.
 * Data queries (getType, getAttr, ...) and external calls (apply).
 */
struct OuterValueRequest
{
    static constexpr std::string_view tag = "outerValue";
    QueryVariant query;
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
using TraceEntry = detail::CombineVariants<AllEnvRequests<Response>, Queries<Query>, Results<Result>>::type;

/**
 * Trace entry with queries correlated to their results.
 */
using CorrelatedTraceEntry =
    detail::CombineVariants<AllEnvRequests<Response>, Queries<CompletedQuery>, Results<Result>>::type;

/**
 * Parse a JSON entry into a typed TraceEntry.
 * Returns nullopt if the entry type is not recognized.
 */
std::optional<TraceEntry> parseTraceEntry(const nlohmann::json & j);

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
class QueryIndex
{
    std::map<QueryVariant, IndexEntry> index;

public:
    explicit QueryIndex(const std::vector<TraceEntry> & trace);

    template<typename Q>
    std::optional<IndexEntry> lookup(const Q & q) const
    {
        auto it = index.find(q);
        return it != index.end() ? std::optional{it->second} : std::nullopt;
    }
};

} // namespace nix::trace
