#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "nix/expr/trace-types.hh"

namespace nix::trace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Environment request/response round-trips
// ---------------------------------------------------------------------------

TEST(TraceTypes, FileReadRequestRoundTrip)
{
    FileReadRequest req{"/nix/store/abc-foo/default.nix"};
    json j;
    to_json(j, req);
    FileReadRequest req2;
    from_json(j, req2);
    EXPECT_EQ(req.absPath, req2.absPath);
}

TEST(TraceTypes, FileReadResponseRoundTrip)
{
    auto h = Hash::parseSRI("sha256-n4bQgYhMfWWaL+qgxVrQFaO/TxsrC4Is0V1sFbDwCgg=");
    FileReadResponse resp{h};
    json j;
    to_json(j, resp);
    FileReadResponse resp2{h}; // Hash has no default ctor
    from_json(j, resp2);
    EXPECT_EQ(resp.contentHash, resp2.contentHash);
}

TEST(TraceTypes, GetEnvRequestRoundTrip)
{
    GetEnvRequest req{"NIX_PATH"};
    json j;
    to_json(j, req);
    GetEnvRequest req2;
    from_json(j, req2);
    EXPECT_EQ(req.name, req2.name);
}

TEST(TraceTypes, GetEnvResponseWithValue)
{
    GetEnvResponse resp{std::optional<std::string>{"nixpkgs=/nix/store/abc"}};
    json j;
    to_json(j, resp);
    GetEnvResponse resp2;
    from_json(j, resp2);
    ASSERT_TRUE(resp2.value.has_value());
    EXPECT_EQ(*resp.value, *resp2.value);
}

TEST(TraceTypes, GetEnvResponseEmpty)
{
    GetEnvResponse resp{std::nullopt};
    json j;
    to_json(j, resp);
    GetEnvResponse resp2;
    from_json(j, resp2);
    EXPECT_FALSE(resp2.value.has_value());
}

// ---------------------------------------------------------------------------
// Response wrapper round-trip
// ---------------------------------------------------------------------------

TEST(TraceTypes, ResponseWrapperRoundTrip)
{
    auto h = Hash::parseSRI("sha256-n4bQgYhMfWWaL+qgxVrQFaO/TxsrC4Is0V1sFbDwCgg=");
    Response<FileReadRequest> traced{
        .request = FileReadRequest{"/some/file"},
        .response = FileReadResponse{h},
    };
    json j;
    to_json(j, traced);
    Response<FileReadRequest> traced2{
        .request = FileReadRequest{},
        .response = FileReadResponse{h},
    };
    from_json(j, traced2);
    EXPECT_EQ(traced.request.absPath, traced2.request.absPath);
    EXPECT_EQ(traced.response.contentHash, traced2.response.contentHash);
}

// ---------------------------------------------------------------------------
// Query/Result round-trips
// ---------------------------------------------------------------------------

TEST(TraceTypes, QueryExprRoundTrip)
{
    QueryExpr q{"1 + 1", "/some/dir"};
    json j;
    to_json(j, q);
    QueryExpr q2;
    from_json(j, q2);
    EXPECT_EQ(q.expr, q2.expr);
    EXPECT_EQ(q.baseDir, q2.baseDir);
}

TEST(TraceTypes, QueryWrapperRoundTrip)
{
    Query<QueryGetAttr> q{.query = QueryGetAttr{"name", 42}, .v = 99};
    json j;
    to_json(j, q);
    Query<QueryGetAttr> q2;
    from_json(j, q2);
    EXPECT_EQ(q.query.name, q2.query.name);
    EXPECT_EQ(q.query.from, q2.query.from);
    EXPECT_EQ(q.v, q2.v);
}

TEST(TraceTypes, ResultStringRoundTrip)
{
    ResultString r{"hello"};
    json j;
    to_json(j, r);
    ResultString r2;
    from_json(j, r2);
    EXPECT_EQ(r.value, r2.value);
}

TEST(TraceTypes, ResultStringWithContextRoundTrip)
{
    ResultStringWithContext r{"hello", {"ctx1", "ctx2"}};
    json j;
    to_json(j, r);
    ResultStringWithContext r2;
    from_json(j, r2);
    EXPECT_EQ(r.value, r2.value);
    EXPECT_EQ(r.context, r2.context);
}

TEST(TraceTypes, ResultMaybeTypePresent)
{
    ResultMaybeType r{std::optional<std::string>{"attrs"}};
    json j;
    to_json(j, r);
    ResultMaybeType r2;
    from_json(j, r2);
    ASSERT_TRUE(r2.type.has_value());
    EXPECT_EQ(*r.type, *r2.type);
}

TEST(TraceTypes, ResultMaybeTypeAbsent)
{
    ResultMaybeType r{std::nullopt};
    json j;
    to_json(j, r);
    ResultMaybeType r2;
    from_json(j, r2);
    EXPECT_FALSE(r2.type.has_value());
}

