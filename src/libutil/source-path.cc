#include "nix/util/source-path.hh"
#include "nix/util/source-root.hh"

namespace nix {

std::string_view SourcePath::baseName() const
{
    return path.baseName().value_or("source");
}

SourcePath SourcePath::parent() const
{
    auto p = path.parent();
    assert(p);
    return {accessor, std::move(*p)};
}

std::string SourcePath::readFile() const
{
    return accessor->readFile(path);
}

bool SourcePath::pathExists() const
{
    return accessor->pathExists(path);
}

SourceAccessor::Stat SourcePath::lstat() const
{
    return accessor->lstat(path);
}

std::optional<SourceAccessor::Stat> SourcePath::maybeLstat() const
{
    return accessor->maybeLstat(path);
}

SourceAccessor::DirEntries SourcePath::readDirectory() const
{
    return accessor->readDirectory(path);
}

std::string SourcePath::readLink() const
{
    return accessor->readLink(path);
}

void SourcePath::dumpPath(Sink & sink, PathFilter & filter) const
{
    return accessor->dumpPath(path, sink, filter);
}

std::optional<std::filesystem::path> SourcePath::getPhysicalPath() const
{
    return accessor->getPhysicalPath(path);
}

std::string SourcePath::to_string() const
{
    return accessor->showPath(path);
}

SourcePath SourcePath::operator/(const CanonPath & x) const
{
    return {accessor, path / x};
}

SourcePath SourcePath::operator/(std::string_view c) const
{
    return {accessor, path / c};
}

bool SourcePath::operator==(const SourcePath & x) const noexcept
{
    return std::tie(*accessor, path) == std::tie(*x.accessor, x.path);
}

std::strong_ordering SourcePath::operator<=>(const SourcePath & x) const noexcept
{
    return std::tie(*accessor, path) <=> std::tie(*x.accessor, x.path);
}

std::ostream & operator<<(std::ostream & str, const SourcePath & path)
{
    str << path.to_string();
    return str;
}

std::ostream & operator<<(std::ostream & str, const RootedPath & path)
{
    str << path.sourcePath();
    return str;
}

bool RootedPath::operator==(const RootedPath & x) const noexcept
{
    return path == x.path && root->kind == x.root->kind && *root->accessor == *x.root->accessor;
}

std::strong_ordering RootedPath::operator<=>(const RootedPath & x) const noexcept
{
    /* Lex compare on (path, kind, accessor) — chosen so that
       displays sort intuitively (paths first), with kind and
       accessor identity as tiebreakers. Pointer identity of the
       SourceRoot is irrelevant: two roots wrapping the same
       (accessor, kind) compare equal. */
    if (auto cmp = path <=> x.path; cmp != 0)
        return cmp;
    if (auto cmp = root->kind <=> x.root->kind; cmp != 0)
        return cmp;
    return *root->accessor <=> *x.root->accessor;
}

} // namespace nix
