#include "nix/store/globals.hh"
#include "nix/cmd/installable-attr-path.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/util/util.hh"
#include "nix/cmd/command.hh"
#include "nix/expr/attr-path.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/store/derivations.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/store/store-api.hh"
#include "nix/main/shared.hh"
#include "nix/flake/flake.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/evaluation-helpers.hh"
#include "nix/expr/environment/system.hh"
#include "nix/util/url.hh"
#include "nix/fetchers/registry.hh"
#include "nix/store/build-result.hh"

#include <regex>
#include <queue>

#include <nlohmann/json.hpp>

namespace nix {

InstallableAttrPath::InstallableAttrPath(
    ref<EvalState> state,
    ref<Evaluator> evaluator,
    SourceExprCommand & cmd,
    ref<Object> rootObject,
    const std::string & attrPath,
    ExtendedOutputsSpec extendedOutputsSpec)
    : InstallableValue(state, evaluator)
    , cmd(cmd)
    , rootObject(rootObject)
    , attrPath(attrPath)
    , extendedOutputsSpec(std::move(extendedOutputsSpec))
{
}

std::pair<Value *, PosIdx> InstallableAttrPath::toValue(EvalState & state)
{
    // Navigate via the Object interface to preserve replay.
    auto autoArgs = cmd.getAutoArgs(state);
    std::map<std::string, ref<Object>> autoArgsObj;
    for (auto & arg : *autoArgs)
        autoArgsObj.emplace(std::string(state.symbols[arg.name]), state.toObjectCompat(*arg.value));

    auto attrPathTokens = tokenizeString<std::vector<std::string>>(attrPath, ".");
    auto obj =
        *expr::helpers::findAlongAttrPathWithAutoCall(*evaluator, rootObject, attrPath, attrPathTokens, autoArgsObj);

    // Convert to Value at the leaf only
    auto v = obj->defeatCache();
    state.forceValue(**v, noPos);
    return {*v, obj->getPos()};
}

std::pair<Value *, PosIdx> InstallableAttrPath::toValueCached(EvalState & state)
{
    auto autoArgs = cmd.getAutoArgs(state);
    std::map<std::string, ref<Object>> autoArgsObj;
    for (auto & arg : *autoArgs)
        autoArgsObj.emplace(std::string(state.symbols[arg.name]), state.toObjectCompat(*arg.value));

    auto attrPathTokens = tokenizeString<std::vector<std::string>>(attrPath, ".");
    auto obj =
        *expr::helpers::findAlongAttrPathWithAutoCall(*evaluator, rootObject, attrPath, attrPathTokens, autoArgsObj);

    // Bridge via ExprFromObject — creates a lazy thunk that evaluates
    // through the Object interface, preserving cache replay.
    auto * v = state.allocValue();
    auto * expr = new ExprFromObject(obj, evaluator.get_ptr(), nullptr);
    state.mkThunk_(*v, expr);
    return {v, obj->getPos()};
}

DerivedPathsWithInfo InstallableAttrPath::toDerivedPaths()
{
    // Navigate via the Object interface to preserve replay.
    auto autoArgs = cmd.getAutoArgs(*state);
    std::map<std::string, ref<Object>> autoArgsObj;
    for (auto & arg : *autoArgs)
        autoArgsObj.emplace(std::string(state->symbols[arg.name]), state->toObjectCompat(*arg.value));

    auto attrPathTokens = tokenizeString<std::vector<std::string>>(attrPath, ".");
    auto obj =
        *expr::helpers::findAlongAttrPathWithAutoCall(*evaluator, rootObject, attrPath, attrPathTokens, autoArgsObj);

    if (auto derivedPathWithInfo =
            trySinglePathToDerivedPaths(*obj, fmt("while evaluating the attribute '%s'", attrPath))) {
        return {*derivedPathWithInfo};
    }

    // Handle derivations via the Object interface (preserves replay).
    if (expr::helpers::isDerivation(*obj)) {
        auto drvPath = expr::helpers::forceDerivation(*evaluator, *obj, *state->systemEnvironment->store);
        auto outputs = std::visit(
            overloaded{
                [&](const ExtendedOutputsSpec::Default &) -> OutputsSpec {
                    auto outputsToInstall = expr::helpers::getDerivationOutputs(*obj);
                    // getDerivationOutputs defaults to {"out"} when
                    // outputsToInstall metadata is absent. Verify "out"
                    // exists; if not, use the actual outputs.
                    if (outputsToInstall == StringSet{"out"}) {
                        if (auto outputsAttr = obj->maybeGetAttr("outputs")) {
                            auto outputNames = outputsAttr->getListOfStringsNoCtx();
                            bool hasOut =
                                std::find(outputNames.begin(), outputNames.end(), "out") != outputNames.end();
                            if (!hasOut)
                                return OutputsSpec::Names{StringSet(outputNames.begin(), outputNames.end())};
                        }
                    }
                    return OutputsSpec::Names{std::move(outputsToInstall)};
                },
                [&](const ExtendedOutputsSpec::Explicit & e) -> OutputsSpec { return e; },
            },
            extendedOutputsSpec.raw);
        return {{
            .path = DerivedPath::Built{
                .drvPath = makeConstantStorePathRef(drvPath),
                .outputs = outputs,
            },
            .info = make_ref<ExtraPathInfo>(),
        }};
    }

    // Fallback: getDerivations needs a Value — fall through to toValue
    auto [v, pos] = toValue(*state);

    PackageInfos packageInfos;
    getDerivations(*state, *v, "", *autoArgs, packageInfos, false);

    // Backward compatibility hack: group results by drvPath. This
    // helps keep .all output together.
    std::map<StorePath, OutputsSpec> byDrvPath;

    for (auto & packageInfo : packageInfos) {
        auto drvPath = packageInfo.queryDrvPath();
        if (!drvPath)
            throw Error("'%s' is not a derivation", what());

        auto newOutputs = std::visit(
            overloaded{
                [&](const ExtendedOutputsSpec::Default & d) -> OutputsSpec {
                    StringSet outputsToInstall;
                    for (auto & output : packageInfo.queryOutputs(false, true))
                        outputsToInstall.insert(output.first);
                    if (outputsToInstall.empty())
                        outputsToInstall.insert("out");
                    return OutputsSpec::Names{std::move(outputsToInstall)};
                },
                [&](const ExtendedOutputsSpec::Explicit & e) -> OutputsSpec { return e; },
            },
            extendedOutputsSpec.raw);

        auto [iter, didInsert] = byDrvPath.emplace(*drvPath, newOutputs);

        if (!didInsert)
            iter->second = iter->second.union_(newOutputs);
    }

    DerivedPathsWithInfo res;
    for (auto & [drvPath, outputs] : byDrvPath)
        res.push_back({
            .path =
                DerivedPath::Built{
                    .drvPath = makeConstantStorePathRef(drvPath),
                    .outputs = outputs,
                },
            .info = make_ref<ExtraPathInfoValue>(ExtraPathInfoValue::Value{
                .extendedOutputsSpec = outputs,
                /* FIXME: reconsider backwards compatibility above
                   so we can fill in this info. */
            }),
        });

    return res;
}

ref<Object> InstallableAttrPath::getRootObject()
{
    return rootObject;
}

InstallableAttrPath InstallableAttrPath::parse(
    ref<EvalState> state,
    ref<Evaluator> evaluator,
    SourceExprCommand & cmd,
    ref<Object> rootObject,
    std::string_view prefix,
    ExtendedOutputsSpec extendedOutputsSpec)
{
    return {
        state,
        evaluator,
        cmd,
        rootObject,
        prefix == "." ? "" : std::string{prefix},
        std::move(extendedOutputsSpec),
    };
}

} // namespace nix
