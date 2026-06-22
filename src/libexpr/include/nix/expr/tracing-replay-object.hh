#pragma once

#include "nix/expr/arg-scope.hh"
#include "nix/expr/content-identity-via-asks.hh"
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

    /* Argument-scope cell. Apply-result proxies open a fresh cell
       rooted at the fn's cell; navigation children carry the same
       cell as their parent. Cell's own `parent` field gives the
       ancestor chain. */
    std::shared_ptr<const ArgScopeCell> argScope;

    /* Per-cb-apply observation context for the apply that produced
       this object. Set on apply-result wrappers by
       TracingReplayEvaluator::apply when the arg was a cb-arg
       AmbientObject carrying one. Used to compute the apply-result's
       evolved Content Id via cidasks (the ApplyResultSubject's
       recursive arg cdi reflects the observations). Null on
       non-apply-result wrappers (= navigation children). */
    std::shared_ptr<cidasks::ApplyContext> applyContext;
    /* When apply-result, the ApplyResultSubject identifying it
       structurally. Used together with applyContext to compute the
       evolved cdi at lookup time. */
    std::optional<cidasks::Subject> applyResultSubject;

    ref<Object> ensureInner() const;

    /* Compute the apply-result's evolved query-hash prefix (= the
       hex hash to use as `from` for child queries) by running
       cidasks::contentIdAfter on applyResultSubject with the
       accumulated observations in applyContext. Falls back to the
       static triePos.queryHashStr when this is not an apply-result
       wrapper or the context has no observations. */
    std::string evolvedQueryFrom() const;

    /**
     * Cascading lookup for leaf results (getString, getBool, etc.).
     * Tries temporal → structural → shortcut strategies.
     */
    template<typename Q, typename R>
    std::optional<R> lookupResult(const Q & query) const;

    /**
     * Cascading lookup for structural children (getAttr, getListElem).
     * Returns the result payload and child TriePosition for further traversal.
     */
    template<typename Q, typename R>
    std::optional<std::pair<R, TriePosition>> lookupStructuralChild(const Q & query) const;

public:
    TracingReplayObject(
        TracingReplayEvaluator & evaluator, TriePosition triePos, std::function<ref<Object>()> getInner);

    /** Set the proxy's argScope. Returns *this for chaining. */
    TracingReplayObject & withScope(std::shared_ptr<const ArgScopeCell> argScope_)
    {
        argScope = std::move(argScope_);
        return *this;
    }

    /** Attach the per-apply observation context — for apply-result
        wrappers, so subsequent queries can compute the evolved
        Content Id via cidasks. */
    TracingReplayObject & withApplyContext(
        std::shared_ptr<cidasks::ApplyContext> ctx, cidasks::Subject resultSubject)
    {
        applyContext = std::move(ctx);
        applyResultSubject = std::move(resultSubject);
        return *this;
    }

    std::shared_ptr<const ArgScopeCell> getProxyArgScope() const override { return argScope; }

    const TriePosition & getTriePos() const
    {
        return triePos;
    }

    std::optional<std::string> getCdiHex() const override { return triePos.queryHashStr; }

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
    std::vector<std::string> getListOfStringsNoCtx() override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
};

} // namespace nix
