#include "nix/store/store-api.hh"
#include "nix/store/content-address.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/source-root.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/fetchers/fetch-to-store.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

SourcePath EvalState::rootPath(CanonPath path)
{
    return {rootFS, std::move(path)};
}

SourcePath EvalState::rootPath(std::string_view path)
{
    /* FIXME: Move this out of EvalState, since it's using native
       std::filesystem::path and current working directory. */
    return {rootFS, CanonPath(absPath(path).string())};
}

RootedPath EvalState::rootedPath(CanonPath path)
{
    return {rootFSRoot, std::move(path)};
}

RootedPath EvalState::rootedPath(std::string_view path)
{
    return {rootFSRoot, CanonPath(absPath(path).string())};
}

SourcePath EvalState::storePath(const StorePath & path)
{
    return {rootFS, CanonPath{store->printStorePath(path)}};
}

ref<SourceRoot> EvalState::getOrCreateFetcherRoot(ref<SourceAccessor> accessor)
{
    SourceAccessor * key = &*accessor;
    /* Hot path: read-only lookup. Each fetcher accessor is admitted
       once and re-emitted on every `fetchTree` materialisation, so
       cache hits dominate. */
    std::optional<ref<SourceRoot>> hit;
    fetcherRoots->cvisit(key, [&](const auto & kv) { hit = kv.second; });
    if (hit)
        return *hit;
    /* Miss: allocate, insert. Race-tolerant — each accessor maps to a
       single Copyable-kinded SourceRoot, so concurrent inserts produce
       structurally identical entries; the loser's allocation is
       dropped via the visitor. */
    ref<SourceRoot> result = make_ref<SourceRoot>(accessor, SourceRootKind::Copyable);
    fetcherRoots->emplace_or_visit(key, result, [&](const auto & kv) { result = kv.second; });
    return result;
}

void EvalState::ensureLazyPathCopied(const StorePath & path)
{
    if (settings.isReadOnly())
        return;

    auto mount = storeFS->getMount(CanonPath(store->printStorePath(path)));
    if (!mount)
        return;

    /* TODO: We could memoise this in-memory if necessary. */
    auto storePath = fetchToStore(
        fetchSettings,
        *store,
        SourcePath{ref(mount)},
        /* Force a copy. mountInput does a dryRun to just calculate the storePath and narHash. */
        FetchMode::Copy,
        path.name());

    /* This can happen if the source gets modified by another process while we are evaluaing
       from it. Alternatively, the caching might be unsound and fetcher cache is poisoned somehow.
       See https://github.com/NixOS/nix/issues/14317. */
    if (storePath != path) {
        throw Error(
            (unsigned int) 102,
            "store path ('%1%') was hashed to avoid a full copy at first, but upon reading it again, the contents have changed ('%2%'), so we can not proceed. Make sure files do not change during evaluation",
            store->printStorePath(path),
            store->printStorePath(storePath));
    }
}

void EvalState::ensureLazyPathsCopied(const NixStringContext & context)
{
    for (const auto & c : context)
        if (auto * o = std::get_if<NixStringContextElem::Opaque>(&c.raw))
            /* TODO: This could be done in parallel. */
            ensureLazyPathCopied(o->path);
}

void EvalState::lockInput(fetchers::Input & input, const fetchers::Input & originalInput, ref<SourceAccessor> accessor)
{
    /* Walk the tree once to compute the lockfile-grade narHash; surface it on
       input.attrs so callers treat the input as locked. No mount, no
       allowlist: with lazy paths, downstream code keeps SourcePath values
       rooted at the fetcher's accessor and reads through it directly, so
       there is nothing for storeFS to expose. */
    auto [_, narHash] = fetchToStore2(fetchSettings, *store, accessor, FetchMode::DryRun, input.getName());
    input.attrs.insert_or_assign("narHash", narHash.to_string(HashFormat::SRI, true));
    if (originalInput.getNarHash() && narHash != *originalInput.getNarHash())
        throw Error(
            (unsigned int) 102,
            "NAR hash mismatch in input '%s', expected '%s' but got '%s'",
            originalInput.to_string(),
            narHash.to_string(HashFormat::SRI, true),
            originalInput.getNarHash()->to_string(HashFormat::SRI, true));
}

StorePath
EvalState::mountInput(fetchers::Input & input, const fetchers::Input & originalInput, ref<SourceAccessor> accessor)
{
    /* Two cases:
       - `originalInput` carries a narHash (the user asserted one in
         `builtins.fetchGit { narHash = ...; }` etc.). Hash the tree
         to verify the assertion, so a wrong one is caught loudly at
         eval time — *before* anything depending on the storePath can
         be memoised. The hashing is via `fetchToStore2(DryRun)`,
         which only actually walks the accessor on the first
         retrieval of a given fingerprint (typically a git rev);
         subsequent calls hit the `sourcePathToHash` cache
         (sqlite-backed, persists across sessions) and return
         without reading any blobs.
       - Otherwise, trust the narHash already on `input` (typically
         from a flake.lock entry surfaced through the fetcher). Derive
         the storePath via the fixed-output formula — no walk. The
         actual walk-and-copy happens later in `ensureLazyPathCopied`
         if eval ends with this storePath in the string context. */
    auto storePath = [&]() -> StorePath {
        if (originalInput.getNarHash()) {
            auto [sp, narHash] = fetchToStore2(fetchSettings, *store, accessor, FetchMode::DryRun, input.getName());
            if (narHash != *originalInput.getNarHash())
                throw Error(
                    (unsigned int) 102,
                    "NAR hash mismatch in input '%s', expected '%s' but got '%s'",
                    originalInput.to_string(),
                    narHash.to_string(HashFormat::SRI, true),
                    originalInput.getNarHash()->to_string(HashFormat::SRI, true));
            input.attrs.insert_or_assign("narHash", narHash.to_string(HashFormat::SRI, true));
            return sp;
        }
        auto narHash = input.getNarHash();
        assert(narHash);
        return store->makeFixedOutputPathFromCA(
            input.getName(),
            ContentAddressWithReferences::fromParts(ContentAddressMethod::Raw::NixArchive, *narHash, {}));
    }();

    allowPath(storePath); // FIXME: should just whitelist the entire virtual store
    storeFS->mount(CanonPath(store->printStorePath(storePath)), [acc = accessor]() { return acc; });

    /* Pre-populate the SourcePath→StorePath cache so coerceToString of
       a path-typed `input.outPath` (at the accessor's root) returns
       this storePath without walking the tree. */
    srcToStore->try_emplace(SourcePath{accessor, CanonPath::root}, storePath);

    return storePath;
}

} // namespace nix
