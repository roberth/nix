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
    std::shared_ptr<const ArgCell> argCell;

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
        TracingReplayEvaluator & evaluator, TriePosition triePos, std::function<ref<Object>()> getInner);

    /** Set the proxy's argCell. Returns *this for chaining. */
    TracingReplayObject & withArgCell(std::shared_ptr<const ArgCell> argScope_)
    {
        argCell = std::move(argScope_);
        return *this;
    }

    /** Attach the apply-result producer Selector. Mirrors the
        writer-side `TracingObject::withProducer`. */
    TracingReplayObject & withProducer(ref<const trace::Selector> p)
    {
        producer = std::move(p);
        return *this;
    }

    std::optional<ref<const trace::Selector>> getSelector() const override { return producer; }

    /** Symmetric to `TracingObject::withCbApplyOrigin`. Walker
        propagates through navigation children so their
        `producer` matches cold's Q payloads. */
    TracingReplayObject & withCbApplyOrigin()
    {
        cbApplyOrigin = true;
        return *this;
    }

    /** Pre-populate `cachedWHNF` at wrapper construction. Used by
        cell-migration Phase B: `TracingReplayEvaluator::apply`
        pre-invokes lookup(SelectorApply{...}) via the cell and, on
        hit, populates the walker-side applyResult wrapper's cached
        WHNF from the Terminal's Result payload. Downstream `.foo`
        probes on this wrapper use the cached WHNF for membership
        without a separate walk. */
    TracingReplayObject & withCachedWHNF(trace::ResultWHNF whnf_)
    {
        cachedWHNF = std::move(whnf_);
        return *this;
    }

    /** Mark this TRO as walker-missed at construction: TRE::apply's
        cell-anchor SelectorApply lookup returned nullopt, so we're
        wrapping an unrecorded value. maybeGetAttr/getListElem check
        this and skip their own walker call — descendants of a
        never-recorded parent are (in practice) also never recorded,
        so the walker attempt is nearly-guaranteed miss with a heavy
        cost. Trades the rare case of "descendant was recorded via
        some other path" for avoiding the walker overhead on the
        common case. */
    TracingReplayObject & withWalkerMissed()
    {
        walkerMissed = true;
        return *this;
    }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

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
    std::optional<FunctionInfo> getFunctionInfo() override;
    /** Object-method apply entry. Delegates to the
        TracingReplayEvaluator's apply (= walker lookup + cached
        TracingReplayObject wrapping with the lazy callback to
        inner), then returns the result as a shared_ptr<Object>.
        This is the Object-method counterpart of
        TracingReplayEvaluator::apply, letting callers route apply
        through queryApply uniformly. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
