#pragma once
/**
 * @file
 * ArgScopeCell — the per-apply intrinsic cell that rides on
 * cache-boundary proxies. See the Argument scope: it rides on the
 * proxy graph and Boundary-trace-only discipline sections of
 * doc/design/tracing-eval-cache-content-identity.md.
 *
 * Each cache-boundary proxy (AmbientObject, TracingReplayObject,
 * ReplayLocalObject, and recording-side counterparts) carries a
 * `parent` pointer to whichever proxy produced it. Apply-result
 * and top-level (seed) proxies additionally carry a shared_ptr to
 * an `ArgScopeCell`: a mutable intrinsic hash that XOR-folds
 * observations attributed to this scope, plus a `parent` pointer
 * to the next-outer cell (forming a chain rooted at the cache
 * call's argument). Navigation children (from `maybeGetAttr` /
 * `getListElem`) don't open a new cell; they reuse the parent's
 * view.
 *
 * `contentId()` returns the cell's content-defined identity at
 * this moment: this cell's intrinsic XOR-folded with all ancestor
 * cells' intrinsics (state-creep), combined with a depth-encoded
 * structural marker (reverse-De-Bruijn) so cells at different
 * apply depths don't collide on the empty-intrinsic case.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/hash.hh"

#include <memory>

namespace nix {

struct ArgScopeCell : std::enable_shared_from_this<ArgScopeCell>
{
    /** Reverse-De-Bruijn depth: 0 at the cache call's argument,
        N+1 in a cell whose parent is at depth N. Set at
        construction, immutable. Encoded into `contentId()` as the
        structural marker that distinguishes cells at different
        depths when their intrinsics are equal (in particular,
        empty intrinsics at apply time). */
    int depth = 0;

    /** Observation intrinsic for this scope. XOR-folded with each
        `(queryHashBlanked, responseHash)` contribution from
        observations attributed to this cell. Starts at the
        empty-set hash (the value's content-defined identity at
        apply time, before any observation has happened).
        `mutable` so absorb() can run through a shared_ptr<const
        ArgScopeCell> — only `intrinsic` evolves; depth, parent,
        liveObject, frozenCdi are fixed at construction. */
    mutable Hash intrinsic;

    /** Snapshot of `contentId()` at construction time. By design,
        the seed proxy attached as `liveObject` has its cdi set to
        this same value: `seedProxy.cdi == cell.frozenCdi`. Held on
        the cell because the cell is the natural home for the
        scope's frozen identity — the proxy carries a copy today
        for convenience, but the authority lives here. Stays fixed
        even as `intrinsic` absorbs and `contentId()` evolves; the
        walker uses this for cell-chain matching so the matching
        doesn't need to go through the proxy's virtual getCdiHex(). */
    Hash frozenCdi;

    /** Next-outer cell. Null at the root (the cache call's
        argument). State creep folds parent cells' intrinsics into
        this cell's `contentId()`. */
    std::shared_ptr<const ArgScopeCell> parent;

    /** The live Object the cell represents.
        Held as shared_ptr deliberately. For seed cells this
        creates a cycle: the AmbientObject holds the cell via
        argScope, and the cell holds the AmbientObject via
        liveObject. The cycle leaks the proxy + cell pair for the
        cache call's lifetime. That's a tolerated trade: a weak_ptr
        risked silently going null (proxy released earlier than
        expected) and quietly breaking dispatch — accidental
        missing references are harder to debug than a known leak
        bounded by the cache-call duration. */
    std::shared_ptr<Object> liveObject;

    ArgScopeCell()
        : intrinsic(TracingDecisionGraph::emptySetHash())
        , frozenCdi(TracingDecisionGraph::emptySetHash())
    {
    }

    /** Construct a cell whose parent is `parent_`. depth is one
        deeper than parent (or 0 if parent is null). intrinsic
        starts empty. `liveObject_` may be null at construction
        if the live proxy isn't yet constructed; assign to the
        cell's `liveObject` field afterwards. */
    static std::shared_ptr<ArgScopeCell> make(
        std::shared_ptr<const ArgScopeCell> parent_,
        std::shared_ptr<Object> liveObject_)
    {
        auto cell = std::make_shared<ArgScopeCell>();
        cell->parent = parent_;
        cell->depth = parent_ ? parent_->depth + 1 : 0;
        /* Snapshot contentId() now, before any absorb can run. The
           snapshot captures depth + ancestor intrinsics XOR-folded
           with the empty-set hash. By design seed proxies attached as
           liveObject get this same hash as their cdi — the
           construction dance in makeCachedFnPrimOp.impl wires it that
           way explicitly. */
        cell->frozenCdi = cell->contentId();
        if (liveObject_)
            cell->liveObject = std::move(liveObject_);
        return cell;
    }

    /** Content-defined identity at this moment. Combines:
        - reverse-De-Bruijn depth marker (so empty-intrinsic cells
          at different depths don't collide)
        - this cell's intrinsic
        - XOR-fold of ancestor cells' intrinsics (state creep). */
    Hash contentId() const
    {
        Hash structural = hashString(HashAlgorithm::SHA256, "ambient-" + std::to_string(depth));
        Hash h = TracingDecisionGraph::xorHashes(intrinsic, structural);
        for (auto p = parent; p; p = p->parent)
            h = TracingDecisionGraph::xorHashes(h, p->intrinsic);
        return h;
    }

    /** Fold a single `(queryHashBlanked, responseHash)` observation
        contribution into the cell's intrinsic. Const because
        intrinsic is mutable — callable through shared_ptr<const>. */
    void absorb(const Hash & queryHashBlanked, const Hash & responseHash) const
    {
        intrinsic = TracingDecisionGraph::xorFactIntoHash(
            intrinsic, queryHashBlanked, responseHash);
    }
};

/** Return the proxy's argScope cell — the nearest enclosing apply's
    cell. Navigation children carry the parent's cell directly; apply
    results carry their own fresh cell. Returns null for non-proxy
    Objects or for proxies that haven't been scoped. */
inline std::shared_ptr<const ArgScopeCell> effectiveArgScope(const Object & obj)
{
    return obj.getProxyArgScope();
}

} // namespace nix
