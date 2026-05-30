#pragma once

#include <filesystem>
#include <list>

#include "nix/util/canon-path.hh"
#include "nix/util/error.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/strings.hh"

namespace nix {

/**
 * Resolve any symlinks in `rawPath` against `accessor` according to
 * the given resolution mode.
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
inline CanonPath
resolveSymlinks(SourceAccessor & accessor, std::string_view rawPath, SymlinkResolution mode = SymlinkResolution::Full)
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
