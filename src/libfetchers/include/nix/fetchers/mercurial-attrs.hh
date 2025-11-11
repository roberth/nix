#pragma once

#include "nix/fetchers/typed-inputs.hh"
#include "nix/util/types.hh"
#include "nix/util/hash.hh"
#include "nix/util/url-parts.hh"

namespace nix::fetchers {

/**
 * Base class for Mercurial typed inputs, shared across all three states.
 */
struct MercurialInputBase : InputBase
{
    std::string url;
    std::optional<std::string> name;

    MercurialInputBase(const Settings & settings, std::string url, std::optional<std::string> name = std::nullopt)
        : InputBase(settings, "hg")
        , url(std::move(url))
        , name(std::move(name))
    {
    }

    virtual ~MercurialInputBase() = default;

    std::string getName() const override
    {
        if (name)
            return *name;
        return InputBase::getName();
    }
};

/**
 * Unlocked Mercurial input - has URL with optional ref or rev.
 */
struct MercurialUnlockedInput : MercurialInputBase
{
    std::optional<std::string> ref;
    std::optional<Hash> rev;

    MercurialUnlockedInput(
        const Settings & settings,
        std::string url,
        std::optional<std::string> name = std::nullopt,
        std::optional<std::string> ref = std::nullopt,
        std::optional<Hash> rev = std::nullopt)
        : MercurialInputBase(settings, std::move(url), std::move(name))
        , ref(std::move(ref))
        , rev(std::move(rev))
    {
    }
};

/**
 * Locked Mercurial input - resolved to a specific changeset hash.
 */
struct MercurialLockedInput : MercurialInputBase
{
    std::optional<std::string> ref; // Branch/tag name
    std::optional<Hash> rev;        // Commit hash (optional for dirty trees)
    std::optional<uint64_t> revCount;
    LockingMetadata locking;

    // Constructor for locked input with rev
    MercurialLockedInput(
        const Settings & settings,
        std::string url,
        std::string ref,
        Hash rev,
        uint64_t revCount,
        LockingMetadata locking,
        std::optional<std::string> name = std::nullopt)
        : MercurialInputBase(settings, std::move(url), std::move(name))
        , ref(std::move(ref))
        , rev(std::move(rev))
        , revCount(revCount)
        , locking(std::move(locking))
    {
    }

    // Constructor for locked input without rev (dirty tree)
    MercurialLockedInput(
        const Settings & settings,
        std::string url,
        std::string ref,
        LockingMetadata locking,
        std::optional<std::string> name = std::nullopt)
        : MercurialInputBase(settings, std::move(url), std::move(name))
        , ref(std::move(ref))
        , locking(std::move(locking))
    {
    }

    /**
     * Serialize to Attrs for boundary conversion.
     */
    virtual Attrs toAttrs() const;
};

/**
 * Final Mercurial input - has narHash after fetching.
 */
struct MercurialFinalInput : MercurialLockedInput
{
    FinalizationData finalization;

    MercurialFinalInput(
        const Settings & settings,
        std::string url,
        std::string ref,
        Hash rev,
        uint64_t revCount,
        LockingMetadata locking,
        FinalizationData finalization,
        std::optional<std::string> name = std::nullopt)
        : MercurialLockedInput(
              settings, std::move(url), std::move(ref), std::move(rev), revCount, std::move(locking), std::move(name))
        , finalization(std::move(finalization))
    {
    }

    /**
     * Serialize to Attrs including final-state attributes.
     */
    Attrs toAttrs() const override;
};

// Convenience type alias
using MercurialInputStates = InputStates<MercurialUnlockedInput, MercurialLockedInput, MercurialFinalInput>;

// Conversion functions for backward compatibility
MercurialUnlockedInput mercurialInputFromAttrs(const Settings & settings, const Attrs & attrs);
Attrs mercurialInputToAttrs(const MercurialUnlockedInput & input);
Attrs mercurialInputToAttrs(const MercurialLockedInput & input);
Attrs mercurialInputToAttrs(const MercurialFinalInput & input);

} // namespace nix::fetchers
