#include <gtest/gtest.h>

#include "nix/expr/request-set-trie.hh"

namespace nix::trace::rst {

/* ─────────────────────────────────────────────────────────────────────
   Test fixture + helpers
   ───────────────────────────────────────────────────────────────────── */

class RequestSetTrieTest : public ::testing::Test
{
protected:
    /* Deterministic distinct hashes derived from a small seed — the
       actual byte contents matter (bucket-index depends on top bits)
       so we don't want all-zero or repeated patterns. */
    static Hash h(uint64_t seed)
    {
        std::string s = "req#" + std::to_string(seed);
        return tracingHash(s);
    }

    /* Build a vector of `n` distinct hashes for bulk tests. */
    static std::vector<Hash> hashes(size_t n)
    {
        std::vector<Hash> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i)
            out.push_back(h(i));
        return out;
    }
};

/* ─────────────────────────────────────────────────────────────────────
   FrozenNode: leaves and payload round-trip
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, EmptyLeafPayloadRoundTrip)
{
    FrozenNodeCache cache;
    auto leaf = cache.internLeaf({});
    /* leaf is ref<>, non-null by construction */
    EXPECT_TRUE(leaf->isLeaf());
    EXPECT_EQ(leaf->size(), 0u);
    auto payload = leaf->toPayload();
    ASSERT_EQ(payload.size(), 1u);
    EXPECT_EQ(payload[0], 0x00);
    auto round = cache.intern(leaf->hash, payload);
    EXPECT_EQ(round, leaf);  // interned identity
}

TEST_F(RequestSetTrieTest, SingletonLeafPayloadRoundTrip)
{
    FrozenNodeCache cache;
    auto leaf = cache.internLeaf({h(1)});
    /* leaf is ref<>, non-null by construction */
    EXPECT_TRUE(leaf->isLeaf());
    EXPECT_EQ(leaf->size(), 1u);
    EXPECT_TRUE(leaf->contains(h(1)));
    EXPECT_FALSE(leaf->contains(h(2)));
    auto payload = leaf->toPayload();
    EXPECT_EQ(payload.size(), 1u + tracingHashSize);
    auto round = cache.intern(leaf->hash, payload);
    EXPECT_EQ(round, leaf);
}

TEST_F(RequestSetTrieTest, LeafSortedIndependentOfInsertOrder)
{
    FrozenNodeCache cache;
    auto a = cache.internLeaf({h(5), h(2), h(9)});
    auto b = cache.internLeaf({h(9), h(5), h(2)});
    /* a is ref<>, non-null by construction */
    /* Same members via different orders → same interned pointer. */
    EXPECT_EQ(a, b);
}

TEST_F(RequestSetTrieTest, LeafDeduplicatesRepeatedMembers)
{
    FrozenNodeCache cache;
    auto a = cache.internLeaf({h(1), h(2), h(3)});
    auto b = cache.internLeaf({h(1), h(1), h(2), h(3), h(3)});
    EXPECT_EQ(a, b);
    EXPECT_EQ(a->size(), 3u);
}

/* ─────────────────────────────────────────────────────────────────────
   FrozenNode: internal nodes (over threshold → split)
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, AboveThresholdBuildsInternal)
{
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(TRIE_SPLIT_THRESHOLD + 1));
    /* root is ref<>, non-null by construction */
    /* With 17 members and 16-way radix, the root is very likely
       internal (near-certain given uniform hash distribution). */
    EXPECT_FALSE(root->isLeaf());
    EXPECT_EQ(root->size(), TRIE_SPLIT_THRESHOLD + 1);
}

TEST_F(RequestSetTrieTest, InternalPreservesAllMembers)
{
    FrozenNodeCache cache;
    auto members = hashes(100);
    auto root = cache.internSet(members);
    /* root is ref<>, non-null by construction */
    EXPECT_EQ(root->size(), 100u);
    for (const auto & m : members)
        EXPECT_TRUE(root->contains(m)) << "missing member from full set";
    /* A member not in the set is not found. */
    EXPECT_FALSE(root->contains(h(200)));
}

