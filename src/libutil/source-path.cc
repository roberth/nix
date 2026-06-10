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

namespace {

/* Compare two NAR nodes at their roots, optionally recursing into
   directory children. `lstat`s are passed in to avoid a double
   stat — the caller has already inspected them to short-circuit on a
   type mismatch. */
bool nodesEqual(
    const SourcePath & a,
    const SourceAccessor::Stat & sa,
    const SourcePath & b,
    const SourceAccessor::Stat & sb,
    bool recurse)
{
    /* The caller pre-checks types match; reasserting is cheap and
       keeps this function self-contained for direct hint use. */
    if (sa.type != sb.type)
        return false;

    switch (sa.type) {
    case SourceAccessor::tRegular:
        if (sa.isExecutable != sb.isExecutable)
            return false;
        return a.readFile() == b.readFile();
    case SourceAccessor::tSymlink:
        return a.readLink() == b.readLink();
    case SourceAccessor::tDirectory: {
        auto da = a.readDirectory();
        auto db = b.readDirectory();
        /* DirEntries is std::map<std::string, optional<Type>>; equal
           via std::map's lex compare gives us same name-set and
           same per-entry types in one shot. */
        if (da != db)
            return false;
        if (!recurse)
            return true;
        for (const auto & [name, _] : da) {
            auto ca = a / name;
            auto cb = b / name;
            auto sca = ca.lstat();
            auto scb = cb.lstat();
            if (sca.type != scb.type)
                return false;
            if (!nodesEqual(ca, sca, cb, scb, /*recurse=*/true))
                return false;
        }
        return true;
    }
    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
        /* Not part of the NAR model. Conservatively unequal — even
           if they were the same kind, NAR would refuse to serialise
           them. */
        return false;
    }
    unreachable();
}

} // namespace

bool contentsEqual(ref<SourceAccessor> a, ref<SourceAccessor> b, std::optional<CanonPath> hint)
{
    /* Step 1: same accessor pointer is the trivial yes. */
    if (&*a == &*b)
        return true;

    /* Step 2: fingerprint shortcut at the root. Both accessors
       must produce a fingerprint and agree on it (and on the
       internal path that fingerprint covers). */
    auto [fpaPath, fpa] = a->getFingerprint(CanonPath::root);
    auto [fpbPath, fpb] = b->getFingerprint(CanonPath::root);
    if (fpa && fpb && *fpa == *fpb && fpaPath == fpbPath)
        return true;

    SourcePath rootA{a, CanonPath::root};
    SourcePath rootB{b, CanonPath::root};

    /* Step 3: optional hint discriminator. Cheap subpath check
       meant to fail fast for definitely-unequal trees (think
       `flake.nix` for two flake roots). Low-level: we lstat the
       hint directly, no symlink resolution. */
    if (hint) {
        auto subA = rootA / *hint;
        auto subB = rootB / *hint;
        auto hsA = subA.maybeLstat();
        auto hsB = subB.maybeLstat();
        if (hsA && hsB && !nodesEqual(subA, *hsA, subB, *hsB, /*recurse=*/false))
            return false;
    }

    /* Step 4: full recursive NAR-semantics comparison from the
       roots. Treat both-roots-absent as unequal — callers should
       not be asking "are these two non-trees the same?" and false
       is the safer answer to misuse. */
    auto sa = rootA.maybeLstat();
    auto sb = rootB.maybeLstat();
    if (!sa || !sb)
        return false;
    if (sa->type != sb->type)
        return false;
    return nodesEqual(rootA, *sa, rootB, *sb, /*recurse=*/true);
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
