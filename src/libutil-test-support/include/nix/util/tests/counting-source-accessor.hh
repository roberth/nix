#pragma once
///@file
///
/// Test-only instrumentation: a SourceAccessor wrapper that counts
/// operations on the underlying accessor, so tests can pin caching /
/// memoisation invariants by asserting that a given walk happened at
/// most N times.

#include "nix/util/source-accessor.hh"
#include "nix/util/ref.hh"

#include <atomic>

namespace nix {

/**
 * Wraps another `SourceAccessor` and counts operations.
 * `readFileCount` is the cleanest "did the tree get walked" signal:
 * NAR-serialisation reads every file exactly once per walk, while
 * `readDirectory` is called multiple times per directory by the
 * case-hack pass. `rootListCount` is kept as a coarser cross-check.
 */
struct CountingSourceAccessor : SourceAccessor
{
    ref<SourceAccessor> next;
    std::atomic<size_t> readFileCount{0};
    std::atomic<size_t> rootListCount{0};

    explicit CountingSourceAccessor(ref<SourceAccessor> next_)
        : next(std::move(next_))
    {
    }

    void anchor() override {}

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        ++readFileCount;
        next->readFile(path, sink, sizeCallback);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        return next->maybeLstat(path);
    }

    Stat lstat(const CanonPath & path) override
    {
        return next->lstat(path);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        if (path.isRoot())
            ++rootListCount;
        return next->readDirectory(path);
    }

    void readDirectory(
        const CanonPath & dirPath,
        std::function<void(SourceAccessor & subdirAccessor, const CanonPath & subdirRelPath)> callback) override
    {
        if (dirPath.isRoot())
            ++rootListCount;
        /* Call back with `*this` so recursion stays inside the
           wrapper. Forwarding to `next->readDirectory(..., callback)`
           would invoke the callback with `next` as the sub-accessor,
           bypassing the counters for every nested operation. */
        callback(*this, dirPath);
    }

    std::string readLink(const CanonPath & path) override
    {
        return next->readLink(path);
    }

    std::string showPath(const CanonPath & path) override
    {
        return next->showPath(path);
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return next->getPhysicalPath(path);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        return next->getFingerprint(path);
    }

    std::optional<time_t> getLastModified() override
    {
        return next->getLastModified();
    }

    bool pathExists(const CanonPath & path) override
    {
        return next->pathExists(path);
    }
};

} // namespace nix
