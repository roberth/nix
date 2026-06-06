#include <stdint.h>
#include <map>
#include <optional>
#include <string>
#include <variant>

#include "nix/flake/flake-primops.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/source-root.hh"
#include "nix/flake/flake.hh"
#include "nix/flake/flakeref.hh"
#include "nix/flake/settings.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/util/error.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/pos-table.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"
#include "nix/store/store-api.hh"

namespace nix::flake::primops {

PrimOp getFlake(const Settings & settings)
{
    auto prim_getFlake = [&settings](EvalState & state, const PosIdx pos, Value ** args, Value & v) {
        state.forceValue(*args[0], pos);

        LockFlags lockFlags{
            .updateLockFile = false,
            .writeLockFile = false,
            .useRegistries = !state.settings.pureEval && settings.useRegistries,
            .allowUnlocked = !state.settings.pureEval,
        };

        /* Lock + evaluate via a FlakeRef. Used by the string form and
           by the System branch of the path-value form (the latter
           builds the FlakeRef from attrs directly, no string round-
           trip). The pure-eval lock check applies uniformly here. */
        auto lockByFlakeRef = [&](nix::FlakeRef flakeRef, const std::string & flakeRefS) {
            if (state.settings.pureEval && !flakeRef.input.isLocked(state.fetchSettings))
                throw Error(
                    "cannot call 'getFlake' on unlocked flake reference '%s', at %s (use --impure to override)",
                    flakeRefS,
                    state.positions[pos]);

            /* Backwards compatibility: since flakes used to be copied to the store eagerly, some users
               relied on being able to do builtins.getFlake on a flakeref with discarded string context.
               So if a flake input has a physical source path that is inside the store, first try to look it up in the
               storeFS. */
            if (auto sourcePath = flakeRef.input.getSourcePath();
                flakeRef.input.getType() == "path" && sourcePath && state.store->isInStore(sourcePath->string())) {
                auto [storePath, subPath] = state.store->toStorePath(sourcePath->string());
                if (auto mount = state.storeFS->getMount(CanonPath(state.store->printStorePath(storePath)))) {
                    /* `mount` was registered by a fetcher (the only
                       site that mounts an individual storePath — the
                       constructor only mounts `/` and `/nix/store`),
                       so it is Copyable by construction. Use it
                       directly instead of constructing a rootFS-based
                       SourcePath, so the resulting NodeLocation
                       carries a Copyable accessor like every other
                       NodeLocation in the post-lazy-paths regime. */
                    auto mountRef = ref(mount);
                    /* mountInput is the only mounter into storeFS
                       (other than the constructor's root and
                       /nix/store), so any per-storepath mount is
                       by construction Copyable -- no runtime check
                       needed; the wrap below admits it as such. */
                    auto subdir = CanonPath(subPath);
                    if (!flakeRef.subdir.empty()) {
                        /* Part 3: route `?dir=` composition through
                           the kind-aware wrapper. Same shape as the
                           companion fix in `readFlake` — a user-
                           supplied `?dir=../escape` must be rejected,
                           not silently clamped to the storepath root.
                           The mount is Copyable by construction
                           (mountInput is the only mounter into
                           storeFS), so the wrapper applies
                           StrictAccessorBoundary. */
                        auto joined = subdir.abs() + "/" + flakeRef.subdir;
                        SourceRoot adhoc{mountRef, SourceRootKind::Copyable};
                        try {
                            subdir =
                                nix::resolveSymlinks(adhoc, std::string_view{joined}, SymlinkResolution::Ancestors);
                        } catch (AccessorBoundaryEscape &) {
                            throw Error(
                                "flake input subdir '%s' escapes the source tree at %s",
                                flakeRef.subdir,
                                mountRef->showPath(CanonPath::root));
                        }
                    }
                    auto path = SourcePath{mountRef, subdir};
                    auto location = nix::flake::NodeLocation{
                        .tree =
                            nix::fetchers::MountableTree{
                                .storePath = storePath,
                                .accessor = [acc = mountRef]() { return acc; },
                            },
                        .subdir = std::string{subdir.rel()},
                    };
                    return callFlake(state, lockFlake(settings, state, path, std::move(location), lockFlags), v);
                }
            }

            callFlake(state, lockFlake(settings, state, flakeRef, lockFlags), v);
        };

        if (args[0]->type() == nPath) {
            auto rp = args[0]->rootedPath();
            auto path = state.realisePath(pos, *args[0]);
            /* Dispatch by the path Value's SourceRoot kind:

               - `Copyable` accessors *are* fetched-tree views: the
                 source flake's input cache already holds the external
                 narHash that pins them. Use them directly via the
                 SourcePath form of `lockFlake`; no re-fetch, no copy.

               - `System` accessors point into the real filesystem.
                 Reachable for path values rooted on rootFS — e.g.
                 `--impure --expr 'builtins.getFlake /nix/store/X'`
                 with a literal absolute path. Build a `path:` Input
                 from attrs — no string round-trip — attaching the
                 store's externally-recorded narHash when the path
                 is in-store. lockByFlakeRef's in-store shortcut
                 then picks up any already-mounted storeFS entry
                 (the Copyable mount above).

               - `Internal` accessors hold nix-internal helpers and
                 aren't user-facing tree surfaces. Reject
                 defensively. */
            switch (rp.root->kind) {
            case SourceRootKind::Copyable: {
                auto location = nix::flake::NodeLocation{
                    .tree =
                        nix::fetchers::MountableTree{
                            .storePath = std::nullopt,
                            .accessor = [acc = path.accessor]() { return acc; },
                        },
                    .subdir = std::string{path.path.rel()},
                };
                callFlake(state, lockFlake(settings, state, path, std::move(location), lockFlags), v);
                break;
            }
            case SourceRootKind::System: {
                auto absStr = path.path.abs();
                fetchers::Attrs attrs;
                attrs.insert_or_assign("type", std::string("path"));
                std::string subdir;
                if (state.store->isInStore(absStr)) {
                    auto [storePath, subPath] = state.store->toStorePath(absStr);
                    attrs.insert_or_assign("path", state.store->printStorePath(storePath));
                    /* Externally-recorded narHash from the store, not
                       computed from current eval. Makes the synthesised
                       FlakeRef satisfy `isLocked()` in pure-eval, and
                       lets the `path:` fetcher's in-store shortcut
                       reuse the storepath verbatim. */
                    attrs.insert_or_assign(
                        "narHash", state.store->queryPathInfo(storePath)->narHash.to_string(HashFormat::SRI, true));
                    subdir = std::string{CanonPath(subPath).rel()};
                } else {
                    /* No external narHash to attach. `isLocked()` will
                       return false; pure-eval rejects via the check in
                       `lockByFlakeRef`. Impure mode proceeds and copies
                       through the fetcher. */
                    attrs.insert_or_assign("path", std::string{absStr});
                }
                FlakeRef flakeRef{fetchers::Input::fromAttrs(state.fetchSettings, std::move(attrs)), std::move(subdir)};
                lockByFlakeRef(flakeRef, flakeRef.to_string());
                break;
            }
            case SourceRootKind::Internal:
                state
                    .error<EvalError>(
                        "cannot call 'builtins.getFlake' on an internal path value at '%s'",
                        path.accessor->showPath(path.path))
                    .atPos(pos)
                    .debugThrow();
            }
        } else {
            std::string flakeRefS(
                state.forceStringNoCtx(*args[0], pos, "while evaluating the argument passed to builtins.getFlake"));
            lockByFlakeRef(nix::parseFlakeRef(state.fetchSettings, flakeRefS, {}, true), flakeRefS);
        }
    };

    return PrimOp{
        .name = "__getFlake",
        .args = {"args"},
        .doc = R"(
          Fetch a flake from a flake reference or a path, and return its output attributes and some metadata. For example:

          ```nix
          (builtins.getFlake "nix/55bc52401966fbffa525c574c14f67b00bc4fb3a").packages.x86_64-linux.nix
          ```

          Unless impure evaluation is allowed (`--impure`), the flake reference
          must be "locked", e.g. contain a Git revision or content hash. An
          example of an unlocked usage is:

          ```nix
          (builtins.getFlake "github:edolstra/dwarffs").rev
          ```
        )",
        .impl = prim_getFlake,
        .experimentalFeature = Xp::Flakes,
    };
}

