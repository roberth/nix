#pragma once
/**
 * @file
 * In-memory RequestSet trie with two node representations:
 *
 * - FrozenNode: immutable, content-addressed, matches the DB payload
 *   format. Shared by hash via a `FrozenNodeCache`. Its subtree
 *   references are all `FrozenNodePtr`. Payload round-trips exactly.
 *
 * - MutableNode: algorithmic scratch. Children can be either owned
 *   `unique_ptr<MutableNode>` or shared `FrozenNodePtr` (COW: the
 *   immutable child is walked read-only until we need to modify, at
 *   which point it's cloned into a mutable copy).
 *
 * Freeze operation walks the mutable tree, interning each node by its
 * hash into the cache. Existing frozen subtrees short-circuit — no
 * rebuild if the hash is already known.
 *
 * Set operations (`difference`, `isSubset`, ...) parallel-walk two
 * frozen trees; any subtree pair with equal hash collapses to a
 * no-op. Cost tracks the size of the difference, not the size of
 * either input.
 */

#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/hash.hh"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nix::trace::rst {

/** Bits per trie level. Matches the DB storage format. */
constexpr int TRIE_RADIX_BITS = 4;
constexpr int TRIE_RADIX = 1 << TRIE_RADIX_BITS; // 16
constexpr size_t TRIE_SPLIT_THRESHOLD = 16;

/** Bucket index at trie depth `d` for hash `h`: the top TRIE_RADIX_BITS
    bits starting at bit `d * TRIE_RADIX_BITS`, MSB first. */
uint8_t bucketAt(const Hash & h, int depth);

class FrozenNode;
using FrozenNodePtr = std::shared_ptr<const FrozenNode>;

/** Content-addressed trie node. Immutable after construction. Instances
    are shared via `FrozenNodePtr`; the identity is `hash`, computed at
    construction from the byte payload. */
class FrozenNode
{
public:
    /** Sorted lex-ascending, size ≤ TRIE_SPLIT_THRESHOLD. */
    struct Leaf
    {
        std::vector<Hash> members;
    };

    /** Sparse, sorted by bucket index. Matches DB payload encoding. */
    struct Internal
    {
        std::vector<std::pair<uint8_t, FrozenNodePtr>> children;
    };

    std::variant<Leaf, Internal> body;
    Hash hash{HashAlgorithm::SHA256};

    bool isLeaf() const noexcept { return std::holds_alternative<Leaf>(body); }
    const Leaf & asLeaf() const { return std::get<Leaf>(body); }
    const Internal & asInternal() const { return std::get<Internal>(body); }

    /** Total number of member hashes in this subtree. */
    size_t size() const noexcept;

    /** O(log_radix N) membership check. */
    bool contains(const Hash & h) const noexcept;

    /** Serialize to the DB payload byte format:
        Leaf:     [0x00] hash_1 hash_2 ... hash_n
        Internal: [0x01] (bucket_byte, child_hash)+ */
    std::string toPayload() const;

    /** Materialize all members into a flat vector (recursive walk).
        Preserves the trie's lex-sorted order across bucket boundaries. */
    std::vector<Hash> allMembers() const;

    /* Constructed only by FrozenNodeCache; user code interacts via
       FrozenNodePtr. Not private so make_shared can invoke; users
       should not construct directly. */
    FrozenNode() = default;
};

/** Global cache: `Hash → FrozenNodePtr`. Deduplicates across all
    interning operations, both from decoded payloads (DB read) and from
    freshly-built member lists (writer). */
class FrozenNodeCache
{
public:
    /** Look up by hash. Returns nullopt if the node isn't cached. */
    std::optional<FrozenNodePtr> lookup(const Hash & hash) const;

    /** Intern a node from its raw payload bytes and precomputed hash.
        If already cached under this hash, returns the existing pointer
        without re-parsing. */
    FrozenNodePtr intern(const Hash & hash, std::string_view payload);

    /** Build (or reuse) a leaf node with the given members. Sorts +
        dedups internally; hash and interning are handled here. */
    FrozenNodePtr internLeaf(std::vector<Hash> members);

    /** Build (or reuse) a set from the given members. If the set fits
        in a single leaf (≤ TRIE_SPLIT_THRESHOLD) returns a leaf;
        otherwise buckets recursively into internal + leaf nodes. */
    FrozenNodePtr internSet(std::vector<Hash> members);

    /** Intern an internal node from a pre-built children list. Children
        must already be interned. Recomputes hash. */
    FrozenNodePtr internInternal(std::vector<std::pair<uint8_t, FrozenNodePtr>> children);

    /** Total number of `internLeaf` / `internInternal` calls (i.e.,
        payload-and-hash operations). Used by tests to prove that
        freeze reuses unchanged subtrees. */
    size_t internAttempts() const noexcept { return internAttemptCount; }

private:
    std::unordered_map<Hash, FrozenNodePtr> byHash;
    size_t internAttemptCount = 0;
};

/** Mutable trie node. Children may be owned mutable subtrees or shared
    frozen subtrees. Mutation walks down; on entering a frozen subtree
    we materialize it into a mutable copy (COW) before descending
    further.

    NOT thread-safe. Intended for single-writer algorithmic use, then
    freeze at the end for sharing/persistence. */
class MutableNode
{
public:
    struct Leaf
    {
        std::vector<Hash> members; // kept sorted
    };

    /** Child slot in an internal node. At most one of `mut` / `frozen`
        is populated. Absent slot (both null) = no members in that
        bucket. */
    struct Child
    {
        std::unique_ptr<MutableNode> mut;
        FrozenNodePtr frozen;

        bool empty() const noexcept { return !mut && !frozen; }
        size_t size() const noexcept;
        bool contains(const Hash & h, int depth) const noexcept;
    };

    struct Internal
    {
        std::array<Child, TRIE_RADIX> children;
    };

    std::variant<Leaf, Internal> body;

    /** Cached result of the last freeze, if still valid (no mutation
        since). Invalidated at the top of every insert. Set at the end
        of every freeze. Enables O(depth) freeze after O(depth) insert
        when the caller freezes after every step. */
    mutable FrozenNodePtr cachedFrozen;

    /** Fresh empty mutable tree (empty leaf). */
    MutableNode() = default;

    /** Wrap a frozen root as a mutable tree. The frozen subtree is
        referenced (not copied) until modification descends into it. */
    explicit MutableNode(FrozenNodePtr root);

    void insert(const Hash & h);
    bool contains(const Hash & h) const noexcept;
    size_t size() const noexcept;

    /** Freeze into a shared FrozenNodePtr. Reuses `cachedFrozen` if
        no mutation happened since the last freeze. Recursively reuses
        each child's `cachedFrozen`, so incremental build-and-freeze
        stays O(depth of the modified path) per step, not O(tree
        size). */
    FrozenNodePtr freeze(FrozenNodeCache & cache);

private:
    void insertAtDepth(const Hash & h, int depth);
    bool containsAtDepth(const Hash & h, int depth) const noexcept;
    /* Split a leaf that has just exceeded threshold into an internal
       node whose children hold the redistributed members. */
    void splitLeafAt(int depth);
};

/** Set operations on frozen trees. Parallel walk short-circuits at
    subtree pairs whose hashes match. */

/** A \ B — members present in A but not in B. */
std::vector<Hash> difference(const FrozenNode & a, const FrozenNode & b);

/** A ⊆ B — every member of A is a member of B. */
bool isSubset(const FrozenNode & a, const FrozenNode & b);

} // namespace nix::trace::rst
