#include "nix/fetchers/indirect-typed.hh"
#include "nix/fetchers/attrs.hh"

namespace nix::fetchers {

IndirectUnlockedInput indirectInputFromAttrs(const Settings & settings, const Attrs & attrs)
{
    auto id = getStrAttr(attrs, "id");
    auto ref = maybeGetStrAttr(attrs, "ref");

    std::optional<Hash> rev;
    if (auto revStr = maybeGetStrAttr(attrs, "rev"))
        rev = Hash::parseAny(*revStr, HashAlgorithm::SHA1);

    return IndirectUnlockedInput(settings, id, ref, rev);
}

Attrs indirectInputToAttrs(const IndirectFinalInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "indirect");
    attrs.insert_or_assign("id", input.id);

    attrs.insert_or_assign("rev", input.rev.gitRev());

    attrs.insert_or_assign("narHash", input.finalization.narHash.to_string(HashFormat::SRI, true));

    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

} // namespace nix::fetchers
