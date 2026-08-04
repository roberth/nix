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
    ref<Object> inner;
    ref<const trace::Selector> producer;  ///< Static structural identifier as a Selector
    TracingWriter & writer;
    ref<SourceRoot> rootFSRoot;

    /** This local's Q hash. */
    OuterId localId() const { return producer->cachedHash; }

    /* The callback-firing cell this local belongs to. Navigation
       children share the parent's cell. */
    ref<RecordingCallbackArgCell> argCell;

    /* Guard for the self-WHNF observation. First getter to enter
       `whnf()` records `(producer, computeWHNFFromObject(*inner))` on
       the enclosing cell and flips this true; subsequent getters
       recompute the payload but skip the record. Set true at
       construction when the caller already recorded the fact upstream
       (H2 wrapper in `TCA::queryApply`). */
    bool whnfRecorded;
    trace::ResultWHNF whnf();

    void recordObservation(ref<const trace::Selector> query, const trace::ResultVariant & result);

public:
    TracingCallbackArg(
        ref<Object> inner,
        ref<const trace::Selector> producer,
        TracingWriter & writer,
        ref<SourceRoot> rootFSRoot,
        ref<RecordingCallbackArgCell> argCell,
        bool whnfAlreadyRecorded = false);

    /** Typed accessor for the callback firing's cell — lets consumers
        that hold a TCA propagate a `RecordingCallbackArgCell` handle without
        going through the base-typed `getProxyArgCell()`. */
    ref<RecordingCallbackArgCell> getCallbackArgCell() const { return argCell; }

    std::optional<ref<const trace::Selector>> getSelector() const override { return producer; }

    std::shared_ptr<ArgCell> getProxyArgCell() const override { return argCell.get_ptr(); }

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
    /** #217: primop that on apply records a SelectorCallbackApply
        observation with an argObsSet of inner-lambda's probes on
        the outer-arg. Warm reconstructs the same argObsSet by
        replaying recorded probes on the live arg for divergence
        detection. */
    RootValue toValueOrProxy(EvalState & state, std::shared_ptr<struct OuterResolver> resolver) override;
    /** Function-typed TCA materialises as a `<cb-apply>` primop —
        delegate to `toValueOrProxy` which already builds it. */
    Value * materialiseAsFunctionValue(
        EvalState & state,
        std::shared_ptr<struct OuterResolver> resolver,
        std::shared_ptr<Evaluator> /* innerEvaluator */) override
    {
        return *toValueOrProxy(state, std::move(resolver));
    }
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

    std::optional<std::string> getSelectorHashHex() const override
    {
        return localId().toHex();
    }
};

} // namespace nix
