#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/hash.hh"
#include "nix/fetchers/attrs.hh"

#include <optional>
#include <variant>

namespace nix::fetchers {

struct Settings;

/**
 * Metadata added when an input is locked.
 * Common across all input types.
 */
struct LockingMetadata
{
    time_t lastModified;

    LockingMetadata()
        : lastModified(0)
    {
    }

    explicit LockingMetadata(time_t lastModified)
        : lastModified(lastModified)
    {
    }
};

/**
 * Data added when an input is finalized (copied to store).
 * The presence of this struct indicates the input is final.
 */
struct FinalizationData
{
    Hash narHash;

    explicit FinalizationData(Hash narHash)
        : narHash(std::move(narHash))
    {
    }
};

/**
 * Template for defining the three-state type hierarchy for an input scheme.
 * Each scheme should define:
 * - UnlockedType: User-provided specification (may be incomplete)
 * - LockedType: After resolution/fetching (has all identifying info)
 * - FinalType: After copying to store (has narHash)
 */
template<typename Unlocked, typename Locked, typename Final>
struct InputStates
{
    using UnlockedType = Unlocked;
    using LockedType = Locked;
    using FinalType = Final;
};

/**
 * Concepts for compile-time validation of typed input hierarchies.
 */
template<typename T>
concept IsUnlockedInput = requires(T t) {
    // Unlocked inputs may have optional identifying information
    typename T::type;
};

template<typename T>
concept IsLockedInput = requires(T t) {
    // Locked inputs must have locking metadata
    { t.locking } -> std::convertible_to<LockingMetadata>;
};

template<typename T>
concept IsFinalInput = IsLockedInput<T> && requires(T t) {
    // Final inputs must have finalization data
    { t.finalization } -> std::convertible_to<FinalizationData>;
};

/**
 * Conversion utilities between Attrs and typed inputs.
 * These enable gradual migration from dynamic to static typing.
 */

/**
 * Convert Attrs to a typed input.
 * Throws if required attributes are missing or have wrong types.
 */
template<typename T>
T inputFromAttrs(const Settings & settings, const Attrs & attrs);

/**
 * Convert a typed input to Attrs.
 * Used for serialization and backward compatibility.
 */
template<typename T>
Attrs inputToAttrs(const T & input);

} // namespace nix::fetchers
