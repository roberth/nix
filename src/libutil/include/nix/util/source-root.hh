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
struct SourceRoot
{
    ref<SourceAccessor> accessor;
    SourceRootKind kind;

    /* Default construction is forbidden — every SourceRoot must
       name an admission site that chose a kind. libutil cannot pick
       a default because it doesn't know what the values mean. */
    SourceRoot() = delete;

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

    bool operator==(const RootedPath & x) const noexcept = default;
};

} // namespace nix
