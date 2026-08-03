#include <gtest/gtest.h>

#include "nix/expr/request-set-trie.hh"

namespace nix::trace::rst {

/* ─────────────────────────────────────────────────────────────────────
   Fixture — deterministic hash helpers
   ───────────────────────────────────────────────────────────────────── */

class RequestSetTrieTest : public ::testing::Test
{
protected:
    static Hash h(uint64_t seed)
    {
        return tracingHash("req#" + std::to_string(seed));
    }

    static std::vector<Hash> hashes(size_t n)
    {
        std::vector<Hash> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i)
            out.push_back(h(i));
        return out;
    }

    /* XOR-fold a set of member hashes into an identity value. */
    static Hash xorMembers(const std::vector<Hash> & members)
    {
        Hash acc(HashAlgorithm::SHA256);
        acc.hashSize = tracingHashSize;
        for (const auto & m : members)
            for (size_t i = 0; i < tracingHashSize; ++i)
                acc.hash[i] ^= m.hash[i];
        return acc;
    }
};

/* ─────────────────────────────────────────────────────────────────────
   Leaf shape (≤ LEAF_MAX_MEMBERS)
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, EmptyIsLeaf)
{
    FrozenNodeCache cache;
    auto root = cache.internSet({});
    EXPECT_TRUE(root->isLeaf());
    EXPECT_EQ(root->size(), 0u);
}

TEST_F(RequestSetTrieTest, SmallSetIsLeaf)
{
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(LEAF_MAX_MEMBERS));
    EXPECT_TRUE(root->isLeaf());
    EXPECT_EQ(root->size(), LEAF_MAX_MEMBERS);
}

TEST_F(RequestSetTrieTest, LeafSortedIndependentOfInsertOrder)
{
    FrozenNodeCache cache;
    auto a = cache.internSet({h(5), h(2), h(9)});
    auto b = cache.internSet({h(9), h(5), h(2)});
    EXPECT_EQ(a, b);
    ASSERT_TRUE(a->isLeaf());
    EXPECT_TRUE(std::is_sorted(a->asLeaf().members.begin(), a->asLeaf().members.end()));
}

TEST_F(RequestSetTrieTest, DedupesRepeatedMembers)
{
    FrozenNodeCache cache;
    auto a = cache.internSet({h(1), h(2), h(3)});
    auto b = cache.internSet({h(1), h(1), h(2), h(3), h(3)});
    EXPECT_EQ(a, b);
    EXPECT_EQ(a->size(), 3u);
}

/* ─────────────────────────────────────────────────────────────────────
   Threshold-crossing: LEAF_MAX_MEMBERS+1 always becomes Internal
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, AboveThresholdIsInternal)
{
    /* Under HAMT with skip-single-slot, LEAF_MAX_MEMBERS+1 members
       always exceed the flat-leaf cap and materialize as an Internal.
       Depth is deterministic: whatever bit-group first partitions
       them into ≥ 2 slots. Uniform hashes → depth 0 is overwhelmingly
       the divergence point. */
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(LEAF_MAX_MEMBERS + 1));
    EXPECT_FALSE(root->isLeaf());
    EXPECT_EQ(root->size(), LEAF_MAX_MEMBERS + 1);
}

TEST_F(RequestSetTrieTest, InternalPreservesAllMembers)
{
    FrozenNodeCache cache;
    auto members = hashes(100);
    auto root = cache.internSet(members);
    EXPECT_EQ(root->size(), 100u);
    for (const auto & m : members)
        EXPECT_TRUE(root->contains(m)) << "missing member from full set";
    EXPECT_FALSE(root->contains(h(999)));
}

