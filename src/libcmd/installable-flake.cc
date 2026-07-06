#include "nix/cmd/installable-flake.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/util/util.hh"
#include "nix/cmd/command.hh"
#include "nix/expr/attr-path.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/evaluation-helpers.hh"
#include "nix/flake/flake.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/coarse-eval-cache.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/interpreter.hh"

#include <nlohmann/json.hpp>

namespace nix {

std::vector<std::string> InstallableFlake::getActualAttrPaths()
{
    std::vector<std::string> res;
    if (attrPaths.size() == 1 && attrPaths.front().starts_with(".")) {
        attrPaths.front().erase(0, 1);
        res.push_back(attrPaths.front());
        return res;
    }

    for (auto & prefix : prefixes)
        res.push_back(prefix + *attrPaths.begin());

    for (auto & s : attrPaths)
        res.push_back(s);

    return res;
}

static std::string showAttrPaths(const std::vector<std::string> & paths)
{
    std::string s;
    for (const auto & [n, i] : enumerate(paths)) {
        if (n > 0)
            s += n + 1 == paths.size() ? " or " : ", ";
        s += '\'';
        s += i;
        s += '\'';
    }
    return s;
}

InstallableFlake::InstallableFlake(
    SourceExprCommand * cmd,
    ref<EvalState> state,
    FlakeRef && flakeRef,
    std::string_view fragment,
    ExtendedOutputsSpec extendedOutputsSpec,
    Strings attrPaths,
    Strings prefixes,
    const flake::LockFlags & lockFlags)
    /* Wrap whatever Evaluator the caller's EvalState already exposes —
       under tracing-eval-cache that is the
       TracingReplayEvaluator → TracingEvaluator → Interpreter stack
       pre-populated by EvalCommand::getEvalState(), so flake-installable
       traffic flows through the trie like attr-path traffic already
       does. With tracing off, `toEvaluatorCompat()` falls back to a
       plain Interpreter — same shape as before. */
    : InstallableValue(state, make_ref<CoarseEvalCache>(state->toEvaluatorCompat()))
    , flakeRef(flakeRef)
    , attrPaths(fragment == "" ? attrPaths : Strings{(std::string) fragment})
    , prefixes(fragment == "" ? Strings{} : prefixes)
    , extendedOutputsSpec(std::move(extendedOutputsSpec))
    , lockFlags(lockFlags)
    , coarseEvalCache(evaluator.cast<CoarseEvalCache>())
{
    if (cmd && cmd->getAutoArgs(*state)->size())
        throw UsageError("'--arg' and '--argstr' are incompatible with flakes");
}

DerivedPathsWithInfo InstallableFlake::toDerivedPaths()
{
    Activity act(*logger, lvlTalkative, actUnknown, fmt("evaluating derivation '%s'", what()));

    auto root = getRootObject();

    auto attrPaths = getActualAttrPaths();
    auto attrResult = expr::helpers::tryAttrPaths(*root, attrPaths, *state);
    if (!attrResult) {
        throw Error(
            attrResult.getSuggestions(),
            "flake '%s' does not provide attribute %s",
            flakeRef,
            showAttrPaths(attrPaths));
    }

    auto [attr, attrPath] = *attrResult;

    if (!expr::helpers::isDerivation(*attr)) {

        if (auto derivedPathWithInfo =
                trySinglePathToDerivedPaths(*attr, fmt("while evaluating the flake output attribute '%s'", attrPath))) {
            return {*derivedPathWithInfo};
        } else {
            auto v = attr->defeatCache();
            throw Error(
                "expected flake output attribute '%s' to be a derivation or path but found %s: %s",
                attrPath,
                showType(**v),
                ValuePrinter(*this->state, **v, errorPrintOptions));
        }
    }

    auto drvPath = expr::helpers::forceDerivation(*evaluator, *attr, evaluator->getStore());

    std::optional<NixInt::Inner> priority;

    if (attr->maybeGetAttr(std::string(state->symbols[state->s.outputSpecified]))) {
    } else if (auto aMeta = attr->maybeGetAttr(std::string(state->symbols[state->s.meta]))) {
        if (auto aPriority = aMeta->maybeGetAttr("priority"))
            priority = aPriority->getInt("while getting priority").value;
    }

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
        .info = make_ref<ExtraPathInfoFlake>(
            ExtraPathInfoValue::Value{
                .priority = priority,
                .attrPath = attrPath,
                .extendedOutputsSpec = extendedOutputsSpec,
            },
            ExtraPathInfoFlake::Flake{
                .originalRef = flakeRef,
                .lockedRef = getLockedFlake()->flake.lockedRef,
            }),
    }};
}

std::pair<Value *, PosIdx> InstallableFlake::toValue(EvalState & state)
{
    /* When tracing is on, route through `getRootObject` (which calls
       `callFlakeViaEvaluator`) and walk the attr path via the Object
       interface, so the work is recorded/replayed by the trie. The
       cursor-based fall-through uses `openEvalCache`'s legacy path and
       bypasses the trie. */
    if (state.settings.useTracingEvalCache) {
        auto root = getRootObject();
        auto attrPaths = getActualAttrPaths();
        auto attrResult = expr::helpers::tryAttrPaths(*root, attrPaths, state);
        if (!attrResult)
            throw Error(
                attrResult.getSuggestions(),
                "flake '%s' does not provide attribute %s",
                flakeRef,
                showAttrPaths(attrPaths));
        auto [attr, attrPath] = *attrResult;
        auto v = attr->defeatCache();
        state.forceValue(**v, noPos);
        return {*v, attr->getPos()};
    }
    return {&getCursor(state)->forceValue(), noPos};
}

