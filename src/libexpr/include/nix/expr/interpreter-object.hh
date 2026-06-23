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
    PosIdx pos;

public:
    InterpreterObject(EvalState & state, RootValue value, PosIdx pos = noPos);

    std::shared_ptr<Object> maybeGetAttr(const std::string & name) override;

    std::vector<std::string> getAttrNames() override;

    std::string getStringIgnoreContext() override;

    std::pair<std::string, NixStringContext> getStringWithContext() override;

    std::string getStringWithoutContext() override;

    RootedPath getPath() override;

    bool getBool(std::string_view errorCtx) override;

    NixInt getInt(std::string_view errorCtx) override;

    NixFloat getFloat(std::string_view errorCtx) override;

    size_t getListSize() override;

    std::shared_ptr<Object> getListElem(size_t index) override;

    std::vector<std::string> getListOfStringsNoCtx() override;

    ObjectType getTypeLazy() override;

    ObjectType getType() override;

    RootValue defeatCache() override;

    std::optional<FunctionInfo> getFunctionInfo() override;

    PosIdx getPos() override;

    /** Value-level apply: this Value is a function (or thunk
        reducing to one); apply it to argObj's Value. Mirrors
        `Interpreter::apply` but as the Object-method entry point so
        callers can route apply through queryApply uniformly. */
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object> argObj) override;
};

} // namespace nix