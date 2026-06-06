#pragma once
/**
 * @file
 *
 * @brief Completes the libutil-side forward declaration of
 * `SourceRootKind` with the language-level enumerators.
 *
 * The tag splits path values by how they should behave when admitted
 * into the language: where do positions resolve to, what does string
 * coercion produce, can the path be copied to the store?
 *
 * libutil holds the tag opaquely (see <nix/util/source-root.hh>) so
 * its IO primitives (`SourceAccessor`, `SourcePath`) stay free of
 * language semantics. libexpr defines the enumerators and the
 * dispatch sites that consume them.
 */

#include "nix/util/resolve-symlinks.hh"
#include "nix/util/source-root.hh"

namespace nix {

enum class SourceRootKind : std::uint8_t {
    /**
     * Nix-internal helpers: corepkgs, derivation-internal,
     * empty fallbacks, test fixtures. Path values rooted here
     * should not be user-visible — coercing one to a string is an
     * impurity, positions resolve to `null`, and `copyPathToStore`
     * rejects them.
     */
    Internal,

    /**
     * Filesystem path. `toString` returns the raw absolute path;
     * `"${...}"` interpolation copies the specific subpath into
     * the store. Matches Nix's historical behaviour for `/etc/foo`,
     * `/nix/store/X-source` literals, etc.
     */
    System,

    /**
     * Fetched tree (the typical `fetchTree` result). Both
     * `toString` and `"${...}"` have copy-to-store semantics: the
     * root materialises into a storepath and the subpath is
     * appended. Path concatenation with a Copyable path as a
     * *non-first* operand is rejected — the resulting subpath
     * would not be a meaningful address against the fetched tree.
     * A Copyable path as the *first* operand followed by a string
     * is allowed and produces a Copyable-rooted path at the joined
     * subpath (the common `fetchTreeResult + "/sub"` use case).
     */
    Copyable,
};

/**
 * Boundary policy for `Copyable` accessors. Adds absolute-symlink
 * rejection on top of `StrictAccessorBoundary`'s `..` rejection.
 *
 * Why absolute symlinks are rejected for Copyable but not for the
 * libutil-level `StrictAccessorBoundary`: a Copyable tree is meant
 * to be position-independent — it materialises into a storepath
 * at some point, and `/` shifts meaning at that boundary. Pre-
 * materialisation, a symlink whose target is `/sibling` resolves
 * to `<accessor-root>/sibling`. Post-materialisation, the same
 * symlink file in the store points at the *real* `/sibling` (the
 * system root). Two different files would be read at the same
 * symlink depending on whether the tree has been materialised
 * yet. Refusing absolute symlinks at admission keeps Copyable
 * trees actually relocatable.
 *
 * `StrictAccessorBoundary` itself stays absolute-symlink-tolerant
 * because its other callers (flake-input layer in main, etc.)
 * don't have the materialisation-relocation concern Copyable does.
 */
struct StrictCopyableBoundary : StrictAccessorBoundary
{
    using StrictAccessorBoundary::StrictAccessorBoundary;

    void onAbsoluteSymlink(const CanonPath & link, std::string_view target) const
    {
        throw AccessorBoundaryEscape(
            "absolute symlink '%s' (target '%s') in the source tree at %s is not allowed; "
            "Copyable trees must be position-independent so that materialising them to the "
            "store does not change which file a symlink resolves to",
            accessor.showPath(link),
            std::string(target),
            accessor.showPath(CanonPath::root));
    }
};

/**
 * Kind-aware `resolveSymlinks`: walks `path` through `root.accessor`
 * and applies a boundary policy chosen by `root.kind`.
 *
 * - `Copyable`: `StrictCopyableBoundary`. A fetched tree is a
 *   contained unit; an `..`-past-root has no meaningful target
 *   inside the tree, and the silent lexical clamp inherited from
 *   `CanonPath`'s raw-string constructor would name something
 *   wrong. Absolute symlinks are also rejected — see
 *   `StrictCopyableBoundary`. Throws `AccessorBoundaryEscape` on
 *   either case.
 * - `System`: bare lenient walker, no boundary check. rootFS *is*
 *   the real filesystem, and the historical lexical-`..` clamp at
 *   accessor root has the meaning the user expects. Preserves the
 *   long-standing `./foo + "../bar"` idiom that depends on it.
 * - `Internal`: not handled. Internal accessors hold trusted
 *   nix-internal helpers (corepkgs, derivation-internal, ...);
 *   user code shouldn't reach them via dispatch sites that have
 *   already rejected. The few internal eval paths that *do* read
 *   Internal-rooted files (e.g. corepkgs imports) bypass this
 *   wrapper and call the bare resolver directly. If Internal
 *   reaches here, `unreachable()` fires so the reach terminates
 *   the process rather than silently picking a default — the
 *   intent is that a `catch` block somewhere upstream cannot
 *   swallow this design-gap signal.
 *
 * Default `mode` is `Full`, matching the bare `resolveSymlinks`
 * default. Pick explicitly when the shape differs:
 * - reading a file (`parseExprFromFile`, `readFile` primops): `Full`
 *   — the trailing component may itself be a symlink.
 * - resolving to a directory whose trailing component should stay
 *   intact (flake-input subdir, `copyPathToStore`'s ancestor walk):
 *   `Ancestors`.
 *
 * Callers that want a domain-specific diagnostic catch
 * `AccessorBoundaryEscape` and rewrap (see e.g.
 * `flake.cc::resolveRelativePath`).
 */
inline CanonPath
resolveSymlinks(const SourceRoot & root, std::string_view path, SymlinkResolution mode = SymlinkResolution::Full)
{
    switch (root.kind) {
    case SourceRootKind::System:
        return nix::resolveSymlinks(*root.accessor, path, mode);
    case SourceRootKind::Copyable:
        return nix::resolveSymlinks(*root.accessor, path, mode, StrictCopyableBoundary{*root.accessor});
    case SourceRootKind::Internal:
        /* No `throw std::logic_error` here, even though "loud
           programmer error" is the intent: a `std::` exception is
           non-idiomatic in this codebase (which uses
           `MakeError`/`Error`), and an `Error` subclass *could* be
           swallowed by an upstream `catch (Error &)`. `unreachable()`
           terminates outright — no catch can intercept it — which is
           the loudest signal available. The design-gap message
           lives in the doc comment above. */
        unreachable();
    }
    unreachable();
}

inline CanonPath
resolveSymlinks(const SourceRoot & root, const CanonPath & path, SymlinkResolution mode = SymlinkResolution::Full)
{
    return resolveSymlinks(root, std::string_view{path.abs()}, mode);
}

inline CanonPath resolveSymlinks(const RootedPath & rp, SymlinkResolution mode = SymlinkResolution::Full)
{
    return resolveSymlinks(*rp.root, rp.path, mode);
}

} // namespace nix
