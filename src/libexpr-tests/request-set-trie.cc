#include <gtest/gtest.h>

#include "nix/expr/request-set-trie.hh"

namespace nix::trace::rst {

/* HAMT tests are being reintroduced alongside the implementation.
   This file will be repopulated as the top-down bit-group HAMT design
   lands. See request-set-trie.hh for the intended public interface. */

TEST(RequestSetTrieTest, PublicInterfaceLinks)
{
    /* Placeholder: exercises just enough to prove the module links.
       Removed once the HAMT implementation begins landing tests. */
    FrozenNodeCache cache;
    (void) cache.lookup(tracingHash("nothing"));
}

} // namespace nix::trace::rst
