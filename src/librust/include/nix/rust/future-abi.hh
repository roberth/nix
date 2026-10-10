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

struct nix_exception
{
    std::exception_ptr ptr;
};

namespace nix::rs {

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