TEST_F(RequestSetTrieTest, InternalPayloadRoundTrip)
{
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(50));
    /* root is ref<>, non-null by construction */
    ASSERT_FALSE(root->isLeaf());
    auto payload = root->toPayload();
    EXPECT_EQ(payload[0], 0x01);
    /* Re-interning the same payload returns the same pointer. */
    auto round = cache.intern(root->hash, payload);
    EXPECT_EQ(round, root);
}

/* ─────────────────────────────────────────────────────────────────────
   Cache interning
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, CacheDeduplicatesByHash)
{
    FrozenNodeCache cache;
    auto a = cache.internSet(hashes(30));
    auto b = cache.internSet(hashes(30));
    EXPECT_EQ(a, b);
    /* Cache lookup returns the same pointer. */
    auto looked = cache.lookup(a->hash);
    ASSERT_TRUE(looked.has_value());
    EXPECT_EQ(*looked, a);
}

TEST_F(RequestSetTrieTest, CacheLookupMissReturnsNullopt)
{
    FrozenNodeCache cache;
    auto miss = cache.lookup(h(42));
    EXPECT_FALSE(miss.has_value());
}

/* ─────────────────────────────────────────────────────────────────────
   MutableNode: build, freeze, agree with internSet
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, MutableFreezeEqualsInternSet)
{
    FrozenNodeCache cache;
    MutableNode mut;
    for (auto & m : hashes(25))
        mut.insert(m);
    auto frozen = mut.freeze(cache);
    /* frozen is ref<>, non-null by construction */
    auto direct = cache.internSet(hashes(25));
    EXPECT_EQ(frozen, direct);
}

TEST_F(RequestSetTrieTest, MutableInsertIdempotent)
{
    FrozenNodeCache cache;
    MutableNode mut;
    for (int i = 0; i < 5; ++i) {
        mut.insert(h(1));
        mut.insert(h(2));
    }
    EXPECT_EQ(mut.size(), 2u);
    auto frozen = mut.freeze(cache);
    EXPECT_EQ(frozen->size(), 2u);
    EXPECT_TRUE(frozen->contains(h(1)));
    EXPECT_TRUE(frozen->contains(h(2)));
    EXPECT_FALSE(frozen->contains(h(3)));
}

TEST_F(RequestSetTrieTest, MutableCOWFromFrozen)
{
    FrozenNodeCache cache;
    /* Freeze a starting tree, then mutate it via a MutableNode wrapper. */
    auto seed = cache.internSet(hashes(20));
    MutableNode mut(seed);
    EXPECT_EQ(mut.size(), 20u);
    for (auto & m : hashes(20))
        EXPECT_TRUE(mut.contains(m));
    /* Add a new member. The seed's frozen data must not be mutated —
       COW clones any subtree we descend into to modify. */
    auto extra = h(1000);
    mut.insert(extra);
    EXPECT_TRUE(mut.contains(extra));
    EXPECT_EQ(mut.size(), 21u);
    /* Original frozen unchanged. */
    EXPECT_EQ(seed->size(), 20u);
    EXPECT_FALSE(seed->contains(extra));
}

TEST_F(RequestSetTrieTest, MutableFreezeIsCacheHitWhenSameSet)
{
    FrozenNodeCache cache;
    auto original = cache.internSet(hashes(30));

    MutableNode mut;
    for (auto & m : hashes(30))
        mut.insert(m);
    auto frozen = mut.freeze(cache);
    EXPECT_EQ(frozen, original);
}