/* ─────────────────────────────────────────────────────────────────────
   XOR identity — the property that makes cache dedup + short-circuit work
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, IdentityIsXorOfMembersAsLeaf)
{
    FrozenNodeCache cache;
    auto members = hashes(5);
    auto leaf = cache.internSet(members);
    EXPECT_EQ(leaf->hash, xorMembers(members));
}

TEST_F(RequestSetTrieTest, IdentityIsXorOfMembersAsInternal)
{
    FrozenNodeCache cache;
    auto members = hashes(200);
    auto root = cache.internSet(members);
    ASSERT_FALSE(root->isLeaf());
    EXPECT_EQ(root->hash, xorMembers(members));
}

TEST_F(RequestSetTrieTest, IdentityIsXorAtEverySubtree)
{
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(500));
    /* Every subtree's identity = XOR of its own members. */
    auto walk = [&](const FrozenNode & n, auto & self) -> void {
        auto members = n.allMembers();
        EXPECT_EQ(n.hash, xorMembers(members));
        if (!n.isLeaf())
            for (const auto & child : n.asInternal().slots)
                if (child)
                    self(*child, self);
    };
    walk(*root, walk);
}

TEST_F(RequestSetTrieTest, IdentityInvariantUnderInsertOrder)
{
    FrozenNodeCache cache;
    /* Same set, different insertion orders → same interned pointer. */
    std::vector<Hash> forward = hashes(300);
    std::vector<Hash> reverse(forward.rbegin(), forward.rend());
    auto a = cache.internSet(forward);
    auto b = cache.internSet(reverse);
    EXPECT_EQ(a, b);
}

/* ─────────────────────────────────────────────────────────────────────
   Structural determinism / canonical form
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, IndependentCachesProduceSameHash)
{
    FrozenNodeCache a, b;
    auto rootA = a.internSet(hashes(100));
    auto rootB = b.internSet(hashes(100));
    EXPECT_NE(rootA, rootB);            // distinct FrozenNode instances
    EXPECT_EQ(rootA->hash, rootB->hash); // same identity
    EXPECT_EQ(rootA->size(), rootB->size());
}

TEST_F(RequestSetTrieTest, InternalDepthReflectsBitGroupPartitioning)
{
    /* With uniform random hashes and ≥ 32 members, depth 0 partitions
       into most of the 16 slots — divergence happens at depth 0, so
       the root Internal has depth 0. */
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(200));
    ASSERT_FALSE(root->isLeaf());
    EXPECT_EQ(root->asInternal().depth, 0u);
    /* At least two slots must be populated (otherwise we would have
       skipped-single-slot to a deeper depth). */
    size_t populated = 0;
    for (const auto & slot : root->asInternal().slots)
        if (slot)
            ++populated;
    EXPECT_GE(populated, 2u);
}

TEST_F(RequestSetTrieTest, ChildSlotIndexMatchesMemberBitGroup)
{
    /* Every member reachable through slot i at depth d must satisfy
       slotFor(m, d) == i. Deterministically enforced by construction. */
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(300));
    ASSERT_FALSE(root->isLeaf());
    const auto & inter = root->asInternal();
    for (size_t i = 0; i < RADIX; ++i) {
        if (!inter.slots[i])
            continue;
        for (const auto & m : inter.slots[i]->allMembers())
            EXPECT_EQ(slotFor(m, inter.depth), i);
    }
}

