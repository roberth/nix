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
    OuterValueRequest req{SelectorGetAttr{"x", "0"}};
    json j;
    to_json(j, req);
    OuterValueRequest req2{SelectorGetAttr{}};
    from_json(j, req2);
    auto * q = std::get_if<SelectorGetAttr>(&req2.query);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->name, "x");
    EXPECT_EQ(q->from, "0");
}

TEST(TraceTypes, AmbientOutgoingResponseWrapperRoundTrip)
{
    Response<OuterValueRequest> traced{
        .request = {SelectorGetAttr{"x", "0"}},
        .response = {ResultWHNF{"int", WHNFInt{7}}},
    };
    json j;
    to_json(j, traced);
    EXPECT_EQ(j["type"], "outerValue");

    Response<OuterValueRequest> traced2;
    from_json(j, traced2);
    auto * q = std::get_if<SelectorGetAttr>(&traced2.request.query);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->name, "x");
}

TEST(TraceTypes, AmbientQueryParseTraceEntry)
{
    Response<OuterValueRequest> original{
        .request = {SelectorGetAttr{"key", "42"}},
        .response = {ResultWHNF{"int", WHNFInt{9}}},
    };
    json j;
    to_json(j, original);

    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * resp = std::get_if<Response<OuterValueRequest>>(&*parsed);
    ASSERT_NE(resp, nullptr);
    auto * q = std::get_if<SelectorGetAttr>(&resp->request.query);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->from, "42");
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
    SelectorExpr q{"1 + 1", "/some/dir"};
    json j;
    to_json(j, q);
    SelectorExpr q2;
    from_json(j, q2);
    EXPECT_EQ(q.expr, q2.expr);
    EXPECT_EQ(q.baseDir, q2.baseDir);
}

TEST(TraceTypes, QueryWrapperRoundTrip)
{
    Query<SelectorGetAttr> q{.query = SelectorGetAttr{"name", "42"}, .v = 99};
    json j;
    to_json(j, q);
    Query<SelectorGetAttr> q2;
    from_json(j, q2);
    EXPECT_EQ(q.query.name, q2.query.name);
    EXPECT_EQ(q.query.from, q2.query.from);
    EXPECT_EQ(q.v, q2.v);
}

TEST(TraceTypes, QueryGetAttrRoundTrip)
{
    SelectorGetAttr q{"foo", "42"};
    json j;
    to_json(j, q);
    SelectorGetAttr q2;
    from_json(j, q2);
    EXPECT_EQ(q.name, q2.name);
    EXPECT_EQ(q.from, q2.from);
}

TEST(TraceTypes, QueryGetListElemRoundTrip)
{
    SelectorGetListElem q{"99", 5};
    json j;
    to_json(j, q);
    SelectorGetListElem q2;
    from_json(j, q2);
    EXPECT_EQ(q.from, q2.from);
    EXPECT_EQ(q.index, q2.index);
}

TEST(TraceTypes, ResultWrapperRoundTrip)
{
    Result<ResultWHNF> r{.result = ResultWHNF{"int", WHNFInt{42}}, .v = 7};
    json j;
    to_json(j, r);
    Result<ResultWHNF> r2;
    from_json(j, r2);
    EXPECT_EQ(r.result.type, r2.result.type);
    EXPECT_EQ(r.v, r2.v);
}

// ---------------------------------------------------------------------------
// Tag constants
// ---------------------------------------------------------------------------

TEST(TraceTypes, QueryTagConstants)
{
    EXPECT_EQ(SelectorExpr::tag, "expr");
    EXPECT_EQ(SelectorImport::tag, "import");
    EXPECT_EQ(SelectorGetAttr::tag, "getAttr");
    EXPECT_EQ(SelectorGetListElem::tag, "getListElem");
    EXPECT_EQ(SelectorGetWHNF::tag, "getWHNF");
    EXPECT_EQ(SelectorGetFunctionInfo::tag, "getFunctionInfo");
    EXPECT_EQ(SelectorApply::tag, "apply");
    EXPECT_EQ(FileReadRequest::tag, "fileRead");
    EXPECT_EQ(GetEnvRequest::tag, "getEnv");
}

// ---------------------------------------------------------------------------
// Query comparison operators
// ---------------------------------------------------------------------------