/* ─────────────────────────────────────────────────────────────────────
   Diff / subset / intersection with structure sharing short-circuit
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, DifferenceSameSetIsEmpty)
{
    FrozenNodeCache cache;
    auto a = cache.internSet(hashes(30));
    auto b = a;
    EXPECT_TRUE(difference(*a, *b).empty());
}

TEST_F(RequestSetTrieTest, DifferenceDisjointReturnsAllOfA)
{
    FrozenNodeCache cache;
    std::vector<Hash> aMembers, bMembers;
    for (size_t i = 0; i < 20; ++i) aMembers.push_back(h(i));
    for (size_t i = 100; i < 120; ++i) bMembers.push_back(h(i));
    auto a = cache.internSet(aMembers);
    auto b = cache.internSet(bMembers);
    auto d = difference(*a, *b);
    /* All of A should be in the diff. */
    EXPECT_EQ(d.size(), 20u);
    std::unordered_set<Hash> ds(d.begin(), d.end());
    for (const auto & m : aMembers) EXPECT_TRUE(ds.count(m));
}

TEST_F(RequestSetTrieTest, DifferenceProperSubsetLosesShared)
{
    FrozenNodeCache cache;
    std::vector<Hash> allMembers, subMembers;
    for (size_t i = 0; i < 30; ++i) allMembers.push_back(h(i));
    for (size_t i = 0; i < 10; ++i) subMembers.push_back(h(i));
    auto full = cache.internSet(allMembers);
    auto sub  = cache.internSet(subMembers);
    /* full \ sub should give the 20 tail members. */
    auto d = difference(*full, *sub);
    EXPECT_EQ(d.size(), 20u);
    std::unordered_set<Hash> ds(d.begin(), d.end());
    for (size_t i = 10; i < 30; ++i) EXPECT_TRUE(ds.count(h(i)));
    for (size_t i = 0; i < 10; ++i) EXPECT_FALSE(ds.count(h(i)));
    /* sub \ full should be empty (sub ⊂ full). */
    EXPECT_TRUE(difference(*sub, *full).empty());
}

TEST_F(RequestSetTrieTest, IsSubsetVariousCases)
{
    FrozenNodeCache cache;
    auto a = cache.internSet({h(1), h(2), h(3)});
    auto b = cache.internSet({h(1), h(2), h(3), h(4), h(5)});
    EXPECT_TRUE(isSubset(*a, *b));
    EXPECT_FALSE(isSubset(*b, *a));
    /* Reflexive. */
    EXPECT_TRUE(isSubset(*a, *a));
    /* Empty is subset of everything. */
    auto empty = cache.internLeaf({});
    EXPECT_TRUE(isSubset(*empty, *a));
    EXPECT_FALSE(isSubset(*a, *empty));
}

TEST_F(RequestSetTrieTest, DifferenceSameFrozenPointerShortCircuits)
{
    /* This test can't observe the short-circuit directly without
       instrumentation, but it exercises the guarantee: two references
       to the same FrozenNode should produce empty diff even for a very
       large set (walking would be expensive if not short-circuited). */
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(500));
    /* root is ref<>, non-null by construction */
    EXPECT_TRUE(difference(*root, *root).empty());
    EXPECT_TRUE(isSubset(*root, *root));
}

/* ─────────────────────────────────────────────────────────────────────
   Interop with existing DB payload format
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, HashMatchesDbPayloadForSingleton)
{
    FrozenNodeCache cache;
    auto leaf = cache.internLeaf({h(1)});
    /* The hash should be sha256 of the byte payload:
       [0x00] || h(1).hash */
    std::string expectedPayload;
    expectedPayload.push_back(0x00);
    expectedPayload.append(reinterpret_cast<const char *>(h(1).hash), tracingHashSize);
    auto expectedHash = tracingHash(expectedPayload);
    EXPECT_EQ(leaf->hash, expectedHash);
}

/* ─────────────────────────────────────────────────────────────────────
   Multi-level trie (forces deep splits)
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, MultiLevelTrieAllMembersPresent)
{
    FrozenNodeCache cache;
    /* 500 members forces multiple internal levels (average bucket load
       500/16 = 31 → each level-1 bucket also splits). */
    auto members = hashes(500);
    auto root = cache.internSet(members);
    /* root is ref<>, non-null by construction */
    ASSERT_FALSE(root->isLeaf());
    EXPECT_EQ(root->size(), 500u);
    for (const auto & m : members)
        EXPECT_TRUE(root->contains(m));
}

