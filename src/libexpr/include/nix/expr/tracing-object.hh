#pragma once

#include "nix/expr/arg-scope.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/ref.hh"

#include <optional>
#include <string>

namespace nix {

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

    /* Argument-scope cell. Apply-result proxies (constructed by
       TracingEvaluator::apply) open a fresh cell rooted at the fn's
       cell; navigation children (maybeGetAttr / getListElem) inherit
       the parent's cell. Cell's own `parent` field carries the
       ancestor chain. */
    std::shared_ptr<const ArgScopeCell> argScope;

    TracingObject(ref<Object> inner, TracingWriter & writer, ValueHandle valueNum, std::optional<TriePosition> triePos);

public:
    static ref<TracingObject> create(
        ref<Object> inner,
        TracingWriter & writer,
        ValueHandle valueNum,
        std::optional<TriePosition> triePos = std::nullopt);

    /** Set the proxy's argScope. Returns *this for chaining. */
    TracingObject & withScope(std::shared_ptr<const ArgScopeCell> argScope_)
    {
        argScope = std::move(argScope_);
        return *this;
    }

    std::shared_ptr<const ArgScopeCell> getProxyArgScope() const override { return argScope; }

    /** Get the query hash string for trie identity, if available. */
    std::optional<std::string> getQueryHashStr() const
    {
        return triePos ? std::optional{triePos->queryHashStr} : std::nullopt;
    }

    std::optional<std::string> getCdiHex() const override { return getQueryHashStr(); }

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
    PosIdx getPos() override;
    std::optional<std::vector<std::string>> getAttrPath() override;
};

} // namespace nix
