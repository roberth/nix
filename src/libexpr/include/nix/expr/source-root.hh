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

} // namespace nix
