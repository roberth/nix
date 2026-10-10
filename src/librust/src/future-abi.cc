#include "nix/rust/future-abi.hh"

#include <atomic>
#include <stdexcept>
#include <string>

/* The C functions of `future-abi.h` that concern exceptions. They are in
   libnixrust because both the Rust code in libnixrust and the C++ libraries
   above it use them, and libnixrust cannot refer to the libraries above. */

static std::atomic<nix_rust_error_factory> errorFactory{nullptr};

extern "C" {

void nix_exception_free(nix_exception * exception)
{
    delete exception;
}

const char * nix_exception_what(const nix_exception * exception)
{
    try {
        std::rethrow_exception(exception->ptr);
    } catch (const std::exception & e) {
        /* The exception object lives as long as `exception->ptr`, so the
           string does too. */
        return e.what();
    } catch (...) {
        return "unknown C++ exception";
    }
}

nix_exception * nix_exception_from_rust(nix_rust_error_kind kind, const char * message, size_t length)
{
    if (auto factory = errorFactory.load(std::memory_order_acquire))
        return factory(kind, message, length);
    std::string text(message, length);
    if (kind == NIX_RUST_PANIC)
        text = "Rust panic: " + text;
    return new nix_exception{std::make_exception_ptr(std::runtime_error(text))};
}

void nix_set_rust_error_factory(nix_rust_error_factory factory)
{
    errorFactory.store(factory, std::memory_order_release);
}

} // extern "C"
