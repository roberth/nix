#pragma once
///@file
///
/// The C++ side of `future-abi.h`: the definition of `nix_exception`, and
/// the names of the types that a `nix_future` can carry.

#include "nix/rust/future-abi.h"
#include "rust/cxx.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

struct nix_exception
{
    std::exception_ptr ptr;
};

namespace nix::rs {

/**
 * A Rust value that a C++ exception owns: the error of a Rust `Err`, or the
 * payload of a Rust panic.
 *
 * A C++ exception type that carries a Rust value derives from this class.
 * When the exception crosses back into Rust, the Rust code takes the value
 * out (`nix_exception_take_rust_value`) and continues with the original
 * error, or resumes the original panic. Copies of the exception, for
 * example from `throwClone()`, share the value; the first take empties it
 * for all of them.
 */
class RustValue
{
    struct Shared;
    std::shared_ptr<Shared> shared;

public:
    explicit RustValue(nix_rust_value value);
    RustValue(const RustValue &) = default;
    RustValue(RustValue &&) = default;
    RustValue & operator=(const RustValue &) = default;
    RustValue & operator=(RustValue &&) = default;
    virtual ~RustValue();

    nix_rust_error_kind rustErrorKind() const noexcept;

    /**
     * Move the value to `out`, if `owner` made it and nobody took it yet.
     */
    bool takeRustValue(const void * owner, nix_rust_value & out) const noexcept;
};

/**
 * The exception for a Rust value while no library above registered a
 * factory (`nix_set_rust_error_factory`).
 */
class RustException : public std::runtime_error, public RustValue
{
public:
    RustException(const std::string & message, nix_rust_value value);
    ~RustException() override;
};

/**
 * Wrap the exception that is being handled. Call it in a `catch` block.
 */
inline nix_exception * currentException()
{
    return new nix_exception{std::current_exception()};
}

/**
 * Rethrow an exception that crossed the boundary, with its dynamic type,
 * and release the wrapper.
 */
[[noreturn]] inline void rethrowException(nix_exception * exception)
{
    std::unique_ptr<nix_exception> owned(exception);
    std::rethrow_exception(std::move(owned->ptr));
}

/**
 * The name of the Rust type that corresponds to `T` in
 * `nix_future_vtable::output_type`. The Rust trait `AbiType` gives the same
 * names (`src/librust/src/future.rs`).
 *
 * Only types whose layout cxx guarantees to be the same in both
 * languages have a name. A type without a specialization cannot be the
 * output of a future that crosses the boundary, and using it fails at
 * compile time.
 */
template<typename T>
struct AbiType;

template<>
struct AbiType<void>
{
    static constexpr const char * name = "()";
};

template<>
struct AbiType<bool>
{
    static constexpr const char * name = "bool";
};

template<>
struct AbiType<std::uint64_t>
{
    static constexpr const char * name = "u64";
};

template<>
struct AbiType<std::int64_t>
{
    static constexpr const char * name = "i64";
};

template<>
struct AbiType<::rust::String>
{
    static constexpr const char * name = "String";
};

template<>
struct AbiType<::rust::Vec<std::uint8_t>>
{
    static constexpr const char * name = "Vec<u8>";
};

} // namespace nix::rs
