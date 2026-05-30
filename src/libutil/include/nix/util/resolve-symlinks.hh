#pragma once

#include <filesystem>
#include <list>

#include "nix/util/canon-path.hh"
#include "nix/util/error.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/strings.hh"

namespace nix {

/**
 * Resolve any symlinks in `path` against `accessor` according to the
 * given resolution mode.
 *
 * Free-function form of `SourceAccessor::resolveSymlinks`. Header-only
 * so the implementation lives next to the declaration and so future
 * extensions (policy callbacks, alternative input shapes, …) can grow
 * the API without churning `source-accessor.hh`.
 *
 * @param mode might only be a temporary solution. See the discussion
 * in https://github.com/NixOS/nix/pull/9985.
 */
inline CanonPath
resolveSymlinks(SourceAccessor & accessor, const CanonPath & path, SymlinkResolution mode = SymlinkResolution::Full)
{
    auto res = CanonPath::root;

    int linksAllowed = 1024;

    std::list<std::string> todo;
    for (auto & c : path)
        todo.push_back(std::string(c));

    while (!todo.empty()) {
        auto c = *todo.begin();
        todo.pop_front();
        if (c == "" || c == ".")
            ;
        else if (c == "..") {
            if (!res.isRoot())
                res.pop();
        } else {
            res.push(c);
            if (mode == SymlinkResolution::Full || !todo.empty()) {
                if (auto st = accessor.maybeLstat(res); st && st->type == SourceAccessor::tSymlink) {
                    if (!linksAllowed--)
                        throw Error("infinite symlink recursion in path '%s'", accessor.showPath(path));
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
    }

    return res;
}

} // namespace nix
