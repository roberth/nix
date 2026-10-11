#include <gtest/gtest.h>

#include "nix/util/file-system.hh"
#include "nix/util/rust-error.hh"
#include "nix/util/task.hh"
#include "nix/rust/lib.rs.h"
#include "nix/rust/tarball_cache.rs.h"
#include "nix/rust/tests/probes.hh"

namespace nix {

/* These tests cross the boundary between the C++ libraries and the Rust
   code: the cxx bridges of `libnixrust`, and the Rust test support library
   `libnix-rust-test-support` (`src/librust-test-support/src/lib.rs`). */

TEST(rust, returnsValue)
{
    /* The same compiler builds both Rust libraries. */
    EXPECT_EQ(std::string(rs::rustc_version()), std::string(nix_rust_test_rustc_version()));
}

TEST(rust, errBecomesException)
{
    auto tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir, true);

    detail::registerRustErrorFactory();
    auto reader = rs::call([&] { return rs::tarball_cache_reader_open(tmpDir.string()); });
    try {
        rs::call([&] { return reader->read_blob(std::array<uint8_t, 20>{}); });
        FAIL() << "read_blob returned normally";
    } catch (const Error & e) {
        /* The Rust error itself, not only its message. */
        EXPECT_EQ(typeid(e), typeid(RustError));
        EXPECT_EQ(
            e.info().msg.str(),
            "Git object '0000000000000000000000000000000000000000' does not exist in the tarball cache");
    }
}

TEST(rust, panicUnwindsThroughRustFrames)
{
    EXPECT_EQ(rs::tests::probeString(nix_rust_test_catch_panic), "caught: probe panic");
}

} // namespace nix
