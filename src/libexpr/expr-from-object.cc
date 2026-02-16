#include "nix/expr/expr-from-object.hh"
#include "nix/expr/eval.hh"

namespace nix {

void ExprFromObject::eval(EvalState & state, Env & env, Value & v)
{
    auto type = obj->getType();

    switch (type) {
    case nAttrs: {
        auto names = obj->getAttrNames();
        auto attrs = state.buildBindings(names.size());
        for (const auto & name : names) {
            auto childObj = obj->maybeGetAttr(name);
            if (childObj) {
                // Create a thunk for the child object
                auto childExpr = new ExprFromObject(std::move(childObj));
                attrs.insert(state.symbols.create(name), childExpr->maybeThunk(state, env));
            }
        }
        v.mkAttrs(attrs);
        break;
    }

    case nList: {
        // Lists need getListOfStringsNoCtx or similar - but Object interface
        // doesn't have a generic list accessor. For now, we only support
        // string lists via getListOfStringsNoCtx.
        // TODO: Add getListSize() and getListElem(i) to Object interface?
        auto strings = obj->getListOfStringsNoCtx();
        auto builder = state.buildList(strings.size());
        for (size_t i = 0; i < strings.size(); i++) {
            builder.elems[i] = state.allocValue();
            builder.elems[i]->mkString(strings[i]);
        }
        v.mkList(builder);
        break;
    }

    case nString: {
        auto [str, ctx] = obj->getStringWithContext();
        v.mkString(str, ctx);
        break;
    }

    case nPath: {
        auto path = obj->getPath();
        v.mkPath(path);
        break;
    }

    case nInt: {
        auto i = obj->getInt();
        v.mkInt(i);
        break;
    }

    case nFloat: {
        // Object interface doesn't have getFloat - would need to add it
        // For now, fall through to error
        state.error<TypeError>("ExprFromObject: float not supported").debugThrow();
        break;
    }

    case nBool: {
        auto b = obj->getBool();
        v.mkBool(b);
        break;
    }

    case nNull: {
        v.mkNull();
        break;
    }

    case nFunction:
    case nExternal:
    case nThunk:
        // These types cannot be represented through the Object interface
        state.error<TypeError>("ExprFromObject: cannot represent type %s", showType(type)).debugThrow();
        break;
    }
}

} // namespace nix
