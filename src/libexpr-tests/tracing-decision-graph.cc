#include <gtest/gtest.h>

#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/file-system.hh"

#include <fstream>

namespace nix {

class TracingDecisionGraphTest : public ::testing::Test
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

    static Hash sha(std::string_view s)
    {
        return hashString(HashAlgorithm::SHA256, s);
    }
};

/* ─────────────────────────────────────────────────────────────────────
   Atomic content-addressed pools
   ───────────────────────────────────────────────────────────────────── */

TEST_F(TracingDecisionGraphTest, AtomInsertAndGetRoundTrip)
{
    TracingDecisionGraph g(dbPath);

    auto reqHash = sha("request-payload-1");
    auto respHash = sha("response-payload-1");
    auto qHash = sha("query-payload-1");
    auto rHash = sha("result-payload-1");

    g.insertRequest(reqHash, "request-payload-1");
    g.insertResponse(respHash, "response-payload-1");
    g.insertQuery(qHash, "query-payload-1");
    g.insertResult(rHash, "result-payload-1");

    EXPECT_EQ(*g.getRequestPayload(reqHash), "request-payload-1");
    EXPECT_EQ(*g.getResponsePayload(respHash), "response-payload-1");
    EXPECT_EQ(*g.getQueryPayload(qHash), "query-payload-1");
    EXPECT_EQ(*g.getResultPayload(rHash), "result-payload-1");
}

TEST_F(TracingDecisionGraphTest, AtomLookupMissesReturnNullopt)
{
    TracingDecisionGraph g(dbPath);
    auto h = sha("never-inserted");
    EXPECT_FALSE(g.getRequestPayload(h).has_value());
    EXPECT_FALSE(g.getResponsePayload(h).has_value());
    EXPECT_FALSE(g.getQueryPayload(h).has_value());
    EXPECT_FALSE(g.getResultPayload(h).has_value());
}

TEST_F(TracingDecisionGraphTest, AtomInsertIsIdempotent)
{
    TracingDecisionGraph g(dbPath);
    auto h = sha("payload");
    g.insertRequest(h, "payload");
    g.insertRequest(h, "payload"); // idempotent, no-op
    g.insertRequest(h, "DIFFERENT"); // INSERT OR IGNORE: keeps the first
    EXPECT_EQ(*g.getRequestPayload(h), "payload");
}

/* ─────────────────────────────────────────────────────────────────────
   Set pools: canonical hashing
   ───────────────────────────────────────────────────────────────────── */

TEST_F(TracingDecisionGraphTest, RequestSetHashIsOrderIndependent)
{
    auto a = sha("a"), b = sha("b"), c = sha("c");
    auto h1 = TracingDecisionGraph::computeRequestSetHash({a, b, c});
    auto h2 = TracingDecisionGraph::computeRequestSetHash({c, a, b});
    auto h3 = TracingDecisionGraph::computeRequestSetHash({b, c, a});
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1, h3);
}

TEST_F(TracingDecisionGraphTest, RequestSetHashDeduplicates)
{
    auto a = sha("a"), b = sha("b");
    auto h1 = TracingDecisionGraph::computeRequestSetHash({a, b});
    auto h2 = TracingDecisionGraph::computeRequestSetHash({a, b, a, b});
    EXPECT_EQ(h1, h2);
}

TEST_F(TracingDecisionGraphTest, EmptySetHashIsConstant)
{
    auto h1 = TracingDecisionGraph::emptySetHash();
    auto h2 = TracingDecisionGraph::computeRequestSetHash({});
    EXPECT_EQ(h1, h2);
}

TEST_F(TracingDecisionGraphTest, FactSetHashIsOrderIndependentAndDedups)
{
    TracingDecisionGraph::Fact f1{sha("q1"), sha("r1")};
    TracingDecisionGraph::Fact f2{sha("q2"), sha("r2")};

    auto h1 = TracingDecisionGraph::computeFactSetHash({f1, f2});
    auto h2 = TracingDecisionGraph::computeFactSetHash({f2, f1});
    auto h3 = TracingDecisionGraph::computeFactSetHash({f1, f2, f1});
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1, h3);
}

TEST_F(TracingDecisionGraphTest, FactWithSameRequestDifferentResponseIsDistinct)
{
    /* The cache distinguishes Responses: (req, resp1) and (req, resp2)
       are two different Facts, even though they share the Request. */
    TracingDecisionGraph::Fact f1{sha("req"), sha("resp1")};
    TracingDecisionGraph::Fact f2{sha("req"), sha("resp2")};
    EXPECT_NE(TracingDecisionGraph::computeFactSetHash({f1}),
              TracingDecisionGraph::computeFactSetHash({f2}));
}

