#include "nix/expr/evaluation-helpers.hh"
#include "nix/util/error.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/eval.hh"
#include "nix/util/util.hh"

namespace nix::expr::helpers {

// Intentional duplication: see EvalState::isDerivation(Value &) for Value version.
// That version stays for hot-path callers; this version is for Object interface users.
bool isDerivation(Object & obj)
{
    auto typeAttr = obj.maybeGetAttr("type");
    if (!typeAttr) {
        return false;
    }

    if (typeAttr->getType() != nString) {
        return false;
    }

    auto typeStr = typeAttr->getStringIgnoreContext();
    return typeStr == "derivation";
}

// Intentional duplication: see EvalState::coerceToSingleDerivedPath(PosIdx, Value &, ...) for Value version.
// That version stays for callers that work with raw Values; this version is for Object interface users.
SingleDerivedPath coerceToSingleDerivedPath(Object & obj, Evaluator & evaluator, std::string_view errorCtx)
{
    auto & state = evaluator.getEvalState();
    auto [s, context] = obj.getStringWithContext();

    if (context.size() != 1)
        state
            .error<EvalError>(
                "string '%s' has %d entries in its context. It should only have exactly one entry", s, context.size())
            .withTrace(obj.getPos(), errorCtx)
            .debugThrow();

    auto derivedPath = std::visit(
        overloaded{
            [&](NixStringContextElem::Opaque && o) -> SingleDerivedPath { return std::move(o); },
            [&](NixStringContextElem::DrvDeep &&) -> SingleDerivedPath {
                state
                    .error<EvalError>(
                        "string '%s' has a context which refers to a complete source and binary closure. This is not supported at this time",
                        s)
                    .withTrace(obj.getPos(), errorCtx)
                    .debugThrow();
            },
            [&](NixStringContextElem::Built && b) -> SingleDerivedPath { return std::move(b); },
        },
        ((NixStringContextElem &&) *context.begin()).raw);

    auto sExpected = state.mkSingleDerivedPathStringRaw(derivedPath);
    if (s != sExpected) {
        std::visit(
            overloaded{
                [&](const SingleDerivedPath::Opaque & o) {
                    state.error<EvalError>("path string '%s' has context with the different path '%s'", s, sExpected)
                        .withTrace(obj.getPos(), errorCtx)
                        .debugThrow();
                },
                [&](const SingleDerivedPath::Built & b) {
                    state
                        .error<EvalError>(
                            "string '%s' has context with the output '%s' from derivation '%s', but the string is not the right placeholder for this derivation output. It should be '%s'",
                            s,
                            b.output,
                            b.drvPath->to_string(evaluator.getStore()),
                            sExpected)
                        .withTrace(obj.getPos(), errorCtx)
                        .debugThrow();
                }},
            derivedPath.raw());
    }

    return derivedPath;
}

// AttrCursor::forceDerivation() delegates here via toObjectCompat().
// PackageInfo::requireDrvPath() stays separate: different interface (memoization, optional return).
StorePath forceDerivation(Evaluator & evaluator, Object & obj, Store & store)
{
    auto drvPathAttr = obj.maybeGetAttr("drvPath");
    if (!drvPathAttr) {
        throw Error("derivation does not contain a 'drvPath' attribute");
    }

    // getStringWithContext() handles regeneration of GC'd derivations:
    // it checks if cached paths are valid, and if not, calls forceValue() to re-evaluate.
    // See also: ForceDerivationRegenTest in force-derivation-regen.cc
    auto [drvPathStr, context] = drvPathAttr->getStringWithContext();

    StorePath drvPath = store.parseStorePath(drvPathStr);

    try {
        drvPath.requireDerivation();
    } catch (Error & e) {
        e.addTrace({}, "while evaluating the 'drvPath' attribute of a derivation");
        throw;
    }

    return drvPath;
}

StringSet getDerivationOutputs(Object & obj)
{
    StringSet outputsToInstall;

    if (auto aOutputSpecified = obj.maybeGetAttr("outputSpecified")) {
        if (aOutputSpecified->getBool("while checking outputSpecified")) {
            if (auto aOutputName = obj.maybeGetAttr("outputName")) {
                if (aOutputName->getType() == nString) {
                    outputsToInstall = {aOutputName->getStringIgnoreContext()};
                }
            }
        }
    } else if (auto aMeta = obj.maybeGetAttr("meta")) {
        if (auto aOutputsToInstall = aMeta->maybeGetAttr("outputsToInstall")) {
            for (auto & s : aOutputsToInstall->getListOfStringsNoCtx())
                outputsToInstall.insert(s);
        }
    }

    if (outputsToInstall.empty())
        outputsToInstall.insert("out");

    return outputsToInstall;
}

OrSuggestions<std::shared_ptr<Object>> findAlongAttrPath(Object & obj, const std::vector<std::string> & attrPath)
{
    std::shared_ptr<Object> current = obj.shared_from_this();

    for (const auto & attrName : attrPath) {
        auto next = current->maybeGetAttr(attrName);
        if (!next) {
            auto attrNames = current->getAttrNames();
            StringSet strAttrNames;
            for (auto & name : attrNames)
                strAttrNames.insert(name);

            return OrSuggestions<std::shared_ptr<Object>>::failed(Suggestions::bestMatches(strAttrNames, attrName));
        }
        current = next;
    }

    return current;
}

OrSuggestions<std::pair<std::shared_ptr<Object>, std::string>>
tryAttrPaths(Object & obj, const std::vector<std::string> & attrPaths, EvalState & state)
{
    Suggestions suggestions;

    for (auto & attrPath : attrPaths) {
        auto attrPathSymbols = AttrPath::parse(state, attrPath);
        std::vector<std::string> attrPathStrings;
        for (const auto & sym : attrPathSymbols) {
            attrPathStrings.push_back(std::string(state.symbols[sym]));
        }

        auto objResult = findAlongAttrPath(obj, attrPathStrings);
        if (objResult) {
            return std::make_pair(*objResult, attrPath);
        } else {
            suggestions += objResult.getSuggestions();
        }
    }

    return OrSuggestions<std::pair<std::shared_ptr<Object>, std::string>>::failed(suggestions);
}

ref<Object> autoApply(Evaluator & evaluator, ref<Object> obj, const std::map<std::string, ref<Object>> & args)
{
    auto type = obj->getType();

    // Handle __functor attrsets
    if (type == nAttrs) {
        auto functor = obj->maybeGetAttr("__functor");
        if (functor) {
            auto result = evaluator.apply(ref<Object>(functor), obj);
            return autoApply(evaluator, result, args);
        }
    }

    // Not a function - return as-is
    if (type != nFunction)
        return obj;

    // Function without formals - return as-is
    auto funcInfo = obj->getFunctionInfo();
    if (!funcInfo)
        return obj;

    std::map<std::string, ref<Object>> callArgs;

    if (funcInfo->ellipsis) {
        // With ellipsis: pass all provided args
        callArgs = args;
    } else {
        // Without ellipsis: only pass args matching formal names
        for (const auto & [formalName, hasDefault] : funcInfo->formals) {
            auto it = args.find(formalName);
            if (it != args.end()) {
                callArgs.insert(*it);
            } else if (!hasDefault) {
                // Get full error info via defeatCache
                auto value = obj->defeatCache();
                auto & state = evaluator.getEvalState();
                auto lambda = (*value)->lambda();
                auto formals = lambda.fun->getFormals();
                // Find the formal's position
                PosIdx formalPos = noPos;
                if (formals) {
                    for (auto & f : formals->formals) {
                        if (state.symbols[f.name] == formalName) {
                            formalPos = f.pos;
                            break;
                        }
                    }
                }
                state
                    .error<MissingArgumentError>(
                        R"(cannot evaluate a function that has an argument without a value ('%1%')

Nix attempted to evaluate a function as a top level expression; in
this case it must have its arguments supplied either by default
values, or passed explicitly with '--arg' or '--argstr'. See
https://nix.dev/manual/nix/stable/language/syntax.html#functions.)",
                        formalName)
                    .atPos(formalPos)
                    .withFrame(*lambda.env, *lambda.fun)
                    .debugThrow();
            }
        }
    }

    auto argObj = evaluator.mkAttrs(callArgs);
    return evaluator.apply(obj, argObj);
}

OrSuggestions<ref<Object>> findAlongAttrPathWithAutoCall(
    Evaluator & evaluator,
    ref<Object> obj,
    const std::string & attrPathStr,
    const std::vector<std::string> & attrPath,
    const std::map<std::string, ref<Object>> & autoArgs)
{
    ref<Object> current = obj;

    for (const auto & attr : attrPath) {
        // Auto-call at each step (matches original: autoCallFunction before navigation)
        current = autoCall(evaluator, current, autoArgs);

        // Is attr an index (integer) or a normal attribute name?
        auto attrIndex = string2Int<unsigned int>(attr);

        if (!attrIndex) {
            // Navigate to attribute
            auto type = current->getType();
            if (type != nAttrs)
                throw Error(
                    "the expression selected by the selection path '%1%' should be a set but is %2%",
                    attrPathStr,
                    showType(type));

            if (attr.empty())
                throw Error("empty attribute name in selection path '%1%'", attrPathStr);

            auto next = current->maybeGetAttr(attr);
            if (!next) {
                auto attrNames = current->getAttrNames();
                StringSet strAttrNames;
                for (auto & name : attrNames)
                    strAttrNames.insert(name);

                auto suggestions = Suggestions::bestMatches(strAttrNames, attr);
                throw AttrPathNotFound(
                    suggestions, "attribute '%1%' in selection path '%2%' not found", attr, attrPathStr);
            }
            current = ref<Object>(next);
        } else {
            // Navigate to list index
            auto type = current->getType();
            if (type != nList)
                throw Error(
                    "the expression selected by the selection path '%1%' should be a list but is %2%",
                    attrPathStr,
                    showType(type));

            auto listSize = current->getListSize();
            if (*attrIndex >= listSize)
                throw Error("list index %1% in selection path '%2%' is out of range", *attrIndex, attrPathStr);

            current = ref<Object>(current->getListElem(*attrIndex));
        }
    }

    return OrSuggestions<ref<Object>>(current);
}

} // namespace nix::expr::helpers