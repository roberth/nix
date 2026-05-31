#include "nix/util/mounted-source-accessor.hh"

#include <atomic>
#include <memory>
#include <mutex>

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

namespace {

/* Unfortunate that we reimplement memo() logic, but fun<> can not
   give us bool fired, needed for invalidateCache. */
struct MountEntry
{
    fun<ref<SourceAccessor>()> thunk;
    std::once_flag flag;
    std::atomic<bool> fired{false};
    std::optional<ref<SourceAccessor>> cached;

    explicit MountEntry(fun<ref<SourceAccessor>()> thunk)
        : thunk(std::move(thunk))
    {
    }
};

struct MountedSourceAccessorImpl : MountedSourceAccessor
{
private:
    void anchor() override {};

public:
    boost::concurrent_flat_map<CanonPath, std::shared_ptr<MountEntry>> mounts;

    MountedSourceAccessorImpl(std::map<CanonPath, fun<ref<SourceAccessor>()>> _mounts)
    {
        displayPrefix.clear();

        // Currently we require a root filesystem. This could be relaxed.
        assert(_mounts.contains(CanonPath::root));

        for (auto & [path, thunk] : _mounts)
            mount(path, std::move(thunk));

        // FIXME: return dummy parent directories automatically?
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->readFile(subpath, sink, sizeCallback);
    }

    Stat lstat(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->lstat(subpath);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->maybeLstat(subpath);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->readDirectory(subpath);
    }

    std::string readLink(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->readLink(subpath);
    }

    std::string showPath(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return displayPrefix + accessor->showPath(subpath) + displaySuffix;
    }

    std::pair<ref<SourceAccessor>, CanonPath> resolve(CanonPath path)
    {
        // Find the nearest parent of `path` that is a mount point.
        std::vector<std::string> subpath;
        while (true) {
            if (auto mount = getMount(path)) {
                std::reverse(subpath.begin(), subpath.end());
                return {ref(mount), CanonPath(subpath)};
            }

            assert(!path.isRoot());
            subpath.push_back(std::string(*path.baseName()));
            path.pop();
        }
    }

    void invalidateCache() override
    {
        /* Only invalidate mounts that have actually been materialised.
           Un-fired thunks have nothing to invalidate; firing them just
           to invalidate would defeat the laziness. */
        mounts.visit_all([](auto & kv) {
            auto & entry = *kv.second;
            if (entry.fired.load(std::memory_order_acquire))
                (*entry.cached)->invalidateCache();
        });
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->getPhysicalPath(subpath);
    }

    void mount(CanonPath mountPoint, fun<ref<SourceAccessor>()> accessor) override
    {
        mounts.emplace(std::move(mountPoint), std::make_shared<MountEntry>(std::move(accessor)));
    }

    std::shared_ptr<SourceAccessor> getMount(CanonPath mountPoint) override
    {
        auto entry_opt = getConcurrent(mounts, mountPoint);
        if (!entry_opt)
            return nullptr;
        auto entry = *entry_opt;
        std::call_once(entry->flag, [&]() {
            entry->cached.emplace(entry->thunk());
            /* `release` pairs with the `acquire` in invalidateCache;
               other threads that observe `fired == true` will also see
               the write to `cached`. */
            entry->fired.store(true, std::memory_order_release);
        });
        return entry->cached->get_ptr();
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return {path, fingerprint};
        auto [accessor, subpath] = resolve(path);
        return accessor->getFingerprint(subpath);
    }
};

} // namespace

MountedSourceAccessor::~MountedSourceAccessor() {}

ref<MountedSourceAccessor> makeMountedSourceAccessor(std::map<CanonPath, fun<ref<SourceAccessor>()>> mounts)
{
    return make_ref<MountedSourceAccessorImpl>(std::move(mounts));
}

} // namespace nix
