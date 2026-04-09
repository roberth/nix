#include "nix/expr/environment.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

Hash Environment::getFileHash(const std::string & path)
{
    auto contents = fsRoot()->readFile(CanonPath(path));
    return hashString(HashAlgorithm::SHA256, contents);
}

} // namespace nix
