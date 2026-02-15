#pragma once

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
 * Objects are only created for validated trie positions. Child lookups
 * perform incremental validation from the child back to the current position.
 */
class TracingReplayObject : public Object
{
    TracingReplayEvaluator & evaluator;
    TriePosition triePos;

    /**
     * Lazy fallback: produces the real Object from the inner evaluator.
     * Called on first cache miss.
     */
    std::function<ref<Object>()> getInner;
    mutable std::optional<ref<Object>> inner;

    ref<Object> ensureInner() const;

    /**
     * Look up a query result using the trie index.
     * Uses temporal child queries (for getAttrNames, getString, etc.).
     * Validates dependencies before returning cached results.
     * Returns the result payload, or nullopt on miss.
     */
    template<typename Q, typename R>
    std::optional<R> lookupResult(const Q & query) const;

    /**
     * Look up a structural child query (for getAttr).
     * Uses structural parent relationship to find attributes.
     * Validates dependencies before returning cached results.
     * Returns the result payload and child position, or nullopt on miss.
     */
    template<typename Q, typename R>
    std::optional<std::pair<R, TriePosition>> lookupStructuralChild(const Q & query) const;

public:
    /**
     * Create a replay object for a validated trie position.
     */
    TracingReplayObject(
        TracingReplayEvaluator & evaluator, TriePosition triePos, std::function<ref<Object>()> getInner);

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;
    std::vector<std::string> getAttrNames() override;
    std::string getStringIgnoreContext() override;
    std::pair<std::string, NixStringContext> getStringWithContext() override;
    SourcePath getPath() override;
    bool getBool(std::string_view errorCtx = "") override;
    NixInt getInt(std::string_view errorCtx = "") override;
    std::vector<std::string> getListOfStringsNoCtx() override;
    ObjectType getTypeLazy() override;
    ObjectType getType() override;
    RootValue defeatCache() override;
};

} // namespace nix