TEST_F(RequestSetTrieTest, MutableInsertManyMembersFreezeMatches)
{
    FrozenNodeCache cache;
    MutableNode mut;
    for (auto & m : hashes(500))
        mut.insert(m);
    EXPECT_EQ(mut.size(), 500u);
    auto frozen = mut.freeze(cache);
    auto direct = cache.internSet(hashes(500));
    EXPECT_EQ(frozen, direct)
        << "mutable-built and direct-built should reach the same interned root";
}

/* ─────────────────────────────────────────────────────────────────────
   Cross-cache identity: same hash reused across cache instances
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, IndependentCachesProduceSameHash)
{
    FrozenNodeCache a, b;
    auto rootA = a.internSet(hashes(50));
    auto rootB = b.internSet(hashes(50));
    /* Different pointer identities (each cache owns its own instances)
       but the content hash is identical. */
    EXPECT_NE(rootA, rootB);
    EXPECT_EQ(rootA->hash, rootB->hash);
    EXPECT_EQ(rootA->size(), rootB->size());
}

/* ─────────────────────────────────────────────────────────────────────
   Payload intern requires child cache prepopulation
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, InternInternalRequiresChildrenCached)
{
    FrozenNodeCache src, dst;
    auto root = src.internSet(hashes(50));
    ASSERT_FALSE(root->isLeaf());
    /* Try to intern the root payload into a cache with no children — fails. */
    auto payload = root->toPayload();
    EXPECT_THROW(dst.intern(root->hash, payload), Error);
    /* Prepopulate all descendants into dst, then it works. */
    auto walk = [&](const FrozenNode & node, auto & self) -> void {
        if (!node.isLeaf())
            for (const auto & [_, child] : node.asInternal().children)
                self(*child, self);
        dst.intern(node.hash, node.toPayload());
    };
    walk(*root, walk);
    /* Now the root is interned; lookup returns it. */
    auto looked = dst.lookup(root->hash);
    ASSERT_TRUE(looked.has_value());
    EXPECT_EQ((*looked)->hash, root->hash);
}

/* ─────────────────────────────────────────────────────────────────────
   COW deep inside a frozen tree
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, MutableCOWDoesntMutateFrozenAtDepth)
{
    FrozenNodeCache cache;
    auto seedMembers = hashes(200);
    auto seedRoot = cache.internSet(seedMembers);
    ASSERT_FALSE(seedRoot->isLeaf());

    /* Snapshot the seed's payload before we mutate a wrapper. */
    auto seedRootHashBefore = seedRoot->hash;

    MutableNode mut(seedRoot);
    /* Insert many new members. Should force COW into multiple buckets. */
    for (uint64_t i = 1000; i < 1100; ++i)
        mut.insert(h(i));
    auto frozen = mut.freeze(cache);

    /* Original seed is unchanged. */
    EXPECT_EQ(seedRoot->hash, seedRootHashBefore);
    for (const auto & m : seedMembers)
        EXPECT_TRUE(seedRoot->contains(m));
    /* Frozen has both old and new. */
    EXPECT_EQ(frozen->size(), 300u);
    for (const auto & m : seedMembers)
        EXPECT_TRUE(frozen->contains(m));
    for (uint64_t i = 1000; i < 1100; ++i)
        EXPECT_TRUE(frozen->contains(h(i)));
    /* Sanity: the mutation-plus-freeze produced a fresh hash. */
    EXPECT_NE(frozen->hash, seedRootHashBefore);
}