TEST(TraceTypes, QueryExprComparison)
{
    SelectorExpr a{"1 + 1", "/"};
    SelectorExpr b{"1 + 1", "/"};
    SelectorExpr c{"2 + 2", "/"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_TRUE(a < c || c < a); // strict weak ordering
}

TEST(TraceTypes, QueryGetAttrComparison)
{
    SelectorGetAttr a{"name", "1"};
    SelectorGetAttr b{"name", "1"};
    SelectorGetAttr c{"other", "1"};
    SelectorGetAttr d{"name", "2"};
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
    Query<SelectorExpr> original{
        .query = {.expr = "1 + 1", .baseDir = "/home/user"},
        .v = 42,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * q = std::get_if<Query<SelectorExpr>>(&*parsed);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->query.expr, "1 + 1");
    EXPECT_EQ(q->v, 42u);
}

TEST(TraceTypes, ParseResultType)
{
    Result<ResultWHNF> original{
        .result = ResultWHNF{"set", WHNFAttrs{{}}},
        .v = 0,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Result<ResultWHNF>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->result.type, "set");
}

TEST(TraceTypes, ParseResultWHNFAttrs)
{
    Result<ResultWHNF> original{
        .result = ResultWHNF{"set", WHNFAttrs{{"a", "b"}}},
        .v = 5,
    };
    auto j = json(original);
    auto parsed = parseTraceEntry(j);
    ASSERT_TRUE(parsed.has_value());
    auto * r = std::get_if<Result<ResultWHNF>>(&*parsed);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->result.type, "set");
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
        Query<SelectorExpr>{
            .query = {.expr = "{ x = 1; }", .baseDir = "/"},
            .v = 0,
        },
        Result<ResultWHNF>{
            .result = ResultWHNF{"set", WHNFAttrs{{}}},
            .v = 0,
        },
        Query<SelectorGetAttr>{
            .query = {.name = "x", .from = "0"},
            .v = 1,
        },
        Result<ResultWHNF>{
            .result = ResultWHNF{"int", WHNFInt{1}},
            .v = 1,
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
        Query<SelectorExpr>{
            .query = {.expr = "{ x = 1; }", .baseDir = "/"},
            .v = 0,
        },
        Result<ResultWHNF>{
            .result = ResultWHNF{"set", WHNFAttrs{{}}},
            .v = 0,
        },
        Query<SelectorGetAttr>{
            .query = {.name = "x", .from = "0"},
            .v = 1,
        },
        Result<ResultWHNF>{
            .result = ResultWHNF{"int", WHNFInt{1}},
            .v = 1,
        },
    };

    auto correlated = correlateTrace(trace);
    ASSERT_EQ(correlated.size(), 4u);

    auto * q0 = std::get_if<CompletedQuery<SelectorExpr>>(&correlated[0]);
    ASSERT_NE(q0, nullptr);
    EXPECT_EQ(q0->resultIndex, 1u);

    auto * q2 = std::get_if<CompletedQuery<SelectorGetAttr>>(&correlated[2]);
    ASSERT_NE(q2, nullptr);
    EXPECT_EQ(q2->resultIndex, 3u);
}

// ---------------------------------------------------------------------------
// SelectorIndex
// ---------------------------------------------------------------------------

TEST(TraceTypes, QueryIndexLookup)
{
    std::vector<TraceEntry> trace = {
        Query<SelectorExpr>{
            .query = {.expr = "42", .baseDir = "/"},
            .v = 0,
        },
        Result<ResultWHNF>{
            .result = ResultWHNF{"int", WHNFInt{0}},
            .v = 0,
        },
        Query<SelectorGetAttr>{
            .query = {.name = "foo", .from = "0"},
            .v = 1,
        },
        Result<ResultWHNF>{
            .result = ResultWHNF{"int", WHNFInt{1}},
            .v = 1,
        },
    };

    SelectorIndex idx(trace);

    auto e1 = idx.lookup(SelectorExpr{"42", "/"});
    ASSERT_TRUE(e1.has_value());
    EXPECT_EQ(e1->queryIndex, 0u);
    EXPECT_EQ(e1->resultIndex, 1u);

    auto e2 = idx.lookup(SelectorGetAttr{"foo", "0"});
    ASSERT_TRUE(e2.has_value());
    EXPECT_EQ(e2->queryIndex, 2u);
    EXPECT_EQ(e2->resultIndex, 3u);

    // Miss
    auto e3 = idx.lookup(SelectorExpr{"999", "/"});
    EXPECT_FALSE(e3.has_value());
}

TEST(TraceTypes, QueryIndexSkipsOrphanedQueries)
{
    std::vector<TraceEntry> trace = {
        Query<SelectorExpr>{
            .query = {.expr = "orphan", .baseDir = "/"},
            .v = 99,
        },
        // No matching result for v=99
    };

    SelectorIndex idx(trace);
    auto e = idx.lookup(SelectorExpr{"orphan", "/"});
    EXPECT_FALSE(e.has_value());
}

} // namespace nix::trace
