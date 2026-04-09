#include "nix/expr/environment.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* Out-of-line virtual destructor so the abstract base gets a key
   function — without it, clang's `-Wweak-vtables` reports the vtable
   as emitted in every TU. */
Environment::~Environment() = default;

Hash Environment::getFileHash(const std::string & path)
{
    auto contents = fsRoot()->readFile(CanonPath(path));
    return hashString(HashAlgorithm::SHA256, contents);
}

} // namespace nix
