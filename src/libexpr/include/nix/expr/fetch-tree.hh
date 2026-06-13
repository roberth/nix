#pragma once

#include "nix/expr/eval.hh"
#include "nix/fetchers/mountable-tree.hh"

namespace nix {

/**
 * Convert a libfetchers `Input` to libexpr `Value`. The shape of the
 * emitted `outPath` is the caller's choice:
 *
 * - `lazy = false` (default): `outPath` is a store-path string
 *   (the legacy/eager rendering). Requires `tree.storePath` to be
 *   set.
 * - `lazy = true`: `outPath` is a path-typed Value rooted on
 *   `tree.accessor`'s root. Reads through `outPath` go through the
 *   fetcher's accessor without forcing a store copy; string coercion
 *   resolves through `copyPathToStore`. `tree.storePath` is unused
 *   in this shape and may be `std::nullopt`.
 *
 * Both shapes share the same metadata block (narHash, rev,
 * lastModified, …) — there's no chance of drift between them.
 */
void emitTreeAttrs(
    EvalState & state,
    const fetchers::MountableTree & tree,
    const fetchers::Input & input,
    Value & v,
    bool emptyRevFallback = false,
    bool forceDirty = false,
    bool lazy = false);

} // namespace nix
