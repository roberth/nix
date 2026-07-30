#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/file-system.hh"

namespace nix::trace {

/* Tests for the recursive Selector + SelectorPool + resolve/unresolve
   machinery. Behavior-level only — per-alternative serde correctness is
   subsumed by the datatype-generic mechanisms (NIX_SELECTOR_STR_SERDE
   macro identical per type, detail::fromJsonByTag fold over variant Ts,
   overloaded visits, VariantTagsDistinct static_assert, DECLARE_SELECTOR_RESULT
   trait). Enumerating per-alternative here would be dead weight — see #208
   NON-goals list. */

class TraceTypesTest : public ::testing::Test
{
protected:
    std::filesystem::path tempDir;
    std::filesystem::path dbPath;

    void SetUp() override
    {
        tempDir = createTempDir();
        dbPath = tempDir / "index.sqlite";
    }

    void TearDown() override
    {
        std::filesystem::remove_all(tempDir);
    }
};

/* 1. resolve(unresolve(sel)) == sel — one property test, few
   representative samples across the shape space (leaf / single-step /
   deep chain / plain application / callback application). In-memory
   round-trip only: `pool` already holds the interned parents when
   `resolve` runs, so this exercises the resolve/unresolve algebra —
   the DB path is Test 3 and Test 5. */
TEST_F(TraceTypesTest, ResolveUnresolveRoundTrip)
{
    TracingDecisionGraph g(dbPath);
    auto & pool = g.selectorPool;

    auto leaf = pool.intern(SelectorExpr{"1 + 1", "/base"});
    auto imp  = pool.intern(SelectorImport{"/tmp/x.nix"});
    auto step = pool.intern(SelectorGetAttr{"x", imp});
    auto list = pool.intern(SelectorGetListElem{5, imp});
    auto deep = pool.intern(SelectorGetAttr{"deep", list});
    auto app  = pool.intern(SelectorApply{imp});
    auto cba  = pool.intern(SelectorCallbackApply{"abc123", imp});

    for (auto sel : std::vector{leaf, step, deep, app, cba}) {
        auto raw = unresolve(sel->node);
        auto restored = resolve(raw, pool);
        ASSERT_TRUE(restored.has_value());
        EXPECT_EQ((*restored)->cachedHash, sel->cachedHash);
    }
}

/* 2. SelectorPool::intern is idempotent — same content, same ref.
   Leaves and step selectors go through the same hash-keyed cache;
   include one of each to guard against a future intern-path split. */
TEST_F(TraceTypesTest, InternIdempotence)
{
    TracingDecisionGraph g(dbPath);
    auto & pool = g.selectorPool;

    auto leaf1 = pool.intern(SelectorImport{"/x.nix"});
    auto leaf2 = pool.intern(SelectorImport{"/x.nix"});
    EXPECT_EQ(leaf1.get_ptr().get(), leaf2.get_ptr().get());

    auto step1 = pool.intern(SelectorGetAttr{"y", leaf1});
    auto step2 = pool.intern(SelectorGetAttr{"y", leaf1});
    EXPECT_EQ(step1.get_ptr().get(), step2.get_ptr().get());
}

/* 3. DB-facade cross-pool findByHash — a Selector interned into one
   pool is reachable from a fresh pool bound to the same DB. Proves
   the unresolve → CBOR → DB → CBOR → resolve → intern pipeline. */
TEST_F(TraceTypesTest, DbFacadeCrossPoolFind)
{
    Hash h{HashAlgorithm::SHA256};
    {
        TracingDecisionGraph g(dbPath);
        auto sel = g.selectorPool.intern(SelectorImport{"/target.nix"});
        h = sel->cachedHash;
    }
    TracingDecisionGraph g2(dbPath);
    auto found = g2.selectorPool.find(h);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ((*found)->cachedHash, h);
    auto & node = std::get<SelectorImport>((*found)->node);
    EXPECT_EQ(node.path, "/target.nix");
}

/* 4. SelectorPool::findByHex — nullopt on malformed input (parse
   failure) and on valid-hex-not-in-DB (memory miss + DB miss). */
TEST_F(TraceTypesTest, FindByHexMissAndMalformed)
{
    TracingDecisionGraph g(dbPath);
    auto & pool = g.selectorPool;

    // Malformed hex — parse fails.
    EXPECT_FALSE(pool.findByHex("not-a-hex-string").has_value());
    EXPECT_FALSE(pool.findByHex("").has_value());
    EXPECT_FALSE(pool.findByHex("zzz").has_value());

    // Valid-shape hex that no Selector has produced — memory miss + DB miss.
    auto neverInterned = hashString(HashAlgorithm::SHA256, "never-interned");
    EXPECT_FALSE(pool.findByHex(neverInterned.to_string(HashFormat::Base16, false)).has_value());
}

/* 5. Deep-chain DB reconstruction — depth-N Selector's find recurses
   through N DB reads to rebuild parents. Verifies both the recursion
   and the resolved chain matches the interned shape. */
TEST_F(TraceTypesTest, DeepChainDbReconstruction)
{
    Hash outerHash{HashAlgorithm::SHA256};
    {
        TracingDecisionGraph g(dbPath);
        auto & pool = g.selectorPool;
        auto imp  = pool.intern(SelectorImport{"/base.nix"});
        auto app  = pool.intern(SelectorApply{imp});
        auto attr = pool.intern(SelectorGetAttr{"foo", app});
        auto list = pool.intern(SelectorGetListElem{0, attr});
        outerHash = list->cachedHash;
    }
    TracingDecisionGraph g2(dbPath);
    auto found = g2.selectorPool.find(outerHash);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ((*found)->cachedHash, outerHash);
    auto & outerNode = std::get<SelectorGetListElem>((*found)->node);
    EXPECT_EQ(outerNode.index, 0u);
    auto & attrNode = std::get<SelectorGetAttr>(outerNode.parent->node);
    EXPECT_EQ(attrNode.name, "foo");
    auto & appNode = std::get<SelectorApply>(attrNode.parent->node);
    auto & impNode = std::get<SelectorImport>(appNode.parent->node);
    EXPECT_EQ(impNode.path, "/base.nix");
}

/* 6. resolveFromJson on malformed payload returns nullopt without
   throwing — walker dispatch treats unparseable request payloads as
   misses, not fatal errors. */
TEST_F(TraceTypesTest, ResolveFromJsonMalformed)
{
    TracingDecisionGraph g(dbPath);
    auto & pool = g.selectorPool;

    EXPECT_FALSE(resolveFromJson(nlohmann::json{{"name", "x"}}, pool).has_value());
    EXPECT_FALSE(resolveFromJson(nlohmann::json{{"tag", "unknown"}}, pool).has_value());
    EXPECT_FALSE(resolveFromJson(nlohmann::json::array({1, 2, 3}), pool).has_value());
    EXPECT_FALSE(resolveFromJson(nlohmann::json{{"tag", "getAttr"}}, pool).has_value());
}

} // namespace nix::trace
