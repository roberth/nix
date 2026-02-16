#pragma once
/**
 * @file
 * Object wrapper for Value.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/value.hh"

namespace nix {

/**
 * Object implementation that wraps a Value.
 */
class InterpreterObject : public Object
{
    EvalState & state;
    RootValue value;

public:
    InterpreterObject(EvalState & state, RootValue value);

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;

    std::vector<std::string> getAttrNames() override;

    std::string getStringIgnoreContext() override;

    std::pair<std::string, NixStringContext> getStringWithContext() override;

    std::string getStringWithoutContext() override;

    SourcePath getPath() override;

    bool getBool(std::string_view errorCtx) override;

    NixInt getInt(std::string_view errorCtx) override;

    NixFloat getFloat(std::string_view errorCtx) override;

    size_t getListSize() override;

    std::shared_ptr<Object> getListElem(size_t index) override;

    std::vector<std::string> getListOfStringsNoCtx() override;

    ObjectType getTypeLazy() override;

    ObjectType getType() override;

    RootValue defeatCache() override;
};

} // namespace nix