#include "nix/rust/future-abi.hh"

#include <atomic>
#include <stdexcept>
#include <string>
#include <string_view>

/* The C functions of `future-abi.h` that concern exceptions. They are in
   libnixrust because both the Rust code in libnixrust and the C++ libraries
   above it use them, and libnixrust cannot refer to the libraries above. */

namespace nix::rs {

struct RustValue::Shared
{
    nix_rust_error_kind kind;
    void (*drop)(void *);
    const void * owner;
    /* Exchanged, so that two threads that take the value at the same time
       cannot both get it. */
    std::atomic<void *> data;

    explicit Shared(nix_rust_value value)
        : kind(value.kind)
        , drop(value.drop)
        , owner(value.owner)
        , data(value.data)
    {
    }

    ~Shared()
    {
        if (auto * p = data.exchange(nullptr))
            drop(p);
    }
};

RustValue::RustValue(nix_rust_value value)
    : shared(std::make_shared<Shared>(value))
{
}

/* The key function of the class: its vtable and type information are
   strong symbols of libnixrust, so that a `catch` of `RustValue` in any
   library matches. */
RustValue::~RustValue() = default;

nix_rust_error_kind RustValue::rustErrorKind() const noexcept
{
    return shared->kind;
}

bool RustValue::takeRustValue(const void * owner, nix_rust_value & out) const noexcept
{
    if (shared->owner != owner)
        return false;
    auto * data = shared->data.exchange(nullptr);
    if (!data)
        return false;
    out = nix_rust_value{
        .kind = shared->kind,
        .data = data,
        .drop = shared->drop,
        .owner = shared->owner,
    };
    return true;
}

RustException::RustException(const std::string & message, nix_rust_value value)
    : std::runtime_error(message)
    , RustValue(value)
{
}

RustException::~RustException() = default;

void rethrowRustError(const ::rust::Error & error)
{
    std::string_view message = error.what();
    if (auto * original = nix_rust_take_pending_exception(message.data(), message.size()))
        rethrowException(original);
    throw;
}

} // namespace nix::rs

static std::atomic<nix_rust_error_factory> errorFactory{nullptr};

extern "C" {

void nix_exception_free(nix_exception * exception) noexcept
{
    delete exception;
}

const char * nix_exception_what(const nix_exception * exception) noexcept
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

/* `noexcept`: an allocation failure terminates the process, as an
   allocation failure in Rust aborts it. Rust cannot receive a C++
   exception. */
nix_exception * nix_exception_from_rust(const char * message, size_t length, nix_rust_value value) noexcept
{
    if (auto factory = errorFactory.load(std::memory_order_acquire))
        return factory(message, length, value);
    std::string text(message, length);
    if (value.kind == NIX_RUST_PANIC)
        text = "Rust panic: " + text;
    return new nix_exception{std::make_exception_ptr(nix::rs::RustException(text, value))};
}

bool nix_exception_take_rust_value(const nix_exception * exception, const void * owner, nix_rust_value * out) noexcept
{
    try {
        std::rethrow_exception(exception->ptr);
    } catch (const nix::rs::RustValue & value) {
        return value.takeRustValue(owner, *out);
    } catch (...) {
        return false;
    }
}

void nix_set_rust_error_factory(nix_rust_error_factory factory) noexcept
{
    errorFactory.store(factory, std::memory_order_release);
}

} // extern "C"
