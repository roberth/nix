#include "nix/expr/request-set-trie.hh"
#include "nix/util/error.hh"

#include <algorithm>
#include <cstring>

namespace nix::trace::rst {

/* ─────────────────────────────────────────────────────────────────────
   Bucket / payload primitives
   ───────────────────────────────────────────────────────────────────── */

uint8_t bucketAt(const Hash & h, int depth)
{
    const int bitOffset = depth * TRIE_RADIX_BITS;
    const int byteIdx = bitOffset / 8;
    const int bitInByte = bitOffset % 8;
    uint16_t word = (uint16_t) h.hash[byteIdx] << 8;
    if (byteIdx + 1 < (int) h.hashSize)
        word |= (uint16_t) h.hash[byteIdx + 1];
    return (uint8_t) ((word >> (16 - TRIE_RADIX_BITS - bitInByte)) & ((1 << TRIE_RADIX_BITS) - 1));
}

static std::string leafPayload(const std::vector<Hash> & sortedMembers)
{
    std::string out;
    out.reserve(1 + sortedMembers.size() * tracingHashSize);
    out.push_back(0x00);
    for (const auto & h : sortedMembers)
        out.append(reinterpret_cast<const char *>(h.hash), h.hashSize);
    return out;
}

static std::string internalPayload(const std::vector<std::pair<uint8_t, FrozenNodePtr>> & children)
{
    std::string out;
    out.reserve(1 + children.size() * (1 + tracingHashSize));
    out.push_back(0x01);
    for (const auto & [bucket, child] : children) {
        out.push_back(static_cast<char>(bucket));
        out.append(reinterpret_cast<const char *>(child->hash.hash), child->hash.hashSize);
    }
    return out;
}

static std::vector<Hash> sortAndDedup(std::vector<Hash> members)
{
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    return members;
}

/* Identity primitive: XOR two tracing hashes byte-wise. Zero is the
   identity element (XOR-fold over an empty set → zero hash). */
static Hash xorHashes(const Hash & a, const Hash & b)
{
    Hash out(HashAlgorithm::SHA256);
    out.hashSize = tracingHashSize;
    for (size_t i = 0; i < tracingHashSize; ++i)
        out.hash[i] = a.hash[i] ^ b.hash[i];
    return out;
}

static Hash zeroHash()
{
    Hash z(HashAlgorithm::SHA256);
    z.hashSize = tracingHashSize;
    return z;  // hash bytes zero-initialised by the array default
}

static Hash xorOfLeafMembers(const std::vector<Hash> & members)
{
    Hash acc = zeroHash();
    for (const auto & m : members)
        acc = xorHashes(acc, m);
    return acc;
}

static Hash xorOfChildren(const std::vector<std::pair<uint8_t, FrozenNodePtr>> & children)
{
    Hash acc = zeroHash();
    for (const auto & [_, c] : children)
        acc = xorHashes(acc, c->hash);
    return acc;
}

/* ─────────────────────────────────────────────────────────────────────
   FrozenNode
   ───────────────────────────────────────────────────────────────────── */

size_t FrozenNode::size() const noexcept
{
    if (isLeaf())
        return asLeaf().members.size();
    size_t n = 0;
    for (const auto & [_, child] : asInternal().children)
        n += child->size();
    return n;
}

bool FrozenNode::contains(const Hash & h) const noexcept
{
    auto walk = [](const FrozenNode & node, const Hash & target, int depth, auto & self) -> bool {
        if (node.isLeaf()) {
            const auto & m = node.asLeaf().members;
            return std::binary_search(m.begin(), m.end(), target);
        }
        auto b = bucketAt(target, depth);
        for (const auto & [bucket, child] : node.asInternal().children) {
            if (bucket == b)
                return self(*child, target, depth + 1, self);
            if (bucket > b)
                return false; // children sorted by bucket
        }
        return false;
    };
    return walk(*this, h, 0, walk);
}

std::string FrozenNode::toPayload() const
{
    if (isLeaf())
        return leafPayload(asLeaf().members);
    return internalPayload(asInternal().children);
}

std::vector<Hash> FrozenNode::allMembers() const
{
    std::vector<Hash> out;
    out.reserve(size());
    auto walk = [&](const FrozenNode & node, auto & self) -> void {
        if (node.isLeaf()) {
            const auto & m = node.asLeaf().members;
            out.insert(out.end(), m.begin(), m.end());
        } else {
            for (const auto & [_, child] : node.asInternal().children)
                self(*child, self);
        }
    };
    walk(*this, walk);
    return out;
}

/* ─────────────────────────────────────────────────────────────────────
   FrozenNodeCache
   ───────────────────────────────────────────────────────────────────── */

std::optional<FrozenNodePtr> FrozenNodeCache::lookup(const Hash & hash) const
{
    auto it = byHash.find(hash);
    if (it == byHash.end())
        return std::nullopt;
    return it->second;
}

FrozenNodePtr FrozenNodeCache::intern(const Hash & hash, std::string_view payload)
{
    if (auto existing = lookup(hash))
        return *existing;
    if (payload.empty())
        throw Error("rst: malformed frozen node payload (empty)");
    auto sp = std::make_shared<FrozenNode>();
    sp->hash = hash;
    if (payload[0] == 0x00) {
        FrozenNode::Leaf leaf;
        const size_t hs = tracingHashSize;
        if ((payload.size() - 1) % hs != 0)
            throw Error("rst: malformed frozen leaf (size=%d)", payload.size());
        leaf.members.reserve((payload.size() - 1) / hs);
        for (size_t i = 1; i + hs <= payload.size(); i += hs) {
            Hash h(HashAlgorithm::SHA256);
            h.hashSize = hs;
            std::memcpy(h.hash, payload.data() + i, hs);
            leaf.members.push_back(h);
        }
        sp->body = std::move(leaf);
    } else if (payload[0] == 0x01) {
        FrozenNode::Internal inter;
        const size_t hs = tracingHashSize;
        const size_t entrySize = 1 + hs;
        if ((payload.size() - 1) % entrySize != 0)
            throw Error("rst: malformed frozen internal node (size=%d)", payload.size());
        for (size_t i = 1; i + entrySize <= payload.size(); i += entrySize) {
            uint8_t bucket = static_cast<uint8_t>(payload[i]);
            Hash childHash(HashAlgorithm::SHA256);
            childHash.hashSize = hs;
            std::memcpy(childHash.hash, payload.data() + i + 1, hs);
            auto childPtr = lookup(childHash);
            if (!childPtr)
                throw Error("rst: intern of internal node references child not yet cached");
            inter.children.emplace_back(bucket, *childPtr);
        }
        sp->body = std::move(inter);
    } else {
        throw Error("rst: malformed frozen node (unknown tag %d)", (int) (unsigned char) payload[0]);
    }
    FrozenNodePtr r(std::static_pointer_cast<const FrozenNode>(sp));
    byHash.emplace(hash, r);
    return r;
}

FrozenNodePtr FrozenNodeCache::internLeaf(std::vector<Hash> members)
{
    ++internAttemptCount;
    auto sorted = sortAndDedup(std::move(members));
    /* Identity = XOR of leaf members. Same value for any subtree with
       these members, regardless of whether it's stored as a flat leaf
       or has been split into an internal — enables sharing across
       structural transitions and short-circuit at any depth in
       diff/intersect. */
    auto hash = xorOfLeafMembers(sorted);
    if (auto existing = lookup(hash))
        return *existing;
    auto sp = std::make_shared<FrozenNode>();
    sp->hash = hash;
    sp->body = FrozenNode::Leaf{std::move(sorted)};
    FrozenNodePtr r(std::static_pointer_cast<const FrozenNode>(sp));
    byHash.emplace(hash, r);
    return r;
}

FrozenNodePtr FrozenNodeCache::internInternal(std::vector<std::pair<uint8_t, FrozenNodePtr>> children)
{
    ++internAttemptCount;
    std::sort(children.begin(), children.end(),
        [](const auto & a, const auto & b) { return a.first < b.first; });
    /* Identity = XOR of children's identities. Since each child's
       identity is XOR-of-its-members, this transitively equals XOR
       of all leaf request hashes in the subtree — same rule as leaf
       nodes. */
    auto hash = xorOfChildren(children);
    if (auto existing = lookup(hash))
        return *existing;
    auto sp = std::make_shared<FrozenNode>();
    sp->hash = hash;
    sp->body = FrozenNode::Internal{std::move(children)};
    FrozenNodePtr r(std::static_pointer_cast<const FrozenNode>(sp));
    byHash.emplace(hash, r);
    return r;
}

void FrozenNodeCache::persist(const FrozenNodePtr & root, PersistSink & sink)
{
    if (root->persisted)
        return;
    auto walk = [&](const FrozenNode & n, auto & self) -> void {
        if (n.persisted)
            return;
        if (!n.isLeaf())
            for (const auto & [_, child] : n.asInternal().children)
                self(*child, self);
        sink(n.hash, n.toPayload());
        n.persisted = true;
    };
    walk(*root, walk);
}

FrozenNodePtr FrozenNodeCache::internSet(std::vector<Hash> members)
{
    auto sorted = sortAndDedup(std::move(members));
    auto build = [this](std::vector<Hash> ms, int depth, auto & self) -> FrozenNodePtr {
        if (ms.size() <= TRIE_SPLIT_THRESHOLD)
            return internLeaf(std::move(ms));
        std::array<std::vector<Hash>, TRIE_RADIX> buckets;
        for (auto & m : ms)
            buckets[bucketAt(m, depth)].push_back(std::move(m));
        std::vector<std::pair<uint8_t, FrozenNodePtr>> children;
        for (uint8_t i = 0; i < TRIE_RADIX; ++i) {
            if (buckets[i].empty())
                continue;
            children.emplace_back(i, self(std::move(buckets[i]), depth + 1, self));
        }
        return internInternal(std::move(children));
    };
    return build(std::move(sorted), 0, build);
}

/* ─────────────────────────────────────────────────────────────────────
   MutableNode
   ───────────────────────────────────────────────────────────────────── */

size_t MutableNode::Child::size() const noexcept
{
    if (mut) return mut->size();
    if (frozen) return frozen->size();
    return 0;
}

bool MutableNode::Child::contains(const Hash & h, int depth) const noexcept
{
    if (mut) return mut->containsAtDepth(h, depth);
    if (frozen) return frozen->contains(h);
    return false;
}

MutableNode::MutableNode(FrozenNodePtr root)
{
    /* Fresh mutable that references the frozen root. Eagerly
       materialize the top level: Leaf → copy members. Internal →
       wrap each child as a Child{.frozen=childPtr}. */
    if (root->isLeaf()) {
        body = Leaf{root->asLeaf().members};
    } else {
        Internal inter;
        for (const auto & [bucket, child] : root->asInternal().children) {
            /* ref → shared_ptr via implicit conversion. */
            inter.children[bucket].frozen = child;
        }
        body = std::move(inter);
    }
}

size_t MutableNode::size() const noexcept
{
    if (std::holds_alternative<Leaf>(body))
        return std::get<Leaf>(body).members.size();
    size_t n = 0;
    for (const auto & c : std::get<Internal>(body).children)
        n += c.size();
    return n;
}

bool MutableNode::contains(const Hash & h) const noexcept
{
    return containsAtDepth(h, 0);
}

bool MutableNode::containsAtDepth(const Hash & h, int depth) const noexcept
{
    if (auto * leaf = std::get_if<Leaf>(&body)) {
        return std::binary_search(leaf->members.begin(), leaf->members.end(), h);
    }
    auto b = bucketAt(h, depth);
    return std::get<Internal>(body).children[b].contains(h, depth + 1);
}

void MutableNode::insert(const Hash & h)
{
    insertAtDepth(h, 0);
}

void MutableNode::insertAtDepth(const Hash & h, int depth)
{
    /* Any mutation invalidates the cached freeze at this node. Only
       the ancestor chain of the modified leaf sees invalidation;
       untouched sibling subtrees keep their `cachedFrozen` and are
       reused wholesale on the next freeze. */
    cachedFrozen = nullptr;
    if (auto * leaf = std::get_if<Leaf>(&body)) {
        auto pos = std::lower_bound(leaf->members.begin(), leaf->members.end(), h);
        if (pos != leaf->members.end() && *pos == h)
            return; // duplicate
        leaf->members.insert(pos, h);
        if (leaf->members.size() > TRIE_SPLIT_THRESHOLD)
            splitLeafAt(depth);
        return;
    }
    auto & inter = std::get<Internal>(body);
    auto b = bucketAt(h, depth);
    auto & child = inter.children[b];
    if (child.empty()) {
        child.mut = std::make_unique<MutableNode>();
        child.mut->insertAtDepth(h, depth + 1);
        return;
    }
    if (child.frozen) {
        /* COW: materialize the frozen child into a mutable copy. */
        auto frozen = std::move(child.frozen);
        child.mut = std::make_unique<MutableNode>(FrozenNodePtr(frozen));
    }
    child.mut->insertAtDepth(h, depth + 1);
}

void MutableNode::splitLeafAt(int depth)
{
    auto leaf = std::move(std::get<Leaf>(body));
    Internal inter;
    for (auto & m : leaf.members) {
        auto b = bucketAt(m, depth);
        auto & child = inter.children[b];
        if (!child.mut)
            child.mut = std::make_unique<MutableNode>();
        child.mut->insertAtDepth(m, depth + 1);
    }
    body = std::move(inter);
}

FrozenNodePtr MutableNode::freeze(FrozenNodeCache & cache)
{
    /* Fast path: this node hasn't mutated since the last freeze. */
    if (cachedFrozen)
        return FrozenNodePtr(cachedFrozen);
    if (auto * leaf = std::get_if<Leaf>(&body)) {
        auto r = cache.internLeaf(leaf->members);
        cachedFrozen = r;  // ref → shared_ptr
        return r;
    }
    auto & inter = std::get<Internal>(body);
    std::vector<std::pair<uint8_t, FrozenNodePtr>> frozenChildren;
    for (uint8_t i = 0; i < TRIE_RADIX; ++i) {
        auto & c = inter.children[i];
        if (c.empty()) continue;
        /* Frozen child: reuse directly. Mutable child: recurse — its
           own `cachedFrozen` short-circuits sibling subtrees that
           didn't change. */
        FrozenNodePtr child = c.frozen ? FrozenNodePtr(c.frozen) : c.mut->freeze(cache);
        frozenChildren.emplace_back(i, std::move(child));
    }
    auto r = cache.internInternal(std::move(frozenChildren));
    cachedFrozen = r;
    return r;
}

/* ─────────────────────────────────────────────────────────────────────
   Set operations: parallel-walk with hash-equality short-circuit
   ───────────────────────────────────────────────────────────────────── */

std::vector<Hash> difference(const FrozenNode & a, const FrozenNode & b)
{
    std::vector<Hash> out;
    auto walk = [&](const FrozenNode & na, const FrozenNode & nb, auto & self) -> void {
        if (na.hash == nb.hash) return; // identical subtree, nothing to add
        if (na.isLeaf() && nb.isLeaf()) {
            const auto & am = na.asLeaf().members;
            const auto & bm = nb.asLeaf().members;
            std::set_difference(am.begin(), am.end(), bm.begin(), bm.end(),
                std::back_inserter(out));
            return;
        }
        if (na.isLeaf()) {
            /* Any A-member not in B is in the diff. */
            for (const auto & m : na.asLeaf().members)
                if (!nb.contains(m))
                    out.push_back(m);
            return;
        }
        if (nb.isLeaf()) {
            /* Enumerate A; skip those present in B. */
            auto am = na.allMembers();
            for (const auto & m : am)
                if (!nb.contains(m))
                    out.push_back(m);
            return;
        }
        /* Both internal: parallel-walk by bucket. */
        const auto & ac = na.asInternal().children;
        const auto & bc = nb.asInternal().children;
        size_t i = 0, j = 0;
        while (i < ac.size()) {
            while (j < bc.size() && bc[j].first < ac[i].first) ++j;
            if (j == bc.size() || bc[j].first > ac[i].first) {
                /* A has content in bucket ac[i].first, B doesn't → all of A's subtree. */
                auto subMembers = ac[i].second->allMembers();
                out.insert(out.end(), subMembers.begin(), subMembers.end());
            } else {
                /* Both have content in the same bucket — recurse. */
                self(*ac[i].second, *bc[j].second, self);
            }
            ++i;
        }
    };
    walk(a, b, walk);
    return out;
}

FrozenNodePtr intersection(const FrozenNodePtr & a, const FrozenNodePtr & b, FrozenNodeCache & cache)
{
    /* Recursive: return the shared subtree pointer directly when
       hashes match (this is the whole point — no rebuild, no
       re-hash). Empty results converge on the interned empty leaf. */
    auto walk = [&](const FrozenNodePtr & na, const FrozenNodePtr & nb, auto & self) -> FrozenNodePtr {
        if (na->hash == nb->hash)
            return na; // could be either — pick one
        if (na->isLeaf() && nb->isLeaf()) {
            const auto & am = na->asLeaf().members;
            const auto & bm = nb->asLeaf().members;
            std::vector<Hash> intersected;
            std::set_intersection(am.begin(), am.end(), bm.begin(), bm.end(),
                std::back_inserter(intersected));
            return cache.internLeaf(std::move(intersected));
        }
        if (na->isLeaf()) {
            std::vector<Hash> intersected;
            for (const auto & m : na->asLeaf().members)
                if (nb->contains(m))
                    intersected.push_back(m);
            return cache.internLeaf(std::move(intersected));
        }
        if (nb->isLeaf())
            return self(nb, na, self); // symmetric
        /* Both internal — parallel bucket walk. */
        const auto & ac = na->asInternal().children;
        const auto & bc = nb->asInternal().children;
        std::vector<std::pair<uint8_t, FrozenNodePtr>> outChildren;
        size_t i = 0, j = 0;
        while (i < ac.size() && j < bc.size()) {
            if (ac[i].first < bc[j].first) { ++i; continue; }
            if (bc[j].first < ac[i].first) { ++j; continue; }
            auto childInter = self(ac[i].second, bc[j].second, self);
            if (childInter->size() > 0)
                outChildren.emplace_back(ac[i].first, std::move(childInter));
            ++i; ++j;
        }
        if (outChildren.empty())
            return cache.internLeaf({});
        return cache.internInternal(std::move(outChildren));
    };
    return walk(a, b, walk);
}

bool isSubset(const FrozenNode & a, const FrozenNode & b)
{
    auto walk = [&](const FrozenNode & na, const FrozenNode & nb, auto & self) -> bool {
        if (na.hash == nb.hash) return true;
        if (na.isLeaf() && nb.isLeaf()) {
            const auto & am = na.asLeaf().members;
            const auto & bm = nb.asLeaf().members;
            return std::includes(bm.begin(), bm.end(), am.begin(), am.end());
        }
        if (na.isLeaf()) {
            for (const auto & m : na.asLeaf().members)
                if (!nb.contains(m))
                    return false;
            return true;
        }
        if (nb.isLeaf()) {
            /* A is internal but B is a leaf. A ⊆ B possible only if
               every A member is one of B's ≤ threshold members. */
            auto am = na.allMembers();
            for (const auto & m : am)
                if (!nb.contains(m))
                    return false;
            return true;
        }
        const auto & ac = na.asInternal().children;
        const auto & bc = nb.asInternal().children;
        size_t j = 0;
        for (const auto & [aBucket, aChild] : ac) {
            while (j < bc.size() && bc[j].first < aBucket) ++j;
            if (j == bc.size() || bc[j].first > aBucket)
                return false; // A has content in a bucket B lacks
            if (!self(*aChild, *bc[j].second, self))
                return false;
        }
        return true;
    };
    return walk(a, b, walk);
}

} // namespace nix::trace::rst