/* ─────────────────────────────────────────────────────────────────────
   Diff/subset at internal-level boundaries
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, DifferenceOneSideLeafOtherInternal)
{
    FrozenNodeCache cache;
    /* Leaf with few members; internal (100 members). */
    auto few = cache.internSet(hashes(3));
    auto many = cache.internSet(hashes(100));
    ASSERT_TRUE(few->isLeaf());
    ASSERT_FALSE(many->isLeaf());
    /* few ⊂ many (first 3 members are in the 100). */
    EXPECT_TRUE(isSubset(*few, *many));
    EXPECT_TRUE(difference(*few, *many).empty());
    /* many \ few = 97 members. */
    auto d = difference(*many, *few);
    EXPECT_EQ(d.size(), 97u);
    /* Symmetry check via count: no h(0..2) in the result. */
    for (const auto & m : d) {
        EXPECT_NE(m, h(0));
        EXPECT_NE(m, h(1));
        EXPECT_NE(m, h(2));
    }
}

TEST_F(RequestSetTrieTest, IsSubsetPartialOverlapNotSubset)
{
    FrozenNodeCache cache;
    std::vector<Hash> aMembers, bMembers;
    for (size_t i = 0; i < 30; ++i) aMembers.push_back(h(i));
    for (size_t i = 25; i < 60; ++i) bMembers.push_back(h(i));
    auto a = cache.internSet(aMembers);
    auto b = cache.internSet(bMembers);
    /* Overlap on [25..30) — neither is a subset of the other. */
    EXPECT_FALSE(isSubset(*a, *b));
    EXPECT_FALSE(isSubset(*b, *a));
    /* Diff sizes: a\b = 25, b\a = 30. */
    EXPECT_EQ(difference(*a, *b).size(), 25u);
    EXPECT_EQ(difference(*b, *a).size(), 30u);
}

TEST_F(RequestSetTrieTest, DifferenceMembersAreLexSorted)
{
    /* Trie iteration walks buckets in ascending order → the emitted
       members should be lex-sorted (buckets are sorted by top-bits,
       within-bucket members are also sorted). */
    FrozenNodeCache cache;
    auto full = cache.internSet(hashes(200));
    auto sub  = cache.internSet(hashes(50));
    auto d = difference(*full, *sub);
    EXPECT_TRUE(std::is_sorted(d.begin(), d.end()));
}

/* ─────────────────────────────────────────────────────────────────────
   Split-boundary insertion via mutable (leaf → internal transition)
   ───────────────────────────────────────────────────────────────────── */

/* ─────────────────────────────────────────────────────────────────────
   Incremental build-and-freeze: 500 insert+freeze cycles.
   Verifies both correctness AND that freeze reuses unchanged subtrees
   (via cache identity), so the total work stays O(depth) per step
   instead of O(total nodes).
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, IncrementalInsertAndFreezeReusesUnchangedSubtrees)
{
    FrozenNodeCache cache;
    MutableNode mut;
    /* Build up 500 members, freezing after every insert.
       Correctness assertions on each step, and count the total intern
       work — a well-designed freeze reuses unchanged subtrees, so the
       cost per step is bounded by tree depth, not tree size. */
    /* ref<> has no default-construct, so hold the running "last" via
       shared_ptr and rewrap when we compare against ref returns. */
    std::shared_ptr<const FrozenNode> last;
    for (uint64_t i = 0; i < 500; ++i) {
        mut.insert(h(i));
        auto frozen = mut.freeze(cache);
        /* frozen is ref<>, non-null by construction */
        EXPECT_EQ(frozen->size(), i + 1);
        EXPECT_TRUE(frozen->contains(h(i)));
        last = frozen;  // ref → shared_ptr
    }
    /* Final tree matches the all-at-once build. */
    auto before = cache.internAttempts();
    auto direct = cache.internSet(hashes(500));
    EXPECT_EQ(last, FrozenNodePtr(direct).get_ptr());
    auto internSetCost = cache.internAttempts() - before;
    /* Recording the "reference cost" of building the whole set once,
       from scratch, so we can compare against the amortised
       per-insert cost of the incremental build below. */

    /* Freeze-with-no-mutation is free — cached at each mutable node. */
    auto attemptsBeforeReFreeze = cache.internAttempts();
    auto again = mut.freeze(cache);
    EXPECT_EQ(std::shared_ptr<const FrozenNode>(again), last);
    EXPECT_EQ(cache.internAttempts(), attemptsBeforeReFreeze)
        << "no-op freeze must not re-hash any node";

    /* Total intern attempts across 500 insert+freeze steps should be
       bounded by O(steps × depth). A radix-16 trie of 500 members has
       depth ≤ 3 (16^3 = 4096 buckets is far more than 500). So per
       step we expect ≤ ~4 nodes touched (each level's ancestor of the
       newly-inserted leaf). Allowing a generous slack for splits,
       expect ≪ 500 × 500 = 250 000 (which is what an unreused freeze
       would cost). */
    (void) internSetCost;  // silence unused warning if we drop cap
    EXPECT_LT(cache.internAttempts(), 10000u)
        << "500 insert+freeze cycles cost far more than O(depth) — "
           "freeze isn't reusing unchanged subtrees";
}

