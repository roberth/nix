#include "nix/expr/evaluation-helpers.hh"
#include "nix/util/error.hh"
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

} // namespace nix::expr::helpers