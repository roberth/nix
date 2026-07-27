#pragma once
/**
 * @file
 * TracingCallbackApplyResult — writer-side wrapper for the result of
 * applying an inner-supplied lambda (crossing back from outer's body
 * via `<cached-fn>(TracingCallbackArg).impl`) to its contraArg.
 *
 * Sibling of TracingObject; the difference is *where* method-level
 * observations land:
 *
 *  - TracingObject's getType/getInt/etc. record sub-Q `Terminals`
 *    rows in the main trie via `writer.logSelector + logResult`.
 *    Appropriate for cached-fn results and other apply-results whose
 *    evolved state hash participates in the env history.
 *
 *  - TracingCallbackApplyResult's methods record observations via
 *    `writer.logCallbackObservation`, which routes them into the
 *    enclosing CallbackCell's `runningObsSet`. Those observations
 *    are later snapshotted (by value) into an ObservationSet and
 *    referenced from a SelectorCallbackApply request via the
 *    `argObsSet` payload field.
 *
 * See tracing-cache-callback-model.md for the recording protocol
 * and the sampling moments where the obsSet is snapshotted.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

class TracingCallbackApplyResult : public Object
{
    ref<Object> inner;
    TracingWriter & writer;

    /* ApplyResultSubject{PostulatedIdempotentRead{TracingCallbackArg.state hash}, contraArg.subject}.
       Subject used to attribute observations recorded on the
       apply-result. */
    Subject applyResultSubject;

    /* Scope inherited from the cb-apply — = contraArg's
       argAncestry = the resolver's callArgAncestry. */
    Hash applyArgAncestry;

    /* subjectId(applyResultSubject, applyArgAncestry) hex — the
       content-only apply-result state hash exposed via getStateHashHex. Computed
       once at construction to match `TracingEvaluator::apply`'s
       `applyArgAncestryStateHashHex` (= what the walker computes too). */
    std::string applyArgAncestryStateHashHex;

    /* Argument-argAncestry cell — same shape as TracingObject. */
    std::shared_ptr<const ArgCell> argCell;

    /* #184: the enclosing callback firing's cell. Observations in
       recordD2 append to callbackCell->callbackState->runningObsSet
       directly — replaces the previous applyId-based lookup through
       writer.callbackCells. Populated by withCallbackCell at
       construction site. */
    std::shared_ptr<const ArgCell> callbackCell;

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which records ONE SelectorGetWHNF
       observation. Subsequent calls decode the cached result. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    void recordD2(const trace::SelectorVariant & query, const trace::ResultVariant & result);

public:
    TracingCallbackApplyResult(
        ref<Object> inner,
        TracingWriter & writer,
        Subject applyResultSubject,
        Hash applyArgAncestry);

    TracingCallbackApplyResult & withArgCell(std::shared_ptr<const ArgCell> cell)
    {
        argCell = std::move(cell);
        return *this;
    }

    TracingCallbackApplyResult & withCallbackCell(std::shared_ptr<const ArgCell> cell)
    {
        callbackCell = std::move(cell);
        return *this;
    }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    /** Symmetric to TracingObject/TracingReplayObject: surface the
        ApplyResultSubject so a subsequent apply on this wrapper
        composes evolving ApplyResultSubject constituents instead of
        the frozen PostulatedIdempotentRead{applyArgAncestryStateHashHex} fallback. */
    const Subject * getSubject() const override { return &applyResultSubject; }
    Hash getArgAncestry() const override { return applyArgAncestry; }

    /** #183: producer Selector for the Selector-only identity path. */
    std::optional<trace::SelectorVariant> getProducer() const override
    {
        return subjectAsSelector(applyResultSubject, applyArgAncestry);
    }

    std::optional<std::string> getStateHashHex() const override { return applyArgAncestryStateHashHex; }

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
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
