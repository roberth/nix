#pragma once
/**
 * @file
 * In-memory RequestSet store as a top-down HAMT (Hash Array Mapped Trie).
 *
 * Each internal node has RADIX=16 slots. At depth `d`, the slot for a
 * request hash `h` is `slotFor(h, d)` — the 4-bit group of `h` at bit
 * offset `d * RADIX_BITS`. Because slot indices are pure functions of
 * the content bits (not of position, insertion order, or history), two
 * sets with the same members always produce identical trees at every
 * subtree. This eliminates representational entropy that content-defined
 * chunking exposes on the tail of its size distribution.
 *
 * Flat leaves for small sets: a node with ≤ LEAF_MAX_MEMBERS members is
 * stored as a `Leaf` (sorted flat list). At more than that, the node
 * splits into an `Internal` and each slot's members recurse.
 *
 * Identity is XOR of the full member set. Two subtrees anywhere in the
 * tree with the same members have the same identity, so intersection,
 * difference, and isSubset can short-circuit at any hash-equal subtree
 * pair (cost tracks the size of the delta, not the size of either
 * input).
 */

#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/fun.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nix::trace::rst {

/** Bits per HAMT level. Each internal node has 2^RADIX_BITS = 16 slots
    keyed by the corresponding 4-bit group of a request hash. */
constexpr int RADIX_BITS = 4;
constexpr size_t RADIX = 1u << RADIX_BITS;

/** A node with at most this many members is stored as a flat sorted
    Leaf. On insertion crossing this threshold the node promotes to an
    Internal at its current depth. */
constexpr size_t LEAF_MAX_MEMBERS = RADIX;

/** Slot index for `h` at HAMT depth `d`. Depth 0 uses the top nibble
    of hash[0]; depth 1 uses the low nibble; depth 2 the top nibble of
    hash[1]; and so on. Callers walking down the tree pass their
    current depth. */
size_t slotFor(const Hash & h, size_t depth);

class FrozenNode;
/** Non-nullable shared reference to a FrozenNode. Held wherever a
    subtree reference is definite (return of intern/freeze, ops
    outputs). Nullable slots inside an Internal use
    `std::shared_ptr<const FrozenNode>` because empty is intrinsic. */
using FrozenNodePtr = ref<const FrozenNode>;

/** Content-addressed HAMT node. Immutable; identity is `hash`. */
class FrozenNode
{
public:
    /** Flat sorted (lex-ascending) member list. Size ≤ LEAF_MAX_MEMBERS. */
    struct Leaf
    {
        std::vector<Hash> members;
    };

    /** Sparse RADIX-slot array. Empty slots are `nullptr`. Slot index
        for a member `m` at this Internal's depth is `slotFor(m, depth)`.

        Depth may exceed (parent's depth + 1). When all of this
        subtree's members share the same nibble at some intermediate
        depth, we skip the "wrap" level rather than materialize a
        single-slot Internal. The wrap Internal would be redundant
        with its lone child — the same member set represented two ways
        (once with an outer envelope, once without). Skipping strips
        that representational entropy so a given member set always
        reduces to exactly one subtree shape. Diff, intersection, and
        subset can then short-circuit whenever two references point at
        subtrees with equivalent content, since matching content
        implies matching identity. */
    struct Internal
    {
        uint8_t depth;
        std::array<std::shared_ptr<const FrozenNode>, RADIX> slots;
    };

    std::variant<Leaf, Internal> body;
    Hash hash{HashAlgorithm::SHA256};

    /** Set true by `FrozenNodeCache::persist` once the payload has been
        enqueued for the writer thread. Already-persisted subtrees
        short-circuit the persist walk on subsequent freezes. */
    mutable bool persisted = false;

    /** Memoized flat member list. Populated on first `allMembers()`
        call and reused thereafter — since FrozenNodes are interned,
        one materialisation per unique set. */
    mutable std::optional<std::vector<Hash>> cachedAllMembers;

    bool isLeaf() const noexcept { return std::holds_alternative<Leaf>(body); }
    const Leaf & asLeaf() const { return std::get<Leaf>(body); }
    const Internal & asInternal() const { return std::get<Internal>(body); }

    /** Total number of member hashes in this subtree. */
    size_t size() const noexcept;

    /** Membership check by hash. O(depth) — walks slot-by-slot. */
    bool contains(const Hash & h) const noexcept;

    /** Serialize to DB payload bytes.
        Leaf:     [0x00] hash_1 hash_2 ... hash_n
        Internal: [0x01] bitmap_lo bitmap_hi child_hash_1 ... child_hash_k
        The bitmap is 16 bits (little-endian), one bit per populated
        slot; children follow in slot order. */
    std::string toPayload() const;

    /** Materialize all members into a flat vector via recursive walk.
        Order is lex-ascending because slot indices at every depth
        preserve the top-bit ordering. */
    std::vector<Hash> allMembers() const;

    FrozenNode() = default;
};