static void prim_parseFlakeRef(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    std::string flakeRefS(
        state.forceStringNoCtx(*args[0], pos, "while evaluating the argument passed to builtins.parseFlakeRef"));
    auto attrs = nix::parseFlakeRef(state.fetchSettings, flakeRefS, {}, true).toAttrs();
    auto binds = state.buildBindings(attrs.size());
    for (const auto & [key, value] : attrs) {
        auto s = state.symbols.create(key);
        auto & vv = binds.alloc(s);
        auto resolved = forceAttr(value);
        std::visit(
            overloaded{
                [&vv, &state](const std::string & value) { vv.mkString(value, state.mem); },
                [&vv](const uint64_t & value) { vv.mkInt(value); },
                [&vv](const Explicit<bool> & value) { vv.mkBool(value.t); }},
            resolved);
    }
    v.mkAttrs(binds);
}

nix::PrimOp parseFlakeRef({
    .name = "__parseFlakeRef",
    .args = {"flake-ref"},
    .doc = R"(
      Parse a flake reference, and return its exploded form.

      For example:

      ```nix
      builtins.parseFlakeRef "github:NixOS/nixpkgs/23.05?dir=lib"
      ```

      evaluates to:

      ```nix
      { dir = "lib"; owner = "NixOS"; ref = "23.05"; repo = "nixpkgs"; type = "github"; }
      ```
    )",
    .impl = prim_parseFlakeRef,
    .experimentalFeature = Xp::Flakes,
});

