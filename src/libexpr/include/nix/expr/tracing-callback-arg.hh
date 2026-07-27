#pragma once
/**
 * @file
 * TracingCallbackArg — wraps an inner-owned Object passed to the
 * outer evaluator during a covariant callback. Each method call
 * records an observation into the enclosing CallbackCell's
 * runningObsSet via `writer.logCallbackObservation`, then delegates
 * to the wrapped Object.
 *
 * The recorded observations are later snapshotted (by value) into an
 * ObservationSet referenced from a SelectorCallbackApply request. At
 * replay the walker reconstructs the callback arg from that recorded
 * obsSet — the inner isn't running, so probes are served from stored
 * observations rather than dispatched live.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/observation-set.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/trace-ids.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-decision-graph.hh"

#include <functional>
#include <memory>
#include <vector>

namespace nix {

class TracingWriter;

/**
 * Object decorator that records every access made on it as an
 * observation on the enclosing callback firing's contra-arg. Used
 * to wrap the argObj a covariant callback receives from the inner
 * side, so the outer's accesses land in the enclosing CallbackCell's
 * runningObsSet.
 */
class TracingCallbackArg : public Object
{
    std::shared_ptr<Object> inner;
    trace::SelectorVariant producer;  ///< Static structural identifier as a Selector
    TracingWriter & writer;
    ref<SourceRoot> rootFSRoot;

    /** This local's Q hash, computed on demand from `producer`. */
    OuterId localId() const { return TracingDecisionGraph::computeSelectorHash(producer); }

    /* The argCell cell this local belongs to. Navigation children
       share the parent's cell. Used for scope-graph topology only;
       identity is derived from `producer`, not the cell. */
    std::shared_ptr<const ArgCell> argCell;

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which records ONE observation
       keyed on this value's producer Selector. Subsequent calls decode
       the cached result. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    void recordObservation(const trace::SelectorVariant & query, const trace::ResultVariant & result);

public:
    TracingCallbackArg(
        std::shared_ptr<Object> inner,
        trace::SelectorVariant producer,
        TracingWriter & writer,
        ref<SourceRoot> rootFSRoot,
        std::shared_ptr<const ArgCell> argCell);

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;
    std::vector<std::string> getAttrNames() override;
    std::string getStringIgnoreContext() override;
    std::string getStringWithoutContext() override;
    std::pair<std::string, NixStringContext> getStringWithContext() override;
    RootedPath getPath() override;
    bool getBool(std::string_view errorCtx = "") override;
    NixInt getInt(std::string_view errorCtx = "") override;
    NixFloat getFloat(std::string_view errorCtx = "") override;
    size_t getListSize() override;
    std::shared_ptr<Object> getListElem(size_t index) override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;
    /** Object-method apply entry. Records the apply as an observation
        (= outer is applying this local to argObj), then delegates to
        `inner->queryApply(argObj)` and wraps the result as another
        TracingCallbackArg with a SelectorApply producer so further
        accesses on the result continue to route observations to the
        enclosing callback cell. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;

    OuterId getStateHash() const { return localId(); }

    std::optional<std::string> getSelectorHashHex() const override
    {
        return localId().to_string(HashFormat::Base16, false);
    }
};

} // namespace nix