/* ─────────────────────────────────────────────────────────────────────
   Intersection
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, IntersectionSameSetIsSelf)
{
    FrozenNodeCache cache;
    auto a = cache.internSet(hashes(30));
    auto both = intersection(a, a, cache);
    EXPECT_EQ(both, a);
}

TEST_F(RequestSetTrieTest, IntersectionDisjointIsEmpty)
{
    FrozenNodeCache cache;
    std::vector<Hash> aMembers, bMembers;
    for (size_t i = 0; i < 20; ++i) aMembers.push_back(h(i));
    for (size_t i = 100; i < 120; ++i) bMembers.push_back(h(i));
    auto a = cache.internSet(aMembers);
    auto b = cache.internSet(bMembers);
    auto ab = intersection(a, b, cache);
    EXPECT_EQ(ab->size(), 0u);
    EXPECT_TRUE(ab->isLeaf());
}

TEST_F(RequestSetTrieTest, IntersectionSubsetReturnsSubset)
{
    FrozenNodeCache cache;
    std::vector<Hash> allM, subM;
    for (size_t i = 0; i < 30; ++i) allM.push_back(h(i));
    for (size_t i = 0; i < 10; ++i) subM.push_back(h(i));
    auto full = cache.internSet(allM);
    auto sub  = cache.internSet(subM);
    auto both = intersection(full, sub, cache);
    EXPECT_EQ(both, sub);
    /* Symmetric. */
    auto both2 = intersection(sub, full, cache);
    EXPECT_EQ(both2, sub);
}

TEST_F(RequestSetTrieTest, IntersectionPartialOverlap)
{
    FrozenNodeCache cache;
    std::vector<Hash> aM, bM;
    for (size_t i = 0; i < 30; ++i) aM.push_back(h(i));
    for (size_t i = 25; i < 60; ++i) bM.push_back(h(i));
    auto a = cache.internSet(aM);
    auto b = cache.internSet(bM);
    auto ab = intersection(a, b, cache);
    /* Overlap is [25..30) — 5 members. */
    EXPECT_EQ(ab->size(), 5u);
    for (size_t i = 25; i < 30; ++i) EXPECT_TRUE(ab->contains(h(i)));
    for (size_t i = 0; i < 25; ++i)  EXPECT_FALSE(ab->contains(h(i)));
    for (size_t i = 30; i < 60; ++i) EXPECT_FALSE(ab->contains(h(i)));
}

TEST_F(RequestSetTrieTest, IntersectionCommutative)
{
    FrozenNodeCache cache;
    auto a = cache.internSet(hashes(50));
    /* Skip a few members to give a distinct b. */
    std::vector<Hash> bM;
    for (size_t i = 3; i < 45; ++i) bM.push_back(h(i));
    auto b = cache.internSet(bM);
    auto ab = intersection(a, b, cache);
    auto ba = intersection(b, a, cache);
    EXPECT_EQ(ab, ba);
}

