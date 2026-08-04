#pragma once

#include "nix/expr/arg-cell.hh"
#include "nix/expr/observation-set.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"

#include <functional>

namespace nix {

class Store;
class TracingReplayEvaluator;

/**
 * Object that returns cached results from the trie index, with lazy fallback
 * to the inner evaluator when cache misses occur.
 *
 * Uses cascading lookup: temporal children → structural children → shortcuts.
 * Objects are only created for validated trie positions. Child lookups
 * perform incremental validation from the child back to the current position.
 */
class TracingReplayObject : public Object
{
    TracingReplayEvaluator & evaluator;
    TriePosition triePos;

    std::function<ref<Object>()> getInner;
    mutable std::optional<ref<Object>> inner;

    /* Apply-result proxies open a fresh cell rooted at the fn's cell;
       navigation children carry the same cell as their parent. */
    std::shared_ptr<ArgCell> argCell;

    /* When apply-result, the producer Selector identifying it
       structurally. */
    std::optional<ref<const trace::Selector>> producer;

    /* Marks this wrapper as cb-apply-descendant, symmetric to
       TracingObject::cbApplyOrigin. Propagated by navigation. */
    bool cbApplyOrigin = false;

    /* Set by TRE::apply when the SelectorApply lookup missed. Nav
       methods (maybeGetAttr / getListElem / getFunctionInfo) then
       skip their own walker call and go straight to the inner
       evaluator. See withWalkerMissed() rationale. Propagated to
       children constructed on this TRO. */
    bool walkerMissed = false;

    ref<Object> ensureInner() const;

    /**
     * Cascading lookup for leaf results. Returns the parsed R plus
     * the recorded resultHash so callers can push an observation
     * onto the per-invocation history for chain symmetry with the writer.
     */
    template<typename Q, typename R>
    std::optional<std::pair<R, TracingHash>> lookupResult(const Q & query) const;

    /**
     * Cascading lookup for structural children (getAttr, getListElem).
     * Returns the result payload and child TriePosition for further traversal.
     */
    template<typename Q, typename R>
    std::optional<std::pair<R, TriePosition>> lookupStructuralChild(const Q & query) const;

    /* Memoized WHNF lookup. First call to any of the WHNF-subsumed
       getters (getType / getInt / getString / etc.) fires `whnf()`
       which decodes the WHNF from the wrapper's triePos.resultNodeHash
       (parent Selector's Terminal). */
    mutable std::optional<trace::ResultWHNF> cachedWHNF;
    std::optional<const trace::ResultWHNF *> whnf();

public:
    TracingReplayObject(
        TracingReplayEvaluator & evaluator,
        TriePosition triePos,
        std::function<ref<Object>()> getInner,
        std::shared_ptr<ArgCell> argCell,
        std::optional<ref<const trace::Selector>> producer = std::nullopt,
        std::optional<trace::ResultWHNF> cachedWHNF = std::nullopt,
        bool cbApplyOrigin = false,
        bool walkerMissed = false);

    std::optional<ref<const trace::Selector>> getSelector() const override { return producer; }

    std::shared_ptr<ArgCell> getProxyArgCell() const override { return argCell; }

    std::optional<std::string> getSelectorHashHex() const override { return triePos.queryHashStr; }

    /** Read-only accessor for the walker's structural-anchor fallback:
        triePos.factSetHash is this proxy's terminalCur, which serves
        as the startCur candidate for child-Q walks that anchored their
        structural landing chain there. */
    const TriePosition & getTriePos() const { return triePos; }

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
    Value * materialiseAsFunctionValue(
        EvalState & state,
        std::shared_ptr<struct OuterResolver> resolver,
        std::shared_ptr<Evaluator> innerEvaluator) override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    /* No `queryApply` override: cache-boundary apply on a TRO lands
       in `TracingReplayEvaluator::apply`, which owns walker lookup
       and lazy inner activation. Base `Object::queryApply` throws —
       reaching it means a call site skipped the evaluator method. */
};

} // namespace nix
