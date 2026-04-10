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
// Ambient interaction round-trips
// ---------------------------------------------------------------------------

TEST(TraceTypes, AmbientOutgoingRequestRoundTrip)
{
    AmbientOutgoingRequest req{QueryGetAttr{"x", "0"}};
    json j;
    to_json(j, req);
    AmbientOutgoingRequest req2{QueryGetAttr{}};
    from_json(j, req2);
    auto * q = std::get_if<QueryGetAttr>(&req2.query);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->name, "x");
    EXPECT_EQ(q->from, "0");
}

TEST(TraceTypes, AmbientOutgoingResponseRoundTrip)
{
    AmbientOutgoingResponse resp{ResultString{"hello"}};
    json j;
    to_json(j, resp);
    AmbientOutgoingResponse resp2{ResultString{}};
    from_json(j, resp2);
    auto * r = std::get_if<ResultString>(&resp2.result);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->value, "hello");
}

TEST(TraceTypes, AmbientOutgoingResponseWrapperRoundTrip)
{
    Response<AmbientOutgoingRequest> traced{
        .request = {QueryGetAttr{"x", "0"}},
        .response = {ResultMaybeType{std::optional<std::string>{"nInt"}}},
    };
    json j;
    to_json(j, traced);
    EXPECT_EQ(j["type"], "ambientOutgoing");

    Response<AmbientOutgoingRequest> traced2;
    from_json(j, traced2);
    auto * q = std::get_if<QueryGetAttr>(&traced2.request.query);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->name, "x");
}

TEST(TraceTypes, AmbientQueryParseTraceEntry)
{
    Response<AmbientOutgoingRequest> original{
        .request = {QueryGetString{"42"}},
        .response = {ResultString{"hello"}},
    };
    json j;
    to_json(j, original);

    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * resp = std::get_if<Response<AmbientOutgoingRequest>>(&*parsed);
    ASSERT_NE(resp, nullptr);
    auto * q = std::get_if<QueryGetString>(&resp->request.query);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->from, "42");
    auto * r = std::get_if<ResultString>(&resp->response.result);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->value, "hello");
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
    Query<QueryGetAttr> q{.query = QueryGetAttr{"name", "42"}, .v = 99};
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
    QueryGetListElem q{"99", 5};
    json j;
    to_json(j, q);
    QueryGetListElem q2;
    from_json(j, q2);
    EXPECT_EQ(q.from, q2.from);
    EXPECT_EQ(q.index, q2.index);
}

