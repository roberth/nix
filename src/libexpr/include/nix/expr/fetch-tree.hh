#pragma once

#include "nix/expr/eval.hh"
#include "nix/fetchers/mountable-tree.hh"

namespace nix {

/**
 * Convert a libfetchers `Input` to libexpr `Value` with an eager
 * (store-path-string) `outPath`. The `MountableTree` carries the
 * predicted storePath for the render.
 */
void emitTreeAttrs(
    EvalState & state,
    const fetchers::MountableTree & tree,
    const fetchers::Input & input,
    Value & v,
    bool emptyRevFallback = false,
    bool forceDirty = false);

/**
 * Like the `MountableTree` overload, but `outPath` is a path-typed
 * Value rooted on the supplied `SourcePath`. Used by `fetchTree
 * { lazy = true; }` so reads through `outPath` go through the
 * fetcher's accessor without forcing a store copy.
 */
void emitTreeAttrs(
    EvalState & state,
    const SourcePath & sourcePath,
    const fetchers::Input & input,
    Value & v,
    bool emptyRevFallback = false,
    bool forceDirty = false);

} // namespace nix
