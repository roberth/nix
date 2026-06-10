#pragma once
/**
 * @file
 *
 * @brief SourcePath
 */

#include "nix/util/ref.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/std-hash.hh"

namespace nix {

/**
 * An abstraction for accessing source files during
 * evaluation. Currently, it's just a wrapper around `CanonPath` that
 * accesses files in the regular filesystem, but in the future it will
 * support fetching files in other ways.
 */
struct SourcePath
{
    ref<SourceAccessor> accessor;
    CanonPath path;

    SourcePath(ref<SourceAccessor> accessor, CanonPath path = CanonPath::root)
        : accessor(std::move(accessor))
        , path(std::move(path))
    {
    }

    std::string_view baseName() const;

    /**
     * Construct the parent of this `SourcePath`. Aborts if `this`
     * denotes the root.
     */
    SourcePath parent() const;

    /**
     * If this `SourcePath` denotes a regular file (not a symlink),
     * return its contents; otherwise throw an error.
     */
    std::string readFile() const;

    void readFile(Sink & sink, fun<void(uint64_t)> sizeCallback = [](uint64_t size) {}) const
    {
        return accessor->readFile(path, sink, sizeCallback);
    }

    /**
     * Return whether this `SourcePath` denotes a file (of any type)
     * that exists
     */
    bool pathExists() const;

    /**
     * Return stats about this `SourcePath`, or throw an exception if
     * it doesn't exist.
     */
    SourceAccessor::Stat lstat() const;

    /**
     * Return stats about this `SourcePath`, or std::nullopt if it
     * doesn't exist.
     */
    std::optional<SourceAccessor::Stat> maybeLstat() const;

    /**
     * If this `SourcePath` denotes a directory (not a symlink),
     * return its directory entries; otherwise throw an error.
     */
    SourceAccessor::DirEntries readDirectory() const;

    /**
     * If this `SourcePath` denotes a symlink, return its target;
     * otherwise throw an error.
     */
    std::string readLink() const;

    /**
     * Dump this `SourcePath` to `sink` as a NAR archive.
     */
    void dumpPath(Sink & sink, PathFilter & filter = defaultPathFilter) const;

    /**
     * Return the location of this path in the "real" filesystem, if
     * it has a physical location.
     */
    std::optional<std::filesystem::path> getPhysicalPath() const;

    std::string to_string() const;

    /**
     * Append a `CanonPath` to this path.
     */
    SourcePath operator/(const CanonPath & x) const;

    /**
     * Append a single component `c` to this path. `c` must not
     * contain a slash. A slash is implicitly added between this path
     * and `c`.
     */
    SourcePath operator/(std::string_view c) const;

    bool operator==(const SourcePath & x) const noexcept;
    std::strong_ordering operator<=>(const SourcePath & x) const noexcept;

    /**
     * Convenience wrapper around `SourceAccessor::resolveSymlinks()`.
     */
    SourcePath resolveSymlinks(SymlinkResolution mode = SymlinkResolution::Full) const
    {
        return {accessor, accessor->resolveSymlinks(path, mode)};
    }

    friend class std::hash<nix::SourcePath>;
};

std::ostream & operator<<(std::ostream & str, const SourcePath & path);

/**
 * Decide whether two `SourceAccessor`s denote NAR-equivalent trees
 * (compared from their roots).
 *
 * Equality is contents-based: identical NAR serialisations iff
 * true, up to the parts NAR records — file type, regular-file
 * bytes plus the executable bit, symlink target, directory entries
 * (names and types, recursively). Timestamps, ownership, and
 * non-NAR-serialisable node types compare unequal. Both accessors
 * empty at the root compares equal (an empty NAR is still a NAR).
 *
 * The signature takes `ref<SourceAccessor>` rather than
 * `SourcePath`: the operation is rooted by NAR semantics. Callers
 * that need to compare a subtree must wrap a sub-accessor; the
 * SourcePath-with-subpath signature this commit replaces let callers
 * spell that question in a way the primitive couldn't actually
 * answer (the NAR walk always starts at the accessor root). The
 * narrowing is intentional: subtree NARs are a different operation
 * this primitive deliberately doesn't provide.
 *
 * Cheap shortcuts first:
 *  1. Same accessor pointer → true.
 *  2. Both accessors expose a fingerprint at the root and they
 *     agree → true. A disagreement is not conclusive (different
 *     origins can still produce identical contents) and falls
 *     through.
 *  3. If a `hint` subpath is given and exists on both sides,
 *     compare it as a single NAR node (no symlink resolution, no
 *     recursion into directories at the hint). Mismatch → false;
 *     otherwise continue. The hint mechanism is *fast-fail only*:
 *     a matching hint does not fast-confirm equality, since the
 *     rest of the tree could still differ.
 *  4. Walk both trees in lockstep under NAR semantics.
 */
bool contentsEqual(ref<SourceAccessor> a, ref<SourceAccessor> b, std::optional<CanonPath> hint = std::nullopt);

inline std::size_t hash_value(const SourcePath & path)
{
    std::size_t hash = 0;
    boost::hash_combine(hash, path.accessor->number);
    boost::hash_combine(hash, path.path);
    return hash;
}

} // namespace nix

template<>
struct std::hash<nix::SourcePath>
{
    using is_avalanching = std::true_type;

    std::size_t operator()(const nix::SourcePath & s) const noexcept
    {
        return nix::hash_value(s);
    }
};