/* ─────────────────────────────────────────────────────────────────────
   Persistence
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, PersistWalksTreeAndEnqueuesEachNodeOnce)
{
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(200));
    ASSERT_FALSE(root->isLeaf());
    /* Count nodes in the tree by walking. */
    size_t treeNodes = 0;
    auto count = [&](const FrozenNode & n, auto & self) -> void {
        ++treeNodes;
        if (!n.isLeaf())
            for (const auto & [_, c] : n.asInternal().children)
                self(*c, self);
    };
    count(*root, count);

    /* Capture what would be sent to the DB writer. */
    std::vector<std::pair<Hash, std::string>> writes;
    FrozenNodeCache::PersistSink sink =
        [&](const Hash & h, std::string_view p) { writes.emplace_back(h, std::string(p)); };

    cache.persist(root, sink);

    EXPECT_EQ(writes.size(), treeNodes)
        << "each tree node should be persisted exactly once";

    /* Every node reports as persisted afterward. */
    auto verifyAllPersisted = [&](const FrozenNode & n, auto & self) -> void {
        EXPECT_TRUE(n.persisted);
        if (!n.isLeaf())
            for (const auto & [_, c] : n.asInternal().children)
                self(*c, self);
    };
    verifyAllPersisted(*root, verifyAllPersisted);

    /* Second persist call is a no-op — persisted=true short-circuits. */
    writes.clear();
    cache.persist(root, sink);
    EXPECT_TRUE(writes.empty())
        << "already-persisted tree should not re-enqueue any writes";
}

TEST_F(RequestSetTrieTest, PersistIncrementalOnlyEnqueuesNewSubtrees)
{
    FrozenNodeCache cache;
    /* Freeze + persist a 100-member tree. */
    auto rootA = cache.internSet(hashes(100));
    std::vector<Hash> writeHashes;
    FrozenNodeCache::PersistSink sink =
        [&](const Hash & h, std::string_view) { writeHashes.push_back(h); };
    cache.persist(rootA, sink);
    auto persistedBefore = writeHashes.size();

    /* Extend to 105 members via a MutableNode COW from the frozen root. */
    MutableNode mut(rootA);
    for (uint64_t i = 100; i < 105; ++i) mut.insert(h(i));
    auto rootB = mut.freeze(cache);

    /* Persist rootB. Only newly-created subtrees should be enqueued —
       unchanged subtrees (still bit-identical to rootA's children)
       were already persisted, so their FrozenNodePtr's `persisted` is
       true and the walk skips them. */
    writeHashes.clear();
    cache.persist(rootB, sink);
    auto persistedIncremental = writeHashes.size();

    EXPECT_GT(persistedIncremental, 0u)
        << "some new subtrees should be persisted";
    EXPECT_LT(persistedIncremental, persistedBefore)
        << "incremental persist must not rewrite the whole tree";
}

TEST_F(RequestSetTrieTest, MutableInsertCrossesLeafThreshold)
{
    FrozenNodeCache cache;
    MutableNode mut;
    /* Insert exactly threshold members — still a leaf. */
    for (size_t i = 0; i < TRIE_SPLIT_THRESHOLD; ++i)
        mut.insert(h(i));
    EXPECT_EQ(mut.size(), TRIE_SPLIT_THRESHOLD);
    /* Freeze — should be a single leaf. */
    auto f1 = mut.freeze(cache);
    EXPECT_TRUE(f1->isLeaf());
    /* Cross the threshold. */
    mut.insert(h(TRIE_SPLIT_THRESHOLD));
    EXPECT_EQ(mut.size(), TRIE_SPLIT_THRESHOLD + 1);
    auto f2 = mut.freeze(cache);
    /* With 17 members and uniform hashing, almost certainly internal.
       (The RequestSetTrie design accepts a tiny probability of the
       17th bucket colliding, but for our seeded hashes we can rely on
       the specific outcome.) */
    EXPECT_FALSE(f2->isLeaf()) << "expected split at threshold+1 with our seeded hashes";
}

} // namespace nix::trace::rst
