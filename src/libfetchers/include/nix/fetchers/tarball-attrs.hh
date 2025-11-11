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
    std::string_view type; // "tarball" or "file"
    std::string url;
    std::optional<std::string> name;
    std::optional<bool> unpack; // Whether to unpack the tarball

    TarballInputBase(
        const Settings & settings,
        std::string_view type,
        std::string url,
        std::optional<std::string> name = std::nullopt)
        : InputBase(settings)
        , type(type)
        , url(std::move(url))
        , name(std::move(name))
    {
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

    TarballUnlockedInput(
        const Settings & settings,
        std::string_view type,
        std::string url,
        std::optional<std::string> name = std::nullopt)
        : TarballInputBase(settings, type, std::move(url), std::move(name))
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
    std::optional<Hash> treeHash; // Git tree hash for tarball cache

    // Optional Git metadata for git archives
    std::optional<std::string> rev;
    std::optional<uint64_t> revCount;

    TarballLockedInput(
        const Settings & settings,
        std::string_view type,
        std::string url,
        std::string effectiveUrl,
        LockingMetadata locking,
        std::optional<Hash> treeHash = std::nullopt,
        std::optional<std::string> name = std::nullopt)
        : TarballInputBase(settings, type, std::move(url), std::move(name))
        , effectiveUrl(std::move(effectiveUrl))
        , locking(std::move(locking))
        , treeHash(std::move(treeHash))
    {
    }

    /**
     * Serialize to Attrs for boundary conversion.
     */
    virtual Attrs toAttrs() const;
};

/**
 * Final tarball input - includes narHash.
 */
struct TarballFinalInput : TarballLockedInput
{
    FinalizationData finalization;

    TarballFinalInput(
        const Settings & settings,
        std::string_view type,
        std::string url,
        std::string effectiveUrl,
        LockingMetadata locking,
        std::optional<Hash> treeHash,
        FinalizationData finalization,
        std::optional<std::string> name = std::nullopt)
        : TarballLockedInput(
              settings,
              type,
              std::move(url),
              std::move(effectiveUrl),
              std::move(locking),
              std::move(treeHash),
              std::move(name))
        , finalization(std::move(finalization))
    {
    }

    /**
     * Serialize to Attrs including final-state attributes.
     */
    Attrs toAttrs() const override;
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
