#pragma once

#include "nix/expr/arg-scope.hh"
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

    /* Argument-scope wiring (Phase 2 of the proxy-graph rollout).
       `parent` points to the proxy that produced this one;
       `argScope` is set on apply-result proxies (the result of a
       cache-boundary apply) and on the cached-value root, null on
       navigation children. Nothing reads these yet. */
    std::shared_ptr<Object> parent;
    std::optional<ArgScopeCell> argScope;

    ref<Object> ensureInner() const;

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

    /** Phase 2: set the proxy-graph back-pointers. Call right after
        construction at boundary sites. Returns *this for chaining. */
    TracingReplayObject & withScope(std::shared_ptr<Object> parent_, std::optional<ArgScopeCell> argScope_)
    {
        parent = std::move(parent_);
        argScope = std::move(argScope_);
        return *this;
    }

    const TriePosition & getTriePos() const
    {
        return triePos;
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
    std::vector<std::string> getListOfStringsNoCtx() override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
    std::optional<FunctionInfo> getFunctionInfo() override;
};

} // namespace nix
