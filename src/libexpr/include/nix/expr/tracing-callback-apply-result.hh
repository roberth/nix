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
 *  - TracingObject's getType/getInt/etc. record sub-Q Terminals in
 *    the main trie via `writer.logQuery + logQueryResult`.
 *    Appropriate for cached-fn results and other apply-results whose
 *    Q hash participates in the env history.
 *
 *  - TracingCallbackApplyResult's methods route observations into the
 *    enclosing callback cell's `runningObsSet` via `recordD2`. Those
 *    observations are later snapshotted (by value) into an
 *    ObservationSet and referenced from a SelectorCallbackApply
 *    request via the `argObsSet` payload field.
 *
 * See tracing-cache-callback-model.md for the recording protocol
 * and the sampling moments where the obsSet is snapshotted.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/observation-set.hh"
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

    /* SelectorApply producer identifying this apply-result. */
    ref<const trace::Selector> producer;

    /* Cached hex of producer's Q hash. */
    std::string qHex;

    /* Argument cell — same shape as TracingObject. */
    std::shared_ptr<ArgCell> argCell;

    /* The enclosing callback firing's cell. Observations in recordD2
       append to callbackCell->callbackState.runningObsSet directly. */
    ref<CallbackArgCell> callbackCell;

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which records ONE observation
       keyed on the producer Selector. Subsequent calls decode the
       cached result. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    void recordD2(ref<const trace::Selector> query, const trace::ResultVariant & result);

public:
    TracingCallbackApplyResult(
        ref<Object> inner,
        TracingWriter & writer,
        ref<const trace::Selector> producer,
        ref<CallbackArgCell> callbackCell);

    std::optional<ref<const trace::Selector>> getSelector() const override { return producer; }

    TracingCallbackApplyResult & withArgCell(std::shared_ptr<ArgCell> cell)
    {
        argCell = std::move(cell);
        return *this;
    }

    std::shared_ptr<ArgCell> getProxyArgCell() const override { return argCell; }

    /** The apply-result's Q hash hex — content hash of the stored
        SelectorApply producer. */
    std::optional<std::string> getSelectorHashHex() const override { return qHex; }

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
