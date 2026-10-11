#include <gtest/gtest.h>

#include "nix/util/error.hh"
#include "nix/util/rust-error.hh"
#include "nix/util/task.hh"
#include "nix/rust/tests/probes.hh"

#include <cerrno>
#include <typeinfo>

namespace nix {

/* A value that crosses the boundary keeps its identity, and comes back out
   unchanged when it crosses back: a C++ exception is the same exception
   object, a Rust error the same allocation, and a Rust panic resumes with
   the same payload. The Rust side is
   `src/librust-test-support/src/identity.rs`. */

namespace {

/**
 * A C++ task without a value that rethrows `exception`.
 */
Task<void> rustIdentityRethrowing(std::exception_ptr exception)
{
    std::rethrow_exception(exception);
    co_return;
}

/**
 * The exception of a Rust future without a value.
 */
std::exception_ptr rustIdentityFailureOf(nix_future future)
{
    try {
        runBlocking(awaitCFuture<void>(future));
    } catch (...) {
        return std::current_exception();
    }
    return nullptr;
}

} // namespace

TEST(RustIdentity, rustErrorIsARustErrorInCxx)
{
    auto exception = rustIdentityFailureOf(nix_rust_test_future_probe_error(1));
    try {
        std::rethrow_exception(exception);
    } catch (const Error & e) {
        EXPECT_EQ(typeid(e), typeid(RustError));
        EXPECT_EQ(e.info().msg.str(), "probe error 1");
    }
}

TEST(RustIdentity, rustErrorComesBackAsTheSameValue)
{
    auto exception = rustIdentityFailureOf(nix_rust_test_future_probe_error(2));
    auto description = runBlocking(
        awaitCFuture<rust::String>(nix_rust_test_future_identify(toCFuture(rustIdentityRethrowing(exception)))));
    EXPECT_EQ(std::string(description), "same Rust error 2");
}

TEST(RustIdentity, rustPanicIsARustPanicInCxx)
{
    auto exception = rustIdentityFailureOf(nix_rust_test_future_probe_panic(3));
    try {
        std::rethrow_exception(exception);
    } catch (const Error & e) {
        EXPECT_EQ(typeid(e), typeid(RustPanic));
        EXPECT_EQ(e.info().msg.str(), "Rust panic: probe panic 3");
    }
}

TEST(RustIdentity, rustPanicResumesWithTheSamePayload)
{
    auto exception = rustIdentityFailureOf(nix_rust_test_future_probe_panic(4));
    auto description = runBlocking(
        awaitCFuture<rust::String>(nix_rust_test_future_identify(toCFuture(rustIdentityRethrowing(exception)))));
    EXPECT_EQ(std::string(description), "same panic: probe panic 4");
}

TEST(RustIdentity, cxxExceptionComesBackAsTheSameObject)
{
    std::exception_ptr original;
    try {
        throw SysError(ENOENT, "opening a file");
    } catch (...) {
        original = std::current_exception();
    }
    auto inner = [](std::exception_ptr exception) -> Task<rust::String> {
        std::rethrow_exception(exception);
        co_return rust::String();
    }(original);
    rs::tests::ProbeLog log([](std::string_view) {});
    try {
        runBlocking(awaitCFuture<rust::String>(nix_rust_test_future_forward(toCFuture(std::move(inner)), log)));
        FAIL() << "the future did not fail";
    } catch (const SysError &) {
        EXPECT_EQ(std::current_exception(), original);
    }
}

} // namespace nix
