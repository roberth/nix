#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "nix/expr/trace-file.hh"
#include "nix/expr/trace-types.hh"

namespace nix {

namespace fs = std::filesystem;
using json = nlohmann::json;

class TraceFileTest : public ::testing::Test
{
protected:
    fs::path tmpDir;
    fs::path tracePath;

    void SetUp() override
    {
        tmpDir = fs::temp_directory_path() / ("nix-trace-test-" + std::to_string(getpid()));
        fs::create_directories(tmpDir);
        tracePath = tmpDir / "test-trace.json";
    }

    void TearDown() override
    {
        fs::remove_all(tmpDir);
    }

    json readTraceFile()
    {
        std::ifstream f(tracePath);
        return json::parse(f);
    }
};

TEST_F(TraceFileTest, WritesValidJsonArray)
{
    {
        TraceFile tf(tracePath);
        tf.logQuery(trace::QueryExpr{"1 + 1", "/"});
        tf.logResult(uint64_t(0), trace::ResultType{"int"});
    }

    auto j = readTraceFile();
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 2u);
}

TEST_F(TraceFileTest, QueryAndResultLinkedByHandle)
{
    {
        TraceFile tf(tracePath);
        auto v = tf.logQuery(trace::QueryExpr{"42", "/"});
        tf.logResult(v, trace::ResultType{"int"});
    }

    auto j = readTraceFile();
    ASSERT_EQ(j.size(), 2u);
    auto queryV = j[0].at("v").get<uint64_t>();
    auto resultV = j[1].at("v").get<uint64_t>();
    EXPECT_EQ(queryV, resultV);
}

TEST_F(TraceFileTest, AllocatesIncreasingHandles)
{
    {
        TraceFile tf(tracePath);
        auto v1 = tf.logQuery(trace::QueryExpr{"1", "/"});
        auto v2 = tf.logQuery(trace::QueryImport{"/tmp/test.nix"});
        EXPECT_EQ(v1, 0u);
        EXPECT_EQ(v2, 1u);
    }
}

TEST_F(TraceFileTest, EnvResponseLogged)
{
    {
        TraceFile tf(tracePath);
        tf.logEnvResponse(
            trace::Response<trace::GetEnvRequest>{
                .request = {.name = "HOME"},
                .response = {.value = "/home/user"},
            });
    }

    auto j = readTraceFile();
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0].at("request").at("name"), "HOME");
    EXPECT_EQ(j[0].at("response").at("value"), "/home/user");
}

TEST_F(TraceFileTest, EmptyTraceFileIsValidJson)
{
    {
        TraceFile tf(tracePath);
        // No entries logged
    }

    auto j = readTraceFile();
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);
}

} // namespace nix
