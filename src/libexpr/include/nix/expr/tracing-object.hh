#pragma once

#include "nix/expr/arg-cell.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

/** Compute a value's WHNF in one pass by calling the Object's
    per-type getters. Used by TracingObject::whnf to record a single
    SelectorGetWHNF observation, and by the walker's dispatch to compute
    the live response for a recorded SelectorGetWHNF. */
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
       inherit `applyResultSubject` for QCA emission. See
       `withCbApplyOrigin` for rationale. */
    bool cbApplyOrigin = false;

    /* Per-invocation observation context shared with the cb-arg
       OuterObject's queryFn and propagated to derived children
       via shared_ptr. */
    std::shared_ptr<ApplyContext> applyContext;

    /* Compute the wrapper's evolved state hash live from
       applyContext->observations. */
    std::string evolvedQueryFrom() const;

    void pushObservation(const std::string & fromHex, const Hash & selectorHash, const Hash & responseHash);

    /* Memoized WHNF observation. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which records ONE SelectorGetWHNF
       observation against this Object's identity carrying the type
       discriminator plus the type-determined payload. Subsequent calls
       on this Object decode the cached result without re-recording. */
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
        circuits without invoking `SelectorGetWHNF`. */
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
        leave this false so their children stay order-independent —
        the propagation of `producer` to children would otherwise
        route their evolvedQueryFrom through applyContext and break
        tests like cb-deep-indep-orders. */
    TracingObject & withCbApplyOrigin()
    {
        cbApplyOrigin = true;
        return *this;
    }

    bool isCbApplyOrigin() const { return cbApplyOrigin; }

    TracingObject & withApplyContext(std::shared_ptr<ApplyContext> ctx)
    {
        applyContext = std::move(ctx);
        return *this;
    }

    std::shared_ptr<ApplyContext> getApplyContext() const { return applyContext; }

    std::shared_ptr<const ArgCell> getProxyArgCell() const override { return argCell; }

    /** Get the query hash string for trie identity, if available. */
    std::optional<std::string> getQueryHashStr() const
    {
        return triePos ? std::optional{triePos->queryHashStr} : std::nullopt;
    }

    std::optional<std::string> getSelectorHashHex() const override { return getQueryHashStr(); }

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
