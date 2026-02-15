#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "nix/expr/trace-types.hh"

namespace nix::trace {

TEST(TraceTypes, FileReadRequestRoundtrip)
{
    Response<FileReadRequest> original{
        .request = {.absPath = "/foo/bar.nix"},
        .response =
            {.contentHash =
                 Hash::parseAny("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", HashAlgorithm::SHA256)},
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Response<FileReadRequest>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->request.absPath, original.request.absPath);
    ASSERT_EQ(roundtripped->response.contentHash, original.response.contentHash);
}

TEST(TraceTypes, GetEnvRequestRoundtrip)
{
    Response<GetEnvRequest> original{
        .request = {.name = "HOME"},
        .response = {.value = "/home/user"},
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Response<GetEnvRequest>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->request.name, original.request.name);
    ASSERT_EQ(roundtripped->response.value, original.response.value);
}

TEST(TraceTypes, GetEnvRequestRoundtripNullopt)
{
    Response<GetEnvRequest> original{
        .request = {.name = "NONEXISTENT"},
        .response = {.value = std::nullopt},
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Response<GetEnvRequest>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->response.value, std::nullopt);
}

TEST(TraceTypes, QueryExprRoundtrip)
{
    Query<QueryExpr> original{
        .query = {.expr = "1 + 1", .baseDir = "/home/user"},
        .v = 42,
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Query<QueryExpr>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->query.expr, original.query.expr);
    ASSERT_EQ(roundtripped->query.baseDir, original.query.baseDir);
    ASSERT_EQ(roundtripped->v, original.v);
}

TEST(TraceTypes, QueryImportRoundtrip)
{
    Query<QueryImport> original{
        .query = {.path = "/foo/default.nix"},
        .v = 0,
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Query<QueryImport>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->query.path, original.query.path);
    ASSERT_EQ(roundtripped->v, original.v);
}

TEST(TraceTypes, QueryGetAttrRoundtrip)
{
    Query<QueryGetAttr> original{
        .query = {.name = "hello", .from = "parent-hash-5"},
        .v = 6,
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Query<QueryGetAttr>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->query.name, original.query.name);
    ASSERT_EQ(roundtripped->query.from, original.query.from);
    ASSERT_EQ(roundtripped->v, original.v);
}

TEST(TraceTypes, ResultTypeRoundtrip)
{
    Result<ResultType> original{
        .result = {.type = "set"},
        .v = 0,
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Result<ResultType>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->result.type, original.result.type);
    ASSERT_EQ(roundtripped->v, original.v);
}

TEST(TraceTypes, ResultStringRoundtrip)
{
    Result<ResultString> original{
        .result = {.value = "hello world"},
        .v = 1,
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Result<ResultString>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->result.value, original.result.value);
    ASSERT_EQ(roundtripped->v, original.v);
}

TEST(TraceTypes, ResultListOfStringsRoundtrip)
{
    Result<ResultListOfStrings> original{
        .result = {.values = {"a", "b", "c"}},
        .v = 2,
    };
    auto json = nlohmann::json(original);
    auto parsed = parseTraceEntry(json);
    ASSERT_TRUE(parsed.has_value());
    auto * roundtripped = std::get_if<Result<ResultListOfStrings>>(&*parsed);
    ASSERT_NE(roundtripped, nullptr);
    ASSERT_EQ(roundtripped->result.values, original.result.values);
    ASSERT_EQ(roundtripped->v, original.v);
}

TEST(TraceTypes, FullTraceRoundtrip)
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
            .query = {.expr = "(import ./foo.nix).bar", .baseDir = "/home/user/project"},
            .v = 0,
        },
        Response<FileReadRequest>{
            .request = {.absPath = "/home/user/project/foo.nix"},
            .response =
                {.contentHash =
                     Hash::parseAny("sha256-BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB=", HashAlgorithm::SHA256)},
        },
        Result<ResultType>{
            .result = {.type = "set"},
            .v = 0,
        },
        Query<QueryGetAttr>{
            .query = {.name = "bar", .from = "parent-hash-0"},
            .v = 1,
        },
        Result<ResultType>{
            .result = {.type = "set"},
            .v = 1,
        },
        Query<QueryGetAttrNames>{
            .query = {.from = "parent-hash-1"},
            .v = 2,
        },
        Result<ResultListOfStrings>{
            .result = {.values = {"x", "y", "z"}},
            .v = 2,
        },
        Query<QueryGetAttr>{
            .query = {.name = "x", .from = "parent-hash-1"},
            .v = 3,
        },
        Result<ResultType>{
            .result = {.type = "string"},
            .v = 3,
        },
        Query<QueryGetString>{
            .query = {.from = "parent-hash-3"},
            .v = 4,
        },
        Result<ResultString>{
            .result = {.value = "hello"},
            .v = 4,
        },
        Query<QueryGetType>{
            .query = {.from = "parent-hash-1"},
            .v = 5,
        },
        Result<ResultType>{
            .result = {.type = "set"},
            .v = 5,
        },
        Query<QueryGetBool>{
            .query = {.from = "parent-hash-6"},
            .v = 7,
        },
        Result<ResultBool>{
            .result = {.value = true},
            .v = 7,
        },
        Query<QueryGetInt>{
            .query = {.from = "parent-hash-8"},
            .v = 9,
        },
        Result<ResultInt>{
            .result = {.value = 42},
            .v = 9,
        },
        Query<QueryGetPath>{
            .query = {.from = "parent-hash-10"},
            .v = 11,
        },
        Result<ResultPath>{
            .result = {.path = "/nix/store/abc-foo"},
            .v = 11,
        },
        Query<QueryGetListOfStrings>{
            .query = {.from = "parent-hash-12"},
            .v = 13,
        },
        Result<ResultListOfStrings>{
            .result = {.values = {"a", "b"}},
            .v = 13,
        },
        Query<QueryImport>{
            .query = {.path = "/default.nix"},
            .v = 14,
        },
        Result<ResultType>{
            .result = {.type = "lambda"},
            .v = 14,
        },
        Response<GetEnvRequest>{
            .request = {.name = "NONEXISTENT"},
            .response = {.value = std::nullopt},
        },
    };

    // Serialize to JSON array
    nlohmann::json jsonArray = nlohmann::json::array();
    for (const auto & entry : original) {
        std::visit([&](const auto & e) { jsonArray.push_back(nlohmann::json(e)); }, entry);
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
        ASSERT_EQ(original[i].index(), roundtripped[i].index()) << "Type mismatch at index " << i;
    }
}

