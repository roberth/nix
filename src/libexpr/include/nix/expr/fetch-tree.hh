#pragma once

#include "nix/expr/eval.hh"
#include "nix/fetchers/mountable-tree.hh"

namespace nix {

/**
 * Convert a libfetchers `Input` to libexpr `Value`.
 *
 * The `MountableTree` carries the predicted storePath used for the
 * eager `outPath` string render, plus a lazy accessor thunk that's
 * available for the future path-typed outPath shape (`fetchTree
 * { lazy = true; }`).
 *
 * @param `callPos` optional position of the `fetchTree` / `fetchGit` / ... call
 *   that produces this attrset, added to every attribute of the returned
 *   attrset as its `Attr::pos` for diagnostics and `unsafeGetAttrPos`.
 */
void emitTreeAttrs(
    EvalState & state,
    PosIdx callPos,
    const fetchers::MountableTree & tree,
    const fetchers::Input & input,
    Value & v,
    bool emptyRevFallback = false,
    bool forceDirty = false);

} // namespace nix
