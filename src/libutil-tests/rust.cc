#include <gtest/gtest.h>

#include "nix/util/error.hh"
#include "nix/rust/probe.rs.h"

namespace nix {

/* These tests cross the boundary between the C++ libraries and `libnixrust`
   in both directions. See `src/librust/src/probe.rs`. */

TEST(rust, returnsValue)
{
    EXPECT_EQ(std::string(rs::probe_greet("Nix")), "Hello, Nix, from Rust!");
}

TEST(rust, errBecomesException)
{
    try {
        rs::probe_fail("no such thing");
        FAIL() << "probe_fail returned normally";
    } catch (const rust::Error & e) {
        EXPECT_EQ(std::string(e.what()), "no such thing");
    }
}

namespace {

struct ReturningCallback : rs::Callback
{
    rust::String call() const override
    {
        return "value from C++";
    }
};

struct ThrowingCallback : rs::Callback
{
    rust::String call() const override
    {
        throw Error("cannot do this");
    }
};

} // namespace

TEST(rust, upwardCallReturnsValue)
{
    EXPECT_EQ(std::string(rs::probe_call(ReturningCallback{})), "ok: value from C++");
}

TEST(rust, upwardCallExceptionBecomesErr)
{
    /* `Error::what()` contains the ANSI markup of the rendered message. The
       logger removes it later, when the output is not a terminal. */
    EXPECT_EQ(std::string(rs::probe_call(ThrowingCallback{})), "err: \x1B[31;1merror:\x1B[0m cannot do this");
}

TEST(rust, panicUnwindsThroughRustFrames)
{
    EXPECT_EQ(std::string(rs::probe_catch_panic()), "caught: probe panic");
}

} // namespace nix
