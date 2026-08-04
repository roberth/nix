#pragma once
/**
 * @file
 * ReplayCallbackArg: serves recorded responses for an inner-supplied
 * callback arg during replay-time apply invocation.
 *
 * The outer's covariant callback (e.g. `f x` where `f` is an outer
 * lambda and `x` is an inner-supplied arg) lets the outer access
 * inner-side data. On the recording side the inner wraps the arg in
 * TracingCallbackArg so the outer's accesses land in the enclosing
 * CallbackCell's runningObsSet, which is later snapshotted into a
 * SelectorCallbackApply request's referenced ObservationSet. On replay
 * the inner isn't running, so the arg isn't reconstructable as a
 * live Object — but its content was persisted by value inside that
 * recorded obsSet. ReplayCallbackArg reads probes back from the
 * obsSet so the outer can invoke its callback against a
 * deterministic frozen image of the recorded arg.
 *
 * This is what makes covariant-callback caching actually validate:
 * with this object in place, dispatching a recorded
 * SelectorCallbackApply materialises a ReplayCallbackArg backed by the
 * recorded obsSet, invokes `fn->queryApply(replayArg)` live, and
 * compares the response against the recording. If the outer changed
 * (different lambda body) the response differs and the walker
 * misses cleanly.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/observation-set.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-ids.hh"
#include "nix/util/hash.hh"

namespace nix {

class TracingDecisionGraph;

class ReplayCallbackArg : public Object
{
    /* Producer Selector identifying this proxy. Content hash IS the
       proxy's Q identity — the walker matches it against recorded Qs.

       For root (cb-apply) locals: `SelectorArg{depth}`.

       For children minted by maybeGetAttr/getListElem:
       `SelectorGetAttr{name, from=hex(parent producer Q)}` or the
       list-elem equivalent — same shape as OuterObject's navigation
       children on the cold side. */
    ref<const trace::Selector> producer;
    TracingDecisionGraph & decisionGraph;
    ref<SourceRoot> rootFSRoot;
    /* EvalState used for primop construction in `defeatCache`. The
       outer's EvalState (= where the primop will be applied) is the
       right one in principle; in practice any live EvalState works
       because primop construction allocates a Value off the shared
       gc heap and the lambda capture holds onto the construction
       arguments by value. Threaded from `materialiseLocalStandin`. */
    EvalState * state;

    /* obsSet response source: method responses are looked up in
       this map by selectorHash. Populated by the walker's
       callbackApply dispatch from the SelectorCallbackApply's
       referenced observation set — each entry is (selectorHash →
       CBOR response payload). */
    std::shared_ptr<std::map<TracingHash, std::string>> obsSetResponses;

    /* Memoized WHNF response. The recorder logs ONE observation per
       value force (keyed on the value's producer Selector); the walker
       must reuse the cached response on subsequent calls (e.g. when
       queryApply invokes on the same ReplayCallbackArg multiple times
       across dispatched facts). */
    std::optional<trace::ResultWHNF> cachedWHNF;
    /** Read recorded WHNF for this proxy. Memoized; subsequent calls
        return the same result without re-probing. Returns the cached
        WHNF as a const reference so callers can decode the payload
        by alternative. */
    const trace::ResultWHNF & whnf();

    /* Navigation children carry the same cell as their parent; the
       top-level (cb-arg) Local carries the apply's cell. */
    std::shared_ptr<ArgCell> argCell;

public:
    /* Constructor for derived children. Subject is built by the
       parent's maybeGetAttr / getListElem as `DerivedSubject{parent,
       ...}`. */
    ReplayCallbackArg(
        ref<const trace::Selector> producer_,
        TracingDecisionGraph & dg,
        ref<SourceRoot> rootFSRoot,
        EvalState * state,
        std::shared_ptr<ArgCell> argCell)
        : producer(producer_)
        , decisionGraph(dg), rootFSRoot(std::move(rootFSRoot)), state(state), argCell(std::move(argCell)) {}

    std::optional<ref<const trace::Selector>> getSelector() const override { return producer; }

    /** Attach an obsSet response source. Each probe on this
        ReplayCallbackArg (or its derived children, if the
        shared_ptr is passed through) looks up its selectorHash in
        this map and decodes the CBOR payload as the response
        Result. Enables live outer validation from the recorded
        obsSet content. */
    ReplayCallbackArg & withObsSetResponses(
        std::shared_ptr<std::map<TracingHash, std::string>> map)
    {
        obsSetResponses = std::move(map);
        return *this;
    }

    std::shared_ptr<std::map<TracingHash, std::string>> getObsSetResponses() const
    {
        return obsSetResponses;
    }

    const ReplayCallbackArg * asReplayCallbackArg() const override { return this; }

    std::shared_ptr<ArgCell> getProxyArgCell() const override { return argCell; }

    /** Content-defined identity is the producer Selector's cached hash.
        Lets evaluator.apply compute the apply Request hash when this
        ReplayCallbackArg is the arg. */
    std::optional<std::string> getSelectorHashHex() const override
    {
        return producer->cachedHash.toHex();
    }

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
    ObjectType getType() override;
    ObjectType getTypeLazy() override;
    RootValue defeatCache() override;
    /** Materialise a Value-shaped representation of this recorded proxy.
        For non-function types, returns a thunk over ExprFromObject that
        lazily probes the recorded responses. For nFunction, returns a
        primop that throws — higher-order callback application is not
        currently supported (see the impl for details). */
    RootValue toValueOrProxy(EvalState & state, std::shared_ptr<OuterResolver> resolver) override;
    /** Function-typed RCA materialises as a `<cb-replay>` primop —
        delegate to `toValueOrProxy` which already builds it. */
    Value * materialiseAsFunctionValue(
        EvalState & state,
        std::shared_ptr<OuterResolver> resolver,
        std::shared_ptr<Evaluator> /* innerEvaluator */) override
    {
        return *toValueOrProxy(state, std::move(resolver));
    }
    std::optional<FunctionInfo> getFunctionInfo() override;
    /** Recorded frozen callback args can't be applied without
        reconstructing the function body from value-structure atoms
        or comparing the live arg's content to the recorded arg's
        content. Until either lands, an apply on a ReplayCallbackArg
        is undecidable — we don't know whether the recorded result
        still applies for the current live arg. Throw a recognizable
        signal that callers can interpret as "walker miss, fall
        through to live re-eval."

        Today no caller routes here: the apply chain still goes
        through `Object::defeatCache` + value-level `callFunction`,
        not `Object::queryApply`. The override exists so that when
        callers are restructured to call `queryApply` uniformly, this
        is the entry that fires. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
