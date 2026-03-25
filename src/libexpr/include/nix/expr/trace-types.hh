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

#include <optional>
#include <string>
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
    j = nlohmann::json{{"request", r.request}, {"response", r.response}};
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
    std::string expr;
    std::string baseDir;
};
DECLARE_QUERY_RESULT(QueryExpr, ResultType)

/** Import/evaluate a file. */
struct QueryImport
{
    std::string path;
};
DECLARE_QUERY_RESULT(QueryImport, ResultType)

/** Get an attribute from a value. */
struct QueryGetAttr
{
    std::string name;
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetAttr, ResultMaybeType)

/** Get the string value (ignoring context). */
struct QueryGetString
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetString, ResultString)

/** Get string with context. */
struct QueryGetStringWithContext
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetStringWithContext, ResultStringWithContext)

/** Get attribute names from an attrset. */
struct QueryGetAttrNames
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetAttrNames, ResultListOfStrings)

/** Get the type of a value. */
struct QueryGetType
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetType, ResultType)

/** Get a boolean value. */
struct QueryGetBool
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetBool, ResultBool)

/** Get an integer value. */
struct QueryGetInt
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetInt, ResultInt)

/** Get a float value. */
struct QueryGetFloat
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetFloat, ResultFloat)

/** Get a list of strings (no context). */
struct QueryGetListOfStrings
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetListOfStrings, ResultListOfStrings)

/** Get the number of list elements. */
struct QueryGetListSize
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetListSize, ResultListSize)

/** Get a list element by index. */
struct QueryGetListElem
{
    uint64_t from;
    size_t index;
};
DECLARE_QUERY_RESULT(QueryGetListElem, ResultType)

/** Get a path value. */
struct QueryGetPath
{
    uint64_t from;
};
DECLARE_QUERY_RESULT(QueryGetPath, ResultPath)

} // namespace nix::trace
