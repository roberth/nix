#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-types.hh"

#include <functional>
#include <vector>

namespace nix {

class Store;

/**
 * Object that returns cached results from a trace, with lazy fallback
 * to the inner evaluator when cache misses occur.
 */
class TracingReplayObject : public Object
{
    Store & store;
    const std::vector<trace::TraceEntry> & trace;
    const trace::QueryIndex & index;
    uint64_t valueNum;

    /**
     * Lazy fallback: produces the real Object from the inner evaluator.
     * Called on first cache miss.
     */
    std::function<ref<Object>()> getInner;
    mutable std::optional<ref<Object>> inner;

    ref<Object> ensureInner() const;

    /**
     * Look up a query result using the index and ResultOf type family.
     * Returns the result payload, or nullopt on miss (logged at debug level).
     */
    template<typename Q>
    std::optional<typename trace::ResultOf<Q>::Type> lookupResult(const Q & query) const;

public:
    TracingReplayObject(
        Store & store,
        const std::vector<trace::TraceEntry> & trace,
        const trace::QueryIndex & index,
        uint64_t valueNum,
        std::function<ref<Object>()> getInner);

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
