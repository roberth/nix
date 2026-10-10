#include "nix/store/derivations.hh"
#include "nix/store/outputs-query.hh"
#include "nix/store/parsed-derivations.hh"
#include "nix/store/derivation/full-inputs.hh"
#include "nix/store/derivation-options.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/store/nar-info.hh"
#include "nix/store/realisation.hh"
#include "nix/util/topo-sort.hh"
#include "nix/util/callback.hh"
#include "nix/util/closure.hh"
#include "nix/util/task-asio.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/strings.hh"
#include "nix/util/json-utils.hh"

#include <boost/unordered/unordered_flat_set.hpp>

namespace nix {

void Store::computeFSClosure(
    const StorePathSet & startPaths,
    StorePathSet & paths_,
    bool flipDirection,
    bool includeOutputs,
    bool includeDerivers)
{
    std::function<asio::awaitable<StorePathSet>(const StorePath & path)> queryDeps;
    if (flipDirection)
        queryDeps = [this, includeOutputs, includeDerivers](const StorePath & path) -> asio::awaitable<StorePathSet> {
            StorePathSet res;
            StorePathSet referrers;
            queryReferrers(path, referrers);
            for (auto & ref : referrers)
                if (ref != path)
                    res.insert(ref);

            if (includeOutputs)
                for (auto & i : queryValidDerivers(path))
                    res.insert(i);

            if (includeDerivers && path.isDerivation())
                for (auto & [_, maybeOutPath] : queryPartialDerivationOutputMap(path))
                    if (maybeOutPath && isValidPath(*maybeOutPath))
                        res.insert(*maybeOutPath);
            co_return res;
        };
    else
        queryDeps = [this, includeOutputs, includeDerivers](const StorePath & path) -> asio::awaitable<StorePathSet> {
            StorePathSet res;
            auto info = co_await callbackToAwaitable<ref<const ValidPathInfo>>(
                [this, path](Callback<ref<const ValidPathInfo>> cb) { queryPathInfo(path, std::move(cb)); });

            for (auto & ref : info->references)
                if (ref != path)
                    res.insert(ref);

            if (includeOutputs && path.isDerivation())
                for (auto & [_, maybeOutPath] : queryPartialDerivationOutputMap(path))
                    if (maybeOutPath && isValidPath(*maybeOutPath))
                        res.insert(*maybeOutPath);

            if (includeDerivers && info->deriver && isValidPath(*info->deriver))
                res.insert(*info->deriver);
            co_return res;
        };

    computeClosure<StorePath>(startPaths, paths_, GetEdgesAsync<StorePath>(queryDeps));
}

void Store::computeFSClosure(
    const StorePath & startPath, StorePathSet & paths_, bool flipDirection, bool includeOutputs, bool includeDerivers)
{
    StorePathSet paths;
    paths.insert(startPath);
    computeFSClosure(paths, paths_, flipDirection, includeOutputs, includeDerivers);
}

const ContentAddress * getDerivationCA(const Derivation & drv)
{
    auto out = drv.outputs.find("out");
    if (out == drv.outputs.end())
        return nullptr;
    if (auto dof = std::get_if<DerivationOutput::CAFixed>(&out->second.raw)) {
        return &dof->ca;
    }
    return nullptr;
}

/**
 * Ask the substituters in order for one path, and return the info of the
 * first one that has it.
 */
static Task<std::optional<SubstitutablePathInfo>>
querySubstitutablePathInfo(Store & store, StorePath path, std::optional<ContentAddress> ca)
{
    std::optional<Error> lastStoresException = std::nullopt;
    for (ref<Store> sub : getDefaultSubstituters()) {
        if (lastStoresException.has_value()) {
            logError(lastStoresException->info());
            lastStoresException.reset();
        }

        auto subPath(path);

        // Recompute store path so that we can use a different store root.
        if (ca) {
            subPath = store.makeFixedOutputPathFromCA(path.name(), ContentAddressWithReferences::withoutRefs(*ca));
            if (sub->storeDir == store.storeDir)
                assert(subPath == path);
            if (subPath != path)
                debug(
                    "replaced path '%s' with '%s' for substituter '%s'",
                    store.printStorePath(path),
                    sub->printStorePath(subPath),
                    sub->config.getHumanReadableURI());
        } else if (sub->storeDir != store.storeDir)
            continue;

        debug(
            "checking substituter '%s' for path '%s'", sub->config.getHumanReadableURI(), sub->printStorePath(subPath));
        try {
            auto info = co_await sub->queryPathInfoAsync(subPath);

            if (sub->storeDir != store.storeDir && !(info->isContentAddressed(*sub) && info->references.empty()))
                continue;

            auto narInfo = std::dynamic_pointer_cast<const NarInfo>(std::shared_ptr<const ValidPathInfo>(info));
            co_return SubstitutablePathInfo{
                .deriver = info->deriver,
                .references = info->references,
                .downloadSize = narInfo ? narInfo->fileSize : 0,
                .narSize = info->narSize,
            };
        } catch (InvalidPath &) {
        } catch (Error & e) {
            lastStoresException = std::make_optional(std::move(e));
        }
    }
    if (lastStoresException.has_value()) {
        if (!settings.getWorkerSettings().tryFallback) {
            throw std::move(*lastStoresException);
        } else
            logError(lastStoresException->info());
    }
    co_return std::nullopt;
}

/**
 * The default implementation of `Store::querySubstitutablePathInfos`.
 * `queryMissing` calls it directly, also for stores that override the
 * virtual function.
 */
static Task<SubstitutablePathInfos> querySubstitutablePathInfosFromSubstituters(Store & store, StorePathCAMap paths)
{
    SubstitutablePathInfos infos;

    if (!settings.getWorkerSettings().useSubstitutes)
        co_return infos;

    /* Each query returns its own result instead of writing to a shared
       map: a query that is cancelled then cannot leave a partial
       entry behind. */
    std::vector<StorePath> order;
    std::vector<Task<std::optional<SubstitutablePathInfo>>> queries;
    for (auto & [path, ca] : paths) {
        order.push_back(path);
        queries.push_back(querySubstitutablePathInfo(store, path, ca));
    }

    auto results = co_await whenAll(std::move(queries));

    for (size_t i = 0; i < results.size(); ++i)
        if (results[i])
            infos.insert_or_assign(order[i], std::move(*results[i]));

    co_return infos;
}

Task<SubstitutablePathInfos> Store::querySubstitutablePathInfos(StorePathCAMap paths)
{
    co_return co_await querySubstitutablePathInfosFromSubstituters(*this, std::move(paths));
}

MissingPaths Store::queryMissing(const std::vector<DerivedPath> & targets)
{
    Activity act(*logger, lvlDebug, actUnknown, "querying info about missing paths");

    MissingPaths res;

    auto mustBuildDrv = [&](const StorePath & drvPath, const Derivation & drv, std::set<DerivedPath> & edges) {
        res.willBuild.insert(drvPath);
        /* Group the requested outputs by (possibly dynamic) input
           derivation path, so that each input derivation contributes a
           single edge. */
        std::map<SingleDerivedPath, StringSet> byDrvPath;
        for (const auto & input : drv.inputs)
            if (auto * built = std::get_if<SingleDerivedPath::Built>(&input.raw()))
                byDrvPath[*built->drvPath].insert(built->output);
        for (auto & [inputDrv, outputs] : byDrvPath)
            edges.insert(DerivedPath::Built{make_ref<SingleDerivedPath>(inputDrv), std::move(outputs)});
    };

    GetEdgesAsync<DerivedPath> getEdges = [&](const DerivedPath & req) -> asio::awaitable<std::set<DerivedPath>> {
        std::set<DerivedPath> edges;

        co_await std::visit(
            overloaded{
                [&](const DerivedPath::Built & bfd) -> asio::awaitable<void> {
                    auto drvPathP = std::get_if<DerivedPath::Opaque>(&*bfd.drvPath);
                    if (!drvPathP) {
                        // TODO make work in this case.
                        warn(
                            "Ignoring dynamic derivation %s while querying missing paths; not yet implemented",
                            bfd.drvPath->to_string(*this));
                        co_return;
                    }
                    auto & drvPath = drvPathP->path;

                    if (!isValidPath(drvPath)) {
                        // FIXME: we could try to substitute the derivation.
                        res.unknown.insert(drvPath);
                        co_return;
                    }

                    StorePathSet invalid;
                    /* true for regular derivations, and CA derivations for which we
                       have a trust mapping for all wanted outputs. */
                    auto knownOutputPaths = true;
                    for (auto & [outputName, pathOpt] : queryPartialDerivationOutputMap(drvPath)) {
                        if (!pathOpt) {
                            knownOutputPaths = false;
                            break;
                        }
                        if (bfd.outputs.contains(outputName) && !isValidPath(*pathOpt))
                            invalid.insert(*pathOpt);
                    }
                    if (knownOutputPaths && invalid.empty())
                        co_return;

                    auto drv = make_ref<Derivation>(derivationFromPath(drvPath));
                    DerivationOptions<SingleDerivedPath> drvOptions;
                    try {
                        // FIXME: this is a lot of work just to get the value
                        // of `allowSubstitutes`.
                        drvOptions = derivationOptionsFromStructuredAttrs(
                            *this, drv->inputs, drv->env, get(drv->structuredAttrs));
                    } catch (Error & e) {
                        e.addTrace({}, "while parsing derivation '%s'", printStorePath(drvPath));
                        throw;
                    }

                    if (!knownOutputPaths && settings.getWorkerSettings().useSubstitutes
                        && drvOptions.substitutesAllowed(settings.getWorkerSettings())) {
                        experimentalFeatureSettings.require(Xp::CaDerivations);

                        // If there are unknown output paths, attempt to find if the
                        // paths are known to substituters through a realisation.
                        knownOutputPaths = true;

                        for (auto & [outputName, _] : drv->outputs) {
                            if (!bfd.outputs.contains(outputName))
                                continue;

                            bool found = false;
                            for (auto & sub : getDefaultSubstituters()) {
                                /* TODO: Asyncify this. */
                                auto realisation = sub->queryRealisation({drvPath, outputName});
                                if (!realisation)
                                    continue;
                                found = true;
                                if (!isValidPath(realisation->outPath))
                                    invalid.insert(realisation->outPath);
                                break;
                            }
                            if (!found) {
                                // Some paths did not have a realisation, this must be built.
                                knownOutputPaths = false;
                                break;
                            }
                        }
                    }

                    if (knownOutputPaths && settings.getWorkerSettings().useSubstitutes
                        && drvOptions.substitutesAllowed(settings.getWorkerSettings())) {
                        bool mustBuild = false;
                        StorePathSet substitutable;
                        auto * cap = getDerivationCA(*drv);

                        /* Query all outputs concurrently (but not in parallel,
                           computeClosure runs on a strand). If any one is not
                           substitutable then discard all other outputs. */
                        co_await forEachAsync(invalid, [&](const StorePath & outPath) -> asio::awaitable<void> {
                            if (mustBuild)
                                co_return;

                            auto infos = co_await taskToAwaitable(querySubstitutablePathInfosFromSubstituters(
                                *this, {{outPath, cap ? std::optional{*cap} : std::nullopt}}));

                            if (infos.empty())
                                mustBuild = true;
                            else
                                substitutable.insert(outPath);
                        });

                        if (mustBuild)
                            mustBuildDrv(drvPath, *drv, edges);
                        else
                            for (auto & path : substitutable)
                                edges.insert(DerivedPath::Opaque{path});
                    } else {
                        mustBuildDrv(drvPath, *drv, edges);
                    }
                },
                [&](const DerivedPath::Opaque & bo) -> asio::awaitable<void> {
                    if (isValidPath(bo.path))
                        co_return;

                    auto infos = co_await taskToAwaitable(
                        querySubstitutablePathInfosFromSubstituters(*this, {{bo.path, std::nullopt}}));

                    if (infos.empty()) {
                        res.unknown.insert(bo.path);
                        co_return;
                    }

                    auto info = infos.find(bo.path);
                    assert(info != infos.end());
                    res.willSubstitute.insert(bo.path);
                    res.downloadSize += info->second.downloadSize;
                    res.narSize += info->second.narSize;

                    for (auto & ref : info->second.references)
                        edges.insert(DerivedPath::Opaque{ref});
                },
            },
            req.raw());

        co_return edges;
    };

    std::set<DerivedPath> startElts(targets.begin(), targets.end());
    std::set<DerivedPath> visited;
    computeClosure(std::move(startElts), visited, std::move(getEdges));

    return res;
}

StorePaths Store::topoSortPaths(const StorePathSet & paths)
{
    auto result = topoSort(paths, [&](const StorePath & path) {
        try {
            return queryPathInfo(path)->references;
        } catch (InvalidPath &) {
            return StorePathSet();
        }
    });

    return std::visit(
        overloaded{
            [&](const Cycle<StorePath> & cycle) -> StorePaths {
                throw BuildError(
                    BuildResult::Failure::OutputRejected,
                    "cycle detected in the references of '%s' from '%s'",
                    printStorePath(cycle.path),
                    printStorePath(cycle.parent));
            },
            [](const auto & sorted) { return sorted; }},
        result);
}

OutputPathMap resolveDerivedPath(Store & store, const DerivedPath::Built & bfd, Store * evalStore_)
{
    auto drvPath = resolveDerivedPath(store, *bfd.drvPath, evalStore_);

    auto outputsOpt_ = deepQueryPartialDerivationOutputMap(store, drvPath, evalStore_);

    auto outputsOpt = std::visit(
        overloaded{
            [&](const OutputsSpec::All &) {
                // Keep all outputs
                return std::move(outputsOpt_);
            },
            [&](const OutputsSpec::Names & names) {
                // Get just those mentioned by name
                std::map<std::string, std::optional<StorePath>> outputsOpt;
                for (auto & output : names) {
                    auto * pOutputPathOpt = get(outputsOpt_, output);
                    if (!pOutputPathOpt)
                        throw Error(
                            "the derivation '%s' doesn't have an output named '%s'",
                            bfd.drvPath->to_string(store),
                            output);
                    outputsOpt.insert_or_assign(output, std::move(*pOutputPathOpt));
                }
                return outputsOpt;
            },
        },
        bfd.outputs.raw);

    OutputPathMap outputs;
    for (auto & [outputName, outputPathOpt] : outputsOpt) {
        if (!outputPathOpt)
            throw MissingRealisation(store, *bfd.drvPath, drvPath, outputName);
        auto & outputPath = *outputPathOpt;
        outputs.insert_or_assign(outputName, outputPath);
    }
    return outputs;
}

StorePath resolveDerivedPath(Store & store, const SingleDerivedPath & req, Store * evalStore_)
{
    return std::visit(
        overloaded{
            [&](const SingleDerivedPath::Opaque & bo) { return bo.path; },
            [&](const SingleDerivedPath::Built & bfd) {
                auto drvPath = resolveDerivedPath(store, *bfd.drvPath, evalStore_);
                auto outPath = deepQueryPartialDerivationOutput(store, drvPath, bfd.output, evalStore_);
                if (!outPath)
                    throw MissingRealisation(store, *bfd.drvPath, drvPath, bfd.output);
                return *outPath;
            },
        },
        req.raw());
}

} // namespace nix

namespace nlohmann {

nix::TrustedFlag adl_serializer<nix::TrustedFlag>::from_json(const json & json)
{
    using namespace nix;
    return getBoolean(json) ? TrustedFlag::Trusted : TrustedFlag::NotTrusted;
}

void adl_serializer<nix::TrustedFlag>::to_json(json & json, const nix::TrustedFlag & trustedFlag)
{
    json = static_cast<bool>(trustedFlag);
}

} // namespace nlohmann