/* ─────────────────────────────────────────────────────────────────────
   allMembers ordering — HAMT slot walk produces byte-lex order because
   depth-0 uses the top nibble
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, AllMembersIsLexSorted)
{
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(200));
    auto all = root->allMembers();
    EXPECT_TRUE(std::is_sorted(all.begin(), all.end()));
}

/* ─────────────────────────────────────────────────────────────────────
   Sharing — inserting one member changes only the path to it
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, InsertingOneMemberSharesSiblingSubtrees)
{
    /* Build a large set; then a second set that adds one member.
       At the root Internal (depth 0), the new member falls into
       exactly one slot. All OTHER slots should be shared by pointer
       identity with the original tree. */
    FrozenNodeCache cache;
    auto base = hashes(500);
    auto rootA = cache.internSet(base);
    ASSERT_FALSE(rootA->isLeaf());

    auto extra = h(9999);
    auto extended = base;
    extended.push_back(extra);
    auto rootB = cache.internSet(extended);
    ASSERT_FALSE(rootB->isLeaf());
    ASSERT_NE(rootA, rootB);

    /* Both trees at depth 0 with the same divergence pattern. */
    EXPECT_EQ(rootA->asInternal().depth, rootB->asInternal().depth);
    size_t changedSlot = slotFor(extra, rootA->asInternal().depth);

    size_t shared = 0;
    for (size_t i = 0; i < RADIX; ++i) {
        if (i == changedSlot)
            continue;
        EXPECT_EQ(rootA->asInternal().slots[i], rootB->asInternal().slots[i])
            << "slot " << i << " should be pointer-identical (unchanged)";
        if (rootA->asInternal().slots[i])
            ++shared;
    }
    EXPECT_GT(shared, 0u) << "test seed didn't populate any non-changed slot";
}

/* ─────────────────────────────────────────────────────────────────────
   Cache dedup — same members → same FrozenNodePtr
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, CacheDedupesByIdentity)
{
    FrozenNodeCache cache;
    auto a = cache.internSet(hashes(50));
    auto b = cache.internSet(hashes(50));
    EXPECT_EQ(a, b);
    auto looked = cache.lookup(a->hash);
    ASSERT_TRUE(looked.has_value());
    EXPECT_EQ(*looked, a);
}

TEST_F(RequestSetTrieTest, CacheMissReturnsNullopt)
{
    FrozenNodeCache cache;
    EXPECT_FALSE(cache.lookup(h(42)).has_value());
}

/* ─────────────────────────────────────────────────────────────────────
   Payload round-trip
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, LeafPayloadRoundTrip)
{
    FrozenNodeCache src, dst;
    auto leaf = src.internSet(hashes(5));
    ASSERT_TRUE(leaf->isLeaf());
    auto payload = leaf->toPayload();
    ASSERT_EQ(payload[0], char(0x00));
    auto round = dst.intern(leaf->hash, payload);
    EXPECT_EQ(round->hash, leaf->hash);
    EXPECT_EQ(round->size(), leaf->size());
}

TEST_F(RequestSetTrieTest, InternalPayloadRequiresChildrenCached)
{
    FrozenNodeCache src, dst;
    auto root = src.internSet(hashes(100));
    ASSERT_FALSE(root->isLeaf());

    /* Try to intern root's payload before its children — must throw. */
    auto payload = root->toPayload();
    EXPECT_THROW(dst.intern(root->hash, payload), Error);

    /* Populate dst with all descendants children-before-parent, then
       root interns successfully. */
    auto walk = [&](const FrozenNode & node, auto & self) -> void {
        if (!node.isLeaf())
            for (const auto & child : node.asInternal().slots)
                if (child)
                    self(*child, self);
        dst.intern(node.hash, node.toPayload());
    };
    walk(*root, walk);
    auto looked = dst.lookup(root->hash);
    ASSERT_TRUE(looked.has_value());
    EXPECT_EQ((*looked)->hash, root->hash);
    /* And reconstituted trees answer contains correctly. */
    for (const auto & m : root->allMembers())
        EXPECT_TRUE((*looked)->contains(m));
}

