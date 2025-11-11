#include "nix/fetchers/tarball-attrs.hh"
#include "nix/fetchers/attrs.hh"

namespace nix::fetchers {

TarballUnlockedInput tarballInputFromAttrs(const Settings & settings, const Attrs & attrs)
{
    auto type = getStrAttr(attrs, "type");
    auto url = getStrAttr(attrs, "url");
    auto name = maybeGetStrAttr(attrs, "name");

    TarballUnlockedInput input(settings, type, url, name);

    if (auto unpack = maybeGetBoolAttr(attrs, "unpack"))
        input.unpack = *unpack;

    if (auto rev = maybeGetStrAttr(attrs, "rev"))
        input.rev = *rev;

    if (auto revCount = maybeGetIntAttr(attrs, "revCount"))
        input.revCount = *revCount;

    if (auto lastModified = maybeGetIntAttr(attrs, "lastModified"))
        input.lastModified = *lastModified;

    return input;
}

Attrs tarballInputToAttrs(const TarballUnlockedInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", std::string{input.type});
    attrs.insert_or_assign("url", input.url);

    if (input.name)
        attrs.insert_or_assign("name", *input.name);

    if (input.unpack)
        attrs.insert_or_assign("unpack", Explicit<bool>(*input.unpack));

    if (input.rev)
        attrs.insert_or_assign("rev", *input.rev);

    if (input.revCount)
        attrs.insert_or_assign("revCount", *input.revCount);

    if (input.lastModified)
        attrs.insert_or_assign("lastModified", uint64_t(*input.lastModified));

    return attrs;
}

Attrs TarballLockedInput::toAttrs() const
{
    Attrs attrs;

    attrs.insert_or_assign("type", std::string{type});
    attrs.insert_or_assign("url", url);

    if (name)
        attrs.insert_or_assign("name", *name);

    if (unpack)
        attrs.insert_or_assign("unpack", Explicit<bool>(*unpack));

    if (etag)
        attrs.insert_or_assign("etag", *etag);

    if (immutableUrl)
        attrs.insert_or_assign("immutableUrl", *immutableUrl);

    if (rev)
        attrs.insert_or_assign("rev", *rev);

    if (revCount)
        attrs.insert_or_assign("revCount", *revCount);

    attrs.insert_or_assign("lastModified", uint64_t(locking.lastModified));

    return attrs;
}

Attrs tarballInputToAttrs(const TarballLockedInput & input)
{
    return input.toAttrs();
}

Attrs TarballFinalInput::toAttrs() const
{
    // Get all locked-state attributes from parent
    auto attrs = TarballLockedInput::toAttrs();

    // Add final-specific attributes
    attrs.insert_or_assign("narHash", finalization.narHash.to_string(HashFormat::SRI, true));
    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

Attrs tarballInputToAttrs(const TarballFinalInput & input)
{
    return input.toAttrs();
}

} // namespace nix::fetchers
