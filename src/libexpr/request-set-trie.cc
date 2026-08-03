#include "nix/expr/request-set-trie.hh"
#include "nix/util/error.hh"

namespace nix::trace::rst {

/* Skeleton. All bodies unimplemented — this file exists to compile the
   public interface while the HAMT is implemented in follow-up commits. */

size_t slotFor(const Hash &, size_t)
{
    throw Error("rst: slotFor not implemented");
}

size_t FrozenNode::size() const noexcept
{
    return 0;
}

bool FrozenNode::contains(const Hash &) const noexcept
{
    return false;
}

std::string FrozenNode::toPayload() const
{
    throw Error("rst: FrozenNode::toPayload not implemented");
}

std::vector<Hash> FrozenNode::allMembers() const
{
    return {};
}

std::optional<FrozenNodePtr> FrozenNodeCache::lookup(const Hash & hash) const
{
    auto it = byHash.find(hash);
    if (it == byHash.end())
        return std::nullopt;
    return it->second;
}

FrozenNodePtr FrozenNodeCache::intern(const Hash &, std::string_view)
{
    throw Error("rst: FrozenNodeCache::intern not implemented");
}

FrozenNodePtr FrozenNodeCache::internSet(std::vector<Hash>)
{
    throw Error("rst: FrozenNodeCache::internSet not implemented");
}

FrozenNodePtr FrozenNodeCache::build(std::vector<Hash>, size_t)
{
    throw Error("rst: FrozenNodeCache::build not implemented");
}

void FrozenNodeCache::persist(const FrozenNodePtr &, PersistSink &)
{
    throw Error("rst: FrozenNodeCache::persist not implemented");
}

struct MutableNode::Body {};

MutableNode::MutableNode()
    : body(std::make_unique<Body>())
{}

MutableNode::~MutableNode() = default;
MutableNode::MutableNode(MutableNode &&) noexcept = default;
MutableNode & MutableNode::operator=(MutableNode &&) noexcept = default;

MutableNode::MutableNode(FrozenNodePtr)
    : body(std::make_unique<Body>())
{}

void MutableNode::insert(const Hash &)
{
    throw Error("rst: MutableNode::insert not implemented");
}

bool MutableNode::contains(const Hash &) const noexcept
{
    return false;
}

size_t MutableNode::size() const noexcept
{
    return 0;
}

FrozenNodePtr MutableNode::freeze(FrozenNodeCache &)
{
    throw Error("rst: MutableNode::freeze not implemented");
}

std::vector<Hash> difference(const FrozenNode &, const FrozenNode &)
{
    throw Error("rst: difference not implemented");
}

bool isSubset(const FrozenNode &, const FrozenNode &)
{
    throw Error("rst: isSubset not implemented");
}

FrozenNodePtr intersection(const FrozenNodePtr &, const FrozenNodePtr &, FrozenNodeCache &)
{
    throw Error("rst: intersection not implemented");
}

} // namespace nix::trace::rst
