#pragma once
/**
 * @file
 *
 * @brief SourceRoot, RootedPath, and the opaque SourceRootKind tag.
 *
 * libutil declares the tag (forward-declared scoped enum with a known
 * underlying type) but defines none of the values; downstream consumers
 * — currently libexpr in <nix/expr/source-root.hh> — own the meaning.
 * This keeps the IO-level `SourceAccessor` and `SourcePath` free of
 * language-level concepts while still letting Pos::Origin and Value
 * carry a typed kind through libutil layers without per-call virtual
 * dispatch.
 */

#include <cstdint>
#include <memory>

#include "nix/util/canon-path.hh"
#include "nix/util/ref.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/source-path.hh"

namespace nix {

/**
 * Opaque kind tag on a `SourceRoot`. Values are defined in
 * `<nix/expr/source-root.hh>`; libutil never inspects them, only
 * stores and propagates.
 */
enum class SourceRootKind : std::uint8_t;

/**
 * A `SourceAccessor` paired with the language-level semantic kind
 * under which it has been admitted as a path root.
 *
 * The same accessor may be admitted multiple times under different
 * kinds (e.g. a posix accessor exposed both as a System filesystem
 * view and as a Copyable per-storepath view). Each admission site
 * constructs its own `SourceRoot`; downstream consumers read `kind`
 * to dispatch rendering, position semantics, and copy-to-store
 * rejection.
 */
struct SourceRoot : std::enable_shared_from_this<SourceRoot>
{
    ref<SourceAccessor> accessor;
    SourceRootKind kind;

    /* Default construction is forbidden — every SourceRoot must
       name an admission site that chose a kind. libutil cannot pick
       a default because it doesn't know what the values mean. */
    SourceRoot() = delete;

    /**
     * Construct only via `make_ref<SourceRoot>(accessor, kind)` (or
     * `std::make_shared`-equivalent).
     *
     * `enable_shared_from_this` requires the instance to already be
     * owned by a `shared_ptr` before `shared_from_this()` is called;
     * a stack-allocated or raw-`new`-allocated `SourceRoot` would
     * throw `std::bad_weak_ptr` at any capture site (e.g.
     * `Value::rootedPath()`, `ExprConcatStrings`'s
     * `firstPathRoot`). All current admission seams use
     * `make_ref<SourceRoot>`; new admission seams must do the
     * same.
     */
    SourceRoot(ref<SourceAccessor> accessor, SourceRootKind kind)
        : accessor(std::move(accessor))
        , kind(kind)
    {
    }
};

/**
 * A path under a `SourceRoot`. Replaces `SourcePath` in language-
 * level contexts (Value's path field, Pos::Origin's source-file
 * arm). `SourcePath` itself remains the bare IO primitive.
 */
struct RootedPath
{
    ref<SourceRoot> root;
    CanonPath path;

    RootedPath(ref<SourceRoot> root, CanonPath path = CanonPath::root)
        : root(std::move(root))
        , path(std::move(path))
    {
    }

    /**
     * Project to a bare `SourcePath` for IO operations that don't
     * care about the kind.
     */
    SourcePath sourcePath() const
    {
        return {root->accessor, path};
    }

    /**
     * Return a `RootedPath` for the parent directory. Same root,
     * canon path's parent. Asserts at the root (mirrors
     * `SourcePath::parent`).
     */
    RootedPath parent() const
    {
        auto p = path.parent();
        assert(p);
        return {root, std::move(*p)};
    }

    RootedPath operator/(const CanonPath & x) const
    {
        return {root, path / x};
    }

    RootedPath operator/(std::string_view c) const
    {
        return {root, path / c};
    }

    std::optional<std::filesystem::path> getPhysicalPath() const
    {
        return sourcePath().getPhysicalPath();
    }

    bool operator==(const RootedPath & x) const noexcept;
    /**
     * Lex compare on (path, kind, accessor.number). Ordering is
     * deterministic within a process; cross-process ordering is not
     * — `SourceAccessor::number` is assigned in allocation order per
     * process, so a given accessor's number isn't reproducible
     * across runs.
     */
    std::strong_ordering operator<=>(const RootedPath & x) const noexcept;
};

std::ostream & operator<<(std::ostream & str, const RootedPath & path);

} // namespace nix
