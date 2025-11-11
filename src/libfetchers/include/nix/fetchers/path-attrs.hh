#pragma once
///@file

#include "nix/fetchers/typed-inputs.hh"
#include <filesystem>

namespace nix::fetchers {

/**
 * Base attributes for path inputs.
 * Path inputs reference local filesystem paths.
 */
struct PathInputBase : InputBase
{
    std::filesystem::path path;

    PathInputBase(const Settings & settings, std::filesystem::path path)
        : InputBase(settings)
        , path(std::move(path))
    {
    }
};

/**
 * Unlocked path input.
 * Contains the path but may not have lastModified or narHash yet.
 */
struct PathUnlockedInput : PathInputBase
{
    // Optional fake tree info that users can provide
    std::optional<std::string> rev;
    std::optional<uint64_t> revCount;
    std::optional<uint64_t> lastModified;
    std::optional<Hash> narHash;

    PathUnlockedInput(const Settings & settings, std::filesystem::path path)
        : PathInputBase(settings, std::move(path))
    {
    }
};

/**
 * Locked path input.
 * Has lastModified from when the path was accessed.
 */
struct PathLockedInput : PathInputBase
{
    // Optional fake tree info (preserved from unlocked)
    std::optional<std::string> rev;
    std::optional<uint64_t> revCount;
    std::optional<Hash> narHash;

    // Locking metadata
    LockingMetadata locking;

    PathLockedInput(const Settings & settings, std::filesystem::path path, LockingMetadata locking)
        : PathInputBase(settings, std::move(path))
        , locking(std::move(locking))
    {
    }

    /**
     * Serialize to Attrs for boundary conversion.
     */
    virtual Attrs toAttrs() const;
};

/**
 * Final path input.
 * Has narHash from copying to store.
 */
struct PathFinalInput : PathLockedInput
{
    FinalizationData finalization;

    PathFinalInput(
        const Settings & settings, std::filesystem::path path, LockingMetadata locking, FinalizationData finalization)
        : PathLockedInput(settings, std::move(path), std::move(locking))
        , finalization(std::move(finalization))
    {
    }

    /**
     * Serialize to Attrs including final-state attributes.
     */
    Attrs toAttrs() const override;
};

using PathInputStates = InputStates<PathUnlockedInput, PathLockedInput, PathFinalInput>;

/**
 * Conversion functions between Attrs and typed path inputs.
 */
PathUnlockedInput pathInputFromAttrs(const Settings & settings, const Attrs & attrs);
Attrs pathInputToAttrs(const PathUnlockedInput & input);
Attrs pathInputToAttrs(const PathLockedInput & input);
Attrs pathInputToAttrs(const PathFinalInput & input);

} // namespace nix::fetchers
