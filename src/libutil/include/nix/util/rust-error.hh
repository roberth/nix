#pragma once
///@file
///
/// The exceptions that carry a Rust error or a Rust panic through C++.
/// See `doc/manual/source/development/rust.md#panics-exceptions-and-unsafe`.

#include "nix/util/error.hh"
#include "nix/rust/future-abi.hh"

namespace nix {

/**
 * A Rust error that reached C++. It owns the Rust value, so that the same
 * value comes back out when the exception crosses back into Rust. C++ code
 * handles it as a `nix::Error` with the message of the Rust error.
 */
class RustError : public CloneableError<RustError, Error>, public rs::RustValue
{
    void anchor() override;

public:
    RustError(const std::string & message, nix_rust_value value)
        : CloneableError(message)
        , RustValue(value)
    {
    }
};

/**
 * A Rust panic that reached C++. It owns the panic payload, so that the
 * panic resumes with the same payload when the exception crosses back
 * into Rust. Its message is `Rust panic: <message of the panic>`.
 */
class RustPanic : public CloneableError<RustPanic, RustError>
{
    void anchor() override;

public:
    using CloneableError::CloneableError;
};

} // namespace nix
