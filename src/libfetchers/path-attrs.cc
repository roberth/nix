#include "nix/fetchers/path-attrs.hh"
#include "nix/fetchers/attrs.hh"

namespace nix::fetchers {

PathUnlockedInput pathInputFromAttrs(const Settings & settings, const Attrs & attrs)
{
    auto path = getStrAttr(attrs, "path");
    PathUnlockedInput input(path);

    // Optional fake tree info attributes
    if (auto rev = maybeGetStrAttr(attrs, "rev"))
        input.rev = *rev;
    if (auto revCount = maybeGetIntAttr(attrs, "revCount"))
        input.revCount = *revCount;
    if (auto lastModified = maybeGetIntAttr(attrs, "lastModified"))
        input.lastModified = *lastModified;
    if (auto narHash = maybeGetStrAttr(attrs, "narHash"))
        input.narHash = Hash::parseAny(*narHash, HashAlgorithm::SHA256);

    return input;
}

Attrs pathInputToAttrs(const PathUnlockedInput & input)
{
    Attrs attrs;
    attrs.insert_or_assign("type", "path");
    attrs.insert_or_assign("path", input.path.string());

    if (input.rev)
        attrs.insert_or_assign("rev", *input.rev);
    if (input.revCount)
        attrs.insert_or_assign("revCount", *input.revCount);
    if (input.lastModified)
        attrs.insert_or_assign("lastModified", *input.lastModified);
    if (input.narHash)
        attrs.insert_or_assign("narHash", input.narHash->to_string(HashFormat::SRI, true));

    return attrs;
}

Attrs PathLockedInput::toAttrs() const
{
    Attrs attrs;
    attrs.insert_or_assign("type", "path");
    attrs.insert_or_assign("path", path.string());

    if (rev)
        attrs.insert_or_assign("rev", *rev);
    if (revCount)
        attrs.insert_or_assign("revCount", *revCount);
    if (narHash)
        attrs.insert_or_assign("narHash", narHash->to_string(HashFormat::SRI, true));

    // Locking metadata
    attrs.insert_or_assign("lastModified", uint64_t(locking.lastModified));

    return attrs;
}

Attrs pathInputToAttrs(const PathLockedInput & input)
{
    return input.toAttrs();
}

Attrs PathFinalInput::toAttrs() const
{
    // Get all locked-state attributes from parent
    auto attrs = PathLockedInput::toAttrs();

    // Overwrite narHash with the actual finalized one
    attrs.insert_or_assign("narHash", finalization.narHash.to_string(HashFormat::SRI, true));
    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

Attrs pathInputToAttrs(const PathFinalInput & input)
{
    return input.toAttrs();
}

} // namespace nix::fetchers