/* ─────────────────────────────────────────────────────────────────────
   Set pools: insert, get, extend
   ───────────────────────────────────────────────────────────────────── */

TEST_F(TracingDecisionGraphTest, RequestSetInsertRoundTrip)
{
    TracingDecisionGraph g(dbPath);
    auto a = sha("a"), b = sha("b"), c = sha("c");
    auto setHash = g.insertRequestSet({a, b, c});

    auto loaded = g.getRequestSet(setHash);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->size(), 3u);
    /* Stored sorted; verify order-independent lookup by checking set membership. */
    std::set<Hash> got(loaded->begin(), loaded->end());
    std::set<Hash> want{a, b, c};
    EXPECT_EQ(got, want);
}

TEST_F(TracingDecisionGraphTest, FactSetInsertRoundTrip)
{
    TracingDecisionGraph g(dbPath);
    TracingDecisionGraph::Fact f1{sha("q1"), sha("r1")};
    TracingDecisionGraph::Fact f2{sha("q2"), sha("r2")};
    auto setHash = g.insertFactSet({f1, f2});

    auto loaded = g.getFactSet(setHash);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->size(), 2u);
    /* Verify both facts came back. */
    EXPECT_TRUE(
        std::find(loaded->begin(), loaded->end(), f1) != loaded->end());
    EXPECT_TRUE(
        std::find(loaded->begin(), loaded->end(), f2) != loaded->end());
}

TEST_F(TracingDecisionGraphTest, EmptySetGetReturnsEmptyVector)
{
    /* The empty set is canonical; no insert needed for it to be queryable. */
    TracingDecisionGraph g(dbPath);
    auto empty = TracingDecisionGraph::emptySetHash();
    auto rs = g.getRequestSet(empty);
    ASSERT_TRUE(rs.has_value());
    EXPECT_TRUE(rs->empty());
    auto fs = g.getFactSet(empty);
    ASSERT_TRUE(fs.has_value());
    EXPECT_TRUE(fs->empty());
}

TEST_F(TracingDecisionGraphTest, SetGetReturnsNulloptForUnknownHash)
{
    TracingDecisionGraph g(dbPath);
    auto fake = sha("never-stored");
    EXPECT_FALSE(g.getRequestSet(fake).has_value());
    EXPECT_FALSE(g.getFactSet(fake).has_value());
}

TEST_F(TracingDecisionGraphTest, ExtendRequestSetProducesCanonicalUnion)
{
    TracingDecisionGraph g(dbPath);
    auto a = sha("a"), b = sha("b"), c = sha("c");
    auto base = g.insertRequestSet({a, b});
    auto extended = g.extendRequestSet(base, {b, c}); // overlap on b is deduped
    auto canonical = TracingDecisionGraph::computeRequestSetHash({a, b, c});
    EXPECT_EQ(extended, canonical);

    auto loaded = g.getRequestSet(extended);
    ASSERT_TRUE(loaded.has_value());
    std::set<Hash> got(loaded->begin(), loaded->end());
    std::set<Hash> want{a, b, c};
    EXPECT_EQ(got, want);
}

TEST_F(TracingDecisionGraphTest, ExtendFactSetFromEmptyMatchesInsert)
{
    TracingDecisionGraph g(dbPath);
    TracingDecisionGraph::Fact f1{sha("q1"), sha("r1")};
    TracingDecisionGraph::Fact f2{sha("q2"), sha("r2")};

    auto direct = g.insertFactSet({f1, f2});
    auto extended = g.extendFactSet(TracingDecisionGraph::emptySetHash(), {f1, f2});
    EXPECT_EQ(direct, extended);
}

TEST_F(TracingDecisionGraphTest, ExtendIsIdempotentWhenAddingExistingMembers)
{
    TracingDecisionGraph g(dbPath);
    auto a = sha("a"), b = sha("b");
    auto base = g.insertRequestSet({a, b});
    auto re_extended = g.extendRequestSet(base, {a, b}); // nothing new
    EXPECT_EQ(base, re_extended);
}

/* ─────────────────────────────────────────────────────────────────────
   Decision graph layer: Asks and Terminals
   ───────────────────────────────────────────────────────────────────── */

TEST_F(TracingDecisionGraphTest, AsksInsertGetRoundTrip)
{
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto factSet = g.insertFactSet({});
    auto requestSet = g.insertRequestSet({sha("a"), sha("b")});

    g.insertAsks(q, factSet, requestSet);

    auto edges = g.getAsks(q, factSet);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0], requestSet);
}

