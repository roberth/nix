#pragma once

#include "nix/fetchers/typed-inputs.hh"
#include "nix/util/hash.hh"
#include "nix/util/types.hh"

#include <optional>
#include <string>

namespace nix::fetchers {

/**
 * Base class for tarball/file inputs.
 */
struct TarballInputBase : InputBase
{
    std::string url;
    std::optional<std::string> name;
    std::optional<bool> unpack; // Whether to unpack the tarball

    TarballInputBase(const Settings & settings, std::string url, std::optional<std::string> name = std::nullopt)
        : InputBase(settings, "tarball")
        , url(std::move(url))
        , name(std::move(name))
    {
    }

    std::string getName() const override
    {
        if (name)
            return *name;
        return "tarball:" + url;
    }
};

/**
 * Unlocked tarball input - just a URL.
 */
struct TarballUnlockedInput : TarballInputBase
{
    // Optional metadata if known
    std::optional<std::string> rev;
    std::optional<uint64_t> revCount;
    std::optional<time_t> lastModified;

    TarballUnlockedInput(const Settings & settings, std::string url, std::optional<std::string> name = std::nullopt)
        : TarballInputBase(settings, std::move(url), std::move(name))
    {
    }
};

/**
 * Locked tarball input - has etag/immutableUrl for locking.
 */
struct TarballLockedInput : TarballInputBase
{
    std::string effectiveUrl; // URL after redirects
    std::optional<std::string> etag;
    std::optional<std::string> immutableUrl;
    LockingMetadata locking;

    // Optional Git metadata for git archives
    std::optional<std::string> rev;
    std::optional<uint64_t> revCount;

    TarballLockedInput(
        const Settings & settings,
        std::string url,
        std::string effectiveUrl,
        LockingMetadata locking,
        std::optional<std::string> name = std::nullopt)
        : TarballInputBase(settings, std::move(url), std::move(name))
        , effectiveUrl(std::move(effectiveUrl))
        , locking(std::move(locking))
    {
    }
};

/**
 * Final tarball input - includes narHash.
 */
struct TarballFinalInput : TarballLockedInput
{
    FinalizationData finalization;

    TarballFinalInput(
        const Settings & settings,
        std::string url,
        std::string effectiveUrl,
        LockingMetadata locking,
        FinalizationData finalization,
        std::optional<std::string> name = std::nullopt)
        : TarballLockedInput(settings, std::move(url), std::move(effectiveUrl), std::move(locking), std::move(name))
        , finalization(std::move(finalization))
    {
    }
};

using TarballInputStates = InputStates<TarballUnlockedInput, TarballLockedInput, TarballFinalInput>;

/**
 * Convert from Attrs to typed tarball input.
 */
TarballUnlockedInput tarballInputFromAttrs(const Settings & settings, const Attrs & attrs);

/**
 * Convert from typed tarball inputs to Attrs (for all states).
 */
Attrs tarballInputToAttrs(const TarballUnlockedInput & input);
Attrs tarballInputToAttrs(const TarballLockedInput & input);
Attrs tarballInputToAttrs(const TarballFinalInput & input);

} // namespace nix::fetchers