std::pair<Value *, PosIdx> InstallableFlake::toValueCached(EvalState & state)
{
    /* When tracing is on, return a lazy thunk that evaluates through
       the Object interface (via `ExprFromObject`), so the trie's
       cache-replay path stays engaged at leaf-force time too — the
       same shape `InstallableAttrPath::toValueCached` uses. Defeating
       the cache eagerly (as `toValue` must, to return a forced Value)
       throws away the replay we just won. */
    if (state.settings.useTracingEvalCache) {
        auto root = getRootObject();
        auto attrPaths = getActualAttrPaths();
        auto attrResult = expr::helpers::tryAttrPaths(*root, attrPaths, state);
        if (!attrResult)
            throw Error(
                attrResult.getSuggestions(),
                "flake '%s' does not provide attribute %s",
                flakeRef,
                showAttrPaths(attrPaths));
        auto [attr, attrPath] = *attrResult;
        auto * v = state.allocValue();
        /* A function-typed `attr` would route ExprFromObject's nFunction
           branch through makeCachedFnPrimOp because innerEvaluator is
           set; that PrimOp's queryFn dereferences outerResolver, so
           passing null here crashes when --apply (or any caller) applies
           the function. Construct a no-op resolver here — there's no
           inner cache writer/state at this flake-installable boundary,
           but the resolver still needs to exist for the queryFn to
           route ambient queries against the (non-cached) outer arg. */
        auto resolver = makeAmbientResolver(&state, evaluator, nullptr);
        auto * expr = new ExprFromObject(attr, evaluator.get_ptr(), resolver);
        state.mkThunk_(*v, expr);
        return {v, attr->getPos()};
    }
    return InstallableValue::toValueCached(state);
}

std::vector<ref<eval_cache::AttrCursor>> InstallableFlake::getCursors(EvalState & state)
{
    auto evalCache = openEvalCache(state, getLockedFlake());

    auto root = evalCache->getRoot();

    std::vector<ref<eval_cache::AttrCursor>> res;

    Suggestions suggestions;
    auto attrPaths = getActualAttrPaths();

    for (auto & attrPath : attrPaths) {
        debug("trying flake output attribute '%s'", attrPath);

        try {
            auto attr = root->findAlongAttrPath(AttrPath::parse(state, attrPath));
            if (attr) {
                res.push_back(ref(*attr));
            } else {
                suggestions += attr.getSuggestions();
            }
        } catch (TypeError & e) {
            debug("error resolving attribute '%s': %s", attrPath, e.msg());
            // Continue to next attribute path
        }
    }

    if (res.size() == 0)
        throw Error(suggestions, "flake '%s' does not provide attribute %s", flakeRef, showAttrPaths(attrPaths));

    return res;
}

ref<Object> InstallableFlake::getRootObject()
{
    /* With tracing-eval-cache on, route through the trie via the
       Evaluator-interface re-expression of `callFlake`. The lazy
       paths inside each input's `sourceInfo.outPath` carry the
       input's unpinned URL as identity, so the trie keys on the
       structurally-decomposed call (evalFile + mkString + per-input
       mkAttrs + apply chain) rather than on a coarse fingerprint
       that would itself presuppose eager input fetching. The legacy
       `eval-cache` path stays as-is for users who haven't opted in.

       Mutual exclusion: `eval-cache` is treated as the legacy coarse
       knob; when `tracing-eval-cache` is on it wins regardless of
       `eval-cache`. */
    if (state->settings.useTracingEvalCache) {
        return flake::callFlakeViaEvaluator(*evaluator, *state, *getLockedFlake());
    }
    // openEvalCache is memoized in state.evalCaches by fingerprint
    auto evalCache = openEvalCache(*state, getLockedFlake());
    return coarseEvalCache->getRoot(evalCache);
}

ref<flake::LockedFlake> InstallableFlake::getLockedFlake() const
{
    if (!_lockedFlake) {
        flake::LockFlags lockFlagsApplyConfig = lockFlags;
        // FIXME why this side effect?
        lockFlagsApplyConfig.applyNixConfig = true;
        _lockedFlake = make_ref<flake::LockedFlake>(lockFlake(flakeSettings, *state, flakeRef, lockFlagsApplyConfig));
    }
    // _lockedFlake is now non-null but still just a shared_ptr
    return ref<flake::LockedFlake>(_lockedFlake);
}

FlakeRef InstallableFlake::nixpkgsFlakeRef() const
{
    auto lockedFlake = getLockedFlake();

    if (auto nixpkgsInput = lockedFlake->lockFile.findInput({"nixpkgs"})) {
        if (auto lockedNode = std::dynamic_pointer_cast<const flake::LockedNode>(nixpkgsInput)) {
            if (lockedNode->isFlake) {
                debug("using nixpkgs flake '%s'", lockedNode->lockedRef);
                return std::move(lockedNode->lockedRef);
            }
        }
    }

    return defaultNixpkgsFlakeRef();
}

} // namespace nix
