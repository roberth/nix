#include "nix/expr/interpreter-object.hh"
#include "nix/expr/nixexpr.hh"

namespace nix {

InterpreterObject::InterpreterObject(EvalState & state, RootValue value, PosIdx pos)
    : state(state)
    , value(value)
    , pos(pos)
{
}

std::shared_ptr<Object> InterpreterObject::maybeGetAttr(const std::string & name)
{
    state.forceValue(**value, pos);
    if ((*value)->type() != nAttrs)
        return nullptr;
    auto attr = (*value)->attrs()->get(state.symbols.create(name));
    if (!attr)
        return nullptr;
    return std::make_shared<InterpreterObject>(state, allocRootValue(attr->value), attr->pos);
}

std::vector<std::string> InterpreterObject::getAttrNames()
{
    state.forceValue(**value, pos);
    if ((*value)->type() != nAttrs)
        state.error<TypeError>("expected an attribute set but found %s", showType(**value)).debugThrow();

    std::vector<std::string> result;
    for (auto & attr : *(*value)->attrs()) {
        result.push_back(std::string(state.symbols[attr.name]));
    }
    return result;
}

std::string InterpreterObject::getStringIgnoreContext()
{
    state.forceValue(**value, pos);
    if ((*value)->type() != nString)
        state.error<TypeError>("value is %1% while a string was expected", showType(**value)).debugThrow();
    return (*value)->c_str();
}

std::pair<std::string, NixStringContext> InterpreterObject::getStringWithContext()
{
    state.forceValue(**value, pos);
    if ((*value)->type() != nString)
        state.error<TypeError>("value is %1% while a string was expected", showType(**value)).debugThrow();

    // Get the string value
    std::string str = (*value)->c_str();

    // Get the context using the existing copyContext function
    NixStringContext context;
    copyContext(**value, context);

    return std::make_pair(str, context);
}

std::string InterpreterObject::getStringWithoutContext()
{
    return std::string(state.forceStringNoCtx(**value, noPos, ""));
}

SourcePath InterpreterObject::getPath()
{
    state.forceValue(**value, pos);
    if ((*value)->type() != nPath)
        state.error<TypeError>("expected a path but found %1%", showType(**value)).debugThrow();
    return (*value)->path();
}

bool InterpreterObject::getBool(std::string_view errorCtx)
{
    // Avoid adding empty trace when errorCtx is not provided
    if (errorCtx.empty()) {
        state.forceValue(**value, pos);
        if ((*value)->type() != nBool)
            state.error<TypeError>("expected a Boolean but found %1%", showType(**value)).debugThrow();
        return (*value)->boolean();
    }
    return state.forceBool(**value, pos, errorCtx);
}

NixInt InterpreterObject::getInt(std::string_view errorCtx)
{
    // Avoid adding empty trace when errorCtx is not provided
    if (errorCtx.empty()) {
        state.forceValue(**value, pos);
        if ((*value)->type() != nInt)
            state.error<TypeError>("expected an integer but found %1%", showType(**value)).debugThrow();
        return (*value)->integer();
    }
    return state.forceInt(**value, pos, errorCtx);
}

NixFloat InterpreterObject::getFloat(std::string_view errorCtx)
{
    // Avoid adding empty trace when errorCtx is not provided
    if (errorCtx.empty()) {
        state.forceValue(**value, pos);
        if ((*value)->type() != nFloat)
            state.error<TypeError>("expected a float but found %1%", showType(**value)).debugThrow();
        return (*value)->fpoint();
    }
    return state.forceFloat(**value, pos, errorCtx);
}

size_t InterpreterObject::getListSize()
{
    state.forceValue(**value, pos);
    if (!(*value)->isList())
        state.error<TypeError>("expected a list but found %1%", showType(**value)).debugThrow();
    return (*value)->listSize();
}

std::shared_ptr<Object> InterpreterObject::getListElem(size_t index)
{
    state.forceValue(**value, pos);
    if (!(*value)->isList())
        state.error<TypeError>("expected a list but found %1%", showType(**value)).debugThrow();
    if (index >= (*value)->listSize())
        state.error<EvalError>("list index %1% is out of bounds", index).debugThrow();
    return std::make_shared<InterpreterObject>(state, allocRootValue((*value)->listView()[index]));
}

// Override default for efficiency: avoids wrapper objects and provides better error messages
std::vector<std::string> InterpreterObject::getListOfStringsNoCtx()
{
    state.forceValue(**value, pos);
    if (!(*value)->isList())
        state.error<TypeError>("expected a list but found %s", showType(**value)).debugThrow();

    std::vector<std::string> result;
    size_t index = 0;
    for (auto elem : (*value)->listView()) {
        result.push_back(
            std::string(
                state.forceStringNoCtx(*elem, noPos, fmt("while evaluating a list element at index %d", index))));
        index++;
    }
    return result;
}

ObjectType InterpreterObject::getTypeLazy()
{
    return (*value)->type();
}

ObjectType InterpreterObject::getType()
{
    state.forceValue(**value, pos);
    return (*value)->type();
}

RootValue InterpreterObject::defeatCache()
{
    // For InterpreterObject, we already have the Value, just return it
    return value;
}

std::optional<FunctionInfo> InterpreterObject::getFunctionInfo()
{
    state.forceValue(**value, pos);
    if ((*value)->isLambda()) {
        auto formals = (*value)->lambda().fun->getFormals();
        if (!formals)
            return std::nullopt;

        FunctionInfo info;
        info.ellipsis = formals->ellipsis;
        for (const auto & formal : formals->formals) {
            info.formals.emplace(std::string(state.symbols[formal.name]), formal.def != nullptr);
        }
        return info;
    }
    if ((*value)->isPrimOp()) {
        if (auto & gfi = (*value)->primOp()->getFunctionInfo)
            return gfi();
    }
    return std::nullopt;
}

PosIdx InterpreterObject::getPos()
{
    return pos;
}

} // namespace nix