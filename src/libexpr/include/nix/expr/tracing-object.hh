#pragma once

#include "nix/expr/arg-cell.hh"
#include "nix/expr/observation-set.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

/** Compute a value's WHNF in one pass by calling the Object's
    per-type getters. Used by wrappers' whnf() to materialise a
    ResultWHNF payload, and by the walker's live dispatch to compare
    against recorded responses. */
trace::ResultWHNF computeWHNFFromObject(Object & obj);

/**
 * Object wrapper that logs all operations to a trace file and optionally
 * to a trie index via TracingWriter.
 */
class TracingObject : public Object
{
    ref<Object> inner;
    TracingWriter & writer;
    ValueHandle valueNum;
    std::optional<TriePosition> triePos;

    /* Argument-argAncestry cell. Apply-result proxies (constructed by
       TracingEvaluator::apply) open a fresh cell rooted at the fn's
       cell; navigation children (maybeGetAttr / getListElem) inherit
       the parent's cell. Cell's own `parent` field carries the
       ancestor chain. */
    std::shared_ptr<const ArgCell> argCell;

    /* For apply-result wrappers: the producer Selector that identifies
       this apply structurally (SelectorApply{fn=...}). Null on
       non-apply-result wrappers (= navigation children). */
    std::optional<trace::SelectorVariant> producer;

    /* True on wrappers rooted at a cb-apply (OuterApply::run) and on
       navigation descendants of such wrappers. Gates whether children
       inherit `producer` for QCA emission. See `withCbApplyOrigin`
       for rationale. */
    bool cbApplyOrigin = false;

    /* Memoized WHNF payload. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which computes the type
       discriminator plus type-determined payload once. Subsequent
       calls decode the cached result. */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    TracingObject(ref<Object> inner, TracingWriter & writer, ValueHandle valueNum, std::optional<TriePosition> triePos);

public:
    static ref<TracingObject> create(
        ref<Object> inner,
        TracingWriter & writer,
        ValueHandle valueNum,
        std::optional<TriePosition> triePos = std::nullopt);

    /** Set the proxy's argCell. Returns *this for chaining. */
    TracingObject & withArgCell(std::shared_ptr<const ArgCell> argScope_)
    {
        argCell = std::move(argScope_);
        return *this;
    }

    /** Attach the apply-result producer Selector — for apply-result
        wrappers, so subsequent child queries hang off this producer.
        Mirrors TracingReplayObject's machinery. */
    TracingObject & withProducer(trace::SelectorVariant p)
    {
        producer = std::move(p);
        return *this;
    }

    /** Pre-populate `cachedWHNF` at wrapper construction. Used by
        cell-migration Phase B: `TracingEvaluator::apply` computes the
        applyResult's WHNF as part of the atomic apply operation and
        pre-populates the wrapper, so subsequent `.whnf()` short-
        circuits without recomputing. */
    TracingObject & withCachedWHNF(trace::ResultWHNF whnf_)
    {
        cachedWHNF = std::move(whnf_);
        return *this;
    }

    /** Mark this wrapper as originating from a callback-application
        boundary (OuterApply::run). Descendants of a cb-apply-marked
        wrapper inherit the mark and inherit `producer` through
        navigation, so their own whnf fires
        emitCallbackApplyForApplyResult against the enclosing
        CallbackCell (callback-model §7). Non-cb apply results (e.g.
        inner's own function application in TracingEvaluator::apply)
        leave this false so their children stay order-independent. */
    TracingObject & withCbApplyOrigin()
    {
        cbApplyOrigin = true;
        return *this;
    }

    bool isCbApplyOrigin() const { return cbApplyOrigin; }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    /** Get the query hash string for trie identity, if available. */
    std::optional<std::string> getQueryHashStr() const
    {
        return triePos ? std::optional{triePos->queryHashStr} : std::nullopt;
    }

    std::optional<std::string> getSelectorHashHex() const override { return getQueryHashStr(); }

    std::optional<ref<const trace::Selector>> getSelector() const override;

    std::optional<std::string> getProducerSelectorHex(TracingWriter & writer) override;

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
    /** Object-method apply entry. Delegates to inner->queryApply and
        wraps the result as another TracingObject so subsequent
        accesses on the apply result are recorded as queries against
        the apply-result trie position. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