/* ─────────────────────────────────────────────────────────────────────
   MutableNode
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, MutableFreezeEqualsInternSet)
{
    FrozenNodeCache cache;
    MutableNode mut;
    for (auto & m : hashes(50))
        mut.insert(m);
    auto frozen = mut.freeze(cache);
    auto direct = cache.internSet(hashes(50));
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
}

TEST_F(RequestSetTrieTest, MutableFreezeIsCached)
{
    FrozenNodeCache cache;
    MutableNode mut;
    for (auto & m : hashes(20))
        mut.insert(m);
    auto f1 = mut.freeze(cache);
    auto f2 = mut.freeze(cache);
    EXPECT_EQ(f1, f2);
}

TEST_F(RequestSetTrieTest, MutableIncrementalFreezeReusesFrozenSubtrees)
{
    /* 500 insert+freeze cycles. Under the COW mutable impl each freeze
       walks only the modified slot-path (O(depth)) — unchanged sibling
       subtrees stay as frozen refs and don't intern. Total intern
       attempts should therefore scale with steps × depth, not with
       total tree size. A bulk-rebuild MutableNode would cost roughly
       steps × N/16 ≈ 500 * 31 ≈ 15k intern calls; the COW impl
       should be far below that. */
    FrozenNodeCache cache;
    MutableNode mut;
    for (uint64_t i = 0; i < 500; ++i) {
        mut.insert(h(i));
        auto frozen = mut.freeze(cache);
        EXPECT_EQ(frozen->size(), i + 1);
        EXPECT_TRUE(frozen->contains(h(i)));
    }
    auto perCycle = static_cast<double>(cache.internAttempts()) / 500.0;
    EXPECT_LT(perCycle, 12.0)
        << "COW freeze should touch O(depth) nodes per insert; per-cycle="
        << perCycle << " total=" << cache.internAttempts();
    /* And a re-freeze without mutation is free (cachedFrozen hits). */
    auto attemptsBefore = cache.internAttempts();
    auto again = mut.freeze(cache);
    EXPECT_EQ(again->size(), 500u);
    EXPECT_EQ(cache.internAttempts(), attemptsBefore);
}

TEST_F(RequestSetTrieTest, MutableSeededFromFrozen)
{
    FrozenNodeCache cache;
    auto seed = cache.internSet(hashes(30));
    MutableNode mut(seed);
    EXPECT_EQ(mut.size(), 30u);
    for (auto & m : hashes(30))
        EXPECT_TRUE(mut.contains(m));
    mut.insert(h(9999));
    EXPECT_TRUE(mut.contains(h(9999)));
    EXPECT_EQ(mut.size(), 31u);
    /* Original seed unchanged. */
    EXPECT_EQ(seed->size(), 30u);
    EXPECT_FALSE(seed->contains(h(9999)));
}

