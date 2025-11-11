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
    Hash rev;
    std::optional<uint64_t> revCount;
    LockingMetadata locking;

    MercurialLockedInput(
        const Settings & settings,
        std::string url,
        Hash rev,
        std::optional<std::string> name = std::nullopt,
        std::optional<uint64_t> revCount = std::nullopt)
        : MercurialInputBase(settings, std::move(url), std::move(name))
        , rev(std::move(rev))
        , revCount(revCount)
    {
    }
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
        Hash rev,
        Hash narHash,
        std::optional<std::string> name = std::nullopt,
        std::optional<uint64_t> revCount = std::nullopt)
        : MercurialLockedInput(settings, std::move(url), std::move(rev), std::move(name), revCount)
        , finalization(std::move(narHash))
    {
    }
};

// Convenience type alias
using MercurialInputStates = InputStates<MercurialUnlockedInput, MercurialLockedInput, MercurialFinalInput>;

// Conversion functions for backward compatibility
MercurialUnlockedInput mercurialInputFromAttrs(const Settings & settings, const Attrs & attrs);
Attrs mercurialInputToAttrs(const MercurialFinalInput & input);

} // namespace nix::fetchers
