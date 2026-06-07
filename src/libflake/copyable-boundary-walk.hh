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

#include <cassert>

#include "nix/expr/source-root.hh"
#include "nix/util/resolve-symlinks.hh"

namespace nix::flake {

/**
 * Join `base + "/" + suffix` and walk the result through
 * `resolveSymlinks` under `root`. On `AccessorBoundaryEscape`,
 * rethrow whatever `diagnose()` returns — typically a flake-input-
 * specific `Error` that names the offending suffix in user-facing
 * terms.
 *
 * The five current flake-input call sites all pass Copyable
 * roots — they're each composing a path against a fetched
 * tree's accessor, and `StrictCopyableBoundary` is the policy
 * that exists for that case. The diagnostic the name promises
 * ("boundary-escape rewrap") only makes sense in that frame:
 * for a System root the boundary policy is a silent clamp and
 * the catch becomes dead code; for an Internal root the wrapper
 * terminates the process. The helper accordingly asserts
 * `root.kind == Copyable`; future kind-agnostic call sites
 * should be a deliberate signature change rather than an
 * accidental shape match.
 *
 * Taking `ref<SourceRoot>` rather than `ref<SourceAccessor> +
 * kind` keeps the helper out of the SourceRoot lifecycle: the
 * caller already has a root (or builds one once via
 * `state.getOrCreateRoot(accessor, SourceRootKind::Copyable)`,
 * which memoises a (accessor, kind)-keyed root for the eval's
 * lifetime) and hands it through. Today only fetched-tree
 * accessors are admitted as Copyable; if a future caller starts
 * routing rootFS through `getOrCreateRoot` under Copyable kind,
 * the cache would faithfully record that — but the kind contract
 * on this helper would still hold.
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
    ref<SourceRoot> root, const CanonPath & base, std::string_view suffix, SymlinkResolution mode, DiagnoseFn diagnose)
{
    assert(root->kind == SourceRootKind::Copyable);
    /* String-level concatenation, not `CanonPath / suffix`: the
       latter pre-strips `..` lexically in its constructor, which
       loses symlink-aware `..` semantics. The walker tokenises
       again and silently skips the empty component when `base` is
       root, so the surface `//suffix` shape is harmless. */
    auto joined = base.abs() + "/" + std::string(suffix);
    try {
        return nix::resolveSymlinks(*root, std::string_view{joined}, mode);
    } catch (AccessorBoundaryEscape &) {
        throw diagnose();
    }
}

} // namespace nix::flake