TEST_F(TracingDecisionGraphTest, AsksInsertIsIdempotent)
{
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto factSet = TracingDecisionGraph::emptySetHash();
    auto requestSet = g.insertRequestSet({sha("a")});

    g.insertAsks(q, factSet, requestSet);
    g.insertAsks(q, factSet, requestSet);
    g.insertAsks(q, factSet, requestSet);

    EXPECT_EQ(g.getAsks(q, factSet).size(), 1u);
}

TEST_F(TracingDecisionGraphTest, AsksMultipleOutgoingPerPosition)
{
    /* Pre-Patricia, a position can carry multiple outgoing edges. */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto factSet = TracingDecisionGraph::emptySetHash();
    auto rs1 = g.insertRequestSet({sha("a")});
    auto rs2 = g.insertRequestSet({sha("b")});

    g.insertAsks(q, factSet, rs1);
    g.insertAsks(q, factSet, rs2);

    auto edges = g.getAsks(q, factSet);
    std::set<Hash> got(edges.begin(), edges.end());
    std::set<Hash> want{rs1, rs2};
    EXPECT_EQ(got, want);
}

TEST_F(TracingDecisionGraphTest, AsksRemovePicksTheRightEdge)
{
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto factSet = TracingDecisionGraph::emptySetHash();
    auto rs1 = g.insertRequestSet({sha("a")});
    auto rs2 = g.insertRequestSet({sha("b")});

    g.insertAsks(q, factSet, rs1);
    g.insertAsks(q, factSet, rs2);
    g.removeAsks(q, factSet, rs1);

    auto edges = g.getAsks(q, factSet);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0], rs2);
}

TEST_F(TracingDecisionGraphTest, AsksIsolatedByQ)
{
    /* Two different Qs at the same FactSet don't see each other's edges. */
    TracingDecisionGraph g(dbPath);
    auto q1 = sha("Q1"), q2 = sha("Q2");
    auto factSet = TracingDecisionGraph::emptySetHash();
    auto rs1 = g.insertRequestSet({sha("a")});
    auto rs2 = g.insertRequestSet({sha("b")});

    g.insertAsks(q1, factSet, rs1);
    g.insertAsks(q2, factSet, rs2);

    auto e1 = g.getAsks(q1, factSet);
    auto e2 = g.getAsks(q2, factSet);
    ASSERT_EQ(e1.size(), 1u);
    ASSERT_EQ(e2.size(), 1u);
    EXPECT_EQ(e1[0], rs1);
    EXPECT_EQ(e2[0], rs2);
}

TEST_F(TracingDecisionGraphTest, TerminalInsertGetRoundTrip)
{
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto factSet = TracingDecisionGraph::emptySetHash();
    auto result = sha("R");

    g.insertTerminal(q, factSet, result);

    auto hit = g.getTerminal(q, factSet);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, result);
}

TEST_F(TracingDecisionGraphTest, TerminalMissReturnsNullopt)
{
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto factSet = TracingDecisionGraph::emptySetHash();
    EXPECT_FALSE(g.getTerminal(q, factSet).has_value());
}

TEST_F(TracingDecisionGraphTest, TerminalIsolatedByQ)
{
    TracingDecisionGraph g(dbPath);
    auto q1 = sha("Q1"), q2 = sha("Q2");
    auto factSet = TracingDecisionGraph::emptySetHash();
    g.insertTerminal(q1, factSet, sha("R1"));
    EXPECT_TRUE(g.getTerminal(q1, factSet).has_value());
    EXPECT_FALSE(g.getTerminal(q2, factSet).has_value());
}

TEST_F(TracingDecisionGraphTest, PersistsAcrossReopen)
{
    auto q = sha("Q");
    auto rs = TracingDecisionGraph::computeRequestSetHash({sha("a")});
    {
        TracingDecisionGraph g(dbPath);
        g.insertRequestSet({sha("a")});
        g.insertAsks(q, TracingDecisionGraph::emptySetHash(), rs);
        g.insertTerminal(q, TracingDecisionGraph::emptySetHash(), sha("R"));
    }
    {
        TracingDecisionGraph g(dbPath);
        auto edges = g.getAsks(q, TracingDecisionGraph::emptySetHash());
        ASSERT_EQ(edges.size(), 1u);
        EXPECT_EQ(edges[0], rs);

        auto term = g.getTerminal(q, TracingDecisionGraph::emptySetHash());
        ASSERT_TRUE(term.has_value());
        EXPECT_EQ(*term, sha("R"));
    }
}

} // namespace nix