/** Global cache mapping `Hash → FrozenNodePtr`. Deduplicates across
    both freshly-built subtrees (writer) and payload-decoded subtrees
    (DB read). */
class FrozenNodeCache
{
public:
    /** Returns the cached FrozenNodePtr for `hash`, or nullopt if not
        cached. */
    std::optional<FrozenNodePtr> lookup(const Hash & hash) const;

    /** Reconstitute a node from DB payload bytes and its precomputed
        hash. Throws if the payload references child hashes that
        aren't already cached (readers walk children-before-parent). */
    FrozenNodePtr intern(const Hash & hash, std::string_view payload);

    /** Build (or reuse) a HAMT-shaped set from the given members.
        Sorts + dedups internally. */
    FrozenNodePtr internSet(std::vector<Hash> members);

    /** Intern a Leaf from an already-sorted, dedup'd member list.
        Used by MutableNode::freeze to intern a mutable leaf without
        the internSet sort/dedup pass. */
    FrozenNodePtr internLeafFromSorted(std::vector<Hash> sortedMembers);

    /** Intern an Internal from a pre-frozen slot array at the given
        depth. Used by MutableNode::freeze. Slots that are `nullptr`
        are treated as empty. If exactly one slot is populated the
        caller is expected to skip-single-slot beforehand (freeze
        does). */
    FrozenNodePtr internInternalFromSlots(
        uint8_t depth,
        const std::array<std::shared_ptr<const FrozenNode>, RADIX> & slots);

    /** Count of node-construction operations. Tests use this to
        verify that freeze reuses cached subtrees rather than
        rebuilding. */
    size_t internAttempts() const noexcept { return internAttemptCount; }

    /** Walk `root` post-order and call `sink(hash, payload)` for each
        node whose `persisted` bit is false, flipping it true. Already-
        persisted subtrees short-circuit. */
    using PersistSink = fun<void(const Hash &, std::string_view)>;
    void persist(const FrozenNodePtr & root, PersistSink & sink);

private:
    std::unordered_map<Hash, FrozenNodePtr> byHash;
    size_t internAttemptCount = 0;

    /** Internal helper: build a subtree from a member range at the
        given HAMT depth. */
    FrozenNodePtr build(std::vector<Hash> members, size_t depth);
};

/** Mutable HAMT — an insertion buffer with COW from a frozen seed.
    Insertion is O(depth) — walks down along the target slot, XORs
    identity contributions along the way. Freeze walks the mutable
    tree, interning each node; unchanged subtrees (still pointed to
    from before insertion) are already frozen and reuse their existing
    FrozenNodePtr.

    NOT thread-safe. */
/* Pimpl body for MutableNode. Definition lives in request-set-trie.cc. */
struct MutableNodeBody;

class MutableNode
{
public:
    MutableNode();
    ~MutableNode();
    MutableNode(MutableNode &&) noexcept;
    MutableNode & operator=(MutableNode &&) noexcept;

    /** Seed from a frozen root: the mutable is a COW wrapper — no
        immediate copy, only the modified slot-path clones on insert. */
    explicit MutableNode(FrozenNodePtr root);

    void insert(const Hash & h);
    bool contains(const Hash & h) const noexcept;
    size_t size() const noexcept;

    /** Freeze into a shared FrozenNodePtr; identity is the XOR of
        contained members. Repeated freezes without mutation return
        the cached pointer. */
    FrozenNodePtr freeze(FrozenNodeCache & cache);

private:
    std::unique_ptr<MutableNodeBody> body;
};

/** A \ B — members present in A but not in B. */
std::vector<Hash> difference(const FrozenNode & a, const FrozenNode & b);

/** A ⊆ B — every member of A is a member of B. */
bool isSubset(const FrozenNode & a, const FrozenNode & b);

/** A ∩ B — parallel walk over the HAMT slots with hash-equal
    short-circuit. Takes FrozenNodePtrs so an identical-subtree pair
    can be returned by handing back the caller's own reference. */
FrozenNodePtr intersection(const FrozenNodePtr & a, const FrozenNodePtr & b, FrozenNodeCache & cache);

/** A ∪ B — merged trie. Same-identity short-circuit; otherwise seed
    a MutableNode from the larger side and insert the smaller side's
    members. Under CoW MutableNode this preserves all shared subtrees
    of the larger side that aren't touched by the smaller side's
    inserts. Named `union_` because `union` is a C++ keyword. */
FrozenNodePtr union_(const FrozenNodePtr & a, const FrozenNodePtr & b, FrozenNodeCache & cache);

/** Enumerate child-node hashes referenced in an Internal payload
    without interning the node itself. For a Leaf payload, returns
    empty. Used by DB loaders that walk children-before-parent to
    satisfy `FrozenNodeCache::intern`'s "children must be cached"
    precondition. */
std::vector<Hash> childHashesInPayload(std::string_view payload);

} // namespace nix::trace::rst
