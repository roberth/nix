#include "nix/expr/evaluator.hh"

namespace nix {

Value * Object::materialiseAsFunctionValue(
    EvalState & /* state */,
    std::shared_ptr<OuterResolver> /* resolver */,
    std::shared_ptr<Evaluator> /* innerEvaluator */)
{
    /* Default: hand back the raw underlying Value. `mkApp` applies
       it normally; `builtins.functionArgs` inspects it via its
       isLambda branch. Subclasses whose apply must route through
       a boundary primop (OuterObject, TracingObject, cb recorders)
       override. */
    return *defeatCache();
}

std::vector<std::string> Object::getListOfStringsNoCtx()
{
    auto size = getListSize();
    std::vector<std::string> result;
    result.reserve(size);
    for (size_t i = 0; i < size; i++) {
        auto elem = getListElem(i);
        auto [str, ctx] = elem->getStringWithContext();
        if (!ctx.empty())
            throw Error(
                "the string '%1%' is not allowed to refer to a store path (such as '%2%'), while evaluating a list element at index %3%",
                str,
                ctx.begin()->to_string(),
                i);
        result.push_back(std::move(str));
    }
    return result;
}

} // namespace nix