static void prim_flakeRefToString(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], noPos, "while evaluating the argument passed to builtins.flakeRefToString");
    fetchers::Attrs attrs;
    for (const auto & attr : *args[0]->attrs()) {
        state.forceValue(*attr.value, attr.pos);
        auto t = attr.value->type();
        if (t == nInt) {
            auto intValue = attr.value->integer().value;

            if (intValue < 0) {
                state
                    .error<EvalError>(
                        "negative value given for flake ref attr %1%: %2%", state.symbols[attr.name], intValue)
                    .atPos(pos)
                    .debugThrow();
            }

            attrs.emplace(state.symbols[attr.name], uint64_t(intValue));
        } else if (t == nBool) {
            attrs.emplace(state.symbols[attr.name], Explicit<bool>{attr.value->boolean()});
        } else if (t == nString) {
            attrs.emplace(state.symbols[attr.name], std::string(attr.value->string_view()));
        } else {
            state
                .error<EvalError>(
                    "flake reference attribute sets may only contain integers, Booleans, "
                    "and strings, but attribute '%s' is %s",
                    state.symbols[attr.name],
                    showType(*attr.value))
                .debugThrow();
        }
    }
    auto flakeRef = FlakeRef::fromAttrs(state.fetchSettings, attrs);
    v.mkString(flakeRef.to_string(), state.mem);
}

nix::PrimOp flakeRefToString({
    .name = "__flakeRefToString",
    .args = {"attrs"},
    .doc = R"(
      Convert a flake reference from attribute set format to URL format.

      For example:

      ```nix
      builtins.flakeRefToString {
        dir = "lib"; owner = "NixOS"; ref = "23.05"; repo = "nixpkgs"; type = "github";
      }
      ```

      evaluates to

      ```nix
      "github:NixOS/nixpkgs/23.05?dir=lib"
      ```
    )",
    .impl = prim_flakeRefToString,
    .experimentalFeature = Xp::Flakes,
});

} // namespace nix::flake::primops
