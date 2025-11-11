#pragma once

#include "nix/fetchers/typed-inputs.hh"
#include "nix/util/types.hh"
#include "nix/util/hash.hh"

namespace nix::fetchers {

/**
 * Base class for GitHub/GitLab typed inputs, shared across all three states.
 * These fetchers download tarballs from git hosting API endpoints.
 */
struct GitHubInputBase
{
    std::string_view type; // "github", "gitlab", or "sourcehut"
    std::string owner;
    std::string repo;
    std::optional<std::string> host;

    GitHubInputBase(
        std::string_view type, std::string owner, std::string repo, std::optional<std::string> host = std::nullopt)
        : type(type)
        , owner(std::move(owner))
        , repo(std::move(repo))
        , host(std::move(host))
    {
    }

    virtual ~GitHubInputBase() = default;
};

/**
 * Unlocked GitHub/GitLab input - has owner/repo with optional ref or rev.
 */
struct GitHubUnlockedInput : GitHubInputBase
{
    std::optional<std::string> ref;
    std::optional<Hash> rev;

    GitHubUnlockedInput(
        std::string_view type,
        std::string owner,
        std::string repo,
        std::optional<std::string> host = std::nullopt,
        std::optional<std::string> ref = std::nullopt,
        std::optional<Hash> rev = std::nullopt)
        : GitHubInputBase(type, std::move(owner), std::move(repo), std::move(host))
        , ref(std::move(ref))
        , rev(std::move(rev))
    {
    }
};

/**
 * Locked GitHub/GitLab input - resolved to a specific commit hash.
 */
struct GitHubLockedInput : GitHubInputBase
{
    Hash rev;
    LockingMetadata locking;
    std::optional<Hash> treeHash;

    GitHubLockedInput(
        std::string_view type,
        std::string owner,
        std::string repo,
        Hash rev,
        std::optional<std::string> host = std::nullopt,
        std::optional<Hash> treeHash = std::nullopt)
        : GitHubInputBase(type, std::move(owner), std::move(repo), std::move(host))
        , rev(std::move(rev))
        , treeHash(std::move(treeHash))
    {
    }

    /**
     * Serialize to Attrs for boundary conversion.
     */
    virtual Attrs toAttrs() const;
};

/**
 * Final GitHub/GitLab input - has narHash after fetching.
 */
struct GitHubFinalInput : GitHubLockedInput
{
    FinalizationData finalization;

    GitHubFinalInput(
        std::string_view type,
        std::string owner,
        std::string repo,
        Hash rev,
        Hash narHash,
        std::optional<std::string> host = std::nullopt,
        std::optional<Hash> treeHash = std::nullopt)
        : GitHubLockedInput(
              type, std::move(owner), std::move(repo), std::move(rev), std::move(host), std::move(treeHash))
        , finalization(std::move(narHash))
    {
    }

    /**
     * Serialize to Attrs including final-state attributes.
     */
    Attrs toAttrs() const override;
};

// Convenience type aliases for GitHub
using GitHubInputStates = InputStates<GitHubUnlockedInput, GitHubLockedInput, GitHubFinalInput>;

// Conversion functions for backward compatibility
GitHubUnlockedInput githubInputFromAttrs(const Settings & settings, const Attrs & attrs);
Attrs githubInputToAttrs(const GitHubUnlockedInput & input);
Attrs githubInputToAttrs(const GitHubLockedInput & input);
Attrs githubInputToAttrs(const GitHubFinalInput & input);

// GitLab uses the same structure as GitHub
using GitLabUnlockedInput = GitHubUnlockedInput;
using GitLabLockedInput = GitHubLockedInput;
using GitLabFinalInput = GitHubFinalInput;
using GitLabInputStates = InputStates<GitLabUnlockedInput, GitLabLockedInput, GitLabFinalInput>;

GitLabUnlockedInput gitlabInputFromAttrs(const Settings & settings, const Attrs & attrs);
Attrs gitlabInputToAttrs(const GitLabUnlockedInput & input);
Attrs gitlabInputToAttrs(const GitLabLockedInput & input);
Attrs gitlabInputToAttrs(const GitLabFinalInput & input);

} // namespace nix::fetchers
