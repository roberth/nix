#include <nlohmann/json.hpp>
#include <assert.h>
#include <stdint.h>
#include <boost/container/detail/std_fwd.hpp>
#include <boost/core/pointer_traits.hpp>
#include <boost/unordered/detail/foa/table.hpp>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "nix/util/terminal.hh"
#include "nix/util/ref.hh"
#include "nix/util/environment-variables.hh"
#include "nix/flake/flake.hh"
#include "nix/expr/environment/system.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/source-root.hh"

#include "copyable-boundary-walk.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/flake/lockfile.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/util/finally.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/flake/settings.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/fetch-tree.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/expr/value/context.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/registry.hh"
#include "nix/flake/flakeref.hh"
#include "nix/store/path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/configuration.hh"
#include "nix/util/error.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fmt.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/pos-table.hh"
#include "nix/util/source-path.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"

namespace nix {
struct SourceAccessor;

namespace flake {

static void forceTrivialValue(EvalState & state, Value & value, const PosIdx pos)
{
    if (value.isThunk() && value.isTrivial())
        state.forceValue(value, pos);
}

static void expectType(EvalState & state, ValueType type, Value & value, const PosIdx pos)
{
    forceTrivialValue(state, value, pos);
    if (value.type() != type)
        throw Error("expected %s but got %s at %s", showType(type), showType(value.type()), state.positions[pos]);
}

static std::pair<std::map<FlakeId, FlakeInput>, fetchers::Attrs> parseFlakeInputs(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir,
    bool allowSelf);

static void parseFlakeInputAttr(EvalState & state, const Attr & attr, fetchers::Attrs & attrs)
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (attr.value->type()) {
    case nString:
        attrs.emplace(state.symbols[attr.name], std::string(attr.value->string_view()));
        break;
    case nBool:
        attrs.emplace(state.symbols[attr.name], Explicit<bool>{attr.value->boolean()});
        break;
    case nInt: {
        auto intValue = attr.value->integer().value;
        if (intValue < 0)
            state
                .error<EvalError>(
                    "negative value given for flake input attribute %1%: %2%", state.symbols[attr.name], intValue)
                .debugThrow();
        attrs.emplace(state.symbols[attr.name], uint64_t(intValue));
        break;
    }
    default:
        if (attr.name == state.symbols.create("publicKeys")) {
            experimentalFeatureSettings.require(Xp::VerifiedFetches);
            NixStringContext emptyContext = {};
            attrs.emplace(
                state.symbols[attr.name], printValueAsJSON(state, true, *attr.value, attr.pos, emptyContext).dump());
        } else
            state
                .error<TypeError>(
                    "flake input attribute '%s' is %s while a string, Boolean, or integer is expected",
                    state.symbols[attr.name],
                    showType(*attr.value))
                .debugThrow();
    }
#pragma GCC diagnostic pop
}

static FlakeInput parseFlakeInput(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir)
{
    expectType(state, nAttrs, *value, pos);

    FlakeInput input;

    auto sInputs = state.symbols.create("inputs");
    auto sUrl = state.symbols.create("url");
    auto sFlake = state.symbols.create("flake");
    auto sFollows = state.symbols.create("follows");
    auto sCopyToStore = state.symbols.create("copyToStore");

    fetchers::Attrs attrs;
    std::optional<std::string> url;

    for (auto & attr : *value->attrs()) {
        try {
            if (attr.name == sUrl) {
                forceTrivialValue(state, *attr.value, pos);
                if (attr.value->type() == nString)
                    url = attr.value->string_view();
                else if (attr.value->type() == nPath) {
                    auto path = attr.value->path();
                    if (path.accessor != flakeDir.accessor)
                        throw Error(
                            "input attribute path '%s' at %s must be in the same source tree as %s",
                            path,
                            state.positions[attr.pos],
                            flakeDir);
                    url = "path:" + flakeDir.path.makeRelative(path.path);
                } else
                    throw Error(
                        "expected a string or a path but got %s at %s",
                        showType(attr.value->type()),
                        state.positions[attr.pos]);
                attrs.emplace("url", *url);
            } else if (attr.name == sFlake) {
                expectType(state, nBool, *attr.value, attr.pos);
                input.isFlake = attr.value->boolean();
            } else if (attr.name == sCopyToStore) {
                expectType(state, nBool, *attr.value, attr.pos);
                input.copyToStore = attr.value->boolean();
            } else if (attr.name == sInputs) {
                input.overrides =
                    parseFlakeInputs(state, attr.value, attr.pos, lockRootAttrPath, flakeDir, false).first;
            } else if (attr.name == sFollows) {
                expectType(state, nString, *attr.value, attr.pos);
                auto follows(parseInputAttrPath(attr.value->string_view()));
                follows.insert(follows.begin(), lockRootAttrPath.begin(), lockRootAttrPath.end());
                input.follows = follows;
            } else
                parseFlakeInputAttr(state, attr, attrs);
        } catch (Error & e) {
            e.addTrace(
                state.positions[attr.pos], HintFmt("while evaluating flake attribute '%s'", state.symbols[attr.name]));
            throw;
        }
    }

    if (attrs.count("type"))
        try {
            input.ref = FlakeRef::fromAttrs(state.fetchSettings, attrs);
        } catch (Error & e) {
            e.addTrace(state.positions[pos], HintFmt("while evaluating flake input"));
            throw;
        }
    else {
        attrs.erase("url");
        if (!attrs.empty())
            throw Error("unexpected flake input attribute '%s', at %s", attrs.begin()->first, state.positions[pos]);
        if (url)
            input.ref = parseFlakeRef(state.fetchSettings, *url, {}, true, input.isFlake, true);
    }

    if (input.ref && input.follows)
        throw Error("flake input has both a flake reference and a follows attribute, at %s", state.positions[pos]);

    return input;
}

static std::pair<std::map<FlakeId, FlakeInput>, fetchers::Attrs> parseFlakeInputs(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir,
    bool allowSelf)
{
    std::map<FlakeId, FlakeInput> inputs;
    fetchers::Attrs selfAttrs;

    expectType(state, nAttrs, *value, pos);

    for (auto & inputAttr : *value->attrs()) {
        auto inputName = state.symbols[inputAttr.name];
        if (inputName == "self") {
            if (!allowSelf)
                throw Error("'self' input attribute not allowed at %s", state.positions[inputAttr.pos]);
            expectType(state, nAttrs, *inputAttr.value, inputAttr.pos);
            for (auto & attr : *inputAttr.value->attrs())
                parseFlakeInputAttr(state, attr, selfAttrs);
        } else {
            inputs.emplace(
                inputName, parseFlakeInput(state, inputAttr.value, inputAttr.pos, lockRootAttrPath, flakeDir));
        }
    }

    return {inputs, selfAttrs};
}

static Flake readFlake(
    EvalState & state,
    const FlakeRef & originalRef,
    const FlakeRef & resolvedRef,
    const FlakeRef & lockedRef,
    const SourcePath & rootDir,
    const InputAttrPath & lockRootAttrPath)
{
    /* Part 3: route the flake's `?dir=` subdir composition through
       the kind-aware `resolveSymlinks` wrapper. The bare
       `CanonPath(raw)` silently clamps `..`-past-root, so
       `getFlake "path:/nix/store/X-source?dir=../etc"` would
       quietly resolve `subdir` to `/etc` inside the storepath.
       The wrapper raises `AccessorBoundaryEscape`; catch and
       rewrap with the flake-input-specific diagnostic.

       Compose against `rootDir.path`, not bare accessor root: for
       relative-input flakes, `rootDir` already names the input's
       in-tree path (e.g. `/sub`), and `?dir=` is resolved against
       that. Walking from accessor root would land on `/inner`
       instead of `/sub/inner` and read the wrong flake.nix. */
    SourcePath flakeDir = rootDir;
    if (!resolvedRef.subdir.empty()) {
        auto resolved = joinAndCheckCopyable(
            state.getOrCreateRoot(rootDir.accessor, SourceRootKind::Copyable, lockedRef.input.toUnpinnedURL()),
            rootDir.path,
            resolvedRef.subdir,
            SymlinkResolution::Ancestors,
            [&]() {
                return Error(
                    "flake input subdir '%s' escapes the source tree at %s",
                    resolvedRef.subdir,
                    rootDir.accessor->showPath(CanonPath::root));
            });
        flakeDir = SourcePath{rootDir.accessor, resolved};
    }
    auto flakePath = flakeDir / "flake.nix";

    // NOTE evalFile forces vInfo to be an attrset because mustBeTrivial is true.
    Value vInfo;
    /* All `readFlake` callers in this branch admit Copyable
       accessors — the lazy-paths reshape in `feat(libflake):
       consume lazy fetchTreeFinal results in call-flake.nix`
       removed the previous "second call after `mountInput`" path
       that routed through rootFS. The runtime check below stays
       as a defensive no-op against a future caller breaking
       the invariant (would surface as System routing). */
    auto root =
        &*flakePath.accessor == &*state.rootFS
            ? state.rootFSRoot
            : state.getOrCreateRoot(flakePath.accessor, SourceRootKind::Copyable, lockedRef.input.toUnpinnedURL());
    auto flakeRooted = RootedPath{root, flakePath.path};
    state.evalFile(flakeRooted, vInfo, true);

    Flake flake{
        .originalRef = originalRef,
        .resolvedRef = resolvedRef,
        .lockedRef = lockedRef,
        .path = flakePath,
    };

    if (auto description = vInfo.attrs()->get(state.s.description)) {
        expectType(state, nString, *description->value, description->pos);
        flake.description = description->value->string_view();
    }

    auto sInputs = state.symbols.create("inputs");

    if (auto inputs = vInfo.attrs()->get(sInputs)) {
        auto [flakeInputs, selfAttrs] =
            parseFlakeInputs(state, inputs->value, inputs->pos, lockRootAttrPath, flakeDir, true);
        flake.inputs = std::move(flakeInputs);
        flake.selfAttrs = std::move(selfAttrs);
    }

    auto sOutputs = state.symbols.create("outputs");

    if (auto outputs = vInfo.attrs()->get(sOutputs)) {
        expectType(state, nFunction, *outputs->value, outputs->pos);

        if (outputs->value->isLambda()) {
            if (auto formals = outputs->value->lambda().fun->getFormals()) {
                for (auto & formal : formals->formals) {
                    if (formal.name != state.s.self)
                        flake.inputs.emplace(
                            state.symbols[formal.name],
                            FlakeInput{
                                .ref = parseFlakeRef(state.fetchSettings, std::string(state.symbols[formal.name]))});
                }
            }
        }

    } else
        throw Error("flake '%s' lacks attribute 'outputs'", resolvedRef);

    auto sNixConfig = state.symbols.create("nixConfig");

    if (auto nixConfig = vInfo.attrs()->get(sNixConfig)) {
        expectType(state, nAttrs, *nixConfig->value, nixConfig->pos);

        for (auto & setting : *nixConfig->value->attrs()) {
            forceTrivialValue(state, *setting.value, setting.pos);
            if (setting.value->type() == nString)
                flake.config.settings.emplace(
                    state.symbols[setting.name], std::string(state.forceStringNoCtx(*setting.value, setting.pos, "")));
            else if (setting.value->type() == nPath) {
                auto storePath = fetchToStore(
                    state.fetchSettings, *state.systemEnvironment->store, setting.value->path(), FetchMode::Copy);
                flake.config.settings.emplace(
                    state.symbols[setting.name], state.systemEnvironment->store->printStorePath(storePath));
            } else if (setting.value->type() == nInt)
                flake.config.settings.emplace(
                    state.symbols[setting.name], state.forceInt(*setting.value, setting.pos, "").value);
            else if (setting.value->type() == nBool)
                flake.config.settings.emplace(
                    state.symbols[setting.name], Explicit<bool>{state.forceBool(*setting.value, setting.pos, "")});
            else if (setting.value->type() == nList) {
                std::vector<std::string> ss;
                for (auto elem : setting.value->listView()) {
                    if (elem->type() != nString)
                        state
                            .error<TypeError>(
                                "list element in flake configuration setting '%s' is %s while a string is expected",
                                state.symbols[setting.name],
                                showType(*setting.value))
                            .debugThrow();
                    ss.emplace_back(state.forceStringNoCtx(*elem, setting.pos, ""));
                }
                flake.config.settings.emplace(state.symbols[setting.name], ss);
            } else
                state
                    .error<TypeError>(
                        "flake configuration setting '%s' is %s", state.symbols[setting.name], showType(*setting.value))
                    .debugThrow();
        }
    }

    for (auto & attr : *vInfo.attrs()) {
        if (attr.name != state.s.description && attr.name != sInputs && attr.name != sOutputs
            && attr.name != sNixConfig)
            throw Error(
                "flake '%s' has an unsupported attribute '%s', at %s",
                resolvedRef,
                state.symbols[attr.name],
                state.positions[attr.pos]);
    }

    return flake;
}

static FlakeRef applySelfAttrs(const FlakeRef & ref, const Flake & flake)
{
    auto newRef(ref);

    /* `copyToStore` is honoured at `callFlake` emit time (see the
       root-node lookup there), not by the fetcher — listing it here
       just keeps the "is this allowed" guard from rejecting it. */
    StringSet allowedAttrs{"submodules", "lfs", "copyToStore"};

    for (auto & attr : flake.selfAttrs) {
        if (!allowedAttrs.contains(attr.first))
            throw Error("flake 'self' attribute '%s' is not supported", attr.first);
        if (attr.first == "copyToStore")
            continue;
        newRef.input.attrs.insert_or_assign(attr.first, attr.second);
    }

    return newRef;
}

static Flake getFlake(
    EvalState & state,
    const FlakeRef & originalRef,
    fetchers::UseRegistries useRegistries,
    const InputAttrPath & lockRootAttrPath)
{
    // Fetch a lazy tree first.
    auto cachedInput = state.inputCache->getAccessor(
        state.fetchSettings, *state.systemEnvironment->store, originalRef.input, useRegistries);

    auto subdir = fetchers::maybeGetStrAttr(cachedInput.extraAttrs, "dir").value_or(originalRef.subdir);
    auto resolvedRef = FlakeRef(std::move(cachedInput.resolvedInput), subdir);
    auto lockedRef = FlakeRef(std::move(cachedInput.lockedInput), subdir);

    // Parse/eval flake.nix to get at the input.self attributes.
    auto flake = readFlake(state, originalRef, resolvedRef, lockedRef, {cachedInput.accessor()}, lockRootAttrPath);

    // Re-fetch the tree if necessary.
    auto newLockedRef = applySelfAttrs(lockedRef, flake);

    if (lockedRef != newLockedRef) {
        debug("refetching input '%s' due to self attribute", newLockedRef);
        // FIXME: need to remove attrs that are invalidated by the changed input attrs, such as 'narHash'.
        newLockedRef.input.attrs.erase("narHash");
        auto cachedInput2 = state.inputCache->getAccessor(
            state.fetchSettings, *state.systemEnvironment->store, newLockedRef.input, fetchers::UseRegistries::No);
        cachedInput.accessor = cachedInput2.accessor;
        lockedRef = FlakeRef(std::move(cachedInput2.lockedInput), newLockedRef.subdir);
    }

    // Lazy paths: don't mount. flake.path is rooted at the fetcher's
    // accessor; readFlake reads flake.nix straight through that
    // accessor without copying the tree to the store. User code that
    // forces stringification (e.g. `"${self.outPath}"`) materializes
    // the storePath on demand via copyPathToStore. lockInput still
    // surfaces narHash so the lockfile sees this input as locked.
    state.lockInput(lockedRef.input, originalRef.input, cachedInput.accessor());
    SourcePath rootDir{cachedInput.accessor(), CanonPath::root};
    flake = readFlake(state, originalRef, resolvedRef, lockedRef, rootDir, lockRootAttrPath);
    flake.nodeLocation = NodeLocation{
        .tree =
            fetchers::MountableTree{
                .storePath = std::nullopt,
                .accessor = cachedInput.accessor,
            },
        .subdir = lockedRef.subdir,
    };
    return flake;
}

Flake getFlake(EvalState & state, const FlakeRef & originalRef, fetchers::UseRegistries useRegistries)
{
    return getFlake(state, originalRef, useRegistries, {});
}

static LockFile readLockFile(const fetchers::Settings & fetchSettings, const SourcePath & lockFilePath)
{
    return lockFilePath.pathExists() ? LockFile(fetchSettings, lockFilePath.readFile(), fmt("%s", lockFilePath))
                                     : LockFile();
}

LockedFlake lockFlake(
    const Settings & settings, EvalState & state, const FlakeRef & topRef, const LockFlags & lockFlags, Flake flake)
{
    experimentalFeatureSettings.require(Xp::Flakes);

    auto useRegistries = lockFlags.useRegistries.value_or(settings.useRegistries);
    auto useRegistriesTop = useRegistries ? fetchers::UseRegistries::All : fetchers::UseRegistries::No;
    auto useRegistriesInputs = useRegistries ? fetchers::UseRegistries::Limited : fetchers::UseRegistries::No;

    if (lockFlags.applyNixConfig) {
        flake.config.apply(settings);
        state.systemEnvironment->store->setOptions();
    }

    try {
        if (!state.fetchSettings.allowDirty && lockFlags.referenceLockFilePath) {
            throw Error("reference lock file was provided, but the `allow-dirty` setting is set to false");
        }

        auto oldLockFile =
            readLockFile(state.fetchSettings, lockFlags.referenceLockFilePath.value_or(flake.lockFilePath()));

        debug("old lock file: %s", oldLockFile);

        struct OverrideTarget
        {
            FlakeInput input;
            SourcePath sourcePath;
            std::optional<InputAttrPath> parentInputAttrPath; // FIXME: rename to inputAttrPathPrefix?
        };

        std::map<NonEmptyInputAttrPath, OverrideTarget> overrides;
        std::set<NonEmptyInputAttrPath> explicitCliOverrides;
        std::set<NonEmptyInputAttrPath> overridesUsed;
        std::set<InputAttrPath> updatesUsed;
        std::map<ref<Node>, NodeLocation> nodePaths;

        for (auto & i : lockFlags.inputOverrides) {
            overrides.emplace(
                i.first,
                OverrideTarget{
                    .input = FlakeInput{.ref = i.second},
                    /* Note: any relative overrides
                       (e.g. `--override-input B/C "path:./foo/bar"`)
                       are interpreted relative to the top-level
                       flake. */
                    .sourcePath = flake.path,
                });
            explicitCliOverrides.insert(i.first);
        }

        LockFile newLockFile;

        std::vector<FlakeRef> parents;

        std::function<void(
            const FlakeInputs & flakeInputs,
            ref<Node> node,
            const InputAttrPath & inputAttrPathPrefix,
            std::shared_ptr<const Node> oldNode,
            const InputAttrPath & followsPrefix,
            const SourcePath & sourcePath,
            bool trustLock)>
            computeLocks;

        computeLocks = [&](
                           /* The inputs of this node, either from flake.nix or
                              flake.lock. */
                           const FlakeInputs & flakeInputs,
                           /* The node whose locks are to be updated.*/
                           ref<Node> node,
                           /* The path to this node in the lock file graph. */
                           const InputAttrPath & inputAttrPathPrefix,
                           /* The old node, if any, from which locks can be
                              copied. */
                           std::shared_ptr<const Node> oldNode,
                           /* The prefix relative to which 'follows' should be
                              interpreted. When a node is initially locked, it's
                              relative to the node's flake; when it's already locked,
                              it's relative to the root of the lock file. */
                           const InputAttrPath & followsPrefix,
                           /* The source path of this node's flake. */
                           const SourcePath & sourcePath,
                           bool trustLock) {
            debug("computing lock file node '%s'", printInputAttrPath(inputAttrPathPrefix));

            /* Get the overrides (i.e. attributes of the form
               'inputs.nixops.inputs.nixpkgs.url = ...'). */
            auto addOverrides =
                [&](this const auto & addOverrides, const FlakeInput & input, const InputAttrPath & prefix) -> void {
                for (auto & [idOverride, inputOverride] : input.overrides) {
                    auto inputAttrPath = NonEmptyInputAttrPath::append(prefix, idOverride);
                    if (inputOverride.ref || inputOverride.follows)
                        overrides.emplace(
                            inputAttrPath,
                            OverrideTarget{
                                .input = inputOverride,
                                .sourcePath = sourcePath,
                                .parentInputAttrPath = inputAttrPathPrefix});
                    addOverrides(inputOverride, inputAttrPath);
                }
            };

            for (auto & [id, input] : flakeInputs) {
                auto inputAttrPath(inputAttrPathPrefix);
                inputAttrPath.push_back(id);
                addOverrides(input, inputAttrPath);
            }

            /* Check whether this input has overrides for a
               non-existent input. */
            for (auto [inputAttrPath, inputOverride] : overrides) {
                auto follow = inputAttrPath.inputName();
                auto inputAttrPath2 = inputAttrPath.parent();
                if (inputAttrPath2 == inputAttrPathPrefix && !flakeInputs.count(follow))
                    warn(
                        "input '%s' has an override for a non-existent input '%s'",
                        printInputAttrPath(inputAttrPathPrefix),
                        follow);
            }

            /* Go over the flake inputs, resolve/fetch them if
               necessary (i.e. if they're new or the flakeref changed
               from what's in the lock file). */
            for (auto & [id, input2] : flakeInputs) {
                auto nonEmptyInputAttrPath = NonEmptyInputAttrPath::append(inputAttrPathPrefix, id);
                auto inputAttrPath = nonEmptyInputAttrPath.get();
                auto inputAttrPathS = printInputAttrPath(inputAttrPath);
                debug("computing input '%s'", inputAttrPathS);

                try {

                    /* Do we have an override for this input from one of the
                       ancestors? */
                    auto i = overrides.find(nonEmptyInputAttrPath);
                    bool hasOverride = i != overrides.end();
                    bool hasCliOverride = explicitCliOverrides.contains(nonEmptyInputAttrPath);
                    if (hasOverride)
                        overridesUsed.insert(nonEmptyInputAttrPath);
                    auto input = hasOverride ? i->second.input : input2;

                    /* Resolve relative 'path:' inputs relative to
                       the source path of the overrider. */
                    auto overriddenSourcePath = hasOverride ? i->second.sourcePath : sourcePath;

                    /* Respect the "flakeness" of the input even if we
                       override it. */
                    if (hasOverride)
                        input.isFlake = input2.isFlake;

                    /* Resolve 'follows' later (since it may refer to an input
                       path we haven't processed yet. */
                    if (input.follows) {
                        InputAttrPath target;

                        target.insert(target.end(), input.follows->begin(), input.follows->end());

                        debug("input '%s' follows '%s'", inputAttrPathS, printInputAttrPath(target));
                        node->inputs.insert_or_assign(id, target);
                        continue;
                    }

                    if (!input.ref)
                        input.ref =
                            FlakeRef::fromAttrs(state.fetchSettings, {{"type", "indirect"}, {"id", std::string(id)}});

                    auto overriddenParentPath =
                        input.ref->input.isRelative()
                            ? std::optional<InputAttrPath>(
                                  hasOverride ? i->second.parentInputAttrPath : inputAttrPathPrefix)
                            : std::nullopt;

                    auto resolveRelativePath = [&]() -> std::optional<SourcePath> {
                        if (auto relativePath = input.ref->input.isRelative()) {
                            /* Part 3: route the relative-input join
                               through the kind-aware `resolveSymlinks`
                               wrapper, then catch the generic
                               `AccessorBoundaryEscape` and rewrap with a
                               flake-input-specific diagnostic.

                               The parent flake's accessor is by
                               construction a fetched-tree view
                               (Copyable). Wrap it ad-hoc so the wrapper
                               applies `StrictAccessorBoundary`. The
                               check fires on both the literal `..`
                               escape (`path:../foo` against a flake at
                               tree root) and the symlink-target escape
                               (where an ancestor's symlink target pops
                               past root). */
                            auto relStr = relativePath->string();
                            auto parent = overriddenSourcePath.path.parent();
                            assert(parent);
                            auto resolved = joinAndCheckCopyable(
                                state.getOrCreateRoot(
                                    overriddenSourcePath.accessor,
                                    SourceRootKind::Copyable,
                                    input.ref ? std::optional{input.ref->input.toUnpinnedURL()} : std::nullopt),
                                *parent,
                                relStr,
                                SymlinkResolution::Ancestors,
                                [&]() {
                                    return Error(
                                        "relative flake input path '%s' escapes the source tree at %s",
                                        relStr,
                                        overriddenSourcePath.accessor->showPath(CanonPath::root));
                                });
                            return SourcePath{overriddenSourcePath.accessor, resolved};
                        } else
                            return std::nullopt;
                    };

                    /* Get the input flake, resolve 'path:./...'
                       flakerefs relative to the parent flake. */
                    auto getInputFlake = [&](const FlakeRef & ref, const fetchers::UseRegistries useRegistries) {
                        if (auto resolvedPath = resolveRelativePath()) {
                            auto flake = readFlake(state, ref, ref, ref, *resolvedPath, inputAttrPath);
                            /* Compose the in-tree subdir from the
                               post-walk `resolvedPath` (already
                               escape-checked by `resolveRelativePath`)
                               plus this input's parsed `ref.subdir`,
                               routed through the kind-aware wrapper —
                               `path:./sub?dir=../escape` on a
                               relative input would otherwise still
                               escape via `CanonPath`'s silent clamp.
                               Catch and rewrap with the flake-input-
                               specific diagnostic, matching the
                               `readFlake` site. */
                            auto & parentLoc = nodePaths.at(node);
                            auto fullSubdir = joinAndCheckCopyable(
                                state.getOrCreateRoot(
                                    overriddenSourcePath.accessor,
                                    SourceRootKind::Copyable,
                                    input.ref ? std::optional{input.ref->input.toUnpinnedURL()} : std::nullopt),
                                resolvedPath->path,
                                ref.subdir,
                                SymlinkResolution::Ancestors,
                                [&]() {
                                    return Error(
                                        "flake input subdir '%s' escapes the source tree at %s",
                                        ref.subdir,
                                        overriddenSourcePath.accessor->showPath(CanonPath::root));
                                });
                            flake.nodeLocation = NodeLocation{
                                .tree = parentLoc.tree,
                                .subdir = std::string{fullSubdir.rel()},
                            };
                            return flake;
                        } else {
                            return getFlake(state, ref, useRegistries, inputAttrPath);
                        }
                    };

                    /* Do we have an entry in the existing lock file?
                       And the input is not in updateInputs? */
                    std::shared_ptr<LockedNode> oldLock;

                    updatesUsed.insert(inputAttrPath);

                    if (oldNode && !lockFlags.inputUpdates.count(nonEmptyInputAttrPath))
                        if (auto oldLock2 = get(oldNode->inputs, id))
                            if (auto oldLock3 = std::get_if<0>(&*oldLock2))
                                oldLock = *oldLock3;

                    if (oldLock && oldLock->originalRef.canonicalize() == input.ref->canonicalize()
                        && oldLock->parentInputAttrPath == overriddenParentPath && !hasCliOverride) {
                        debug("keeping existing input '%s'", inputAttrPathS);

                        /* Copy the input from the old lock since its flakeref
                           didn't change and there is no override from a
                           higher level flake. */
                        auto childNode = make_ref<LockedNode>(
                            oldLock->lockedRef, oldLock->originalRef, oldLock->isFlake, oldLock->parentInputAttrPath);

                        node->inputs.insert_or_assign(id, childNode);

                        /* When the parent flake declared `inputs.<name>.copyToStore = true`
                           on this input, populate `nodePaths` even on a cache-hit lock so
                           `callFlake`'s loop sees the input and can emit the
                           System-rooted `outPath`. The accessor comes from the input
                           cache — same fingerprint, no network refetch. Other inputs
                           keep their pre-existing fast path (no `nodePaths` entry; their
                           outPath flows through `fetchTreeFinal` in `call-flake.nix`). */
                        if (input.copyToStore) {
                            auto cachedInput = state.inputCache->getAccessor(
                                state.fetchSettings,
                                *state.systemEnvironment->store,
                                oldLock->lockedRef.input,
                                useRegistriesInputs);
                            nodePaths.emplace(
                                childNode,
                                NodeLocation{
                                    .tree =
                                        fetchers::MountableTree{
                                            .storePath = std::nullopt,
                                            .accessor = cachedInput.accessor,
                                        },
                                    .subdir = oldLock->lockedRef.subdir,
                                });
                        }

                        /* If we have this input in updateInputs, then we
                           must fetch the flake to update it. */
                        auto lb = lockFlags.inputUpdates.lower_bound(nonEmptyInputAttrPath);

                        auto mustRefetch = lb != lockFlags.inputUpdates.end() && lb->get().size() > inputAttrPath.size()
                                           && std::equal(inputAttrPath.begin(), inputAttrPath.end(), lb->get().begin());

                        FlakeInputs fakeInputs;

                        if (!mustRefetch) {
                            /* No need to fetch this flake, we can be
                               lazy. However there may be new overrides on the
                               inputs of this flake, so we need to check
                               those. */
                            for (auto & i : oldLock->inputs) {
                                if (auto lockedNode = std::get_if<0>(&i.second)) {
                                    fakeInputs.emplace(
                                        i.first,
                                        FlakeInput{
                                            .ref = (*lockedNode)->originalRef,
                                            .isFlake = (*lockedNode)->isFlake,
                                        });
                                } else if (auto follows = std::get_if<1>(&i.second)) {
                                    if (!trustLock) {
                                        // It is possible that the flake has changed,
                                        // so we must confirm all the follows that are in the lock file are also in the
                                        // flake.
                                        auto overridePath =
                                            NonEmptyInputAttrPath::append(nonEmptyInputAttrPath, i.first);
                                        auto o = overrides.find(overridePath);
                                        // If the override disappeared, we have to refetch the flake,
                                        // since some of the inputs may not be present in the lock file.
                                        if (o == overrides.end()) {
                                            mustRefetch = true;
                                            // There's no point populating the rest of the fake inputs,
                                            // since we'll refetch the flake anyways.
                                            break;
                                        }
                                    }
                                    auto absoluteFollows(followsPrefix);
                                    absoluteFollows.insert(absoluteFollows.end(), follows->begin(), follows->end());
                                    fakeInputs.emplace(
                                        i.first,
                                        FlakeInput{
                                            .follows = absoluteFollows,
                                        });
                                }
                            }
                        }

                        if (mustRefetch) {
                            auto inputFlake = getInputFlake(oldLock->lockedRef, useRegistriesInputs);
                            nodePaths.emplace(childNode, inputFlake.nodeLocation.value());
                            computeLocks(
                                inputFlake.inputs,
                                childNode,
                                inputAttrPath,
                                oldLock,
                                followsPrefix,
                                inputFlake.path,
                                false);
                        } else {
                            computeLocks(
                                fakeInputs, childNode, inputAttrPath, oldLock, followsPrefix, sourcePath, true);
                        }

                    } else {
                        /* We need to create a new lock file entry. So fetch
                           this input. */
                        debug("creating new input '%s'", inputAttrPathS);

                        if (!lockFlags.allowUnlocked && !input.ref->input.isLocked(state.fetchSettings)
                            && !input.ref->input.isRelative())
                            throw Error("cannot update unlocked flake input '%s' in pure mode", inputAttrPathS);

                        /* Note: in case of an --override-input, we use
                            the *original* ref (input2.ref) for the
                            "original" field, rather than the
                            override. This ensures that the override isn't
                            nuked the next time we update the lock
                            file. That is, overrides are sticky unless you
                            use --no-write-lock-file. */
                        auto inputIsOverride = explicitCliOverrides.contains(nonEmptyInputAttrPath);
                        auto ref = (input2.ref && inputIsOverride) ? *input2.ref : *input.ref;

                        if (input.isFlake) {
                            auto inputFlake = getInputFlake(
                                *input.ref, inputIsOverride ? fetchers::UseRegistries::All : useRegistriesInputs);

                            auto childNode =
                                make_ref<LockedNode>(inputFlake.lockedRef, ref, true, overriddenParentPath);

                            node->inputs.insert_or_assign(id, childNode);

                            /* Guard against circular flake imports. */
                            for (auto & parent : parents)
                                if (parent == *input.ref)
                                    throw Error("found circular import of flake '%s'", parent);
                            parents.push_back(*input.ref);
                            Finally cleanup([&]() { parents.pop_back(); });

                            /* Recursively process the inputs of this
                               flake, using its own lock file. */
                            nodePaths.emplace(childNode, inputFlake.nodeLocation.value());
                            computeLocks(
                                inputFlake.inputs,
                                childNode,
                                inputAttrPath,
                                readLockFile(state.fetchSettings, inputFlake.lockFilePath()).root.get_ptr(),
                                inputAttrPath,
                                inputFlake.path,
                                false);
                        }

                        else {
                            auto [loc, lockedRef] = [&]() -> std::tuple<NodeLocation, FlakeRef> {
                                // Handle non-flake 'path:./...' inputs.
                                if (auto resolvedPath = resolveRelativePath()) {
                                    /* Same shape as getInputFlake's
                                       relative branch above: walk
                                       `input.ref->subdir` on top of
                                       the already-escape-checked
                                       `resolvedPath` via the kind-
                                       aware wrapper, catch
                                       AccessorBoundaryEscape, rewrap
                                       with the flake-input-specific
                                       diagnostic. */
                                    auto & parentLoc = nodePaths.at(node);
                                    auto fullSubdir = joinAndCheckCopyable(
                                        state.getOrCreateRoot(
                                            overriddenSourcePath.accessor,
                                            SourceRootKind::Copyable,
                                            input.ref ? std::optional{input.ref->input.toUnpinnedURL()} : std::nullopt),
                                        resolvedPath->path,
                                        input.ref->subdir,
                                        SymlinkResolution::Ancestors,
                                        [&]() {
                                            return Error(
                                                "flake input subdir '%s' escapes the source tree at %s",
                                                input.ref->subdir,
                                                overriddenSourcePath.accessor->showPath(CanonPath::root));
                                        });
                                    return {
                                        NodeLocation{
                                            .tree = parentLoc.tree,
                                            .subdir = std::string{fullSubdir.rel()},
                                        },
                                        *input.ref};
                                } else {
                                    auto cachedInput = state.inputCache->getAccessor(
                                        state.fetchSettings,
                                        *state.systemEnvironment->store,
                                        input.ref->input,
                                        useRegistriesInputs);

                                    auto lockedRef = FlakeRef(std::move(cachedInput.lockedInput), input.ref->subdir);

                                    // See the matching comment in getFlake: no
                                    // mount, the storePath is left nullopt and
                                    // the accessor thunk carries through.
                                    state.lockInput(lockedRef.input, input.ref->input, cachedInput.accessor());

                                    return {
                                        NodeLocation{
                                            .tree =
                                                fetchers::MountableTree{
                                                    .storePath = std::nullopt,
                                                    .accessor = cachedInput.accessor,
                                                },
                                            .subdir = lockedRef.subdir,
                                        },
                                        lockedRef};
                                }
                            }();

                            auto childNode = make_ref<LockedNode>(lockedRef, ref, false, overriddenParentPath);

                            nodePaths.emplace(childNode, loc);

                            node->inputs.insert_or_assign(id, childNode);
                        }
                    }

                } catch (Error & e) {
                    e.addTrace({}, "while updating the flake input '%s'", inputAttrPathS);
                    throw;
                }
            }
        };

        nodePaths.emplace(newLockFile.root, flake.nodeLocation.value());

        computeLocks(
            flake.inputs,
            newLockFile.root,
            {},
            lockFlags.recreateLockFile ? nullptr : oldLockFile.root.get_ptr(),
            {},
            flake.path,
            false);

        for (auto & i : lockFlags.inputOverrides)
            if (!overridesUsed.count(i.first))
                warn(
                    "the flag '--override-input %s %s' does not match any input",
                    printInputAttrPath(i.first),
                    i.second);

        for (auto & i : lockFlags.inputUpdates)
            if (!updatesUsed.count(i))
                warn("'%s' does not match any input of this flake", printInputAttrPath(i));

        /* Check 'follows' inputs. */
        newLockFile.check();

        debug("new lock file: %s", newLockFile);

        auto sourcePath = topRef.input.getSourcePath();

        /* Check whether we need to / can write the new lock file. */
        if (newLockFile != oldLockFile || lockFlags.outputLockFilePath) {

            auto diff = LockFile::diff(oldLockFile, newLockFile);

            if (lockFlags.writeLockFile) {
                if (sourcePath || lockFlags.outputLockFilePath) {
                    if (auto unlockedInput = newLockFile.isUnlocked(state.fetchSettings)) {
                        if (lockFlags.failOnUnlocked)
                            throw Error(
                                "Not writing lock file of flake '%s' because it has an unlocked input ('%s'). "
                                "Use '--allow-dirty-locks' to allow this anyway.",
                                topRef,
                                *unlockedInput);
                        if (state.fetchSettings.warnDirty)
                            warn(
                                "not writing lock file of flake '%s' because it has an unlocked input ('%s')",
                                topRef,
                                *unlockedInput);
                    } else {
                        if (!lockFlags.updateLockFile)
                            throw Error(
                                "flake '%s' requires lock file changes but they're not allowed due to '--no-update-lock-file'",
                                topRef);

                        auto newLockFileS = fmt("%s\n", newLockFile);

                        if (lockFlags.outputLockFilePath) {
                            if (lockFlags.commitLockFile)
                                throw Error("'--commit-lock-file' and '--output-lock-file' are incompatible");
                            writeFile(*lockFlags.outputLockFilePath, newLockFileS);
                        } else {
                            auto relPath = (topRef.subdir == "" ? "" : topRef.subdir + "/") + "flake.lock";
                            auto outputLockFilePath = *sourcePath / relPath;

                            bool lockFileExists = pathExists(outputLockFilePath);

                            auto s = chomp(diff);
                            if (lockFileExists) {
                                if (s.empty())
                                    warn("updating lock file %s", PathFmt(outputLockFilePath));
                                else
                                    warn("updating lock file %s:\n%s", PathFmt(outputLockFilePath), s);
                            } else
                                warn("creating lock file %s: \n%s", PathFmt(outputLockFilePath), s);

                            std::optional<std::string> commitMessage = std::nullopt;

                            if (lockFlags.commitLockFile) {
                                std::string cm;

                                cm = settings.commitLockFileSummary.get();

                                if (cm == "") {
                                    cm = fmt("%s: %s", relPath, lockFileExists ? "Update" : "Add");
                                }

                                cm += "\n\nFlake lock file updates:\n\n";
                                cm += filterANSIEscapes(diff, true);
                                commitMessage = cm;
                            }

                            topRef.input.putFile(
                                CanonPath((topRef.subdir == "" ? "" : topRef.subdir + "/") + "flake.lock"),
                                newLockFileS,
                                commitMessage);
                        }

                        /* Rewriting the lockfile changed the top-level
                           repo, so we should re-read it. FIXME: we could
                           also just clear the 'rev' field... */
                        auto prevLockedRef = flake.lockedRef;
                        flake = getFlake(state, topRef, useRegistriesTop);

                        if (lockFlags.commitLockFile && flake.lockedRef.input.getRev()
                            && prevLockedRef.input.getRev() != flake.lockedRef.input.getRev())
                            warn("committed new revision '%s'", flake.lockedRef.input.getRev()->gitRev());
                    }
                } else
                    throw Error(
                        "cannot write modified lock file of flake '%s' (use '--no-write-lock-file' to ignore)", topRef);
            } else {
                warn("not writing modified lock file of flake '%s':\n%s", topRef, chomp(diff));
                flake.forceDirty = true;
            }
        }

        return LockedFlake{
            .flake = std::move(flake), .lockFile = std::move(newLockFile), .nodePaths = std::move(nodePaths)};

    } catch (Error & e) {
        e.addTrace({}, "while updating the lock file of flake '%s'", flake.lockedRef.to_string());
        throw;
    }
}

LockedFlake
lockFlake(const Settings & settings, EvalState & state, const FlakeRef & topRef, const LockFlags & lockFlags)
{
    auto useRegistries = lockFlags.useRegistries.value_or(settings.useRegistries);
    auto useRegistriesTop = useRegistries ? fetchers::UseRegistries::All : fetchers::UseRegistries::No;
    return lockFlake(settings, state, topRef, lockFlags, getFlake(state, topRef, useRegistriesTop, {}));
}

LockedFlake lockFlake(
    const Settings & settings,
    EvalState & state,
    const SourcePath & flakeDir,
    NodeLocation location,
    const LockFlags & lockFlags)
{
    /* We need a fake flakeref to put in the `Flake` struct, but it's not used for anything. */
    auto fakeRef = parseFlakeRef(state.fetchSettings, "flake:get-flake");
    auto flake = readFlake(state, fakeRef, fakeRef, fakeRef, flakeDir, {});
    flake.nodeLocation = std::move(location);
    return lockFlake(settings, state, fakeRef, lockFlags, std::move(flake));
}

static ref<SourceAccessor> makeInternalFS()
{
    auto internalFS = make_ref<MemorySourceAccessor>(MemorySourceAccessor{});
    internalFS->setPathDisplay("«flakes-internal»", "");
    internalFS->addFile(
        CanonPath("call-flake.nix"),
#include "call-flake.nix.gen.hh" // IWYU pragma: keep
    );
    return internalFS;
}

static auto internalFS = makeInternalFS();
/* The flake-internal accessor is admitted as Internal — its files
   (currently `call-flake.nix`) implement the call-flake machinery
   and aren't meant to surface as user-visible path values. Named
   distinctly from `EvalState::internalFSRoot` (which wraps the
   *evaluator*-internal MemorySourceAccessor for
   `derivation-internal.nix` and `imported-drv-to-derivation.nix`)
   because the two accessors live at different scopes (process-
   static vs per-EvalState) and host different files. */
static auto callFlakeInternalRoot = SourceRoot::make(internalFS, SourceRootKind::Internal);

static Value * requireInternalFile(EvalState & state, CanonPath path)
{
    RootedPath p{callFlakeInternalRoot, path};
    auto v = state.allocValue();
    state.evalFile(p, *v); // has caching
    return v;
}

void callFlake(EvalState & state, const LockedFlake & lockedFlake, Value & vRes)
{
    experimentalFeatureSettings.require(Xp::Flakes);

    auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();

    auto overrides = state.buildBindings(lockedFlake.nodePaths.size());

    for (auto & [node, info] : lockedFlake.nodePaths) {
        auto override = state.buildBindings(3);

        auto & vSourceInfo = override.alloc(state.symbols.create("sourceInfo"));

        auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();

        const auto & lockedRef = lockedNode ? lockedNode->lockedRef : lockedFlake.flake.lockedRef;

        auto key = keyMap.find(node);
        assert(key != keyMap.end());

        /* `inputs.<name>.copyToStore = true` opts a root input back
           into pre-lazy-paths shape: `outPath` is a System-rooted
           path Value with the materialised storepath as its
           CanonPath. Only root inputs honour this — transitive
           children inherit their own flake's config. The lookup
           matches the lockfile key directly because root keys carry
           no path prefix. */
        bool copyToStoreOutPath = false;
        if (auto inputIt = lockedFlake.flake.inputs.find(key->second);
            inputIt != lockedFlake.flake.inputs.end() && inputIt->second.copyToStore)
            copyToStoreOutPath = true;
        /* `inputs.self.copyToStore = true` opts the root flake itself
           into the eager System-rooted shape, mirroring how
           `inputs.<name>.copyToStore` does it for inputs. The flag
           lives on `flake.selfAttrs` (parsed by `parseFlakeInputs`
           and survived `applySelfAttrs`). */
        if (node == ref<Node>(lockedFlake.lockFile.root)) {
            if (auto it = lockedFlake.flake.selfAttrs.find("copyToStore"); it != lockedFlake.flake.selfAttrs.end())
                if (auto b = std::get_if<Explicit<bool>>(&it->second); b && b->t)
                    copyToStoreOutPath = true;
        }

        /* `lazy` follows whether we know the storePath: nodePaths
           entries from getFlake/computeLocks went through lockInput
           (no mountInput, no storePath; the accessor is the fetcher's
           and rooted at the tree), so emit renders `outPath` as a
           path Value. Entries from `lockFlake(SourcePath)` come with
           a known storePath and an accessor rooted at rootFS — those
           must render eagerly because the accessor isn't scoped to
           the tree. */
        bool lazy = !info.tree.storePath.has_value();

        /* For copyToStore, we need the storePath up front. If the
           input came through getFlake (lazy + no storePath),
           materialise it now via `copyPathToStore` on the
           accessor's root — that's the same machinery `toString`
           uses, with the `srcToStore` cache making it
           single-walk. Use a throwaway context so the storepath
           doesn't leak into a `NixStringContext` we don't own. */
        std::optional<fetchers::MountableTree> eagerTree;
        if (copyToStoreOutPath && !info.tree.storePath.has_value()) {
            NixStringContext discardContext;
            auto storePath = state.copyPathToStore(
                discardContext,
                RootedPath{
                    state.getOrCreateRoot(
                        info.tree.accessor(), SourceRootKind::Copyable, lockedRef.input.toUnpinnedURL()),
                    CanonPath::root});
            eagerTree = fetchers::MountableTree{.storePath = storePath, .accessor = info.tree.accessor};
            lazy = false;
        }

        emitTreeAttrs(
            state,
            eagerTree ? *eagerTree : info.tree,
            lockedRef.input,
            vSourceInfo,
            /*emptyRevFallback=*/false,
            /*forceDirty=*/!lockedNode && lockedFlake.flake.forceDirty,
            lazy,
            copyToStoreOutPath);

        override.alloc(state.symbols.create("dir")).mkString(info.subdir, state.mem);

        /* Signal to `call-flake.nix` whether to keep `outPath` as a
           path Value (true) or stringify it via `"${...}"` (the
           default, for back-compat with the lazy / string-with-context
           shapes the existing consumers expect). */
        override.alloc(state.symbols.create("copyToStore")).mkBool(copyToStoreOutPath);

        overrides.alloc(state.symbols.create(key->second)).mkAttrs(override);
    }

    auto & vOverrides = state.allocValue()->mkAttrs(overrides);

    Value * vCallFlake = requireInternalFile(state, CanonPath("call-flake.nix"));

    auto vLocks = state.allocValue();
    vLocks->mkString(lockFileStr, state.mem);

    auto vFetchFinalTree = get(state.internalPrimOps, "fetchFinalTree");
    assert(vFetchFinalTree);

    Value * args[] = {vLocks, &vOverrides, *vFetchFinalTree};
    state.callFunction(*vCallFlake, args, vRes, noPos);
}

/* Mirror `addFetchTreeMetadataAttrs` at the Object level.

   LazyAttrs (the shape `lockInput` installs for narHash and revCount
   on freshly-fingerprinted inputs) are *forced* here rather than
   emitted as a Nix-side thunk. Rationale: we don't have an
   `Evaluator`-interface thunk primitive, and on warm runs the
   `sourcePathToHash`/`fetchToStore2` caches make the force cheap.
   Lockfile-resident inputs come with concrete narHash/revCount on
   `input.attrs` and short-circuit before any walk. */
static void addFetchTreeMetadataAttrsViaEvaluator(
    Evaluator & evaluator,
    EvalState & state,
    const fetchers::Input & input,
    bool emptyRevFallback,
    bool forceDirty,
    std::map<std::string, ref<Object>> & attrs)
{
    auto resolveLazyString = [](const fetchers::LazyAttr & l) -> std::optional<std::string> {
        auto v = l->compute();
        if (auto * s = std::get_if<std::string>(&v))
            return *s;
        return {};
    };
    auto resolveLazyInt = [](const fetchers::LazyAttr & l) -> std::optional<uint64_t> {
        auto v = l->compute();
        if (auto * i = std::get_if<uint64_t>(&v))
            return *i;
        return {};
    };

    if (auto narHashLazy = fetchers::maybeGetLazyAttr(input.attrs, "narHash")) {
        if (auto s = resolveLazyString(*narHashLazy))
            attrs.emplace("narHash", evaluator.mkString(*s));
    } else if (auto narHash = input.getNarHash()) {
        attrs.emplace("narHash", evaluator.mkString(narHash->to_string(HashFormat::SRI, true)));
    }

    if (input.getType() == "git")
        attrs.emplace(
            "submodules", evaluator.mkBool(fetchers::maybeGetBoolAttr(input.attrs, "submodules").value_or(false)));

    if (!forceDirty) {
        if (auto rev = input.getRev()) {
            attrs.emplace("rev", evaluator.mkString(rev->gitRev()));
            attrs.emplace("shortRev", evaluator.mkString(rev->gitShortRev()));
        } else if (emptyRevFallback) {
            auto emptyHash = Hash(HashAlgorithm::SHA1);
            attrs.emplace("rev", evaluator.mkString(emptyHash.gitRev()));
            attrs.emplace("shortRev", evaluator.mkString(emptyHash.gitShortRev()));
        }

        if (auto revCountLazy = fetchers::maybeGetLazyAttr(input.attrs, "revCount")) {
            if (auto n = resolveLazyInt(*revCountLazy))
                attrs.emplace("revCount", evaluator.mkInt(NixInt(int64_t(*n))));
        } else if (auto revCount = input.getRevCount()) {
            attrs.emplace("revCount", evaluator.mkInt(NixInt(int64_t(*revCount))));
        } else if (emptyRevFallback) {
            attrs.emplace("revCount", evaluator.mkInt(NixInt(0)));
        }
    }

    if (auto dirtyRev = fetchers::maybeGetStrAttr(input.attrs, "dirtyRev")) {
        attrs.emplace("dirtyRev", evaluator.mkString(*dirtyRev));
        attrs.emplace("dirtyShortRev", evaluator.mkString(*fetchers::maybeGetStrAttr(input.attrs, "dirtyShortRev")));
    }

    if (auto lastModified = input.getLastModified()) {
        attrs.emplace("lastModified", evaluator.mkInt(NixInt(int64_t(*lastModified))));
        attrs.emplace(
            "lastModifiedDate",
            evaluator.mkString(fmt("%s", std::put_time(std::gmtime(&*lastModified), "%Y%m%d%H%M%S"))));
    }
    (void) state;
}

/* Mirror `emitTreeAttrs(..., lazy=true)` at the Object level. */
static ref<Object> emitTreeAttrsViaEvaluator(
    Evaluator & evaluator,
    EvalState & state,
    const fetchers::MountableTree & tree,
    const fetchers::Input & input,
    bool emptyRevFallback,
    bool forceDirty)
{
    std::map<std::string, ref<Object>> attrs;
    /* `lazy = true`: outPath is rooted at the fetcher's accessor with
       the input's unpinned URL as identity, exactly like
       `emitTreeAttrs(lazy=true)`. No mountInput, no store walk. */
    attrs.emplace(
        "outPath",
        evaluator.mkPath(
            RootedPath{
                state.getOrCreateRoot(tree.accessor(), SourceRootKind::Copyable, input.toUnpinnedURL()),
                CanonPath::root}));
    addFetchTreeMetadataAttrsViaEvaluator(evaluator, state, input, emptyRevFallback, forceDirty, attrs);
    return evaluator.mkAttrs(attrs);
}

ref<Object> callFlakeViaEvaluator(Evaluator & evaluator, EvalState & state, const LockedFlake & lockedFlake)
{
    experimentalFeatureSettings.require(Xp::Flakes);

    auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();

    std::map<std::string, ref<Object>> overrideAttrs;
    for (auto & [node, info] : lockedFlake.nodePaths) {
        auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();
        const auto & lockedRef = lockedNode ? lockedNode->lockedRef : lockedFlake.flake.lockedRef;
        /* As in `callFlake`: `lazy` mirrors whether `mountInput` ran;
           with lazy-paths that's tied to whether we know the
           storePath. We're using `mkPath` keyed on unpinned URL
           either way — the Object-level call doesn't itself care
           about lazy vs eager, but we keep the semantics aligned with
           the Value-level path so the produced trees are
           interchangeable on a cache fall-through. */
        auto sourceInfo = emitTreeAttrsViaEvaluator(
            evaluator,
            state,
            info.tree,
            lockedRef.input,
            /*emptyRevFallback=*/false,
            /*forceDirty=*/!lockedNode && lockedFlake.flake.forceDirty);

        auto override = evaluator.mkAttrs({{"sourceInfo", sourceInfo}, {"dir", evaluator.mkString(info.subdir)}});

        auto key = keyMap.find(node);
        assert(key != keyMap.end());
        overrideAttrs.emplace(key->second, override);
    }

    auto vOverrides = evaluator.mkAttrs(overrideAttrs);

    auto vCallFlake = evaluator.evalFile(
        RootedPath{callFlakeInternalRoot, CanonPath("call-flake.nix")}, "«flakes-internal»/call-flake.nix");

    auto vLocks = evaluator.mkString(lockFileStr);

    auto vFetchFinalTree = evaluator.getInternalPrimOp("fetchFinalTree");

    return evaluator.apply(evaluator.apply(evaluator.apply(vCallFlake, vLocks), vOverrides), vFetchFinalTree);
}

std::optional<Fingerprint> LockedFlake::getFingerprint(Store & store, const fetchers::Settings & fetchSettings) const
{
    if (lockFile.isUnlocked(fetchSettings))
        return std::nullopt;

    auto fingerprint = flake.lockedRef.input.getFingerprint(store);
    if (!fingerprint)
        return std::nullopt;

    *fingerprint += fmt(";%s;%s", flake.lockedRef.subdir, lockFile);

    if (auto revCount = get(flake.lockedRef.input.attrs, "revCount")) {
        if (std::get_if<fetchers::LazyAttr>(revCount)) {
            /* A lazy revCount is computed by the fetcher, so its
               value is functionally determined by `rev`. We only
               need to record its presence, not force its value.

               This means a lazy and a concrete revCount that would
               resolve to the same value produce different
               fingerprints, sacrificing some cache hits to avoid
               the cost of forcing. */
            *fingerprint += ";hasRevCount";
        } else if (auto n = flake.lockedRef.input.getRevCount()) {
            /* A concrete revCount comes from a lockfile or explicit
               user input. The fetcher passes it through as-is, so
               it can affect evaluation and must be fingerprinted. */
            *fingerprint += fmt(";revCount=%d", *n);
        }
    }
    if (auto lastModified = flake.lockedRef.input.getLastModified())
        *fingerprint += fmt(";lastModified=%d", *lastModified);

    // FIXME: as an optimization, if the flake contains a lock file
    // and we haven't changed it, then it's sufficient to use
    // flake.sourceInfo.storePath for the fingerprint.
    return hashString(HashAlgorithm::SHA256, *fingerprint);
}

Flake::~Flake() {}

ref<eval_cache::EvalCache> openEvalCache(EvalState & state, ref<const LockedFlake> lockedFlake)
{
    if (state.settings.useEvalCache && state.settings.pureEval
        && state.fetchSettings.lintFetchWholeSourceToStore != Diagnose::Ignore)
        throw UsageError(
            "`lint-fetch-whole-source-to-store` needs `--no-eval-cache` (or `eval-cache = false`): "
            "the eval cache reads the whole flake source to form its key, and warm hits skip "
            "evaluation entirely.");

    auto fingerprint = state.settings.useEvalCache && state.settings.pureEval
                           ? lockedFlake->getFingerprint(*state.systemEnvironment->store, state.fetchSettings)
                           : std::nullopt;
    auto rootLoader = [&state, lockedFlake]() {
        /* For testing whether the evaluation cache is
           complete. */
        if (getEnv("NIX_ALLOW_EVAL").value_or("1") == "0")
            throw Error("not everything is cached, but evaluation is not allowed");

        auto vFlake = state.allocValue();
        callFlake(state, *lockedFlake, *vFlake);

        state.forceAttrs(*vFlake, noPos, "while parsing cached flake data");

        auto aOutputs = vFlake->attrs()->get(state.symbols.create("outputs"));
        assert(aOutputs);

        return aOutputs->value;
    };

    if (fingerprint) {
        auto search = state.evalCaches.find(fingerprint.value());
        if (search == state.evalCaches.end()) {
            search = state.evalCaches
                         .emplace(fingerprint.value(), make_ref<eval_cache::EvalCache>(fingerprint, state, rootLoader))
                         .first;
        }
        return search->second;
    } else {
        return make_ref<eval_cache::EvalCache>(std::nullopt, state, rootLoader);
    }
}

} // namespace flake

} // namespace nix
