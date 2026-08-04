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

    /* Apply-result proxies (constructed by TracingEvaluator::apply)
       open a fresh cell rooted at the fn's cell; navigation children
       (maybeGetAttr / getListElem) inherit the parent's cell. */
    std::shared_ptr<ArgCell> argCell;

    /* For apply-result wrappers: the producer Selector that identifies
       this apply structurally (SelectorApplyStep{parent=fn}). Null on
       non-apply-result wrappers (= navigation children). */
    std::optional<ref<const trace::Selector>> producer;

    /* True on wrappers rooted at a cb-apply (OuterApply::run) and on
       navigation descendants of such wrappers. Gates whether children
       inherit `producer` for QCA emission. Non-cb apply results (e.g.
       inner's own function application in TracingEvaluator::apply)
       leave this false so their children stay order-independent. */
    bool cbApplyOrigin;

    /* Memoized WHNF payload. First call to any of getType / getInt /
       getString / etc. fires `whnf()`, which computes the type
       discriminator plus type-determined payload once. Subsequent
       calls decode the cached result. Pre-populated at construction
       when the caller already computed it (cell-migration Phase B). */
    std::optional<trace::ResultWHNF> cachedWHNF;
    trace::ResultWHNF & whnf();

    /* Evaluator + resolver context for queryApply's arg-wrapping.
       Same context the CLI's `EvalCommand::getEvalState` wires up
       when tracing-eval-cache is on. Propagated to child TObjects
       so navigation descendants can apply. */
    ref<Evaluator> innerEvaluator;
    ref<OuterResolver> outerResolver;

    TracingObject(
        ref<Object> inner,
        TracingWriter & writer,
        ValueHandle valueNum,
        std::optional<TriePosition> triePos,
        std::shared_ptr<ArgCell> argCell,
        std::optional<ref<const trace::Selector>> producer,
        std::optional<trace::ResultWHNF> cachedWHNF,
        bool cbApplyOrigin,
        ref<Evaluator> innerEvaluator,
        ref<OuterResolver> outerResolver);

public:
    static ref<TracingObject> create(
        ref<Object> inner,
        TracingWriter & writer,
        ValueHandle valueNum,
        std::optional<TriePosition> triePos,
        std::shared_ptr<ArgCell> argCell,
        ref<Evaluator> innerEvaluator,
        ref<OuterResolver> outerResolver,
        std::optional<ref<const trace::Selector>> producer = std::nullopt,
        std::optional<trace::ResultWHNF> cachedWHNF = std::nullopt,
        bool cbApplyOrigin = false);

    std::shared_ptr<ArgCell> getProxyArgCell() const override { return argCell; }

    std::optional<std::string> getSelectorHashHex() const override
    {
        return triePos ? std::optional{triePos->queryHashStr} : std::nullopt;
    }

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
    Value * materialiseAsFunctionValue(
        EvalState & state,
        std::shared_ptr<OuterResolver> resolver,
        std::shared_ptr<Evaluator> innerEvaluator) override;
    std::optional<FunctionInfo> getFunctionInfo() override;
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;

    /** Value-level apply: this Object is a fn (or thunk reducing
        to one); apply it to argObj's value, recording the
        SelectorApply Terminal keyed on the arg's cell factset.
        argObj is expected to already carry a cache-boundary
        identity — TracingEvaluator::apply's preamble wraps raw
        args in an OuterObject before dispatching here. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix
