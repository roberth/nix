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

/** Result containing a string value. */
struct ResultString
{
    std::string value;
};

/** Result containing a string value with context. */
struct ResultStringWithContext
{
    std::string value;
    std::vector<std::string> context;
};

/** Result containing an integer value. */
struct ResultInt
{
    int64_t value;
};

/** Result containing a float value. */
struct ResultFloat
{
    double value;
};

/** Result containing a boolean value. */
struct ResultBool
{
    bool value;
};

/** Result containing a path. */
struct ResultPath
{
    std::string path;
};

/** Result containing a list of strings. */
struct ResultListOfStrings
{
    std::vector<std::string> values;
};

/** Result containing a list size. */
struct ResultListSize
{
    size_t size;
};

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
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetAttr &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetAttr, ResultMaybeType)

/** Get the string value (ignoring context). */
struct QueryGetString
{
    static constexpr std::string_view tag = "getString";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetString &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetString, ResultString)

/** Get string with context. */
struct QueryGetStringWithContext
{
    static constexpr std::string_view tag = "getStringWithContext";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetStringWithContext &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetStringWithContext, ResultStringWithContext)

/** Get attribute names from an attrset. */
struct QueryGetAttrNames
{
    static constexpr std::string_view tag = "getAttrNames";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetAttrNames &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetAttrNames, ResultListOfStrings)

/** Get the type of a value. */
struct QueryGetType
{
    static constexpr std::string_view tag = "getType";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetType &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetType, ResultType)

/** Get a boolean value. */
struct QueryGetBool
{
    static constexpr std::string_view tag = "getBool";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetBool &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetBool, ResultBool)

/** Get an integer value. */
struct QueryGetInt
{
    static constexpr std::string_view tag = "getInt";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetInt &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetInt, ResultInt)

/** Get a float value. */
struct QueryGetFloat
{
    static constexpr std::string_view tag = "getFloat";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetFloat &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetFloat, ResultFloat)

/** Get a list of strings (no context). */
struct QueryGetListOfStrings
{
    static constexpr std::string_view tag = "getListOfStrings";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetListOfStrings &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetListOfStrings, ResultListOfStrings)

/** Get the number of list elements. */
struct QueryGetListSize
{
    static constexpr std::string_view tag = "getListSize";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetListSize &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetListSize, ResultListSize)

/** Get a list element by index. */
struct QueryGetListElem
{
    static constexpr std::string_view tag = "getListElem";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    size_t index;
    auto operator<=>(const QueryGetListElem &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetListElem, ResultType)

/** Get a path value. */
struct QueryGetPath
{
    static constexpr std::string_view tag = "getPath";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetPath &) const = default;
};
DECLARE_QUERY_RESULT(QueryGetPath, ResultPath)

/** Get function argument info (formals). */
struct QueryGetFunctionInfo
{
    static constexpr std::string_view tag = "getFunctionInfo";
    std::string from; ///< Parent object's queryHash (Merkle identity)
    auto operator<=>(const QueryGetFunctionInfo &) const = default;
};

/** Result for getFunctionInfo: optional formals map + ellipsis. */
struct ResultFunctionInfo
{
    bool hasInfo;                          ///< false if not a function with formals
    std::map<std::string, bool> formals;  ///< name -> hasDefault (empty if !hasInfo)
    bool ellipsis = false;
};

DECLARE_QUERY_RESULT(QueryGetFunctionInfo, ResultFunctionInfo)

/** Apply a function to an argument. */
struct QueryApply
{
    static constexpr std::string_view tag = "apply";
    std::string fn;  ///< Function's queryHash identity
    std::string arg; ///< Argument's queryHash identity
    auto operator<=>(const QueryApply &) const = default;
};
DECLARE_QUERY_RESULT(QueryApply, ResultType)

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
    QueryGetString,
    QueryGetStringWithContext,
    QueryGetAttrNames,
    QueryGetType,
    QueryGetBool,
    QueryGetInt,
    QueryGetFloat,
    QueryGetListOfStrings,
    QueryGetListSize,
    QueryGetListElem,
    QueryGetPath,
    QueryGetFunctionInfo,
    QueryApply>;

/**
 * All result payload types.
 */
template<template<typename> class F>
using Results = ApplyWrapper<
    F,
    ResultType,
    ResultMaybeType,
    ResultString,
    ResultInt,
    ResultFloat,
    ResultBool,
    ResultPath,
    ResultListOfStrings,
    ResultStringWithContext,
    ResultListSize,
    ResultFunctionInfo>;

// ---------------------------------------------------------------------------
// Variant types for QueryVariant / ResultVariant
// ---------------------------------------------------------------------------

using QueryVariant = std::variant<
    QueryExpr,
    QueryImport,
    QueryGetAttr,
    QueryGetString,
    QueryGetStringWithContext,
    QueryGetAttrNames,
    QueryGetType,
    QueryGetBool,
    QueryGetInt,
    QueryGetFloat,
    QueryGetListOfStrings,
    QueryGetListSize,
    QueryGetListElem,
    QueryGetPath,
    QueryGetFunctionInfo,
    QueryApply>;

using ResultVariant = std::variant<
    ResultType,
    ResultMaybeType,
    ResultString,
    ResultInt,
    ResultFloat,
    ResultBool,
    ResultPath,
    ResultListOfStrings,
    ResultStringWithContext,
    ResultListSize,
    ResultFunctionInfo>;

// ---------------------------------------------------------------------------
// Contra-query: query on a virtual value provided by the outer evaluator
// ---------------------------------------------------------------------------

/**
 * A contra-query embeds an existing Query/Result pair, issued by the inner
 * evaluator to inspect a virtual value from the outer evaluator. Uses the
 * typed QueryVariant/ResultVariant so the trie can hash and look up the
 * structured data.
 */
struct ContraQueryRequest
{
    static constexpr std::string_view tag = "contraQuery";
    QueryVariant query;
};

struct ContraQueryResponse
{
    ResultVariant result;
};

DECLARE_TRACE_PAIR(ContraQueryRequest, ContraQueryResponse)

// Update EnvRequests to include ContraQueryRequest
template<template<typename> class F>
using AllEnvRequests = ApplyWrapper<F, FileReadRequest, GetEnvRequest, ContraQueryRequest>;

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
