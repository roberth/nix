#pragma once

#include <filesystem>
#include <list>

#include "nix/util/canon-path.hh"
#include "nix/util/error.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/strings.hh"

namespace nix {

/**
 * Observer hooks for `resolveSymlinks`. Inherit from this struct and
 * override only the hooks you care about; un-overridden hooks remain
 * no-ops that the compiler inlines to nothing.
 *
 * Hooks are called directly by the templated `resolveSymlinks` (not
 * via `requires {…}`-style detection): a missing method or wrong
 * signature is a compile error rather than a silent skip. The
 * compile-time guarantee only holds for the free-standing-struct
 * pattern (a struct that defines both hooks directly); inheriting
 * from `NoOpResolveSymlinksCallbacks` and *typo-ing* an override
 * silently falls back to the NoOp default. Either watch for that
 * (e.g. with `override`-style discipline — though there's nothing
 * to override here since these aren't virtual) or define the hooks
 * directly without inheritance when typo-safety matters more than
 * concision.
 */
struct NoOpResolveSymlinksCallbacks
{
    /**
     * Fires before the resolver processes a `..` component, regardless
     * of whether the `..` came from the input path or was spliced in
     * from a symlink target. `current` is the path resolved so far —
     * the path *before* the pop happens. If `current.isRoot()`, the
     * resolver is about to silently clamp (legacy behaviour); throw
     * here to refuse instead.
     */
    void onParent(const CanonPath & /*current*/) const noexcept {}

    /**
     * Fires before the resolver follows an absolute-target symlink
     * (which resets the resolver to accessor root and splices the
     * target onto the todo list). `link` is the path of the symlink
     * itself; `target` is the raw absolute target string.
     */
    void onAbsoluteSymlink(const CanonPath & /*link*/, std::string_view /*target*/) const noexcept {}

    /* Note: there is no `onRelativeSymlink` hook. Relative-target
       follows are a routine name-lookup step in the walker (the
       resulting path is reached via pop+splice; any `..` in the
       target fires `onParent` as usual). Absolute targets are
       different in kind — they rebase the walker on accessor root,
       which is the sort of transition a policy may legitimately want
       to observe or refuse. No current caller needs to observe
       relative-target follows; the hook can be added here if a use
       case appears, without breaking existing callers (NoOp's empty
       default would apply unless overridden). */
};

/**
 * Resolve any symlinks in `rawPath` against `accessor` according to
 * the given resolution mode, optionally calling back into a callbacks
 * object at semantically interesting transitions.
 *
 * This overload tokenises the input string without normalising — `..`
 * components survive into the walk, so the walker can apply
 * symlink-aware semantics to them. This is the *correct* entry point
 * for raw input: `CanonPath(rawPath)` would pre-strip `..` lexically,
 * which produces a different (wrong) answer when a component along
 * the way is a symlink. See the doc on `CanonPath(std::string_view)`
 * for the trade-off.
 *
 * @param mode might only be a temporary solution. See the discussion
 * in https://github.com/NixOS/nix/pull/9985.
 */
template<typename Callbacks = NoOpResolveSymlinksCallbacks>
inline CanonPath resolveSymlinks(
    SourceAccessor & accessor,
    std::string_view rawPath,
    SymlinkResolution mode = SymlinkResolution::Full,
    const Callbacks & cb = {})
{
    auto res = CanonPath::root;

    int linksAllowed = 1024;

    auto todo = tokenizeString<std::list<std::string>>(rawPath, "/");

    while (!todo.empty()) {
        auto c = std::move(todo.front());
        todo.pop_front();
        /* `c.empty()` is defensive: `tokenizeString` collapses runs of
           `/` so a raw input like `/a//b` never produces an empty
           token here. Symlink-target splices also tokenise through
           the same helper. Skipping empty preserves correctness if a
           future caller bypasses the tokeniser. `c == "."` skips the
           one component the tokeniser does pass through (`.` is
           preserved as a token); `CanonPath`'s constructor would
           strip it pre-walk, but raw input from the `string_view`
           overload sees it here. */
        if (c.empty() || c == ".")
            continue;
        if (c == "..") {
            cb.onParent(res);
            if (!res.isRoot())
                res.pop();
            continue;
        }
        res.push(c);
        if (mode == SymlinkResolution::Full || !todo.empty()) {
            if (auto st = accessor.maybeLstat(res); st && st->type == SourceAccessor::tSymlink) {
                if (!linksAllowed--)
                    throw Error(
                        "too many levels of symbolic links in path '%s' (limit %d)", accessor.showPath(rawPath), 1024);
                auto target = accessor.readLink(res);
                if (std::filesystem::path(target).is_absolute()) {
                    cb.onAbsoluteSymlink(res, target);
                    res = CanonPath::root;
                } else {
                    res.pop();
                }
                todo.splice(todo.begin(), tokenizeString<std::list<std::string>>(target, "/"));
            }
        }
    }

    return res;
}

/**
 * `CanonPath` overload — forwards to the `std::string_view` overload
 * using the canonical path's absolute form. Because a `CanonPath` has
 * already had `..` stripped at construction time, this overload is
 * only correct when the canonical form genuinely matches what the
 * walker would produce — i.e. when the path is known not to traverse
 * a symlink. For arbitrary input, prefer the `std::string_view`
 * overload above.
 */
inline CanonPath
resolveSymlinks(SourceAccessor & accessor, const CanonPath & path, SymlinkResolution mode = SymlinkResolution::Full)
{
    return resolveSymlinks(accessor, std::string_view{path.abs()}, mode);
}

} // namespace nix
