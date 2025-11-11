#include "nix/fetchers/github-typed.hh"
#include "nix/fetchers/attrs.hh"

namespace nix::fetchers {

GitHubUnlockedInput githubInputFromAttrs(const Settings & settings, const Attrs & attrs)
{
    auto owner = getStrAttr(attrs, "owner");
    auto repo = getStrAttr(attrs, "repo");
    auto host = maybeGetStrAttr(attrs, "host");
    auto ref = maybeGetStrAttr(attrs, "ref");

    std::optional<Hash> rev;
    if (auto revStr = maybeGetStrAttr(attrs, "rev"))
        rev = Hash::parseAny(*revStr, HashAlgorithm::SHA1);

    return GitHubUnlockedInput(settings, "github", owner, repo, host, ref, rev);
}

Attrs githubInputToAttrs(const GitHubFinalInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "github");
    attrs.insert_or_assign("owner", input.owner);
    attrs.insert_or_assign("repo", input.repo);

    if (input.host)
        attrs.insert_or_assign("host", *input.host);

    attrs.insert_or_assign("rev", input.rev.gitRev());

    if (input.treeHash)
        attrs.insert_or_assign("treeHash", input.treeHash->gitRev());

    attrs.insert_or_assign("lastModified", uint64_t(input.locking.lastModified));
    attrs.insert_or_assign("narHash", input.finalization.narHash.to_string(HashFormat::SRI, true));

    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

GitLabUnlockedInput gitlabInputFromAttrs(const Settings & settings, const Attrs & attrs)
{
    auto owner = getStrAttr(attrs, "owner");
    auto repo = getStrAttr(attrs, "repo");
    auto host = maybeGetStrAttr(attrs, "host");
    auto ref = maybeGetStrAttr(attrs, "ref");

    std::optional<Hash> rev;
    if (auto revStr = maybeGetStrAttr(attrs, "rev"))
        rev = Hash::parseAny(*revStr, HashAlgorithm::SHA1);

    return GitLabUnlockedInput(settings, "gitlab", owner, repo, host, ref, rev);
}

Attrs gitlabInputToAttrs(const GitLabFinalInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "gitlab");
    attrs.insert_or_assign("owner", input.owner);
    attrs.insert_or_assign("repo", input.repo);

    if (input.host)
        attrs.insert_or_assign("host", *input.host);

    attrs.insert_or_assign("rev", input.rev.gitRev());

    if (input.treeHash)
        attrs.insert_or_assign("treeHash", input.treeHash->gitRev());

    attrs.insert_or_assign("lastModified", uint64_t(input.locking.lastModified));
    attrs.insert_or_assign("narHash", input.finalization.narHash.to_string(HashFormat::SRI, true));

    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

} // namespace nix::fetchers
