#pragma once
///@file

#include "nix/fetchers/typed-inputs.hh"
#include "nix/fetchers/path-attrs.hh"
#include "nix/fetchers/git-attrs.hh"
#include "nix/fetchers/tarball-attrs.hh"
#include "nix/fetchers/github-attrs.hh"
#include "nix/fetchers/mercurial-attrs.hh"
#include "nix/fetchers/indirect-attrs.hh"

#include <variant>
#include <optional>

namespace nix::fetchers {

/**
 * Variant that can hold any unlocked typed input.
 * Used for polymorphic storage of inputs before they are locked.
 */
using AnyUnlockedInput = std::variant<
    PathUnlockedInput,
    GitUnlockedInput,
    TarballUnlockedInput,
    GitHubUnlockedInput, // Also covers GitLab and SourceHut
    MercurialUnlockedInput,
    IndirectUnlockedInput>;

/**
 * Variant that can hold any locked typed input.
 * Used for polymorphic storage of inputs after resolution.
 */
using AnyLockedInput = std::variant<
    PathLockedInput,
    GitLockedInput,
    TarballLockedInput,
    GitHubLockedInput, // Also covers GitLab and SourceHut
    MercurialLockedInput,
    IndirectLockedInput>;

/**
 * Variant that can hold any final typed input.
 * Used for polymorphic storage of inputs after fetching to store.
 */
using AnyFinalInput = std::variant<
    PathFinalInput,
    GitFinalInput,
    TarballFinalInput,
    GitHubFinalInput, // Also covers GitLab and SourceHut
    MercurialFinalInput,
    IndirectFinalInput>;

/**
 * Variant that can hold any typed input at any state.
 * Used when the state is unknown at compile time.
 */
using AnyTypedInput = std::variant<
    // Unlocked states
    PathUnlockedInput,
    GitUnlockedInput,
    TarballUnlockedInput,
    GitHubUnlockedInput,
    MercurialUnlockedInput,
    IndirectUnlockedInput,
    // Locked states
    PathLockedInput,
    GitLockedInput,
    TarballLockedInput,
    GitHubLockedInput,
    MercurialLockedInput,
    IndirectLockedInput,
    // Final states
    PathFinalInput,
    GitFinalInput,
    TarballFinalInput,
    GitHubFinalInput,
    MercurialFinalInput,
    IndirectFinalInput>;

/**
 * Get the type string from any typed input variant.
 */
inline std::string getTypedInputType(const AnyTypedInput & input)
{
    return std::visit([](const auto & i) -> std::string { return i.type; }, input);
}

/**
 * Get the Settings from any typed input variant.
 */
inline const Settings * getTypedInputSettings(const AnyTypedInput & input)
{
    return std::visit([](const auto & i) -> const Settings * { return i.settings; }, input);
}

/**
 * Check if a typed input is locked (has locking metadata).
 */
inline bool isTypedInputLocked(const AnyTypedInput & input)
{
    return std::visit(
        [](const auto & i) -> bool {
            using T = std::decay_t<decltype(i)>;
            // Check if the type has a locking member
            if constexpr (requires { i.locking; }) {
                return true;
            } else {
                return false;
            }
        },
        input);
}

/**
 * Check if a typed input is final (has finalization data).
 */
inline bool isTypedInputFinal(const AnyTypedInput & input)
{
    return std::visit(
        [](const auto & i) -> bool {
            using T = std::decay_t<decltype(i)>;
            // Check if the type has a finalization member
            if constexpr (requires { i.finalization; }) {
                return true;
            } else {
                return false;
            }
        },
        input);
}

/**
 * Get the narHash from a final typed input, or nullopt if not final.
 */
inline std::optional<Hash> getTypedInputNarHash(const AnyTypedInput & input)
{
    return std::visit(
        [](const auto & i) -> std::optional<Hash> {
            using T = std::decay_t<decltype(i)>;
            if constexpr (requires { i.finalization.narHash; }) {
                return i.finalization.narHash;
            } else {
                return std::nullopt;
            }
        },
        input);
}

/**
 * Get the lastModified time from a locked or final typed input, or nullopt if unlocked.
 */
inline std::optional<time_t> getTypedInputLastModified(const AnyTypedInput & input)
{
    return std::visit(
        [](const auto & i) -> std::optional<time_t> {
            using T = std::decay_t<decltype(i)>;
            if constexpr (requires { i.locking.lastModified; }) {
                return i.locking.lastModified;
            } else {
                return std::nullopt;
            }
        },
        input);
}

} // namespace nix::fetchers
