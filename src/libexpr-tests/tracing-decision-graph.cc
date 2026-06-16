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
    auto qHash = sha("query-payload-1");
    auto rHash = sha("result-payload-1");

    g.insertRequest(reqHash, "request-payload-1");
    g.insertQuery(qHash, "query-payload-1");
    g.insertResult(rHash, "result-payload-1");

    EXPECT_EQ(*g.getRequestPayload(reqHash), "request-payload-1");
    EXPECT_EQ(*g.getQueryPayload(qHash), "query-payload-1");
    EXPECT_EQ(*g.getResultPayload(rHash), "result-payload-1");
}

TEST_F(TracingDecisionGraphTest, AtomLookupMissesReturnNullopt)
{
    TracingDecisionGraph g(dbPath);
    auto h = sha("never-inserted");
    EXPECT_FALSE(g.getRequestPayload(h).has_value());
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

/* ─────────────────────────────────────────────────────────────────────
   record() and walk() — end-to-end Phase 1 behaviour
   ───────────────────────────────────────────────────────────────────── */

TEST_F(TracingDecisionGraphTest, RecordThenWalkSimpleHit)
{
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto req1 = sha("req1"), resp1 = sha("resp1");
    auto req2 = sha("req2"), resp2 = sha("resp2");
    auto result = sha("R");

    /* Build the factSet the recorder would have observed. */
    auto factSetHash = g.insertFactSet({
        {req1, resp1},
        {req2, resp2},
    });

    g.record(q, factSetHash, result);

    /* Walk with a dispatch that returns the same Responses the
       recorder saw — we should hit. */
    auto hit = g.walk(q, [&](const Hash & req) {
        if (req == req1) return resp1;
        if (req == req2) return resp2;
        ADD_FAILURE() << "unexpected dispatch for " << req.to_string(HashFormat::Base16, false);
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, result);
}

TEST_F(TracingDecisionGraphTest, WalkMissesWhenDispatchReturnsDifferentResponse)
{
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto req = sha("req"), resp = sha("resp"), wrongResp = sha("wrong");
    auto result = sha("R");

    auto factSetHash = g.insertFactSet({{req, resp}});
    g.record(q, factSetHash, result);

    /* Replay with a different Response: should miss. */
    auto miss = g.walk(q, [&](const Hash &) { return wrongResp; });
    EXPECT_FALSE(miss.has_value());
}

TEST_F(TracingDecisionGraphTest, WalkMissesOnEmptyGraph)
{
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto miss = g.walk(q, [](const Hash &) { return Hash(HashAlgorithm::SHA256); });
    EXPECT_FALSE(miss.has_value());
}

TEST_F(TracingDecisionGraphTest, TwoRec_TwoFactsEach_DivergentSecond)
{
    /* Two recordings, each with two Facts. Both share the FIRST
       canonical Fact (same Request, same Response). They diverge
       at the second canonical step. */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");

    /* Use names chosen so that "AAA" < "BBB" < "CCC" by SHA-256
       lexicographic order. */
    auto reqA = sha("a-shared"), respA = sha("a-resp");
    auto reqB = sha("b-only-in-fs1"), respB = sha("b-resp");
    auto reqC = sha("c-only-in-fs2"), respC = sha("c-resp");
    auto r1 = sha("R1"), r2 = sha("R2");

    auto fs1 = g.insertFactSet({{reqA, respA}, {reqB, respB}});
    auto fs2 = g.insertFactSet({{reqA, respA}, {reqC, respC}});
    g.record(q, fs1, r1);
    g.record(q, fs2, r2);

    /* Replay where reqC's response doesn't match fs2's stored
       response — fs2's path is unreachable; the walk must take
       fs1's path. */
    auto hit1 = g.walk(q, [&](const Hash & req) {
        if (req == reqA) return respA;
        if (req == reqB) return respB;
        if (req == reqC) return sha("bogus-c-resp");
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit1.has_value());
    EXPECT_EQ(*hit1, r1);
}

TEST_F(TracingDecisionGraphTest, DivergentResponses_Minimal)
{
    /* Two recordings of Q where Q's first asked Request gets two
       different Responses across recordings. Smallest possible
       version to expose any logic bug.  */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto reqA = sha("reqA");
    auto v1 = sha("v1");
    auto v2 = sha("v2");
    auto r1 = sha("R1"), r2 = sha("R2");

    auto fs1 = g.insertFactSet({{reqA, v1}});
    auto fs2 = g.insertFactSet({{reqA, v2}});
    g.record(q, fs1, r1);
    g.record(q, fs2, r2);

    auto hit1 = g.walk(q, [&](const Hash &) { return v1; });
    ASSERT_TRUE(hit1.has_value());
    EXPECT_EQ(*hit1, r1);

    auto hit2 = g.walk(q, [&](const Hash &) { return v2; });
    ASSERT_TRUE(hit2.has_value());
    EXPECT_EQ(*hit2, r2);
}

TEST_F(TracingDecisionGraphTest, DivergentResponses_OnlyOneRecording)
{
    /* Sanity: with just recording 1, walking with v1 world should hit. */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto reqA = sha("read a.nix");
    auto contentV1 = sha("v1");
    auto reqB = sha("read b.nix"), respB = sha("b-bytes");

    auto factSet1 = g.insertFactSet({
        {reqA, contentV1},
        {reqB, respB},
    });
    auto r1 = sha("Result-1");
    g.record(q, factSet1, r1);

    auto hit = g.walk(q, [&](const Hash & req) {
        if (req == reqA) return contentV1;
        if (req == reqB) return respB;
        ADD_FAILURE() << "unexpected dispatch";
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, r1);
}

TEST_F(TracingDecisionGraphTest, IdempotentRecord)
{
    /* Recording the same (Q, factSet, result) twice is a no-op:
       INSERT OR IGNORE absorbs the duplicates at every step. */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto factSet = g.insertFactSet({{sha("r1"), sha("v1")}, {sha("r2"), sha("v2")}});
    auto result = sha("R");

    g.record(q, factSet, result);
    g.record(q, factSet, result);

    auto hit = g.walk(q, [&](const Hash & req) {
        if (req == sha("r1")) return sha("v1");
        if (req == sha("r2")) return sha("v2");
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, result);
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

/* ─────────────────────────────────────────────────────────────────────
   Scale / stress: many Qs × many recordings, verify isolation,
   measure storage, exercise walk under load.
   ───────────────────────────────────────────────────────────────────── */

TEST_F(TracingDecisionGraphTest, ManyQueriesAreIsolated)
{
    /* Record N distinct Queries, each with a different FactSet and
       Result. Verify each can be replayed back to its own Result
       and that no cross-contamination happens. */
    TracingDecisionGraph g(dbPath);
    constexpr size_t N = 100;

    std::vector<Hash> qs, results;
    std::vector<TracingDecisionGraph::SetHash> factSets;
    for (size_t i = 0; i < N; ++i) {
        auto q = sha("Q-" + std::to_string(i));
        auto req = sha("req-" + std::to_string(i));
        auto resp = sha("resp-" + std::to_string(i));
        auto fs = g.insertFactSet({{req, resp}});
        auto r = sha("R-" + std::to_string(i));

        g.record(q, fs, r);
        qs.push_back(q);
        factSets.push_back(fs);
        results.push_back(r);
    }

    /* Replay each Q with the *correct* dispatch — should hit its
       own Result. */
    for (size_t i = 0; i < N; ++i) {
        auto hit = g.walk(qs[i], [&](const Hash & req) {
            EXPECT_EQ(req, sha("req-" + std::to_string(i)))
                << "dispatch invoked with unexpected Request for Q " << i;
            return sha("resp-" + std::to_string(i));
        });
        ASSERT_TRUE(hit.has_value()) << "Q " << i << " missed";
        EXPECT_EQ(*hit, results[i]) << "Q " << i << " hit wrong Result";
    }

    /* Replay Q[0] with Q[1]'s response — should miss (wrong
       Response → wrong next FactSet hash). */
    auto wrongMiss = g.walk(qs[0], [&](const Hash &) {
        return sha("resp-1");
    });
    EXPECT_FALSE(wrongMiss.has_value())
        << "Q[0] should miss when given Q[1]'s response, not return stale Q[1] result";
}

TEST_F(TracingDecisionGraphTest, ManyRecordingsSameQDeepRecordings)
{
    /* One Q, many recordings with varying-content FactSets, each
       of moderate depth (10 Facts). Stress-tests record() and walk()
       at realistic per-Q breadth. */
    TracingDecisionGraph g(dbPath);
    constexpr size_t N_RECORDINGS = 50;
    constexpr size_t FACTS_PER = 10;

    auto q = sha("Q");
    std::vector<std::vector<TracingDecisionGraph::Fact>> allFactSets;
    std::vector<Hash> allResults;

    for (size_t i = 0; i < N_RECORDINGS; ++i) {
        std::vector<TracingDecisionGraph::Fact> facts;
        for (size_t j = 0; j < FACTS_PER; ++j) {
            auto req = sha("rec-" + std::to_string(i) + "-req-" + std::to_string(j));
            auto resp = sha("rec-" + std::to_string(i) + "-resp-" + std::to_string(j));
            facts.push_back({req, resp});
        }
        auto fs = g.insertFactSet(facts);
        auto r = sha("R-" + std::to_string(i));
        g.record(q, fs, r);
        allFactSets.push_back(std::move(facts));
        allResults.push_back(r);
    }

    /* Every recording should be replayable with its own dispatch
       table. */
    for (size_t i = 0; i < N_RECORDINGS; ++i) {
        std::map<Hash, Hash> table;
        for (const auto & f : allFactSets[i])
            table.emplace(f.request, f.response);

        auto hit = g.walk(q, [&](const Hash & req) -> Hash {
            auto it = table.find(req);
            if (it == table.end()) {
                /* This is the wrong-branch case: walk speculated an
                   edge from another recording. Return a deliberately
                   wrong response so the candidate FactSet doesn't
                   match storage and we fall through to the right
                   branch. */
                return sha("bogus-" + req.to_string(HashFormat::Base16, false).substr(0, 8));
            }
            return it->second;
        });
        ASSERT_TRUE(hit.has_value()) << "recording " << i << " missed";
        EXPECT_EQ(*hit, allResults[i]) << "recording " << i << " hit wrong Result";
    }
}

TEST_F(TracingDecisionGraphTest, DeepRecordingPersistsAcrossReopen)
{
    /* A 50-fact deep recording survives a database close/reopen. */
    constexpr size_t DEPTH = 50;
    auto q = sha("deep-Q");
    auto r = sha("deep-R");
    {
        TracingDecisionGraph g(dbPath);
        std::vector<TracingDecisionGraph::Fact> facts;
        for (size_t i = 0; i < DEPTH; ++i)
            facts.push_back({sha("dr-" + std::to_string(i)), sha("dv-" + std::to_string(i))});
        auto fs = g.insertFactSet(facts);
        g.record(q, fs, r);
    }
    /* Reopen and walk; the recording should be replayable. */
    TracingDecisionGraph g(dbPath);
    auto hit = g.walk(q, [](const Hash & req) {
        /* Use the same naming convention; extract i from the seeded
           Request name doesn't work without round-tripping. Instead
           build a static dispatch table on demand. */
        for (size_t i = 0; i < 1000; ++i) {
            if (req == hashString(HashAlgorithm::SHA256, "dr-" + std::to_string(i)))
                return hashString(HashAlgorithm::SHA256, "dv-" + std::to_string(i));
        }
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, r);
}

/* ─────────────────────────────────────────────────────────────────────
   End-to-end simulation: mimic the on_event recorder + walk replayer
   pattern that integration will eventually use.
   ───────────────────────────────────────────────────────────────────── */

namespace {

/* A minimal recorder that mimics on_event semantics: one global
   factSet that grows as Responses arrive, and a record() call when
   a Result is produced. */
struct OnEventRecorder
{
    TracingDecisionGraph & graph;
    std::vector<TracingDecisionGraph::Fact> factSet; // mutable, sorted on demand

    void onResponse(const Hash & request, const Hash & response)
    {
        factSet.push_back({request, response});
    }

    void onResult(const Hash & queryHash, const Hash & resultHash)
    {
        auto fsHash = graph.insertFactSet(factSet);
        graph.record(queryHash, fsHash, resultHash);
    }
};

} // namespace

TEST_F(TracingDecisionGraphTest, EndToEndOnEventThenWalk)
{
    /* A realistic flow: simulate the box doing two evaluations of
       the same Query, one in each of two "worlds" (different
       Responses for the second Request). Then replay each world
       and verify the correct Result. */
    TracingDecisionGraph g(dbPath);
    auto q = sha("import_a_dot_nix");
    auto reqA = sha("read_a.nix");
    auto reqB = sha("read_b.nix");
    auto contentA_v1 = sha("a_v1");
    auto contentA_v2 = sha("a_v2");
    auto contentB = sha("b_bytes");
    auto result_v1 = sha("evaluated_with_v1");
    auto result_v2 = sha("evaluated_with_v2");

    /* Session 1: box reads a.nix→v1, then b.nix→b_bytes, produces result_v1. */
    {
        OnEventRecorder rec{g, {}};
        rec.onResponse(reqA, contentA_v1);
        rec.onResponse(reqB, contentB);
        rec.onResult(q, result_v1);
    }
    /* Session 2: box reads a.nix→v2 (new content), then b.nix→b_bytes, produces result_v2. */
    {
        OnEventRecorder rec{g, {}};
        rec.onResponse(reqA, contentA_v2);
        rec.onResponse(reqB, contentB);
        rec.onResult(q, result_v2);
    }

    /* Replay in v1 world: dispatch returns contentA_v1 for reqA. */
    auto hit_v1 = g.walk(q, [&](const Hash & req) {
        if (req == reqA) return contentA_v1;
        if (req == reqB) return contentB;
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit_v1.has_value());
    EXPECT_EQ(*hit_v1, result_v1);

    /* Replay in v2 world: dispatch returns contentA_v2 for reqA. */
    auto hit_v2 = g.walk(q, [&](const Hash & req) {
        if (req == reqA) return contentA_v2;
        if (req == reqB) return contentB;
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit_v2.has_value());
    EXPECT_EQ(*hit_v2, result_v2);

    /* Replay in a third world (a.nix is some unknown content):
       should miss because no recording covers this scenario. */
    auto miss = g.walk(q, [&](const Hash & req) {
        if (req == reqA) return sha("a_unknown_v3");
        if (req == reqB) return contentB;
        return Hash(HashAlgorithm::SHA256);
    });
    EXPECT_FALSE(miss.has_value());
}

TEST_F(TracingDecisionGraphTest, EndToEndNestedQueries)
{
    /* Simulates nested Queries within one session: outer Q invokes
       sub-Q1 and sub-Q2 as recursive d=0 evaluations.

       In our global-factSet model, sub-Q's recorded factSet
       includes the parent's prior Facts (the over-approximation
       documented in the data model). Then in a different parent
       context, the same sub-Q would record with different
       parent-context Facts. Both recordings should still be
       replayable when their respective contexts are provided. */
    TracingDecisionGraph g(dbPath);
    auto outerQ = sha("outer-Q");
    auto innerQ = sha("inner-Q");
    auto outerReq = sha("outer-Req");
    auto innerReq = sha("inner-Req");
    auto outerResp = sha("outer-Resp");
    auto innerResp = sha("inner-Resp");

    /* Session 1: outer asks outerReq, then inner asks innerReq,
       inner produces innerResult, outer produces outerResult. */
    OnEventRecorder rec{g, {}};
    rec.onResponse(outerReq, outerResp);
    /* At this point inner-Q starts its evaluation. The recorder
       continues to accumulate facts; in our model the inner's
       recorded factSet includes outer's prior Facts. */
    rec.onResponse(innerReq, innerResp);
    auto innerResult = sha("inner-Result");
    auto outerResult = sha("outer-Result");
    rec.onResult(innerQ, innerResult);
    rec.onResult(outerQ, outerResult);

    /* Replay inner-Q: must provide both outerReq's and innerReq's
       responses because inner's recorded precondition includes
       both. */
    auto innerHit = g.walk(innerQ, [&](const Hash & req) {
        if (req == outerReq) return outerResp;
        if (req == innerReq) return innerResp;
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(innerHit.has_value());
    EXPECT_EQ(*innerHit, innerResult);

    /* Replay outer-Q similarly. */
    auto outerHit = g.walk(outerQ, [&](const Hash & req) {
        if (req == outerReq) return outerResp;
        if (req == innerReq) return innerResp;
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(outerHit.has_value());
    EXPECT_EQ(*outerHit, outerResult);
}

/* ─────────────────────────────────────────────────────────────────────
   Phase 1 design tests
   ─────────────────────────────────────────────────────────────────────
   Specification-by-test for the faithful Phase 1 record() at
   doc/tracing-decision-graph-data-model.md lines 386–415.

   These tests will FAIL against the current per-fact singleton-edge
   strawman in record() and provide the green target for the rewrite.

   The semantic spec, briefly:
     - First-time recording of Q at (∅): one Asks edge whose
       RequestSet is the *whole* remaining set, not one edge per
       fact.
     - Subsequent recording whose remaining is a strict superset of
       an existing edge's useful dispatch: follow that edge to its
       target FactSet, then continue recording from there. No
       duplicate Asks row.
     - Subsequent recording whose remaining partially overlaps an
       existing edge's useful dispatch: Patricia-split the existing
       edge so the shared prefix becomes a single edge to a fresh
       intermediate FactSet, and both the old and new tails fan out
       from that intermediate. The split inserts at most one fresh
       RequestSet node (the shared prefix); tail edges keep their
       original RS references.
     - RequestSet pool is content-addressed and shared across Qs.
   ───────────────────────────────────────────────────────────────────── */

TEST_F(TracingDecisionGraphTest, Phase1_RecordEmitsSingleEdgeForFirstRecording)
{
    /* First-time recording of Q with N>1 facts: the design's
       record() emits ONE Asks edge from (Q, ∅) whose RequestSet
       contains all N requests, not N singleton edges. */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto req1 = sha("req1"), resp1 = sha("resp1");
    auto req2 = sha("req2"), resp2 = sha("resp2");
    auto req3 = sha("req3"), resp3 = sha("resp3");
    auto result = sha("R");

    auto factSet = g.insertFactSet({
        {req1, resp1},
        {req2, resp2},
        {req3, resp3},
    });
    g.record(q, factSet, result);

    /* Exactly one outgoing edge at (Q, ∅). */
    auto outgoing = g.getAsks(q, TracingDecisionGraph::emptySetHash());
    ASSERT_EQ(outgoing.size(), 1u)
        << "first-time recording with " << 3 << " facts should emit one Asks edge, "
        << "got " << outgoing.size();

    /* That edge's RequestSet contains all three requests. */
    auto rs = g.getRequestSet(outgoing[0]);
    ASSERT_TRUE(rs.has_value());
    std::set<Hash> got(rs->begin(), rs->end());
    std::set<Hash> want{req1, req2, req3};
    EXPECT_EQ(got, want);

    /* Replay still works end-to-end. */
    auto hit = g.walk(q, [&](const Hash & req) {
        if (req == req1) return resp1;
        if (req == req2) return resp2;
        if (req == req3) return resp3;
        ADD_FAILURE() << "unexpected dispatch";
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, result);
}

TEST_F(TracingDecisionGraphTest, Phase1_RecordReusesEdgeWhenExtendingSuperset)
{
    /* Two recordings of the same Q where the second's factSet is a
       strict superset of the first's. The design's record() should:
         - First call:  Asks(Q, ∅, RS{r1,r2}) → factSet1; Terminal at factSet1.
         - Second call: follow the existing Asks(Q, ∅, RS{r1,r2}) edge to
                        factSet1, then insert Asks(Q, factSet1, RS{r3}) → factSet2;
                        Terminal at factSet2.
       Net: 2 Asks rows, 2 Terminals — *not* the 5 the strawman would emit. */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto r1 = sha("r1"), v1 = sha("v1");
    auto r2 = sha("r2"), v2 = sha("v2");
    auto r3 = sha("r3"), v3 = sha("v3");

    auto fs1 = g.insertFactSet({{r1, v1}, {r2, v2}});
    auto fs2 = g.insertFactSet({{r1, v1}, {r2, v2}, {r3, v3}});
    g.record(q, fs1, sha("R1"));
    g.record(q, fs2, sha("R2"));

    /* (Q, ∅) carries one edge; that edge's RS is the first
       recording's full request set {r1, r2}. */
    auto rootEdges = g.getAsks(q, TracingDecisionGraph::emptySetHash());
    ASSERT_EQ(rootEdges.size(), 1u);
    auto rs0 = g.getRequestSet(rootEdges[0]);
    ASSERT_TRUE(rs0.has_value());
    std::set<Hash> root_want{r1, r2};
    std::set<Hash> root_got(rs0->begin(), rs0->end());
    EXPECT_EQ(root_got, root_want);

    /* (Q, fs1) carries one edge for the second recording's
       extension; its RS = {r3}. */
    auto fs1Edges = g.getAsks(q, fs1);
    ASSERT_EQ(fs1Edges.size(), 1u);
    auto rs1 = g.getRequestSet(fs1Edges[0]);
    ASSERT_TRUE(rs1.has_value());
    std::set<Hash> ext_want{r3};
    std::set<Hash> ext_got(rs1->begin(), rs1->end());
    EXPECT_EQ(ext_got, ext_want);

    /* Both walks still hit correctly. */
    auto dispatch = [&](const Hash & req) {
        if (req == r1) return v1;
        if (req == r2) return v2;
        if (req == r3) return v3;
        ADD_FAILURE() << "unexpected dispatch";
        return Hash(HashAlgorithm::SHA256);
    };
    auto hit1 = g.walk(q, dispatch);
    ASSERT_TRUE(hit1.has_value());
    /* Walk hits the SHORTER recording (fs1, R1) first because
       walk checks Terminal at every intermediate cur. After
       dispatching {r1,r2} it reaches fs1, where Terminal(Q, fs1, R1)
       is present. */
    EXPECT_EQ(*hit1, sha("R1"));
}

TEST_F(TracingDecisionGraphTest, Phase1_PatriciaSplitsOnOverlappingDivergence)
{
    /* Two recordings of Q that share a request prefix but diverge:
         rec1: factSet1 covers {a, b, c}  (R1)
         rec2: factSet2 covers {a, b, d}  (R2)        — same a/b responses
       After both records, the design's algorithm Patricia-splits so
       the shared prefix RS{a,b} is one edge from ∅ to an intermediate
       FactSet, and {c} vs {d} fan out from there.

       Structurally:
         (Q, ∅) ── RS{a,b} ──▶ FS_int
         (Q, FS_int) ── RS{a,b,c} ──▶ FS1   (kept its original RS reference)
         (Q, FS_int) ── RS{a,b,d} ──▶ FS2   (kept its original RS reference)

       i.e., (Q, ∅) has exactly one outgoing edge after the split. */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    auto a = sha("a"), va = sha("va");
    auto b = sha("b"), vb = sha("vb");
    auto c = sha("c"), vc = sha("vc");
    auto d = sha("d"), vd = sha("vd");

    auto fs1 = g.insertFactSet({{a, va}, {b, vb}, {c, vc}});
    auto fs2 = g.insertFactSet({{a, va}, {b, vb}, {d, vd}});
    g.record(q, fs1, sha("R1"));
    g.record(q, fs2, sha("R2"));

    /* After split: exactly one edge from (Q, ∅), labelled with the
       shared prefix RS{a,b}. */
    auto root = g.getAsks(q, TracingDecisionGraph::emptySetHash());
    ASSERT_EQ(root.size(), 1u)
        << "after Patricia split (Q, ∅) should have exactly one outgoing edge";

    auto rsShared = g.getRequestSet(root[0]);
    ASSERT_TRUE(rsShared.has_value());
    std::set<Hash> shared_got(rsShared->begin(), rsShared->end());
    std::set<Hash> shared_want{a, b};
    EXPECT_EQ(shared_got, shared_want)
        << "the single outgoing edge's RS should be the shared prefix {a,b}";

    /* Both walks reach the correct terminal. */
    auto hit1 = g.walk(q, [&](const Hash & req) {
        if (req == a) return va;
        if (req == b) return vb;
        if (req == c) return vc;
        if (req == d) return vd;
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hit1.has_value());
    /* With both responses available, walk will reach whichever
       terminal sits at the cur it converges to — but it must reach
       *some* recorded Result, not miss. */
    EXPECT_TRUE(*hit1 == sha("R1") || *hit1 == sha("R2"));

    /* Walk where d's response is wrong: only the c-branch survives,
       must hit R1. */
    auto hitC = g.walk(q, [&](const Hash & req) {
        if (req == a) return va;
        if (req == b) return vb;
        if (req == c) return vc;
        if (req == d) return sha("wrong-d");
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hitC.has_value());
    EXPECT_EQ(*hitC, sha("R1"));

    /* Walk where c's response is wrong: only the d-branch survives,
       must hit R2. */
    auto hitD = g.walk(q, [&](const Hash & req) {
        if (req == a) return va;
        if (req == b) return vb;
        if (req == c) return sha("wrong-c");
        if (req == d) return vd;
        return Hash(HashAlgorithm::SHA256);
    });
    ASSERT_TRUE(hitD.has_value());
    EXPECT_EQ(*hitD, sha("R2"));
}

TEST_F(TracingDecisionGraphTest, Phase1_RequestSetSharedAcrossQs)
{
    /* Two distinct Qs recorded with the same factSet. RequestSets is
       content-addressed; both Qs' Asks edges should point to the SAME
       RequestSet hash — one row in the RequestSet pool, two in Asks. */
    TracingDecisionGraph g(dbPath);
    auto q1 = sha("Q1"), q2 = sha("Q2");
    auto r1 = sha("r1"), v1 = sha("v1");
    auto r2 = sha("r2"), v2 = sha("v2");

    auto fs = g.insertFactSet({{r1, v1}, {r2, v2}});
    g.record(q1, fs, sha("R1"));
    g.record(q2, fs, sha("R2"));

    auto q1Edges = g.getAsks(q1, TracingDecisionGraph::emptySetHash());
    auto q2Edges = g.getAsks(q2, TracingDecisionGraph::emptySetHash());
    ASSERT_EQ(q1Edges.size(), 1u);
    ASSERT_EQ(q2Edges.size(), 1u);
    EXPECT_EQ(q1Edges[0], q2Edges[0])
        << "Q1 and Q2 record the same factSet — their Asks edges should "
        << "point to the same content-addressed RequestSet";
}

TEST_F(TracingDecisionGraphTest, Phase1_WalkDispatchesMultiElementRequestSet)
{
    /* Walk must correctly handle an edge whose RequestSet has > 1
       element: dispatch each request, XOR all (req, resp) facts into
       cur in one step (still O(1) per fact, but one Asks-table hop
       per edge, not per fact). */
    TracingDecisionGraph g(dbPath);
    auto q = sha("Q");
    /* Use enough facts that the singleton-strawman vs design difference
       is unambiguous. */
    constexpr int N = 10;
    std::vector<TracingDecisionGraph::Fact> facts;
    std::map<Hash, Hash> dispatchMap;
    for (int i = 0; i < N; ++i) {
        auto req = sha("req-" + std::to_string(i));
        auto resp = sha("resp-" + std::to_string(i));
        facts.push_back({req, resp});
        dispatchMap.emplace(req, resp);
    }
    auto fs = g.insertFactSet(facts);
    g.record(q, fs, sha("R"));

    /* One Asks edge total at (Q, ∅); its RS has N members. */
    auto edges = g.getAsks(q, TracingDecisionGraph::emptySetHash());
    ASSERT_EQ(edges.size(), 1u);
    auto rs = g.getRequestSet(edges[0]);
    ASSERT_TRUE(rs.has_value());
    EXPECT_EQ(rs->size(), static_cast<size_t>(N));

    auto hit = g.walk(q, [&](const Hash & req) {
        auto it = dispatchMap.find(req);
        if (it == dispatchMap.end()) {
            ADD_FAILURE() << "unexpected dispatch";
            return Hash(HashAlgorithm::SHA256);
        }
        return it->second;
    });
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, sha("R"));
}

} // namespace nix
