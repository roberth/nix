#include "nix/cmd/installable-value.hh"
#include "nix/expr/environment/system.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/evaluation-helpers.hh"
#include "nix/fetchers/fetch-to-store.hh"

namespace nix {

std::pair<Value *, PosIdx> InstallableValue::toValueCached(EvalState & state)
{
    // Default: fall back to toValue (defeats cache)
    return toValue(state);
}

std::vector<ref<eval_cache::AttrCursor>> InstallableValue::getCursors(EvalState & state)
{
    auto evalCache =
        std::make_shared<nix::eval_cache::EvalCache>(std::nullopt, state, [&]() { return toValue(state).first; });
    return {evalCache->getRoot()};
}

ref<eval_cache::AttrCursor> InstallableValue::getCursor(EvalState & state)
{
    /* Although getCursors should return at least one element, in case it doesn't,
       bound check to avoid an undefined behavior for vector[0] */
    return getCursors(state).at(0);
}

static UsageError nonValueInstallable(Installable & installable)
{
    return UsageError("installable '%s' does not correspond to a Nix language value", installable.what());
}

InstallableValue & InstallableValue::require(Installable & installable)
{
    auto * castedInstallable = dynamic_cast<InstallableValue *>(&installable);
    if (!castedInstallable)
        throw nonValueInstallable(installable);
    return *castedInstallable;
}

ref<InstallableValue> InstallableValue::require(ref<Installable> installable)
{
    auto castedInstallable = installable.dynamic_pointer_cast<InstallableValue>();
    if (!castedInstallable)
        throw nonValueInstallable(*installable);
    return ref{castedInstallable};
}

std::optional<DerivedPathWithInfo>
InstallableValue::trySinglePathToDerivedPaths(Object & obj, std::string_view errorCtx)
{
    if (obj.getType() == nPath) {
        auto storePath =
            fetchToStore(state->fetchSettings, *state->systemEnvironment->store, obj.getPath(), FetchMode::Copy);
        return {{
            .path =
                DerivedPath::Opaque{
                    .path = std::move(storePath),
                },
            .info = make_ref<ExtraPathInfo>(),
        }};
    }

    else if (obj.getType() == nString) {
        auto [s, context] = obj.getStringWithContext();

        if (context.empty()) {
            // The coarse eval cache lossily stores nPath values as context-less
            // store path strings. Defeat the cache to recover the true type.
            auto v = obj.defeatCache();
            if ((**v).type() == nPath) {
                auto storePath =
                    fetchToStore(state->fetchSettings, *state->systemEnvironment->store, (**v).path(), FetchMode::Copy);
                return {{
                    .path =
                        DerivedPath::Opaque{
                            .path = std::move(storePath),
                        },
                    .info = make_ref<ExtraPathInfo>(),
                }};
            }
        }

        return {{
            .path = DerivedPath::fromSingle(expr::helpers::coerceToSingleDerivedPath(obj, *evaluator, errorCtx)),
            .info = make_ref<ExtraPathInfo>(),
        }};
    }

    else
        return std::nullopt;
}

} // namespace nix
