#pragma once

#include "nix/fetchers/typed-inputs.hh"
#include "nix/util/hash.hh"
#include "nix/util/types.hh"
#include "nix/util/url.hh"

#include <optional>
#include <string>
#include <vector>

namespace nix::fetchers {

// Forward declaration for verified fetches support
struct PublicKey;

/**
 * Base class for all Git input types, containing common attributes.
 */
struct GitInputBase : InputBase
{
    ParsedURL url;
    std::optional<std::string> ref; // Branch or tag name
    bool shallow;
    bool submodules;
    bool lfs;
    bool exportIgnore;
    bool allRefs;
    std::optional<std::string> name;

    // Verified fetches support (experimental)
    std::optional<bool> verifyCommit;
    std::optional<std::string> keytype;
    std::optional<std::string> publicKey;
    std::optional<std::vector<PublicKey>> publicKeys;

    GitInputBase(
        const Settings & settings,
        ParsedURL url,
        std::optional<std::string> ref = std::nullopt,
        bool shallow = false,
        bool submodules = false,
        bool lfs = false,
        bool exportIgnore = false,
        bool allRefs = false,
        std::optional<std::string> name = std::nullopt)
        : InputBase(settings, "git")
        , url(std::move(url))
        , ref(std::move(ref))
        , shallow(shallow)
        , submodules(submodules)
        , lfs(lfs)
        , exportIgnore(exportIgnore)
        , allRefs(allRefs)
        , name(std::move(name))
    {
    }

    std::string getName() const override
    {
        if (name)
            return *name;
        return "git:" + url.to_string();
    }
};

/**
 * Unlocked Git input - may or may not have a specific revision.
 * This is the initial state when a Git repository is first referenced.
 */
struct GitUnlockedInput : GitInputBase
{
    std::optional<Hash> rev; // Specific commit hash, if known

    // For working directory inputs (when a local Git repo has uncommitted changes)
    std::optional<std::string> dirtyRev;      // Full hash with "-dirty" suffix
    std::optional<std::string> dirtyShortRev; // Short hash with "-dirty" suffix

    GitUnlockedInput(
        const Settings & settings,
        ParsedURL url,
        std::optional<std::string> ref = std::nullopt,
        std::optional<Hash> rev = std::nullopt,
        bool shallow = false,
        bool submodules = false,
        bool lfs = false,
        bool exportIgnore = false,
        bool allRefs = false,
        std::optional<std::string> name = std::nullopt)
        : GitInputBase(
              settings,
              std::move(url),
              std::move(ref),
              shallow,
              submodules,
              lfs,
              exportIgnore,
              allRefs,
              std::move(name))
        , rev(std::move(rev))
    {
    }
};

/**
 * Locked Git input - has a specific revision and metadata about that revision.
 * This is the state after resolving a ref to a specific commit.
 * For dirty workdirs, rev may be absent but dirtyRev/dirtyShortRev are set.
 */
struct GitLockedInput : GitInputBase
{
    std::optional<Hash> rev;                  // Specific commit hash (absent for dirty workdirs)
    std::optional<uint64_t> revCount;         // Number of commits (only if not shallow)
    LockingMetadata locking;                  // Contains lastModified
    std::optional<std::string> dirtyRev;      // For dirty workdirs
    std::optional<std::string> dirtyShortRev; // For dirty workdirs

    GitLockedInput(
        const Settings & settings,
        ParsedURL url,
        std::optional<Hash> rev,
        LockingMetadata locking,
        std::optional<std::string> ref = std::nullopt,
        std::optional<uint64_t> revCount = std::nullopt,
        bool shallow = false,
        bool submodules = false,
        bool lfs = false,
        bool exportIgnore = false,
        bool allRefs = false,
        std::optional<std::string> name = std::nullopt)
        : GitInputBase(
              settings,
              std::move(url),
              std::move(ref),
              shallow,
              submodules,
              lfs,
              exportIgnore,
              allRefs,
              std::move(name))
        , rev(std::move(rev))
        , revCount(revCount)
        , locking(std::move(locking))
    {
    }

    /**
     * Serialize to Attrs for boundary conversion back to legacy Input type.
     */
    virtual Attrs toAttrs() const;
};

/**
 * Final Git input - includes the narHash of the fetched content.
 * This is the state after fetching the repository content into the store.
 */
struct GitFinalInput : GitLockedInput
{
    FinalizationData finalization;

    GitFinalInput(
        const Settings & settings,
        ParsedURL url,
        std::optional<Hash> rev,
        LockingMetadata locking,
        FinalizationData finalization,
        std::optional<std::string> ref = std::nullopt,
        std::optional<uint64_t> revCount = std::nullopt,
        bool shallow = false,
        bool submodules = false,
        bool lfs = false,
        bool exportIgnore = false,
        bool allRefs = false,
        std::optional<std::string> name = std::nullopt)
        : GitLockedInput(
              settings,
              std::move(url),
              std::move(rev),
              std::move(locking),
              std::move(ref),
              revCount,
              shallow,
              submodules,
              lfs,
              exportIgnore,
              allRefs,
              std::move(name))
        , finalization(std::move(finalization))
    {
    }

    /**
     * Serialize to Attrs including final-state attributes.
     */
    Attrs toAttrs() const override;
};

/**
 * Type group for Git inputs.
 */
using GitInputStates = InputStates<GitUnlockedInput, GitLockedInput, GitFinalInput>;

/**
 * Convert from Attrs (dynamic) to typed Git input.
 */
GitUnlockedInput gitInputFromAttrs(const Settings & settings, const Attrs & attrs);

/**
 * Convert from typed Git inputs to Attrs (for backward compatibility).
 *
 * Note: GitUnlockedInput serialization exists for completeness but is rarely used
 * in practice - we typically only serialize locked/final inputs back to Attrs.
 * The locked and final versions delegate to their toAttrs() methods.
 */
Attrs gitInputToAttrs(const GitUnlockedInput & input);
Attrs gitInputToAttrs(const GitLockedInput & input);
Attrs gitInputToAttrs(const GitFinalInput & input);

} // namespace nix::fetchers
