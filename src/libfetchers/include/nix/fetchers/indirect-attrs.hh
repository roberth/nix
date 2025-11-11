#pragma once

#include "nix/fetchers/typed-inputs.hh"
#include "nix/util/types.hh"
#include "nix/util/hash.hh"

namespace nix::fetchers {

/**
 * Base class for Indirect typed inputs, shared across all three states.
 * Indirect inputs are resolved via the flake registry (e.g., "nixpkgs" -> actual Git URL).
 */
struct IndirectInputBase : InputBase
{
    std::string id;

    IndirectInputBase(const Settings & settings, std::string id)
        : InputBase(settings)
        , id(std::move(id))
    {
    }

    virtual ~IndirectInputBase() = default;
};

/**
 * Unlocked Indirect input - just an identifier, optionally with ref or rev.
 */
struct IndirectUnlockedInput : IndirectInputBase
{
    std::optional<std::string> ref;
    std::optional<Hash> rev;

    IndirectUnlockedInput(
        const Settings & settings,
        std::string id,
        std::optional<std::string> ref = std::nullopt,
        std::optional<Hash> rev = std::nullopt)
        : IndirectInputBase(settings, std::move(id))
        , ref(std::move(ref))
        , rev(std::move(rev))
    {
    }
};

/**
 * Locked Indirect input - resolved to a specific input.
 * Note: Indirect inputs delegate to other input types after resolution,
 * so the locked state still refers to the indirect identifier.
 */
struct IndirectLockedInput : IndirectInputBase
{
    Hash rev;
    LockingMetadata locking;

    IndirectLockedInput(const Settings & settings, std::string id, Hash rev)
        : IndirectInputBase(settings, std::move(id))
        , rev(std::move(rev))
    {
    }

    /**
     * Serialize to Attrs for boundary conversion.
     */
    virtual Attrs toAttrs() const;
};

/**
 * Final Indirect input - has narHash after fetching resolved input.
 */
struct IndirectFinalInput : IndirectLockedInput
{
    FinalizationData finalization;

    IndirectFinalInput(const Settings & settings, std::string id, Hash rev, Hash narHash)
        : IndirectLockedInput(settings, std::move(id), std::move(rev))
        , finalization(std::move(narHash))
    {
    }

    /**
     * Serialize to Attrs including final-state attributes.
     */
    Attrs toAttrs() const override;
};

// Convenience type alias
using IndirectInputStates = InputStates<IndirectUnlockedInput, IndirectLockedInput, IndirectFinalInput>;

// Conversion functions for backward compatibility
IndirectUnlockedInput indirectInputFromAttrs(const Settings & settings, const Attrs & attrs);
Attrs indirectInputToAttrs(const IndirectUnlockedInput & input);
Attrs indirectInputToAttrs(const IndirectLockedInput & input);
Attrs indirectInputToAttrs(const IndirectFinalInput & input);

} // namespace nix::fetchers
