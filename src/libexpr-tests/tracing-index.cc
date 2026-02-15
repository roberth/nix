#include <gtest/gtest.h>
#include <filesystem>

#include "nix/expr/tracing-index.hh"
#include "nix/util/file-system.hh"

namespace nix {

class TracingIndexTest : public ::testing::Test
{
protected:
    std::filesystem::path testDir;
    std::filesystem::path dbPath;

    void SetUp() override
    {
        testDir = createTempDir();
        dbPath = testDir / "test-index.sqlite";
    }

    void TearDown() override
    {
        std::filesystem::remove_all(testDir);
    }
};

TEST_F(TracingIndexTest, CreateDatabase)
{
    TracingIndex index(dbPath);
    EXPECT_TRUE(std::filesystem::exists(dbPath));
}

TEST_F(TracingIndexTest, InsertQueryAndShortcut)
{
    TracingIndex index(dbPath);

    // Create a simple query
    trace::QueryExpr query{.expr = "1 + 1", .baseDir = "/home/user"};
    auto queryHash = TracingIndex::computeQueryHash(query);
    std::string payload = R"({"expr": "1 + 1", "baseDir": "/home/user"})";

    // Insert as a root query (no afterHash)
    auto nodeHash = index.insertQuery(std::nullopt, queryHash, payload);

    // Verify we can look up via shortcut
    auto shortcuts = index.selectShortcuts(queryHash);
    ASSERT_EQ(shortcuts.size(), 1);
    EXPECT_EQ(shortcuts[0].nodeHash, nodeHash);

    // Verify we can retrieve the query node
    auto queryNode = index.getQuery(nodeHash);
    ASSERT_TRUE(queryNode.has_value());
    EXPECT_EQ(queryNode->nodeHash, nodeHash);
    EXPECT_EQ(queryNode->queryHash, queryHash);
    EXPECT_FALSE(queryNode->afterHash.has_value());
}

TEST_F(TracingIndexTest, InsertResponse)
{
    TracingIndex index(dbPath);

    // First insert a query node
    trace::QueryImport query{.path = "/default.nix"};
    auto queryHash = TracingIndex::computeQueryHash(query);
    auto queryNodeHash = index.insertQuery(std::nullopt, queryHash, "{}");

    // Insert a response after the query
    std::string request = R"({"absPath": "/default.nix"})";
    std::string response = R"({"contentHash": "sha256-abc..."})";
    auto responseNodeHash = index.insertResponse(queryNodeHash, request, response);

    // Verify we can retrieve it
    auto responseNode = index.getResponse(responseNodeHash);
    ASSERT_TRUE(responseNode.has_value());
    EXPECT_EQ(responseNode->afterHash, queryNodeHash);
    EXPECT_EQ(responseNode->request, request);
    EXPECT_EQ(responseNode->response, response);

    // Verify it shows up as a child of the query
    auto children = index.selectChildResponses(queryNodeHash);
    ASSERT_EQ(children.size(), 1);
    EXPECT_EQ(children[0].nodeHash, responseNodeHash);
}

TEST_F(TracingIndexTest, InsertResult)
{
    TracingIndex index(dbPath);

    // Insert query -> response -> result chain
    trace::QueryExpr query{.expr = "import ./foo.nix", .baseDir = "/"};
    auto queryHash = TracingIndex::computeQueryHash(query);
    auto queryNodeHash = index.insertQuery(std::nullopt, queryHash, "{}");

    std::string request = R"({"absPath": "/foo.nix"})";
    std::string response = R"({"contentHash": "sha256-xyz..."})";
    auto responseNodeHash = index.insertResponse(queryNodeHash, request, response);

    std::string resultPayload = R"({"type": "set"})";
    auto resultNodeHash = index.insertResult(responseNodeHash, resultPayload);

    // Verify result retrieval
    auto resultNode = index.getResult(resultNodeHash);
    ASSERT_TRUE(resultNode.has_value());
    EXPECT_EQ(resultNode->afterHash, responseNodeHash);
    EXPECT_EQ(resultNode->payload, resultPayload);

    // Verify it shows up as a child of the response
    auto children = index.selectChildResults(responseNodeHash);
    ASSERT_EQ(children.size(), 1);
    EXPECT_EQ(children[0].nodeHash, resultNodeHash);
}

TEST_F(TracingIndexTest, StructuralLookup)
{
    TracingIndex index(dbPath);

    // Create a root query and its result
    trace::QueryExpr rootQuery{.expr = "{ foo = 1; }", .baseDir = "/"};
    auto rootQueryHash = TracingIndex::computeQueryHash(rootQuery);
    auto rootQueryNodeHash = index.insertQuery(std::nullopt, rootQueryHash, "{}");

    std::string resultPayload = R"({"type": "set"})";
    auto resultNodeHash = index.insertResult(rootQueryNodeHash, resultPayload);

    // Insert a getAttr query with structural parent pointing to the result
    // Use root query hash as "from" for Merkle identity
    auto rootQueryHashStr = rootQueryHash.to_string(HashFormat::Base16, false);
    trace::QueryGetAttr getAttrQuery{.name = "foo", .from = rootQueryHashStr};
    auto getAttrHash = TracingIndex::computeQueryHash(getAttrQuery);
    auto getAttrNodeHash = index.insertQuery(
        resultNodeHash, // temporal: after the result
        getAttrHash,
        R"({"name": "foo", "from": ")" + rootQueryHashStr + R"("})",
        resultNodeHash // structural: the attrset we're accessing
    );

    // Verify structural lookup works
    auto found = index.selectStructuralChildren(resultNodeHash, getAttrHash);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].nodeHash, getAttrNodeHash);
    EXPECT_EQ(found[0].structuralParent, resultNodeHash);
}