/* ─────────────────────────────────────────────────────────────────────
   Set operations
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, DifferenceSameSetIsEmpty)
{
    FrozenNodeCache cache;
    auto a = cache.internSet(hashes(50));
    EXPECT_TRUE(difference(*a, *a).empty());
}

TEST_F(RequestSetTrieTest, DifferenceProperSubsetLosesShared)
{
    FrozenNodeCache cache;
    std::vector<Hash> full, sub;
    for (size_t i = 0; i < 30; ++i) full.push_back(h(i));
    for (size_t i = 0; i < 10; ++i) sub.push_back(h(i));
    auto fullN = cache.internSet(full);
    auto subN  = cache.internSet(sub);
    auto d = difference(*fullN, *subN);
    EXPECT_EQ(d.size(), 20u);
    EXPECT_TRUE(difference(*subN, *fullN).empty());
}

TEST_F(RequestSetTrieTest, IsSubsetCases)
{
    FrozenNodeCache cache;
    auto a = cache.internSet({h(1), h(2), h(3)});
    auto b = cache.internSet({h(1), h(2), h(3), h(4), h(5)});
    EXPECT_TRUE(isSubset(*a, *b));
    EXPECT_FALSE(isSubset(*b, *a));
    EXPECT_TRUE(isSubset(*a, *a));
    auto empty = cache.internSet({});
    EXPECT_TRUE(isSubset(*empty, *a));
    EXPECT_FALSE(isSubset(*a, *empty));
}

TEST_F(RequestSetTrieTest, IntersectionSameSetIsSelf)
{
    FrozenNodeCache cache;
    auto a = cache.internSet(hashes(50));
    auto both = intersection(a, a, cache);
    EXPECT_EQ(both, a);
}

TEST_F(RequestSetTrieTest, IntersectionDisjointIsEmpty)
{
    FrozenNodeCache cache;
    std::vector<Hash> aM, bM;
    for (size_t i = 0; i < 20; ++i) aM.push_back(h(i));
    for (size_t i = 100; i < 120; ++i) bM.push_back(h(i));
    auto a = cache.internSet(aM);
    auto b = cache.internSet(bM);
    auto ab = intersection(a, b, cache);
    EXPECT_EQ(ab->size(), 0u);
    EXPECT_TRUE(ab->isLeaf());
}

TEST_F(RequestSetTrieTest, IntersectionSubsetReturnsSubset)
{
    FrozenNodeCache cache;
    std::vector<Hash> full, sub;
    for (size_t i = 0; i < 30; ++i) full.push_back(h(i));
    for (size_t i = 0; i < 10; ++i) sub.push_back(h(i));
    auto fullN = cache.internSet(full);
    auto subN  = cache.internSet(sub);
    EXPECT_EQ(intersection(fullN, subN, cache), subN);
    EXPECT_EQ(intersection(subN, fullN, cache), subN);
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
    EXPECT_EQ(ab->size(), 5u);
    for (size_t i = 25; i < 30; ++i) EXPECT_TRUE(ab->contains(h(i)));
    for (size_t i = 0; i < 25; ++i)  EXPECT_FALSE(ab->contains(h(i)));
    for (size_t i = 30; i < 60; ++i) EXPECT_FALSE(ab->contains(h(i)));
}

TEST_F(RequestSetTrieTest, IntersectionCommutative)
{
    FrozenNodeCache cache;
    auto a = cache.internSet(hashes(50));
    std::vector<Hash> bM;
    for (size_t i = 3; i < 45; ++i) bM.push_back(h(i));
    auto b = cache.internSet(bM);
    EXPECT_EQ(intersection(a, b, cache), intersection(b, a, cache));
}

/* ─────────────────────────────────────────────────────────────────────
   Persistence
   ───────────────────────────────────────────────────────────────────── */

TEST_F(RequestSetTrieTest, PersistWalksTreeOnce)
{
    FrozenNodeCache cache;
    auto root = cache.internSet(hashes(200));

    size_t total = 0;
    auto count = [&](const FrozenNode & n, auto & self) -> void {
        ++total;
        if (!n.isLeaf())
            for (const auto & child : n.asInternal().slots)
                if (child)
                    self(*child, self);
    };
    count(*root, count);

    std::vector<Hash> writes;
    FrozenNodeCache::PersistSink sink =
        [&](const Hash & h, std::string_view) { writes.push_back(h); };
    cache.persist(root, sink);
    EXPECT_EQ(writes.size(), total);

    /* Idempotent — second persist enqueues nothing. */
    writes.clear();
    cache.persist(root, sink);
    EXPECT_TRUE(writes.empty());
}

TEST_F(RequestSetTrieTest, PersistIncrementalOnlyWritesNewSubtrees)
{
    FrozenNodeCache cache;
    auto rootA = cache.internSet(hashes(200));
    std::vector<Hash> initialWrites;
    FrozenNodeCache::PersistSink sink =
        [&](const Hash & h, std::string_view) { initialWrites.push_back(h); };
    cache.persist(rootA, sink);
    auto initialCount = initialWrites.size();

    /* Add 5 members. Only the modified subtrees on the path to each
       new member are newly-created; all others remain persisted. */
    auto extended = hashes(200);
    for (uint64_t i = 1000; i < 1005; ++i) extended.push_back(h(i));
    auto rootB = cache.internSet(extended);

    initialWrites.clear();
    cache.persist(rootB, sink);
    EXPECT_GT(initialWrites.size(), 0u);
    EXPECT_LT(initialWrites.size(), initialCount)
        << "incremental persist must not rewrite the whole tree";
}

} // namespace nix::trace::rst
