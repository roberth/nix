#include "nix/expr/attr-path.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/evaluation-helpers.hh"
#include "nix/util/util.hh"
#include "nix/util/strings-inline.hh"

namespace nix {

static Strings parseAttrPath(std::string_view s)
{
    Strings res;
    std::string cur;
    auto i = s.begin();
    while (i != s.end()) {
        if (*i == '.') {
            res.push_back(cur);
            cur.clear();
        } else if (*i == '"') {
            ++i;
            while (1) {
                if (i == s.end())
                    throw ParseError("missing closing quote in selection path '%1%'", s);
                if (*i == '"')
                    break;
                cur.push_back(*i++);
            }
        } else
            cur.push_back(*i);
        ++i;
    }
    if (!cur.empty())
        res.push_back(cur);
    return res;
}

AttrPath AttrPath::parse(EvalState & state, std::string_view s)
{
    AttrPath res;
    for (auto & a : parseAttrPath(s))
        res.push_back(state.symbols.create(a));
    return res;
}

std::string AttrPath::to_string(EvalState & state) const
{
    return dropEmptyInitThenConcatStringsSep(".", state.symbols.resolve({*this}));
}

std::vector<SymbolStr> AttrPath::resolve(EvalState & state) const
{
    return state.symbols.resolve({*this});
}

std::pair<Value *, PosIdx>
findAlongAttrPath(EvalState & state, const std::string & attrPath, Bindings & autoArgs, Value & vIn)
{
    auto evaluator = state.toEvaluatorCompat();

    // Convert autoArgs: Bindings → map<string, ref<Object>>
    std::map<std::string, ref<Object>> autoArgsObj;
    for (auto & arg : autoArgs)
        autoArgsObj.emplace(std::string(state.symbols[arg.name]), state.toObjectCompat(*arg.value));

    // Convert root value to Object
    auto rootObj = state.toObjectCompat(vIn);

    // Parse attr path
    Strings tokens = parseAttrPath(attrPath);
    std::vector<std::string> attrPathVec(tokens.begin(), tokens.end());

    // findAlongAttrPathWithAutoCall throws AttrPathNotFound on missing attributes
    auto obj = *expr::helpers::findAlongAttrPathWithAutoCall(*evaluator, rootObj, attrPath, attrPathVec, autoArgsObj);
    return {*obj->defeatCache(), obj->getPos()};
}

std::pair<SourcePath, uint32_t> findPackageFilename(EvalState & state, Value & v, std::string what)
{
    Value * v2;
    try {
        auto & dummyArgs = Bindings::emptyBindings;
        v2 = findAlongAttrPath(state, "meta.position", dummyArgs, v).first;
    } catch (Error &) {
        throw NoPositionInfo("package '%s' has no source location information", what);
    }

    // FIXME: is it possible to extract the Pos object instead of doing this
    //        toString + parsing?
    NixStringContext context;
    auto path =
        state.coerceToPath(noPos, *v2, context, "while evaluating the 'meta.position' attribute of a derivation");

    auto fn = path.path.abs();

    auto fail = [fn]() { throw ParseError("cannot parse 'meta.position' attribute '%s'", fn); };

    auto colon = fn.rfind(':');
    if (colon == std::string::npos)
        fail();

    auto lineno = string2Int<uint32_t>(std::string_view(fn).substr(colon + 1));
    if (!lineno)
        fail();

    return {SourcePath{path.accessor, CanonPath(fn.substr(0, colon))}, *lineno};
}

} // namespace nix
