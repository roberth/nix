#pragma once
/**
 * @file
 * ArgCell — scope-graph node for cache-boundary proxies.
 * Carries only structural topology (depth, parent, liveObject).
 *
 * Under the design in
 * doc/design/tracing-eval-cache-subject-identity.md,
 * state hashes are pure functions of (subject, factset) and are not
 * stored on the cell. The cell exists for navigation through the
 * proxy graph; the `depth` field provides the static positional
 * handle that subjects use as their base for state hashes.
 */

#include "nix/expr/evaluator.hh"

#include <memory>

namespace nix {

struct QState; // defined in q-state.hh; forward-declared here so
               // topology-only cells don't pull the heavy dependencies.

struct ArgCell : std::enable_shared_from_this<ArgCell>
{
    /** Reverse-De-Bruijn depth: 0 at the cache call's argument,
        N+1 in a cell whose parent is at depth N. Set at
        construction, immutable. Used as the positional handle
        when computing state hashes via stateHashAfter. */
    int depth = 0;

    /** Next-outer cell. Null at the root (the cache call's
        argument). */
    std::shared_ptr<const ArgCell> parent;

    /** The live Object the cell represents. The walker's
        cell-chain resolution returns this to identify the live
        proxy for a recorded positional handle. */
    std::shared_ptr<Object> liveObject;

    /** Per-Selector-invocation Q-evolution state. Non-null on cells
        that represent an application (SelectorApply / SelectorCallbackApply /
        root SelectorImport / SelectorExpr) and thus own a Selector
        chain. Null on topology-only cells (localCell / seedCell in
        expr-from-object.cc) that carry proxy identity through a
        boundary without owning a Selector chain.

        `mutable` because ArgCells are held throughout via
        `shared_ptr<const ArgCell>`; the pointer needs to be
        assignable through a const cell. The QState pointee itself is
        not const — its fields evolve as observations attribute to
        this cell.

        See q-state.hh for the field breakdown and the concurrency
        rationale. */
    mutable std::shared_ptr<QState> qState;

    /** Construct a cell whose parent is `parent_`. depth is one
        deeper than parent (or 0 if parent is null). `liveObject_`
        may be null at construction if the live proxy isn't yet
        constructed; assign to the cell's `liveObject` field
        afterwards. */
    static std::shared_ptr<ArgCell> make(
        std::shared_ptr<const ArgCell> parent_,
        std::shared_ptr<Object> liveObject_)
    {
        auto cell = std::make_shared<ArgCell>();
        cell->parent = parent_;
        cell->depth = parent_ ? parent_->depth + 1 : 0;
        if (liveObject_)
            cell->liveObject = std::move(liveObject_);
        return cell;
    }
};

/** Return the proxy's argCell cell — the nearest enclosing apply's
    cell. Navigation children carry the parent's cell directly; apply
    results carry their own fresh cell. Returns null for non-proxy
    Objects or for proxies that haven't been scoped. */
inline std::shared_ptr<const ArgCell> effectiveArgCell(const Object & obj)
{
    return obj.getProxyArgCell();
}

} // namespace nix