TEST(TraceTypes, CorrelateTrace)
{
    // Realistic scenario: getAttrNames and getAttr on the same attrset (v=0)
    std::vector<TraceEntry> trace = {
        Query<QueryExpr>{
            .query = {.expr = "{ x = 1; y = 2; }", .baseDir = "/"},
            .v = 0,
        },
        Result<ResultType>{
            .result = {.type = "set"},
            .v = 0,
        },
        Query<QueryGetAttrNames>{
            .query = {.from = "parent-hash-0"},
            .v = 1,
        },
        Result<ResultListOfStrings>{
            .result = {.values = {"x", "y"}},
            .v = 1,
        },
        Query<QueryGetAttr>{
            .query = {.name = "x", .from = "parent-hash-0"},
            .v = 2,
        },
        Result<ResultMaybeType>{
            .result = {.type = "int"},
            .v = 2,
        },
    };

    auto correlated = correlateTrace(trace);
    ASSERT_EQ(correlated.size(), trace.size());

    // QueryExpr{v=0} → Result<ResultType> at index 1
    auto * q0 = std::get_if<CompletedQuery<QueryExpr>>(&correlated[0]);
    ASSERT_NE(q0, nullptr);
    ASSERT_EQ(q0->resultIndex, 1);

    // QueryGetAttrNames{v=1} → Result<ResultListOfStrings> at index 3
    auto * q2 = std::get_if<CompletedQuery<QueryGetAttrNames>>(&correlated[2]);
    ASSERT_NE(q2, nullptr);
    ASSERT_EQ(q2->resultIndex, 3);

    // QueryGetAttr{v=2} → Result<ResultType> at index 5
    auto * q4 = std::get_if<CompletedQuery<QueryGetAttr>>(&correlated[4]);
    ASSERT_NE(q4, nullptr);
    ASSERT_EQ(q4->resultIndex, 5);
}

} // namespace nix::trace
