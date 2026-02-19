#include "nix/expr/evaluation-helpers.hh"
#include "nix/util/error.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/eval.hh"

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

    // Not a function, or function without formals - return as-is
    auto funcInfo = obj->getFunctionInfo();
    if (type != nFunction || !funcInfo) {
        return obj;
    }

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
                throw Error(
                    "cannot evaluate a function that has an argument without a value ('%1%')\n\n"
                    "Nix attempted to evaluate a function as a top level expression; in\n"
                    "this case it must have its arguments supplied either by default\n"
                    "values, or passed explicitly with '--arg' or '--argstr'. See\n"
                    "https://nix.dev/manual/nix/stable/language/syntax.html#functions.",
                    formalName);
            }
        }
    }

    auto argObj = evaluator.mkAttrs(callArgs);
    return evaluator.apply(obj, argObj);
}

} // namespace nix::expr::helpers