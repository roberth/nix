#include <atomic>
#include "nix/util/source-accessor.hh"
#include "nix/util/resolve-symlinks.hh"

namespace nix {

void SourceAccessorError::anchor() {}

void FileNotFound::anchor() {}

void NotASymlink::anchor() {}

void NotADirectory::anchor() {}

void NotARegularFile::anchor() {}

void RestrictedPathError::anchor() {}

void SymlinkNotAllowed::anchor() {}

void AccessorBoundaryEscape::anchor() {}

static std::atomic<size_t> nextNumber{0};

bool SourceAccessor::Stat::isNotNARSerialisable()
{
    return this->type != tRegular && this->type != tSymlink && this->type != tDirectory;
}

std::string SourceAccessor::Stat::typeString()
{
    switch (this->type) {
    case tRegular:
        return "regular";
    case tSymlink:
        return "symlink";
    case tDirectory:
        return "directory";
    case tChar:
        return "character device";
    case tBlock:
        return "block device";
    case tSocket:
        return "socket";
    case tFifo:
        return "fifo";
    case tUnknown:
    default:
        return "unknown";
    }
    return "unknown";
}

SourceAccessor::SourceAccessor()
    : number(++nextNumber)
    , displayPrefix{"«unknown»"}
{
}

bool SourceAccessor::pathExists(const CanonPath & path)
{
    return maybeLstat(path).has_value();
}

std::string SourceAccessor::readFile(const CanonPath & path)
{
    StringSink sink;
    std::optional<uint64_t> size;
    readFile(path, sink, [&](uint64_t _size) {
        size = _size;
        sink.s.reserve(_size);
    });
    assert(size && *size == sink.s.size());
    return std::move(sink.s);
}

void SourceAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    auto s = readFile(path);
    sizeCallback(s.size());
    sink(s);
}

Hash SourceAccessor::hashPath(const CanonPath & path, PathFilter & filter, HashAlgorithm ha)
{
    HashSink sink(ha);
    dumpPath(path, sink, filter);
    return sink.finish().hash;
}

SourceAccessor::Stat SourceAccessor::lstat(const CanonPath & path)
{
    if (auto st = maybeLstat(path))
        return *st;
    else
        throw FileNotFound("path '%s' does not exist", showPath(path));
}

void SourceAccessor::setPathDisplay(std::string displayPrefix, std::string displaySuffix)
{
    this->displayPrefix = std::move(displayPrefix);
    this->displaySuffix = std::move(displaySuffix);
}

std::string SourceAccessor::showPath(const CanonPath & path)
{
    return displayPrefix + path.abs() + displaySuffix;
}

std::string SourceAccessor::showPath(std::string_view rawPath)
{
    return displayPrefix + std::string(rawPath) + displaySuffix;
}

CanonPath SourceAccessor::resolveSymlinks(const CanonPath & path, SymlinkResolution mode)
{
    return nix::resolveSymlinks(*this, path, mode);
}

} // namespace nix
