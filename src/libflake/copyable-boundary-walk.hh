#pragma once
/**
 * @file
 *
 * @brief Helper for the Part 3 flake-input rewrap sites.
 *
 * Four sites in `flake.cc` and one in `flake-primops.cc` share the
 * same five-line shape: build `base + "/" + suffix`, walk it
 * through `resolveSymlinks` under an ad-hoc Copyable `SourceRoot`,
 * catch any `AccessorBoundaryEscape` and rewrap with a flake-input-
 * specific diagnostic. Factor it here so a future change to the
 * boundary policy (e.g. extending to `Full` mode at one site, or
 * tightening the joined-string handling) touches one place.
 *
 * Internal to libflake — not installed.
 */

#include "nix/expr/source-root.hh"
#include "nix/util/resolve-symlinks.hh"
#include "nix/util/source-accessor.hh"

namespace nix::flake {

/**
 * Join `base + "/" + suffix` and walk the result through
 * `resolveSymlinks` against an ad-hoc Copyable `SourceRoot` over
 * `accessor`. On `AccessorBoundaryEscape`, rethrow whatever
 * `diagnose()` returns — typically a flake-input-specific `Error`
 * that names the offending suffix in user-facing terms.
 *
 * `mode` is required (not defaulted) because the choice between
 * `Ancestors` and `Full` is structurally meaningful:
 *
 * - `Ancestors` leaves the trailing component unresolved. Right
 *   for subdir composition (the trailing component is `.../sub`,
 *   not a symlink whose target the caller wants followed) and for
 *   the relative-input join (the caller's downstream reader will
 *   do `Full` itself at file read time).
 * - `Full` resolves the trailing component too. Right when the
 *   caller is about to read the trailing path itself, not when
 *   it's composing for further use.
 *
 * Defaulting `mode` would invite a silent footgun at a future site
 * that picks the wrong shape.
 */
template<typename DiagnoseFn>
inline CanonPath joinAndCheckCopyable(
    ref<SourceAccessor> accessor,
    const CanonPath & base,
    std::string_view suffix,
    SymlinkResolution mode,
    DiagnoseFn diagnose)
{
    auto adhoc = SourceRoot::make(accessor, SourceRootKind::Copyable);
    /* String-level concatenation, not `CanonPath / suffix`: the
       latter pre-strips `..` lexically in its constructor, which
       loses symlink-aware `..` semantics. The walker tokenises
       again and silently skips the empty component when `base` is
       root, so the surface `//suffix` shape is harmless. */
    auto joined = base.abs() + "/" + std::string(suffix);
    try {
        return nix::resolveSymlinks(*adhoc, std::string_view{joined}, mode);
    } catch (AccessorBoundaryEscape &) {
        throw diagnose();
    }
}

} // namespace nix::flake
