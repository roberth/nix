#pragma once
///@file
///
/// Rust functions for the unit tests of the boundary between C++ and Rust,
/// in `libnix-rust-test-support`. See `src/librust-test-support/src/lib.rs`.
///
/// The library is a second Rust staticlib, apart from `libnixrust`, so that
/// the shipped library contains no test code. It shares no Rust types with
/// `libnixrust`, only the C ABI of `nix/rust/future-abi.h`, so these
/// functions are C functions.

#include "nix/rust/future-abi.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

extern "C" {

/**
 * An event log that a test owns and that the probe futures write to. A
 * future keeps a copy of this structure, so `context` must stay valid for
 * as long as the future exists. The future calls `push` only on the thread
 * that polls or drops it.
 */
typedef struct nix_rust_test_log
{
    void * context;
    void (*push)(void * context, const char * event, size_t length);
} nix_rust_test_log;

/**
 * Receives a string that a probe returns, during the call to the probe.
 */
typedef void (*nix_rust_test_string_sink)(void * context, const char * data, size_t length);

void nix_rust_test_catch_panic(nix_rust_test_string_sink sink, void * context);

nix_future nix_rust_test_future_ready(const char * value, size_t length);

nix_future nix_rust_test_future_error(const char * message, size_t length);

nix_future nix_rust_test_future_panic(const char * message, size_t length);

nix_future nix_rust_test_future_forward(nix_future inner, const nix_rust_test_log * log);

nix_future nix_rust_test_future_pending(const nix_rust_test_log * log);

nix_future nix_rust_test_future_ticking(const nix_rust_test_log * log, uint64_t interval_ms);

size_t nix_rust_test_ticking_polls(void);

nix_future nix_rust_test_future_woken_from_thread(uint64_t delay_ms, uint64_t late_wake_ms);

size_t nix_rust_test_wake_threads_running(void);

void nix_rust_test_waker_roundtrip(const nix_waker * waker, nix_rust_test_string_sink sink, void * context);

/**
 * The version of the Rust compiler that built the library, for example
 * `1.95.0`. The same compiler builds `libnixrust`.
 */
const char * nix_rust_test_rustc_version(void);

} // extern "C"

namespace nix::rs::tests {

/**
 * A `nix_rust_test_log` that calls `push`. It cannot move, because the
 * futures keep its address.
 */
struct ProbeLog
{
    std::function<void(std::string_view event)> push;

    nix_rust_test_log c{
        .context = this,
        .push = [](void * context,
                   const char * event,
                   size_t length) { static_cast<ProbeLog *>(context)->push(std::string_view(event, length)); },
    };

    explicit ProbeLog(std::function<void(std::string_view event)> push)
        : push(std::move(push))
    {
    }

    ProbeLog(const ProbeLog &) = delete;
    ProbeLog & operator=(const ProbeLog &) = delete;

    operator const nix_rust_test_log *() const
    {
        return &c;
    }
};

inline nix_future futureReady(std::string_view value)
{
    return nix_rust_test_future_ready(value.data(), value.size());
}

inline nix_future futureError(std::string_view message)
{
    return nix_rust_test_future_error(message.data(), message.size());
}

inline nix_future futurePanic(std::string_view message)
{
    return nix_rust_test_future_panic(message.data(), message.size());
}

/**
 * Call `probe` with a `nix_rust_test_string_sink`, and return the string
 * that it receives.
 */
template<typename F>
std::string probeString(F && probe)
{
    std::string result;
    probe(
        [](void * context, const char * data, size_t length) {
            static_cast<std::string *>(context)->assign(data, length);
        },
        &result);
    return result;
}

} // namespace nix::rs::tests