TEST_F(TracingIndexTest, SelectDependencies)
{
    TracingIndex index(dbPath);

    // Build a chain: Query -> Response1 -> Response2 -> Result -> Query2
    trace::QueryExpr query1{.expr = "import ./a.nix", .baseDir = "/"};
    auto query1Hash = TracingIndex::computeQueryHash(query1);
    auto query1NodeHash = index.insertQuery(std::nullopt, query1Hash, "{}");

    auto resp1NodeHash = index.insertResponse(query1NodeHash, R"({"file": "a.nix"})", R"({"hash": "aaa"})");
    auto resp2NodeHash = index.insertResponse(resp1NodeHash, R"({"file": "b.nix"})", R"({"hash": "bbb"})");
    auto result1NodeHash = index.insertResult(resp2NodeHash, R"({"type": "set"})");

    auto query1HashStr = query1Hash.to_string(HashFormat::Base16, false);
    trace::QueryGetAttr query2{.name = "x", .from = query1HashStr};
    auto query2Hash = TracingIndex::computeQueryHash(query2);
    auto query2NodeHash = index.insertQuery(result1NodeHash, query2Hash, "{}", result1NodeHash);

    // Collect dependencies for query2
    auto deps = index.selectDependencies(query2NodeHash);

    // Should find resp1 and resp2 in root-to-query order
    ASSERT_EQ(deps.size(), 2);
    EXPECT_EQ(deps[0].nodeHash, resp1NodeHash);
    EXPECT_EQ(deps[1].nodeHash, resp2NodeHash);
}

TEST_F(TracingIndexTest, IdempotentInserts)
{
    TracingIndex index(dbPath);

    trace::QueryExpr query{.expr = "1 + 1", .baseDir = "/"};
    auto queryHash = TracingIndex::computeQueryHash(query);
    std::string payload = "{}";

    // Insert the same query twice
    auto nodeHash1 = index.insertQuery(std::nullopt, queryHash, payload);
    auto nodeHash2 = index.insertQuery(std::nullopt, queryHash, payload);

    // Should get the same nodeHash
    EXPECT_EQ(nodeHash1, nodeHash2);

    // Shortcuts should not duplicate
    auto shortcuts = index.selectShortcuts(queryHash);
    EXPECT_EQ(shortcuts.size(), 1);
}

TEST_F(TracingIndexTest, MultipleShortcutsForSameQuery)
{
    TracingIndex index(dbPath);

    // Same semantic query but different temporal positions
    // Use empty string as "from" for testing (simulates root-level attr access)
    trace::QueryGetAttr query{.name = "foo", .from = ""};
    auto queryHash = TracingIndex::computeQueryHash(query);

    // First occurrence: root query
    auto nodeHash1 = index.insertQuery(std::nullopt, queryHash, "{}");

    // Create a result to use as afterHash for the second occurrence
    auto resultNodeHash = index.insertResult(nodeHash1, R"({"type": "set"})");

    // Second occurrence: after a result (different temporal position)
    auto nodeHash2 = index.insertQuery(resultNodeHash, queryHash, "{}", resultNodeHash);

    // NodeHashes should differ (different afterHash)
    EXPECT_NE(nodeHash1, nodeHash2);

    // Both should be in shortcuts
    auto shortcuts = index.selectShortcuts(queryHash);
    EXPECT_EQ(shortcuts.size(), 2);
}

TEST_F(TracingIndexTest, QueryPayloadStorage)
{
    TracingIndex index(dbPath);

    trace::QueryExpr query{.expr = "let x = 1; in x", .baseDir = "/home"};
    auto queryHash = TracingIndex::computeQueryHash(query);
    std::string payload = R"({"expr": "let x = 1; in x", "baseDir": "/home"})";

    index.insertQuery(std::nullopt, queryHash, payload);

    // Retrieve payload
    auto retrieved = index.getQueryPayload(queryHash);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(*retrieved, payload);
}

} // namespace nix
