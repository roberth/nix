#include "nix/fetchers/tarball-typed.hh"
#include "nix/fetchers/attrs.hh"

namespace nix::fetchers {

TarballUnlockedInput tarballInputFromAttrs(const Settings & settings, const Attrs & attrs)
{
    auto url = getStrAttr(attrs, "url");
    auto name = maybeGetStrAttr(attrs, "name");

    TarballUnlockedInput input(settings, url, name);

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

    attrs.insert_or_assign("type", "tarball");
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

Attrs tarballInputToAttrs(const TarballLockedInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "tarball");
    attrs.insert_or_assign("url", input.url);

    if (input.name)
        attrs.insert_or_assign("name", *input.name);

    if (input.unpack)
        attrs.insert_or_assign("unpack", Explicit<bool>(*input.unpack));

    if (input.etag)
        attrs.insert_or_assign("etag", *input.etag);

    if (input.immutableUrl)
        attrs.insert_or_assign("immutableUrl", *input.immutableUrl);

    if (input.rev)
        attrs.insert_or_assign("rev", *input.rev);

    if (input.revCount)
        attrs.insert_or_assign("revCount", *input.revCount);

    attrs.insert_or_assign("lastModified", uint64_t(input.locking.lastModified));

    return attrs;
}

Attrs tarballInputToAttrs(const TarballFinalInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "tarball");
    attrs.insert_or_assign("url", input.url);

    if (input.name)
        attrs.insert_or_assign("name", *input.name);

    if (input.unpack)
        attrs.insert_or_assign("unpack", Explicit<bool>(*input.unpack));

    if (input.etag)
        attrs.insert_or_assign("etag", *input.etag);

    if (input.immutableUrl)
        attrs.insert_or_assign("immutableUrl", *input.immutableUrl);

    if (input.rev)
        attrs.insert_or_assign("rev", *input.rev);

    if (input.revCount)
        attrs.insert_or_assign("revCount", *input.revCount);

    attrs.insert_or_assign("lastModified", uint64_t(input.locking.lastModified));
    attrs.insert_or_assign("narHash", input.finalization.narHash.to_string(HashFormat::SRI, true));

    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

} // namespace nix::fetchers
