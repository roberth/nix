#pragma once
///@file

#include <optional>

#include "nix/store/path.hh"
#include "nix/util/fun.hh"
#include "nix/util/ref.hh"
#include "nix/util/source-accessor.hh"

namespace nix::fetchers {

/**
 * A source tree's identity (its predicted storePath, when known)
 * paired with a thunk for reading through its accessor.
 * Materialisation happens lazily on first access through the
 * accessor thunk — so holding a `MountableTree` doesn't commit to
 * any materialisation having happened. The same instance can be
 * shared across multiple consumers (e.g. relative-path inputs that
 * share their parent's store object).
 *
 * `storePath` is `std::optional` so callers can defer its
 * computation: a fully-lazy fetcher result that hasn't been
 * `mountInput`-walked has the accessor in hand but not yet the
 * predicted storePath. Consumers that need a storePath string (e.g.
 * `emitTreeAttrs` rendering `outPath` eagerly) check `storePath` and
 * fall back to a path-typed render when it's absent.
 *
 * `MountableTree` is value-copyable: relative-input branches assign
 * `.tree = parentLoc.tree`, which copies the `fun<ref<SourceAccessor>()>`
 * thunk. For *identity-shape* thunks (an in-hand accessor returned
 * verbatim) the copies are independent but always produce the same
 * accessor reference. When a thunk wraps *real work* (a fetch, a
 * narHash walk) its memoisation must live in shared state — e.g. a
 * `shared_ptr`-backed captured cache — so that copies of the
 * `MountableTree` continue to share the single materialisation
 * rather than each driving the work themselves.
 */
struct MountableTree
{
    std::optional<StorePath> storePath;
    fun<ref<SourceAccessor>()> accessor;
};

} // namespace nix::fetchers
