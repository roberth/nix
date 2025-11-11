#include "nix/fetchers/mercurial-attrs.hh"
#include "nix/fetchers/attrs.hh"

namespace nix::fetchers {

MercurialUnlockedInput mercurialInputFromAttrs(const Settings & settings, const Attrs & attrs)
{
    auto url = getStrAttr(attrs, "url");
    auto name = maybeGetStrAttr(attrs, "name");
    auto ref = maybeGetStrAttr(attrs, "ref");

    std::optional<Hash> rev;
    if (auto revStr = maybeGetStrAttr(attrs, "rev"))
        rev = Hash::parseAny(*revStr, HashAlgorithm::SHA1);

    return MercurialUnlockedInput(url, name, ref, rev);
}

Attrs mercurialInputToAttrs(const MercurialUnlockedInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "hg");
    attrs.insert_or_assign("url", input.url);

    if (input.name)
        attrs.insert_or_assign("name", *input.name);

    if (input.ref)
        attrs.insert_or_assign("ref", *input.ref);

    if (input.rev)
        attrs.insert_or_assign("rev", input.rev->gitRev());

    return attrs;
}

Attrs MercurialLockedInput::toAttrs() const
{
    Attrs attrs;

    attrs.insert_or_assign("type", "hg");
    attrs.insert_or_assign("url", url);

    if (name)
        attrs.insert_or_assign("name", *name);

    if (ref)
        attrs.insert_or_assign("ref", *ref);

    if (rev)
        attrs.insert_or_assign("rev", rev->gitRev());

    if (revCount)
        attrs.insert_or_assign("revCount", *revCount);

    if (locking.lastModified != 0)
        attrs.insert_or_assign("lastModified", uint64_t(locking.lastModified));

    return attrs;
}

Attrs mercurialInputToAttrs(const MercurialLockedInput & input)
{
    return input.toAttrs();
}

Attrs MercurialFinalInput::toAttrs() const
{
    // Get all locked-state attributes from parent
    auto attrs = MercurialLockedInput::toAttrs();

    // Add final-specific attributes
    attrs.insert_or_assign("narHash", finalization.narHash.to_string(HashFormat::SRI, true));
    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

Attrs mercurialInputToAttrs(const MercurialFinalInput & input)
{
    return input.toAttrs();
}

} // namespace nix::fetchers
