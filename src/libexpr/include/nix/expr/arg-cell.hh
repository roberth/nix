#pragma once
/**
 * @file
 * ArgCell — argAncestry-graph node for cache-boundary proxies.
 * Carries only structural topology (depth, parent, liveObject).
 *
 * Under the design in
 * doc/design/tracing-eval-cache-content-identity-via-asks.md,
 * argAncestry state ids are pure functions of (subject, factset) and are not
 * stored on the cell. The cell exists for navigation through the
 * proxy graph; the `depth` field provides the static positional
 * handle that subjects use as their content-id seed.
 */

#include "nix/expr/evaluator.hh"

#include <memory>

namespace nix {

struct ArgCell : std::enable_shared_from_this<ArgCell>
{
    /** Reverse-De-Bruijn depth: 0 at the cache call's argument,
        N+1 in a cell whose parent is at depth N. Set at
        construction, immutable. Used as the positional handle
        when computing argAncestry state ids via scopeStateIdAfter. */
    int depth = 0;

    /** Next-outer cell. Null at the root (the cache call's
        argument). */
    std::shared_ptr<const ArgCell> parent;

    /** The live Object the cell represents. The walker's
        cell-chain resolution returns this to identify the live
        proxy for a recorded positional handle. */
    std::shared_ptr<Object> liveObject;

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