TEST(TraceTypes, QueryGetFloatRoundTrip)
{
    QueryGetFloat q{"77"};
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
    QueryGetAttr a{"name", "1"};
    QueryGetAttr b{"name", "1"};
    QueryGetAttr c{"other", "1"};
    QueryGetAttr d{"name", "2"};
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

// ---------------------------------------------------------------------------
// parseTraceEntry round-trips
// ---------------------------------------------------------------------------

TEST(TraceTypes, ParseFileReadResponse)
{
    Response<FileReadRequest> original{
        .request = {.absPath = "/foo/bar.nix"},
        .response =
            {.contentHash =
                 Hash::parseAny("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", HashAlgorithm::SHA256)},
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Response<FileReadRequest>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->request.absPath, original.request.absPath);
    EXPECT_EQ(r->response.contentHash, original.response.contentHash);
}

TEST(TraceTypes, ParseGetEnvResponse)
{
    Response<GetEnvRequest> original{
        .request = {.name = "HOME"},
        .response = {.value = "/home/user"},
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Response<GetEnvRequest>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->request.name, "HOME");
    EXPECT_EQ(r->response.value, "/home/user");
}

TEST(TraceTypes, ParseGetEnvResponseNullopt)
{
    Response<GetEnvRequest> original{
        .request = {.name = "NONEXISTENT"},
        .response = {.value = std::nullopt},
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Response<GetEnvRequest>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->response.value, std::nullopt);
}

TEST(TraceTypes, ParseQueryExpr)
{
    Query<QueryExpr> original{
        .query = {.expr = "1 + 1", .baseDir = "/home/user"},
        .v = 42,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * q = std::get_if<Query<QueryExpr>>(&*parsed);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->query.expr, "1 + 1");
    EXPECT_EQ(q->v, 42u);
}

TEST(TraceTypes, ParseResultType)
{
    Result<ResultType> original{
        .result = {.type = "set"},
        .v = 0,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Result<ResultType>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->result.type, "set");
}

TEST(TraceTypes, ParseResultMaybeType)
{
    Result<ResultMaybeType> original{
        .result = {.type = "int"},
        .v = 5,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Result<ResultMaybeType>>(&*parsed);
    ASSERT_NE(r, nullptr);
    ASSERT_TRUE(r->result.type.has_value());
    EXPECT_EQ(*r->result.type, "int");
}

TEST(TraceTypes, ParseResultFloat)
{
    Result<ResultFloat> original{
        .result = {.value = 3.14},
        .v = 7,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Result<ResultFloat>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_DOUBLE_EQ(r->result.value, 3.14);
}

TEST(TraceTypes, ParseResultListSize)
{
    Result<ResultListSize> original{
        .result = {.size = 42},
        .v = 3,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Result<ResultListSize>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->result.size, 42u);
}

TEST(TraceTypes, ParseResultStringWithContext)
{
    Result<ResultStringWithContext> original{
        .result = {.value = "hello", .context = {"ctx1", "ctx2"}},
        .v = 4,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Result<ResultStringWithContext>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->result.value, "hello");
    EXPECT_EQ(r->result.context, (std::vector<std::string>{"ctx1", "ctx2"}));
}

TEST(TraceTypes, ParseUnrecognizedReturnsNullopt)
{
    auto j = json{{"unknown", "data"}};
    EXPECT_FALSE(parseTraceEntry(j).has_value());
}

// ---------------------------------------------------------------------------
// Full trace round-trip
// ---------------------------------------------------------------------------

TEST(TraceTypes, FullTraceRoundTrip)
{
    std::vector<TraceEntry> original = {
        Response<FileReadRequest>{
            .request = {.absPath = "/foo/bar.nix"},
            .response =
                {.contentHash =
                     Hash::parseAny("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", HashAlgorithm::SHA256)},
        },
        Response<GetEnvRequest>{
            .request = {.name = "HOME"},
            .response = {.value = "/home/user"},
        },
        Query<QueryExpr>{
            .query = {.expr = "{ x = 1; }", .baseDir = "/"},
            .v = 0,
        },
        Result<ResultType>{
            .result = {.type = "set"},
            .v = 0,
        },
        Query<QueryGetAttr>{
            .query = {.name = "x", .from = "0"},
            .v = 1,
        },
        Result<ResultMaybeType>{
            .result = {.type = "int"},
            .v = 1,
        },
        Query<QueryGetInt>{
            .query = {.from = "1"},
            .v = 2,
        },
        Result<ResultInt>{
            .result = {.value = 42},
            .v = 2,
        },
    };

    // Serialize to JSON array
    json jsonArray = json::array();
    for (const auto & entry : original) {
        std::visit([&](const auto & e) { jsonArray.push_back(json(e)); }, entry);
    }

    // Parse back
    std::vector<TraceEntry> roundtripped;
    for (const auto & j : jsonArray) {
        auto parsed = parseTraceEntry(j);
        ASSERT_TRUE(parsed.has_value()) << "Failed to parse: " << j.dump();
        roundtripped.push_back(std::move(*parsed));
    }

    ASSERT_EQ(original.size(), roundtripped.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_EQ(original[i].index(), roundtripped[i].index()) << "Type mismatch at index " << i;
    }
}

// ---------------------------------------------------------------------------
// correlateTrace
// ---------------------------------------------------------------------------

TEST(TraceTypes, CorrelateTrace)
{
    std::vector<TraceEntry> trace = {
        Query<QueryExpr>{
            .query = {.expr = "{ x = 1; }", .baseDir = "/"},
            .v = 0,
        },
        Result<ResultType>{
            .result = {.type = "set"},
            .v = 0,
        },
        Query<QueryGetAttr>{
            .query = {.name = "x", .from = "0"},
            .v = 1,
        },
        Result<ResultMaybeType>{
            .result = {.type = "int"},
            .v = 1,
        },
    };

    auto correlated = correlateTrace(trace);
    ASSERT_EQ(correlated.size(), 4u);

    auto * q0 = std::get_if<CompletedQuery<QueryExpr>>(&correlated[0]);
    ASSERT_NE(q0, nullptr);
    EXPECT_EQ(q0->resultIndex, 1u);

    auto * q2 = std::get_if<CompletedQuery<QueryGetAttr>>(&correlated[2]);
    ASSERT_NE(q2, nullptr);
    EXPECT_EQ(q2->resultIndex, 3u);
}

// ---------------------------------------------------------------------------
// QueryIndex
// ---------------------------------------------------------------------------

TEST(TraceTypes, QueryIndexLookup)
{
    std::vector<TraceEntry> trace = {
        Query<QueryExpr>{
            .query = {.expr = "42", .baseDir = "/"},
            .v = 0,
        },
        Result<ResultType>{
            .result = {.type = "int"},
            .v = 0,
        },
        Query<QueryGetAttr>{
            .query = {.name = "foo", .from = "0"},
            .v = 1,
        },
        Result<ResultMaybeType>{
            .result = {.type = "set"},
            .v = 1,
        },
    };

    QueryIndex idx(trace);

    auto e1 = idx.lookup(QueryExpr{"42", "/"});
    ASSERT_TRUE(e1.has_value());
    EXPECT_EQ(e1->queryIndex, 0u);
    EXPECT_EQ(e1->resultIndex, 1u);

    auto e2 = idx.lookup(QueryGetAttr{"foo", "0"});
    ASSERT_TRUE(e2.has_value());
    EXPECT_EQ(e2->queryIndex, 2u);
    EXPECT_EQ(e2->resultIndex, 3u);

    // Miss
    auto e3 = idx.lookup(QueryExpr{"999", "/"});
    EXPECT_FALSE(e3.has_value());
}

TEST(TraceTypes, QueryIndexSkipsOrphanedQueries)
{
    std::vector<TraceEntry> trace = {
        Query<QueryExpr>{
            .query = {.expr = "orphan", .baseDir = "/"},
            .v = 99,
        },
        // No matching result for v=99
    };

    QueryIndex idx(trace);
    auto e = idx.lookup(QueryExpr{"orphan", "/"});
    EXPECT_FALSE(e.has_value());
}

} // namespace nix::trace
