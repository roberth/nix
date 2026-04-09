#include "nix/expr/expr-from-object.hh"
#include "nix/expr/contra-object.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/interpreter-object.hh"

namespace nix {

// TODO: share with tracing-object.cc, tracing-replay-object.cc, contra-object.cc
static std::string objectTypeToString(ObjectType type)
{
    switch (type) {
    case nAttrs: return "set";
    case nList: return "list";
    case nString: return "string";
    case nPath: return "path";
    case nInt: return "int";
    case nFloat: return "float";
    case nBool: return "bool";
    case nNull: return "null";
    case nFunction: return "lambda";
    case nExternal: return "external";
    case nThunk: return "thunk";
    case nFailed: return "failed";
    }
    return "unknown";
}

/**
 * Stateful resolver that maps virtual value ids to outer Objects.
 * Resolves contra-queries by dispatching to the appropriate Object method,
 * registering child Objects under structurally derived ids.
 */
struct ContraResolver
{
    std::map<std::string, std::shared_ptr<Object>> objects;

    explicit ContraResolver(std::string rootId, std::shared_ptr<Object> rootObj)
    {
        objects[rootId] = std::move(rootObj);
    }

    std::shared_ptr<Object> resolve(const std::string & id)
    {
        auto it = objects.find(id);
        if (it == objects.end())
            throw Error("contra-query: unknown virtual value id '%s'", id);
        return it->second;
    }

    trace::ResultVariant query(const trace::QueryVariant & q)
    {
        return std::visit(
            [&](const auto & query) -> trace::ResultVariant {
                using Q = std::decay_t<decltype(query)>;
                if constexpr (!requires { query.from; }) {
                    throw Error("contra-query: query type has no 'from' field");
                } else {
                    auto obj = resolve(query.from);

                    if constexpr (std::is_same_v<Q, trace::QueryGetType>) {
                        return trace::ResultType{objectTypeToString(obj->getType())};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttr>) {
                        auto child = obj->maybeGetAttr(query.name);
                        if (!child)
                            return trace::ResultMaybeType{std::nullopt};
                        auto childId = query.from + "." + query.name;
                        objects[childId] = child;
                        return trace::ResultMaybeType{
                            std::optional<std::string>{objectTypeToString(child->getType())}};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetString>) {
                        return trace::ResultString{obj->getStringIgnoreContext()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetStringWithContext>) {
                        auto [str, ctx] = obj->getStringWithContext();
                        std::vector<std::string> ctxStrings;
                        for (auto & c : ctx)
                            ctxStrings.push_back(c.to_string());
                        return trace::ResultStringWithContext{str, std::move(ctxStrings)};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttrNames>) {
                        return trace::ResultListOfStrings{obj->getAttrNames()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetBool>) {
                        return trace::ResultBool{obj->getBool()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetInt>) {
                        return trace::ResultInt{obj->getInt().value};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetFloat>) {
                        return trace::ResultFloat{obj->getFloat()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetListSize>) {
                        return trace::ResultListSize{obj->getListSize()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetListElem>) {
                        auto child = obj->getListElem(query.index);
                        auto childId = query.from + "[" + std::to_string(query.index) + "]";
                        objects[childId] = child;
                        return trace::ResultType{objectTypeToString(child->getType())};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetPath>) {
                        return trace::ResultPath{obj->getPath().path.abs()};
                    } else {
                        throw Error("unsupported contra-query type");
                    }
                }
            },
            q);
    }
};

void ExprFromObject::eval(EvalState & state, Env & env, Value & v)
{
    auto type = obj->getType();

    switch (type) {
    case nAttrs: {
        auto names = obj->getAttrNames();
        auto attrs = state.buildBindings(names.size());
        for (const auto & name : names) {
            auto * thunk = state.allocValue();
            auto * expr = new ExprFromObjectAttr(obj, name, innerEvaluator);
            state.mkThunk_(*thunk, expr);
            attrs.insert(state.symbols.create(name), thunk);
        }
        v.mkAttrs(attrs);
        break;
    }

    case nList: {
        auto size = obj->getListSize();
        auto builder = state.buildList(size);
        for (size_t i = 0; i < size; i++) {
            auto childObj = obj->getListElem(i);
            auto childExpr = new ExprFromObject(std::move(childObj), innerEvaluator);
            builder.elems[i] = childExpr->maybeThunk(state, env);
        }
        v.mkList(builder);
        break;
    }

    case nString: {
        auto [str, ctx] = obj->getStringWithContext();
        v.mkString(str, ctx, state.mem);
        break;
    }

    case nPath: {
        auto path = obj->getPath();
        v.mkPath(path, state.mem);
        break;
    }

    case nInt: {
        auto i = obj->getInt();
        v.mkInt(i);
        break;
    }

    case nFloat: {
        auto f = obj->getFloat();
        v.mkFloat(f);
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

    case nFunction: {
        auto objPtr = obj;
        auto innerEval = innerEvaluator;
        auto * primOp = new
#if NIX_USE_BOEHMGC
            (GC)
#endif
                PrimOp{
                    .name = "<cached-fn>",
                    .args = {"args"},
                    .arity = 1,
                    .impl =
                        [objPtr, innerEval](EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                            if (!innerEval) {
                                state
                                    .error<TypeError>(
                                        "cached function calls require an inner evaluator (builtins.cache)")
                                    .atPos(pos)
                                    .debugThrow();
                            }

                            // Wrap the outer argument as a ContraObject
                            state.forceValue(*args[0], pos);
                            auto outerArgObj = state.toObjectCompat(*args[0]);
                            auto resolver = std::make_shared<ContraResolver>("0", outerArgObj.get_ptr());
                            auto contraArg = make_ref<ContraObject>(
                                "0", [resolver](const trace::QueryVariant & q) { return resolver->query(q); });

                            // Apply the cached function to the contra argument
                            auto result = innerEval->apply(ref<Object>(objPtr), contraArg);

                            // Bridge result back to outer evaluator
                            ExprFromObject(result.get_ptr(), innerEval).eval(state, state.baseEnv, v);
                        },
                    .getFunctionInfo = [objPtr]() -> std::optional<FunctionInfo> { return objPtr->getFunctionInfo(); },
                };
        v.mkPrimOp(primOp);
        break;
    }

    case nExternal:
    case nThunk:
    case nFailed:
        state.error<TypeError>("ExprFromObject: cannot represent type %s", showType(type)).debugThrow();
        break;
    }
}

void ExprFromObjectAttr::eval(EvalState & state, Env & env, Value & v)
{
    auto childObj = parentObj->maybeGetAttr(name);
    if (!childObj)
        state.error<TypeError>("ExprFromObjectAttr: attribute '%s' missing", name).debugThrow();
    ExprFromObject(std::move(childObj), innerEvaluator).eval(state, env, v);
}

} // namespace nix
