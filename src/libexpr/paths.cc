#include "nix/store/store-api.hh"
#include "nix/store/content-address.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/source-root.hh"
#include "nix/util/diagnose.hh"
#include "nix/util/memo.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetch-settings.hh"
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

std::optional<std::string> EvalState::allocSourceUnpinnedId(SourceRoot & root)
{
    if (!root.unpinnedId)
        return std::nullopt;
    const auto & url = *root.unpinnedId;
    auto key = std::pair{url, &*root.accessor};

    /* Hot path: same (url, accessor) already memoised → return the
       stored identifier. */
    std::optional<std::string> hit;
    sourceUnpinnedIds->cvisit(key, [&](const auto & kv) { hit = kv.second; });
    if (hit)
        return *hit;

    /* Miss: allocate the next counter for this URL atomically, format
       `<url>#<n>`, and insert. `Sync<map>` for the counter — the
       fetch-and-increment is one critical section. The visitor on
       the `concurrent_flat_map::emplace_or_visit` covers the race
       where two threads miss the lookup for the same key: the loser
       drops its allocation and adopts the winner's identifier. */
    size_t n;
    {
        auto counters(sourceUnpinnedIdCounters->lock());
        n = (*counters)[url]++;
    }
    std::string id = url + "#" + std::to_string(n);
    sourceUnpinnedIds->emplace_or_visit(key, id, [&](const auto & kv) { id = kv.second; });
    return id;
}

ref<SourceRoot>
EvalState::getOrCreateRoot(ref<SourceAccessor> accessor, SourceRootKind kind, std::optional<std::string> unpinnedId)
{
    auto key = std::pair{&*accessor, kind};
    /* Hot path: read-only lookup. Each (accessor, kind) is admitted
       once and re-emitted on every reuse (e.g. every `fetchTree`
       materialisation reusing the same fetcher accessor under
       Copyable), so cache hits dominate. */
    std::optional<ref<SourceRoot>> hit;
    rootCache->cvisit(key, [&](const auto & kv) { hit = kv.second; });
    if (hit)
        return *hit;
    /* Miss: allocate, insert. Race-tolerant — each (accessor, kind)
       maps to a single SourceRoot value, so concurrent inserts
       produce structurally identical entries; the loser's allocation
       is dropped via the visitor. The first admission's `unpinnedId`
       wins; a later call with a different id (shouldn't happen in
       practice) is silently ignored. */
    ref<SourceRoot> result = SourceRoot::make(accessor, kind, std::move(unpinnedId));
    rootCache->emplace_or_visit(key, result, [&](const auto & kv) { result = kv.second; });
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
    /* Defer the narHash computation: install a `LazyAttr` that walks the
       tree only when something actually forces the value (lockfile write,
       `computeStorePath`, user-supplied-narHash verification, ...). No
       mount, no allowlist: with lazy paths, downstream code keeps
       SourcePath values rooted at the fetcher's accessor and reads
       through it directly, so there is nothing for storeFS to expose. */
    /* Snapshot `inputName` out of `input` rather than capturing the
       Input — `LazyAttr` is `ref<LazyAttrComputation>` (shared_ptr-
       wrapped), so the underlying object is shared across copies; what
       multiplies is the number of independent `forceAttr` call sites,
       each of which re-runs `compute` (neither `forceAttr` nor — yet
       — `LazyAttrComputation` itself memoises). `originalInput` is
       safe to copy: in production it's a pre-lock value (FlakeRef-
       derived, strings only). Its `to_string()` is reserved for the
       mismatch branch. */
    auto inputName = input.getName();
    auto originalNarHash = originalInput.getNarHash();
    /* Wrap in `memo<>` so the walk fires at most once even when the
       attr is forced repeatedly (lockfile write + computeStorePath +
       JSON serialization, ...). Crucial for non-fingerprinted
       accessors (dirty trees, in-memory): without memoisation, every
       force re-walks because the `sourcePathToHash` cache in
       `fetchToStore2` only kicks in on a fingerprint. On a mismatch
       throw, `call_once` keeps the flag unset so subsequent forces
       still surface the error. */
    auto lazyHash = make_ref<fetchers::LazyAttrComputation>(fetchers::LazyAttrComputation{
        .compute = memo<fetchers::ResolvedAttr>(
            [this, accessor, inputName, originalNarHash, originalInput]() -> fetchers::ResolvedAttr {
                /* Reuse the hash from a prior walk on this SourcePath
                   (mountInput or copyPathToStore both populate
                   `srcToStore` with `(StorePath, Hash)`). For
                   non-fingerprinted accessors this is the only way the
                   two callsites can share a walk — `fetchToStore2`'s
                   sqlite cache needs a fingerprint to bridge them. */
                Hash narHash = [&] {
                    if (auto hit = getConcurrent(*srcToStore, SourcePath{accessor, CanonPath::root}))
                        return hit->second;
                    /* Lockable inputs are by-construction fetched
                       trees (Copyable). Fire the lint before any
                       walk so callers wired to crash on this setting
                       can catch the offending site. */
                    diagnose(fetchSettings.lintFetchWholeSourceToStore, [&](bool fatal) -> std::optional<Error> {
                        return Error("computing narHash for fetched input '%s' walks the source tree", inputName);
                    });
                    return fetchToStore2(fetchSettings, *store, accessor, FetchMode::DryRun, inputName).second;
                }();
                if (originalNarHash && narHash != *originalNarHash)
                    throw Error(
                        (unsigned int) 102,
                        "NAR hash mismatch in input '%s', expected '%s' but got '%s'",
                        originalInput.to_string(),
                        narHash.to_string(HashFormat::SRI, true),
                        originalNarHash->to_string(HashFormat::SRI, true));
                return narHash.to_string(HashFormat::SRI, true);
            })});
    input.attrs.insert_or_assign("narHash", fetchers::Attr{lazyHash});
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
    auto [storePath, narHash] = [&]() -> std::pair<StorePath, Hash> {
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
            return {sp, narHash};
        }
        auto narHash = input.getNarHash();
        assert(narHash);
        return {
            store->makeFixedOutputPathFromCA(
                input.getName(),
                ContentAddressWithReferences::fromParts(ContentAddressMethod::Raw::NixArchive, *narHash, {})),
            *narHash};
    }();

    allowPath(storePath); // FIXME: should just whitelist the entire virtual store
    storeFS->mount(CanonPath(store->printStorePath(storePath)), [acc = accessor]() { return acc; });

    /* Pre-populate the SourcePath→(StorePath, Hash) cache so coerceToString
       of a path-typed `input.outPath` (at the accessor's root) returns
       this storePath without walking the tree, and `lockInput`'s narHash
       LazyAttr (if not yet forced) can reuse the hash from the same walk. */
    srcToStore->try_emplace(SourcePath{accessor, CanonPath::root}, std::make_pair(storePath, narHash));

    return storePath;
}

} // namespace nix
