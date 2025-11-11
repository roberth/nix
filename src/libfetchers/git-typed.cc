#include "nix/fetchers/git-typed.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/util/url.hh"

namespace nix::fetchers {

GitUnlockedInput gitInputFromAttrs(const Settings & settings, const Attrs & attrs)
{
    // Parse URL
    auto urlStr = getStrAttr(attrs, "url");
    auto url = parseURL(urlStr);

    // Parse ref (branch/tag name)
    auto ref = maybeGetStrAttr(attrs, "ref");

    // Parse rev (commit hash)
    std::optional<Hash> rev;
    if (auto revStr = maybeGetStrAttr(attrs, "rev"))
        rev = Hash::parseAny(*revStr, HashAlgorithm::SHA1);

    // Parse boolean flags with defaults
    bool shallow = maybeGetBoolAttr(attrs, "shallow").value_or(false);
    bool submodules = maybeGetBoolAttr(attrs, "submodules").value_or(false);
    bool lfs = maybeGetBoolAttr(attrs, "lfs").value_or(false);
    bool exportIgnore = maybeGetBoolAttr(attrs, "exportIgnore").value_or(false);
    bool allRefs = maybeGetBoolAttr(attrs, "allRefs").value_or(false);

    // Parse optional name
    auto name = maybeGetStrAttr(attrs, "name");

    // Create the unlocked input
    GitUnlockedInput input(settings, std::move(url), ref, rev, shallow, submodules, lfs, exportIgnore, allRefs, name);

    // Parse dirty state (for working directory inputs)
    if (auto dirtyRevStr = maybeGetStrAttr(attrs, "dirtyRev"))
        input.dirtyRev = Hash::parseAny(*dirtyRevStr, HashAlgorithm::SHA1);
    if (auto dirtyShortRev = maybeGetStrAttr(attrs, "dirtyShortRev"))
        input.dirtyShortRev = *dirtyShortRev;

    // Parse verified fetches attributes
    if (auto verifyCommit = maybeGetBoolAttr(attrs, "verifyCommit"))
        input.verifyCommit = *verifyCommit;
    if (auto keytype = maybeGetStrAttr(attrs, "keytype"))
        input.keytype = *keytype;
    if (auto publicKey = maybeGetStrAttr(attrs, "publicKey"))
        input.publicKey = *publicKey;

    // Parse publicKeys array
    if (auto publicKeysStr = maybeGetStrAttr(attrs, "publicKeys")) {
        // For now, we store the raw string; a full implementation would parse it
        // This matches the existing code structure
        input.publicKeys = std::vector<std::string>{*publicKeysStr};
    }

    return input;
}

Attrs gitInputToAttrs(const GitFinalInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "git");
    attrs.insert_or_assign("url", input.url.to_string());

    if (input.ref)
        attrs.insert_or_assign("ref", *input.ref);

    attrs.insert_or_assign("rev", input.rev.gitRev());

    if (input.shallow)
        attrs.insert_or_assign("shallow", Explicit<bool>(true));

    if (input.submodules)
        attrs.insert_or_assign("submodules", Explicit<bool>(true));

    if (input.lfs)
        attrs.insert_or_assign("lfs", Explicit<bool>(true));

    if (input.exportIgnore)
        attrs.insert_or_assign("exportIgnore", Explicit<bool>(true));

    if (input.allRefs)
        attrs.insert_or_assign("allRefs", Explicit<bool>(true));

    if (input.name)
        attrs.insert_or_assign("name", *input.name);

    if (input.revCount)
        attrs.insert_or_assign("revCount", *input.revCount);

    attrs.insert_or_assign("lastModified", uint64_t(input.locking.lastModified));
    attrs.insert_or_assign("narHash", input.finalization.narHash.to_string(HashFormat::SRI, true));

    // Verified fetches attributes
    if (input.verifyCommit)
        attrs.insert_or_assign("verifyCommit", Explicit<bool>(*input.verifyCommit));
    if (input.keytype)
        attrs.insert_or_assign("keytype", *input.keytype);
    if (input.publicKey)
        attrs.insert_or_assign("publicKey", *input.publicKey);
    if (input.publicKeys && !input.publicKeys->empty())
        attrs.insert_or_assign("publicKeys", input.publicKeys->at(0));

    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

} // namespace nix::fetchers