TEST(TraceTypes, ResultFloatRoundTrip)
{
    ResultFloat r{3.14};
    json j;
    to_json(j, r);
    ResultFloat r2;
    from_json(j, r2);
    EXPECT_DOUBLE_EQ(r.value, r2.value);
}

TEST(TraceTypes, ResultListSizeRoundTrip)
{
    ResultListSize r{42};
    json j;
    to_json(j, r);
    ResultListSize r2;
    from_json(j, r2);
    EXPECT_EQ(r.size, r2.size);
}

TEST(TraceTypes, QueryGetListElemRoundTrip)
{
    QueryGetListElem q{99, 5};
    json j;
    to_json(j, q);
    QueryGetListElem q2;
    from_json(j, q2);
    EXPECT_EQ(q.from, q2.from);
    EXPECT_EQ(q.index, q2.index);
}

TEST(TraceTypes, QueryGetFloatRoundTrip)
{
    QueryGetFloat q{77};
    json j;
    to_json(j, q);
    QueryGetFloat q2;
    from_json(j, q2);
    EXPECT_EQ(q.from, q2.from);
}

TEST(TraceTypes, ResultWrapperRoundTrip)
{
    Result<ResultInt> r{.result = ResultInt{42}, .v = 7};
    json j;
    to_json(j, r);
    Result<ResultInt> r2;
    from_json(j, r2);
    EXPECT_EQ(r.result.value, r2.result.value);
    EXPECT_EQ(r.v, r2.v);
}

// ---------------------------------------------------------------------------
// Tag constants
// ---------------------------------------------------------------------------

TEST(TraceTypes, QueryTagConstants)
{
    EXPECT_EQ(QueryExpr::tag, "expr");
    EXPECT_EQ(QueryImport::tag, "import");
    EXPECT_EQ(QueryGetAttr::tag, "getAttr");
    EXPECT_EQ(QueryGetString::tag, "getString");
    EXPECT_EQ(QueryGetStringWithContext::tag, "getStringWithContext");
    EXPECT_EQ(QueryGetAttrNames::tag, "getAttrNames");
    EXPECT_EQ(QueryGetType::tag, "getType");
    EXPECT_EQ(QueryGetBool::tag, "getBool");
    EXPECT_EQ(QueryGetInt::tag, "getInt");
    EXPECT_EQ(QueryGetFloat::tag, "getFloat");
    EXPECT_EQ(QueryGetListOfStrings::tag, "getListOfStrings");
    EXPECT_EQ(QueryGetListSize::tag, "getListSize");
    EXPECT_EQ(QueryGetListElem::tag, "getListElem");
    EXPECT_EQ(QueryGetPath::tag, "getPath");
    EXPECT_EQ(FileReadRequest::tag, "fileRead");
    EXPECT_EQ(GetEnvRequest::tag, "getEnv");
}

// ---------------------------------------------------------------------------
// Query comparison operators
// ---------------------------------------------------------------------------

TEST(TraceTypes, QueryExprComparison)
{
    QueryExpr a{"1 + 1", "/"};
    QueryExpr b{"1 + 1", "/"};
    QueryExpr c{"2 + 2", "/"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_TRUE(a < c || c < a); // strict weak ordering
}

TEST(TraceTypes, QueryGetAttrComparison)
{
    QueryGetAttr a{"name", 1};
    QueryGetAttr b{"name", 1};
    QueryGetAttr c{"other", 1};
    QueryGetAttr d{"name", 2};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

// ---------------------------------------------------------------------------
// Response type tag in JSON
// ---------------------------------------------------------------------------

TEST(TraceTypes, ResponseHasTypeTag)
{
    auto h = Hash::parseSRI("sha256-n4bQgYhMfWWaL+qgxVrQFaO/TxsrC4Is0V1sFbDwCgg=");
    Response<FileReadRequest> r{
        .request = FileReadRequest{"/file"},
        .response = FileReadResponse{h},
    };
    json j;
    to_json(j, r);
    EXPECT_EQ(j.at("type"), "fileRead");
}

TEST(TraceTypes, ResponseEnvHasTypeTag)
{
    Response<GetEnvRequest> r{
        .request = GetEnvRequest{"HOME"},
        .response = GetEnvResponse{"/home/user"},
    };
    json j;
    to_json(j, r);
    EXPECT_EQ(j.at("type"), "getEnv");
}

// ---------------------------------------------------------------------------
// ResultMaybeType uses "attrType" JSON field
// ---------------------------------------------------------------------------

TEST(TraceTypes, ResultMaybeTypeUsesAttrTypeField)
{
    ResultMaybeType r{std::optional<std::string>{"set"}};
    json j;
    to_json(j, r);
    EXPECT_TRUE(j.contains("attrType"));
    EXPECT_FALSE(j.contains("type"));
    EXPECT_EQ(j.at("attrType"), "set");
}

TEST(TraceTypes, ResultMaybeTypeNullUsesAttrTypeField)
{
    ResultMaybeType r{std::nullopt};
    json j;
    to_json(j, r);
    EXPECT_TRUE(j.contains("attrType"));
    EXPECT_TRUE(j.at("attrType").is_null());
}

} // namespace nix::trace
