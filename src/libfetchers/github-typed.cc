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

Attrs githubInputToAttrs(const GitHubUnlockedInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "github");
    attrs.insert_or_assign("owner", input.owner);
    attrs.insert_or_assign("repo", input.repo);

    if (input.host)
        attrs.insert_or_assign("host", *input.host);

    if (input.ref)
        attrs.insert_or_assign("ref", *input.ref);

    if (input.rev)
        attrs.insert_or_assign("rev", input.rev->gitRev());

    return attrs;
}

Attrs GitHubLockedInput::toAttrs() const
{
    Attrs attrs;

    attrs.insert_or_assign("type", std::string{type});
    attrs.insert_or_assign("owner", owner);
    attrs.insert_or_assign("repo", repo);

    if (host)
        attrs.insert_or_assign("host", *host);

    attrs.insert_or_assign("rev", rev.gitRev());

    if (treeHash)
        attrs.insert_or_assign("treeHash", treeHash->gitRev());

    attrs.insert_or_assign("lastModified", uint64_t(locking.lastModified));

    return attrs;
}

Attrs GitHubFinalInput::toAttrs() const
{
    // Get all locked-state attributes from parent
    auto attrs = GitHubLockedInput::toAttrs();

    // Add final-specific attributes
    attrs.insert_or_assign("narHash", finalization.narHash.to_string(HashFormat::SRI, true));
    attrs.insert_or_assign("__final", Explicit<bool>(true));

    return attrs;
}

Attrs githubInputToAttrs(const GitHubLockedInput & input)
{
    return input.toAttrs();
}

Attrs githubInputToAttrs(const GitHubFinalInput & input)
{
    return input.toAttrs();
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

Attrs gitlabInputToAttrs(const GitLabUnlockedInput & input)
{
    Attrs attrs;

    attrs.insert_or_assign("type", "gitlab");
    attrs.insert_or_assign("owner", input.owner);
    attrs.insert_or_assign("repo", input.repo);

    if (input.host)
        attrs.insert_or_assign("host", *input.host);

    if (input.ref)
        attrs.insert_or_assign("ref", *input.ref);

    if (input.rev)
        attrs.insert_or_assign("rev", input.rev->gitRev());

    return attrs;
}

Attrs gitlabInputToAttrs(const GitLabLockedInput & input)
{
    return input.toAttrs();
}

Attrs gitlabInputToAttrs(const GitLabFinalInput & input)
{
    return input.toAttrs();
}

} // namespace nix::fetchers
