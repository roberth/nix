#pragma once
/**
 * @file
 * ArgScopeCell — the per-apply argument-scope cell that rides on
 * cache-boundary proxies.
 *
 * See doc/design/tracing-eval-cache-content-identity.md, the
 * Argument scope: it rides on the proxy graph section.
 *
 * Each cache-boundary proxy (AmbientObject, TracingReplayObject,
 * ReplayLocalObject, and recording-side counterparts) carries a
 * `parent` pointer to whichever proxy produced it. Apply-result and
 * top-level (seed) proxies additionally carry an `ArgScopeCell`: the
 * AmbientId of the argument that opened the cell, together with the
 * live Object the id resolves to in this proxy's call. Navigation
 * children (from `maybeGetAttr` / `getListElem`) don't open a new
 * cell; they hold a back-pointer with no `argScope`.
 *
 * Resolution walks the `parent` chain through the proxies, checking
 * each apply-result cell's id for a match. State creep at fact
 * emission walks the same chain to XOR-fold ancestor observations
 * into the content-defined identity at each `ambient-N` position.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-ids.hh"

#include <memory>

namespace nix {

struct ArgScopeCell
{
    /** The id under which observations on this argument are tracked. */
    AmbientId id;
    /** The live Object backing the argument in this call. Methods on
        this Object produce live responses for ambient dispatches that
        resolve to `id`. */
    std::shared_ptr<Object> liveObject;
};

} // namespace nix
