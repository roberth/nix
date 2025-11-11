#include "nix/fetchers/mercurial-typed.hh"
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

    return MercurialUnlockedInput(settings, url, name, ref, rev);
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

Attrs mercurialInputToAttrs(const MercurialLockedInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "hg");
    attrs.insert_or_assign("url", input.url);

    if (input.name)
        attrs.insert_or_assign("name", *input.name);

    attrs.insert_or_assign("rev", input.rev.gitRev());

    if (input.revCount)
        attrs.insert_or_assign("revCount", *input.revCount);

    attrs.insert_or_assign("lastModified", uint64_t(input.locking.lastModified));

    return attrs;
}

Attrs mercurialInputToAttrs(const MercurialFinalInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "hg");
    attrs.insert_or_assign("url", input.url);

    if (input.name)
        attrs.insert_or_assign("name", *input.name);

    attrs.insert_or_assign("rev", input.rev.gitRev());

    if (input.revCount)
        attrs.insert_or_assign("revCount", *input.revCount);

    attrs.insert_or_assign("narHash", input.finalization.narHash.to_string(HashFormat::SRI, true));

    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

} // namespace nix::fetchers
