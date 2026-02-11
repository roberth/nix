#include "nix/store/globals.hh"
#include "nix/cmd/installable-attr-path.hh"
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
#include "nix/expr/evaluation-helpers.hh"
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
    auto v = rootObject->defeatCache();
    auto [vRes, pos] = findAlongAttrPath(state, attrPath, *cmd.getAutoArgs(state), **v);
    state.forceValue(*vRes, pos);
    return {vRes, pos};
}

DerivedPathsWithInfo InstallableAttrPath::toDerivedPaths()
{
    auto root = getRootObject();

    auto attrPaths = attrPath.empty() ? std::vector<std::string>{""} : std::vector<std::string>{attrPath};
    auto attrResult = expr::helpers::tryAttrPaths(*root, attrPaths, *state);
    if (!attrResult) {
        throw Error(attrResult.getSuggestions(), "attribute '%s' not found", attrPath);
    }

    auto [attr, resolvedPath] = *attrResult;

    if (!expr::helpers::isDerivation(*attr)) {
        if (auto derivedPath = expr::helpers::trySinglePathToDerivedPath(
                *evaluator, *attr, fmt("while evaluating the attribute '%s'", attrPath))) {
            return {{
                .path = *derivedPath,
                .info = make_ref<ExtraPathInfo>(),
            }};
        } else {
            auto v = attr->defeatCache();
            throw Error("attribute '%s' is not a derivation or path but %s", attrPath, showType(**v));
        }
    }

    auto drvPath = expr::helpers::forceDerivation(*evaluator, *attr, evaluator->getStore());

    return {{
        .path =
            DerivedPath::Built{
                .drvPath = makeConstantStorePathRef(std::move(drvPath)),
                .outputs = std::visit(
                    overloaded{
                        [&](const ExtendedOutputsSpec::Default & d) -> OutputsSpec {
                            auto outputsToInstall = expr::helpers::getDerivationOutputs(*attr);
                            return OutputsSpec::Names{std::move(outputsToInstall)};
                        },
                        [&](const ExtendedOutputsSpec::Explicit & e) -> OutputsSpec { return e; },
                    },
                    extendedOutputsSpec.raw),
            },
        .info = make_ref<ExtraPathInfoValue>(ExtraPathInfoValue::Value{
            .attrPath = attrPath,
            .extendedOutputsSpec = extendedOutputsSpec,
        }),
    }};
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
