#include "nix/expr/request-set-trie.hh"
#include "nix/util/error.hh"

#include <algorithm>
#include <cstring>

namespace nix::trace::rst {

/* ─────────────────────────────────────────────────────────────────────
   Bit-group / identity primitives
   ───────────────────────────────────────────────────────────────────── */

size_t slotFor(const TracingHash & h, size_t depth)
{
    /* Each level consumes RADIX_BITS=4 bits, starting from the top of
       bytes[0]. Depth 0 → top nibble of byte 0; depth 1 → low nibble;
       depth 2 → top nibble of byte 1; etc. */
    size_t byte = depth / 2;
    bool highNibble = (depth % 2) == 0;
    unsigned char b = h.bytes[byte];
    return highNibble ? (b >> 4) : (b & 0x0F);
}

static std::vector<TracingHash> sortAndDedup(std::vector<TracingHash> members)
{
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    return members;
}

/* ─────────────────────────────────────────────────────────────────────
   Payload encoding
   ───────────────────────────────────────────────────────────────────── */

static std::string leafPayload(const std::vector<TracingHash> & sortedMembers)
{
    std::string out;
    out.reserve(1 + sortedMembers.size() * TracingHash::size);
    out.push_back(char(0x00));
    for (const auto & m : sortedMembers)
        out.append(reinterpret_cast<const char *>(m.bytes.data()), TracingHash::size);
    return out;
}

static std::string internalPayload(uint8_t depth, const FrozenNode::Internal & inter)
{
    /* [0x01] [depth] [bitmap_lo] [bitmap_hi] [child_hash_1] ... [child_hash_k]
       Bitmap is 16 bits little-endian: bit i set iff slot i is populated.
       Children are emitted in ascending slot order. */
    uint16_t bitmap = 0;
    size_t populated = 0;
    for (size_t i = 0; i < RADIX; ++i)
        if (inter.slots[i]) {
            bitmap |= (uint16_t(1) << i);
            ++populated;
        }
    std::string out;
    out.reserve(4 + populated * TracingHash::size);
    out.push_back(char(0x01));
    out.push_back(char(depth));
    out.push_back(char(bitmap & 0xFF));
    out.push_back(char((bitmap >> 8) & 0xFF));
    for (size_t i = 0; i < RADIX; ++i)
        if (inter.slots[i])
            out.append(reinterpret_cast<const char *>(inter.slots[i]->hash.bytes.data()),
                       TracingHash::size);
    return out;
}

/* ─────────────────────────────────────────────────────────────────────
   FrozenNode inspection
   ───────────────────────────────────────────────────────────────────── */

size_t FrozenNode::size() const noexcept
{
    if (isLeaf())
        return asLeaf().members.size();
    size_t n = 0;
    for (const auto & child : asInternal().slots)
        if (child)
            n += child->size();
    return n;
}

bool FrozenNode::contains(const TracingHash & h) const noexcept
{
    if (isLeaf()) {
        const auto & m = asLeaf().members;
        return std::binary_search(m.begin(), m.end(), h);
    }
    const auto & inter = asInternal();
    auto & child = inter.slots[slotFor(h, inter.depth)];
    if (!child)
        return false;
    return child->contains(h);
}

std::string FrozenNode::toPayload() const
{
    if (isLeaf())
        return leafPayload(asLeaf().members);
    return internalPayload(asInternal().depth, asInternal());
}

std::vector<TracingHash> FrozenNode::allMembers() const
{
    if (cachedAllMembers)
        return *cachedAllMembers;
    std::vector<TracingHash> out;
    out.reserve(size());
    auto walk = [&](const FrozenNode & node, auto & self) -> void {
        if (node.isLeaf()) {
            const auto & m = node.asLeaf().members;
            out.insert(out.end(), m.begin(), m.end());
            return;
        }
        for (const auto & child : node.asInternal().slots)
            if (child)
                self(*child, self);
    };
    walk(*this, walk);
    cachedAllMembers = out;
    return out;
}

/* ─────────────────────────────────────────────────────────────────────
   FrozenNodeCache — lookup / intern / build
   ───────────────────────────────────────────────────────────────────── */

std::optional<FrozenNodePtr> FrozenNodeCache::lookup(const TracingHash & hash) const
{
    auto it = byHash.find(hash);
    if (it == byHash.end())
        return std::nullopt;
    return it->second;
}

FrozenNodePtr FrozenNodeCache::intern(const TracingHash & hash, std::string_view payload)
{
    if (auto existing = lookup(hash))
        return *existing;
    if (payload.empty())
        throw Error("rst: malformed frozen node payload (empty)");
    auto sp = std::make_shared<FrozenNode>();
    sp->hash = hash;
    if (payload[0] == char(0x00)) {
        FrozenNode::Leaf leaf;
        const size_t hs = TracingHash::size;
        if ((payload.size() - 1) % hs != 0)
            throw Error("rst: malformed frozen leaf (size=%d)", payload.size());
        leaf.members.reserve((payload.size() - 1) / hs);
        for (size_t i = 1; i + hs <= payload.size(); i += hs) {
            TracingHash h;
            std::memcpy(h.bytes.data(), payload.data() + i, hs);
            leaf.members.push_back(h);
        }
        sp->body = std::move(leaf);
    } else if (payload[0] == char(0x01)) {
        if (payload.size() < 4)
            throw Error("rst: malformed frozen internal (size=%d)", payload.size());
        uint8_t depth = static_cast<uint8_t>(payload[1]);
        uint16_t bitmap = static_cast<uint8_t>(payload[2])
                        | (static_cast<uint8_t>(payload[3]) << 8);
        FrozenNode::Internal inter;
        inter.depth = depth;
        const size_t hs = TracingHash::size;
        size_t off = 4;
        for (size_t i = 0; i < RADIX; ++i) {
            if (!(bitmap & (uint16_t(1) << i)))
                continue;
            if (off + hs > payload.size())
                throw Error("rst: malformed frozen internal (truncated at slot %d)", i);
            TracingHash childHash;
            std::memcpy(childHash.bytes.data(), payload.data() + off, hs);
            off += hs;
            auto childPtr = lookup(childHash);
            if (!childPtr)
                throw Error("rst: intern of internal references child not yet cached");
            inter.slots[i] = (*childPtr).get_ptr();
        }
        if (off != payload.size())
            throw Error("rst: trailing bytes in internal payload");
        sp->body = std::move(inter);
    } else {
        throw Error("rst: unknown frozen node tag %d", (int) (unsigned char) payload[0]);
    }
    FrozenNodePtr r(std::static_pointer_cast<const FrozenNode>(sp));
    byHash.emplace(hash, r);
    return r;
}

FrozenNodePtr FrozenNodeCache::internSet(std::vector<TracingHash> members)
{
    return build(sortAndDedup(std::move(members)), 0);
}

FrozenNodePtr FrozenNodeCache::internLeafFromSorted(std::vector<TracingHash> sortedMembers)
{
    ++internAttemptCount;
    TracingHash id = TracingHash::zero();
    for (const auto & m : sortedMembers) id.xorInPlace(m);
    if (auto existing = lookup(id)) return *existing;
    auto sp = std::make_shared<FrozenNode>();
    sp->hash = id;
    sp->body = FrozenNode::Leaf{std::move(sortedMembers)};
    FrozenNodePtr r(std::static_pointer_cast<const FrozenNode>(sp));
    byHash.emplace(id, r);
    return r;
}

FrozenNodePtr FrozenNodeCache::internInternalFromSlots(
    uint8_t depth,
    const std::array<std::shared_ptr<const FrozenNode>, RADIX> & slots)
{
    ++internAttemptCount;
    TracingHash id = TracingHash::zero();
    for (const auto & s : slots)
        if (s) id.xorInPlace(s->hash);
    if (auto existing = lookup(id)) return *existing;
    auto sp = std::make_shared<FrozenNode>();
    sp->hash = id;
    FrozenNode::Internal inter;
    inter.depth = depth;
    inter.slots = slots;
    sp->body = std::move(inter);
    FrozenNodePtr r(std::static_pointer_cast<const FrozenNode>(sp));
    byHash.emplace(id, r);
    return r;
}

FrozenNodePtr FrozenNodeCache::build(std::vector<TracingHash> members, size_t depth)
{
    ++internAttemptCount;

    if (members.size() <= LEAF_MAX_MEMBERS) {
        /* Leaf identity = XOR of members. */
        TracingHash id = TracingHash::zero();
        for (const auto & m : members)
            id.xorInPlace(m);
        if (auto existing = lookup(id))
            return *existing;
        auto sp = std::make_shared<FrozenNode>();
        sp->hash = id;
        sp->body = FrozenNode::Leaf{std::move(members)};
        FrozenNodePtr r(std::static_pointer_cast<const FrozenNode>(sp));
        byHash.emplace(id, r);
        return r;
    }

    /* Skip-single-slot: advance `depth` until members diverge into
       ≥ 2 distinct slots at that depth. A single-slot Internal would
       carry no information its lone child doesn't already carry, so
       skipping strips that representational entropy — two references
       to a subtree that would produce equivalent content always
       produce the exact same tree shape. Members are dedup'd, so
       divergence exists at some depth < 8 * TracingHash::size / RADIX_BITS. */
    while (true) {
        size_t firstSlot = slotFor(members.front(), depth);
        bool diverges = false;
        for (const auto & m : members)
            if (slotFor(m, depth) != firstSlot) {
                diverges = true;
                break;
            }
        if (diverges)
            break;
        ++depth;
    }

    /* Partition into 16 buckets by slot at this depth. Sort is preserved
       within each bucket because slotFor is a top-bit-group extract on
       byte-lex-sorted input. */
    std::array<std::vector<TracingHash>, RADIX> buckets;
    for (auto & m : members) {
        auto s = slotFor(m, depth);
        buckets[s].push_back(std::move(m));
    }

    FrozenNode::Internal inter;
    inter.depth = static_cast<uint8_t>(depth);
    TracingHash id = TracingHash::zero();
    for (size_t i = 0; i < RADIX; ++i) {
        if (buckets[i].empty())
            continue;
        auto child = build(std::move(buckets[i]), depth + 1);
        id.xorInPlace(child->hash);
        inter.slots[i] = child.get_ptr();
    }
    if (auto existing = lookup(id))
        return *existing;
    auto sp = std::make_shared<FrozenNode>();
    sp->hash = id;
    sp->body = std::move(inter);
    FrozenNodePtr r(std::static_pointer_cast<const FrozenNode>(sp));
    byHash.emplace(id, r);
    return r;
}

/* ─────────────────────────────────────────────────────────────────────
   Persistence
   ───────────────────────────────────────────────────────────────────── */

void FrozenNodeCache::persist(const FrozenNodePtr & root, PersistSink & sink)
{
    if (root->persisted)
        return;
    auto walk = [&](const FrozenNode & n, auto & self) -> void {
        if (n.persisted)
            return;
        if (!n.isLeaf())
            for (const auto & child : n.asInternal().slots)
                if (child)
                    self(*child, self);
        sink(n.hash, n.toPayload());
        n.persisted = true;
    };
    walk(*root, walk);
}

/* ─────────────────────────────────────────────────────────────────────
   MutableNode — COW HAMT mirror
   ─────────────────────────────────────────────────────────────────────

   The mutable tree mirrors the HAMT shape. Slots hold one of:

     monostate                          — empty
     shared_ptr<const FrozenNode>       — frozen subtree (COW)
     unique_ptr<Body>                   — owned mutable subtree

   Insertion walks the target slot path (O(depth)). At each level we
   COW frozen subtrees along the path into fresh mutable nodes and
   leave sibling frozen subtrees untouched (still pointer-identical
   to their originals in the cache). identity + memberCount are
   maintained incrementally.

   Freeze walks the mutable tree. Frozen slot refs are handed back
   directly; only mutable subtrees intern. cachedFrozen at each node
   short-circuits repeat-freezes.
   ───────────────────────────────────────────────────────────────────── */

struct MutableNodeBody
{
    struct Leaf { std::vector<TracingHash> members; }; // sorted
    struct Internal
    {
        uint8_t depth;
        using Slot = std::variant<
            std::monostate,
            std::shared_ptr<const FrozenNode>,
            std::unique_ptr<MutableNodeBody>>;
        std::array<Slot, RADIX> slots;
    };
    std::variant<Leaf, Internal> shape;
    TracingHash identity{};
    size_t memberCount = 0;
    /* Valid iff no mutation since last freeze. */
    std::shared_ptr<const FrozenNode> cachedFrozen;
};

/* Fill `identity` with zero so XORs stay in bounds. */
static void initEmptyBody(MutableNodeBody & b)
{
    b.identity = TracingHash::zero();
    b.memberCount = 0;
    b.shape = MutableNodeBody::Leaf{};
}

/* Descend leftmost populated path of a FrozenNode to grab any member. */
static TracingHash firstMemberOf(const FrozenNode & n)
{
    const FrozenNode * cur = &n;
    while (!cur->isLeaf()) {
        for (const auto & slot : cur->asInternal().slots)
            if (slot) { cur = slot.get(); break; }
    }
    return cur->asLeaf().members.front();
}

static void promoteLeafInPlace(MutableNodeBody & b);
static bool bodyInsert(MutableNodeBody & b, const TracingHash & h);

/* Create a fresh MutableLeaf holding {h}. */
static std::unique_ptr<MutableNodeBody> makeSingletonLeaf(const TracingHash & h)
{
    auto b = std::make_unique<MutableNodeBody>();
    b->shape = MutableNodeBody::Leaf{{h}};
    b->identity = h;
    b->memberCount = 1;
    return b;
}

/* Materialize a frozen node's top layer as mutable, with all
   sub-children left as frozen refs (COW). Used before recursing into
   a slot we intend to mutate. */
static std::unique_ptr<MutableNodeBody>
materializeFrozenTop(const std::shared_ptr<const FrozenNode> & fp)
{
    auto b = std::make_unique<MutableNodeBody>();
    b->identity = fp->hash;
    b->memberCount = fp->size();
    if (fp->isLeaf()) {
        b->shape = MutableNodeBody::Leaf{fp->asLeaf().members};
    } else {
        MutableNodeBody::Internal inter;
        inter.depth = fp->asInternal().depth;
        for (size_t i = 0; i < RADIX; ++i)
            if (fp->asInternal().slots[i])
                inter.slots[i] = fp->asInternal().slots[i];
        b->shape = std::move(inter);
    }
    /* cachedFrozen = fp: this materialized body still represents fp's
       exact content. If no mutation lands, freeze returns fp. */
    b->cachedFrozen = fp;
    return b;
}

/* Insert h into a slot at `parentDepth`'s Internal. Returns whether
   the insert was novel (member newly added). */
static bool insertIntoSlot(
    MutableNodeBody::Internal::Slot & slot,
    const TracingHash & h,
    size_t parentDepth)
{
    /* Empty slot — create fresh leaf. */
    if (std::holds_alternative<std::monostate>(slot)) {
        slot = makeSingletonLeaf(h);
        return true;
    }

    /* Frozen slot — either recurse into a COW materialization, or
       split (if h diverges from the frozen sub's prefix). */
    if (std::holds_alternative<std::shared_ptr<const FrozenNode>>(slot)) {
        auto fp = std::get<std::shared_ptr<const FrozenNode>>(slot);
        if (fp->isLeaf() || fp->asInternal().depth == parentDepth + 1) {
            /* No prefix skip to worry about — materialize + recurse. */
            auto mut = materializeFrozenTop(fp);
            bool novel = bodyInsert(*mut, h);
            slot = std::move(mut);
            return novel;
        }
        /* Frozen internal at deeper depth (skip-single-slot chain
           collapsed). Check whether h shares the prefix; if not, split. */
        size_t fpDepth = fp->asInternal().depth;
        TracingHash anchor = firstMemberOf(*fp);
        size_t divergeDepth = parentDepth + 1;
        while (divergeDepth < fpDepth
               && slotFor(h, divergeDepth) == slotFor(anchor, divergeDepth))
            ++divergeDepth;
        if (divergeDepth == fpDepth) {
            /* h fits into fp's shape — materialize + recurse. */
            auto mut = materializeFrozenTop(fp);
            bool novel = bodyInsert(*mut, h);
            slot = std::move(mut);
            return novel;
        }
        /* h and fp diverge at `divergeDepth`. Build a mutable Internal
           at that depth with two children: fp in one slot, {h} in another. */
        auto split = std::make_unique<MutableNodeBody>();
        MutableNodeBody::Internal inter;
        inter.depth = static_cast<uint8_t>(divergeDepth);
        inter.slots[slotFor(anchor, divergeDepth)] = fp;
        inter.slots[slotFor(h, divergeDepth)] = makeSingletonLeaf(h);
        split->shape = std::move(inter);
        split->identity = fp->hash;
        split->identity.xorInPlace(h);
        split->memberCount = fp->size() + 1;
        slot = std::move(split);
        return true;
    }

    /* Mutable slot — recurse. */
    auto & child = std::get<std::unique_ptr<MutableNodeBody>>(slot);
    return bodyInsert(*child, h);
}

static bool leafInsert(MutableNodeBody & b, const TracingHash & h)
{
    auto & leaf = std::get<MutableNodeBody::Leaf>(b.shape);
    auto it = std::lower_bound(leaf.members.begin(), leaf.members.end(), h);
    if (it != leaf.members.end() && *it == h) return false;
    leaf.members.insert(it, h);
    /* If the insert pushed us over the flat cap, promote. */
    if (leaf.members.size() > LEAF_MAX_MEMBERS)
        promoteLeafInPlace(b);
    return true;
}

static bool internalInsert(MutableNodeBody & b, const TracingHash & h)
{
    auto & inter = std::get<MutableNodeBody::Internal>(b.shape);
    return insertIntoSlot(inter.slots[slotFor(h, inter.depth)], h, inter.depth);
}

static bool bodyInsert(MutableNodeBody & b, const TracingHash & h)
{
    bool novel = std::holds_alternative<MutableNodeBody::Leaf>(b.shape)
        ? leafInsert(b, h)
        : internalInsert(b, h);
    if (novel) {
        b.identity.xorInPlace(h);
        ++b.memberCount;
        b.cachedFrozen = nullptr;
    }
    return novel;
}

/* Promote a MutableLeaf that has grown past LEAF_MAX_MEMBERS into a
   MutableInternal at its divergence depth. Members are dedup'd and
   sorted (they came from an incrementally-inserted leaf). */
static void promoteLeafInPlace(MutableNodeBody & b)
{
    auto members = std::move(std::get<MutableNodeBody::Leaf>(b.shape).members);

    /* Find divergence depth by scanning bit-groups. */
    size_t depth = 0;
    while (true) {
        size_t firstSlot = slotFor(members.front(), depth);
        bool diverges = false;
        for (const auto & m : members)
            if (slotFor(m, depth) != firstSlot) { diverges = true; break; }
        if (diverges) break;
        ++depth;
    }

    std::array<std::vector<TracingHash>, RADIX> buckets;
    for (const auto & m : members)
        buckets[slotFor(m, depth)].push_back(m);

    MutableNodeBody::Internal inter;
    inter.depth = static_cast<uint8_t>(depth);
    for (size_t i = 0; i < RADIX; ++i) {
        if (buckets[i].empty()) continue;
        auto sub = std::make_unique<MutableNodeBody>();
        sub->shape = MutableNodeBody::Leaf{std::move(buckets[i])};
        auto & sm = std::get<MutableNodeBody::Leaf>(sub->shape).members;
        sub->identity = TracingHash::zero();
        for (const auto & m : sm) sub->identity.xorInPlace(m);
        sub->memberCount = sm.size();
        if (sm.size() > LEAF_MAX_MEMBERS)
            promoteLeafInPlace(*sub);
        inter.slots[i] = std::move(sub);
    }
    b.shape = std::move(inter);
    /* identity / memberCount unchanged — same member set. */
}

static bool bodyContains(const MutableNodeBody & b, const TracingHash & h)
{
    if (std::holds_alternative<MutableNodeBody::Leaf>(b.shape)) {
        const auto & leaf = std::get<MutableNodeBody::Leaf>(b.shape);
        return std::binary_search(leaf.members.begin(), leaf.members.end(), h);
    }
    const auto & inter = std::get<MutableNodeBody::Internal>(b.shape);
    const auto & slot = inter.slots[slotFor(h, inter.depth)];
    if (std::holds_alternative<std::monostate>(slot)) return false;
    if (std::holds_alternative<std::shared_ptr<const FrozenNode>>(slot))
        return std::get<std::shared_ptr<const FrozenNode>>(slot)->contains(h);
    return bodyContains(*std::get<std::unique_ptr<MutableNodeBody>>(slot), h);
}

static std::shared_ptr<const FrozenNode>
bodyFreeze(MutableNodeBody & b, FrozenNodeCache & cache)
{
    if (b.cachedFrozen) return b.cachedFrozen;
    std::shared_ptr<const FrozenNode> result;
    if (std::holds_alternative<MutableNodeBody::Leaf>(b.shape)) {
        auto & leaf = std::get<MutableNodeBody::Leaf>(b.shape);
        result = cache.internLeafFromSorted(leaf.members).get_ptr();
    } else {
        auto & inter = std::get<MutableNodeBody::Internal>(b.shape);
        std::array<std::shared_ptr<const FrozenNode>, RADIX> frozenSlots;
        size_t populated = 0;
        size_t lastPopulated = 0;
        for (size_t i = 0; i < RADIX; ++i) {
            auto & s = inter.slots[i];
            if (std::holds_alternative<std::monostate>(s)) continue;
            if (std::holds_alternative<std::shared_ptr<const FrozenNode>>(s))
                frozenSlots[i] = std::get<std::shared_ptr<const FrozenNode>>(s);
            else
                frozenSlots[i] = bodyFreeze(
                    *std::get<std::unique_ptr<MutableNodeBody>>(s), cache);
            ++populated;
            lastPopulated = i;
        }
        /* Skip-single-slot on freeze: any Internal that ended up with
           exactly one populated slot collapses to that child. Keeps
           the frozen tree canonical. */
        result = populated == 1
            ? frozenSlots[lastPopulated]
            : cache.internInternalFromSlots(inter.depth, frozenSlots).get_ptr();
    }
    b.cachedFrozen = result;
    return result;
}

MutableNode::MutableNode()
    : body(std::make_unique<MutableNodeBody>())
{
    initEmptyBody(*body);
}

MutableNode::~MutableNode() = default;
MutableNode::MutableNode(MutableNode &&) noexcept = default;
MutableNode & MutableNode::operator=(MutableNode &&) noexcept = default;

MutableNode::MutableNode(FrozenNodePtr root)
    : body(materializeFrozenTop(root.get_ptr()))
{}

void MutableNode::insert(const TracingHash & h)
{
    bodyInsert(*body, h);
}

bool MutableNode::contains(const TracingHash & h) const noexcept
{
    return bodyContains(*body, h);
}

size_t MutableNode::size() const noexcept
{
    return body->memberCount;
}

FrozenNodePtr MutableNode::freeze(FrozenNodeCache & cache)
{
    return FrozenNodePtr(bodyFreeze(*body, cache));
}

/* ─────────────────────────────────────────────────────────────────────
   Set operations
   ───────────────────────────────────────────────────────────────────── */

static void diffWalk(const FrozenNode & a, const FrozenNode & b, std::vector<TracingHash> & out)
{
    /* Hash-equal subtree short-circuit — same members, nothing to add.
       Fires at any depth where two subtrees happen to be structurally
       equal (canonical HAMT + XOR identity = same content means same
       cached FrozenNode). */
    if (a.hash == b.hash)
        return;
    if (a.isLeaf()) {
        for (const auto & m : a.asLeaf().members)
            if (!b.contains(m))
                out.push_back(m);
        return;
    }
    /* a is Internal. If b is a Leaf or an Internal at a different depth
       (skip-single-slot means depths can differ across subtrees), fall
       back to walking a and filtering via b.contains — still cheaper
       than materialising all members up-front. */
    if (b.isLeaf() || b.asInternal().depth != a.asInternal().depth) {
        for (const auto & slot : a.asInternal().slots)
            if (slot)
                diffWalk(*slot, b, out);
        return;
    }
    /* Both Internal at the same depth: parallel slot walk. Slots aligned
       by content bits, so slot i on each side holds members with the
       same bit-group. */
    const auto & aInter = a.asInternal();
    const auto & bInter = b.asInternal();
    for (size_t i = 0; i < RADIX; ++i) {
        const auto & aSlot = aInter.slots[i];
        if (!aSlot)
            continue;
        const auto & bSlot = bInter.slots[i];
        if (!bSlot) {
            /* b has no slot i, so everything under a's slot i is in
               the difference. Flatten and dump. */
            auto members = aSlot->allMembers();
            out.insert(out.end(), members.begin(), members.end());
        } else {
            diffWalk(*aSlot, *bSlot, out);
        }
    }
}

std::vector<TracingHash> difference(const FrozenNode & a, const FrozenNode & b)
{
    std::vector<TracingHash> out;
    diffWalk(a, b, out);
    return out;
}

bool isSubset(const FrozenNode & a, const FrozenNode & b)
{
    if (a.hash == b.hash)
        return true;
    if (a.size() > b.size())
        return false;
    for (const auto & m : a.allMembers())
        if (!b.contains(m))
            return false;
    return true;
}

FrozenNodePtr intersection(const FrozenNodePtr & a, const FrozenNodePtr & b, FrozenNodeCache & cache)
{
    /* Same-subtree short-circuit — hand back the caller's own reference. */
    if (a->hash == b->hash)
        return a;
    /* Iterate the smaller side and filter by containment on the larger.
       O(smaller.size() * depth) — good enough as a first cut; a
       structural parallel walk over slots is a follow-up optimization. */
    const FrozenNodePtr & smaller = (a->size() <= b->size()) ? a : b;
    const FrozenNodePtr & larger  = (a->size() <= b->size()) ? b : a;
    std::vector<TracingHash> keep;
    for (const auto & m : smaller->allMembers())
        if (larger->contains(m))
            keep.push_back(m);
    return cache.internSet(std::move(keep));
}

/* insertSortedMembers walk. Precondition: `members` is sorted
   lex-ascending and deduped. Recurses on Internal slots; on Leaf,
   merges and re-canonicalises via internSet. */
FrozenNodePtr insertSortedMembers(
    const FrozenNodePtr & node,
    std::span<const TracingHash> members,
    FrozenNodeCache & cache)
{
    if (members.empty())
        return node;

    /* Leaf: merge the sorted members with the leaf's sorted members
       (both dedup'd already), then canonicalise. If the result stays
       under the leaf cap we get a fresh Leaf; otherwise internSet's
       skip-single-slot builds the correct Internal at the divergence
       depth. */
    if (node->isLeaf()) {
        const auto & leafMembers = node->asLeaf().members;
        std::vector<TracingHash> merged;
        merged.reserve(leafMembers.size() + members.size());
        std::set_union(leafMembers.begin(), leafMembers.end(),
                       members.begin(), members.end(),
                       std::back_inserter(merged));
        if (merged.size() == leafMembers.size())
            return node;  // no novel members
        return cache.internSet(std::move(merged));
    }

    /* Internal at some depth D. Members are sorted; slotFor at D is a
       top-nibble extract that partitions the sorted stream into
       contiguous runs per slot. */
    const auto & inter = node->asInternal();
    uint8_t depth = inter.depth;

    /* Fast check: does anything actually diverge from a common prefix
       of `members` with the Internal's structural anchor? Under
       skip-single-slot the Internal may live at depth > 0. If any
       incoming member has different bits between root-depth and this
       node's depth from what's already there, the assumption "all
       these members belong under this Internal" doesn't hold and we
       fall through to the flatten-then-internSet path. In the common
       case (recursive call landed here because parent partition
       already established the prefix match), this check is O(1) per
       incoming member. */
    /* (Nothing to check when node came from a call where depth already
       matches — that's the invariant callers below maintain. Explicit
       check would be per-caller; we rely on canonicalisation via
       internSet as a correctness safety net if invariant slips.) */

    std::array<std::shared_ptr<const FrozenNode>, RADIX> newSlots = inter.slots;
    bool anyChange = false;
    size_t i = 0;
    while (i < members.size()) {
        size_t slot = slotFor(members[i], depth);
        size_t j = i + 1;
        while (j < members.size() && slotFor(members[j], depth) == slot)
            ++j;
        std::span<const TracingHash> slotMembers(members.data() + i, j - i);
        if (newSlots[slot]) {
            auto old = newSlots[slot];
            /* Recurse into the existing subtree. Note: if the child
               is at a deeper depth (skip-single-slot chain), the
               child's isLeaf/asInternal logic still works — a Leaf
               absorbs via set_union; an Internal at deeper depth
               may diverge on prefix, in which case the recursive
               call's fallback (internSet re-canonicalisation) kicks
               in for that slot. */
            auto updated = insertSortedMembers(FrozenNodePtr(old), slotMembers, cache);
            if (updated.get_ptr() != old) {
                newSlots[slot] = updated.get_ptr();
                anyChange = true;
            }
        } else {
            /* Fresh subtree from just these members. internSet
               canonicalises to the correct depth automatically. */
            std::vector<TracingHash> ms(slotMembers.begin(), slotMembers.end());
            newSlots[slot] = cache.internSet(std::move(ms)).get_ptr();
            anyChange = true;
        }
        i = j;
    }
    if (!anyChange)
        return node;
    return cache.internInternalFromSlots(depth, newSlots);
}

std::vector<TracingHash> childHashesInPayload(std::string_view payload)
{
    if (payload.empty() || payload[0] != char(0x01))
        return {};
    if (payload.size() < 4)
        throw Error("rst: malformed frozen internal payload (size=%d)", payload.size());
    uint16_t bitmap = static_cast<uint8_t>(payload[2])
                    | (static_cast<uint8_t>(payload[3]) << 8);
    const size_t hs = TracingHash::size;
    std::vector<TracingHash> out;
    size_t off = 4;
    for (size_t i = 0; i < RADIX; ++i) {
        if (!(bitmap & (uint16_t(1) << i)))
            continue;
        if (off + hs > payload.size())
            throw Error("rst: malformed frozen internal payload (truncated at slot %d)", i);
        TracingHash h;
        std::memcpy(h.bytes.data(), payload.data() + off, hs);
        out.push_back(h);
        off += hs;
    }
    return out;
}

FrozenNodePtr union_(const FrozenNodePtr & a, const FrozenNodePtr & b, FrozenNodeCache & cache)
{
    if (a->hash == b->hash) return a;
    /* Seed a MutableNode from the larger side so its whole subtree
       stays as frozen refs (CoW) — inserting the smaller side's
       members clones only the paths that actually change. */
    const FrozenNodePtr & larger  = (a->size() >= b->size()) ? a : b;
    const FrozenNodePtr & smaller = (a->size() >= b->size()) ? b : a;
    MutableNode mut(larger);
    for (const auto & m : smaller->allMembers())
        mut.insert(m);
    return mut.freeze(cache);
}

} // namespace nix::trace::rst
