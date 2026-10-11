#pragma once
/**
 * @file
 *
 * The C ABI of a poll-based future, its waker, and the exceptions that its
 * result can carry.
 *
 * The layout follows Rust's `core::task::RawWaker` and `Future::poll`, so
 * that C++ tasks (`nix::Task`) and Rust futures can drive each other
 * through plain function pointers. This header is valid C.
 *
 * Ownership rules for wakers (the same as Rust's `RawWakerVTable`):
 *
 * - `clone` returns a new waker that refers to the same task. The caller
 *   owns the result.
 * - `wake` wakes the task and consumes the waker. The caller must not use
 *   or drop the waker afterwards.
 * - `wake_by_ref` wakes the task and does not consume the waker.
 * - `drop` releases the waker without waking the task.
 *
 * All four functions are thread-safe: a waker can be cloned, woken and
 * dropped on any thread.
 */

#include <stddef.h>

/* Rust calls the functions of this header, and the functions in the
   vtables, through `extern "C"`, which does not unwind. A C++ exception
   that leaves one of them is undefined behaviour, so the C++
   implementations are `noexcept`: an exception terminates the process
   instead. In the other direction, a Rust panic that leaves an
   `extern "C"` function aborts the process. */
#ifdef __cplusplus
#  define NIX_FUTURE_ABI_NOEXCEPT noexcept
#else
#  define NIX_FUTURE_ABI_NOEXCEPT
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nix_waker nix_waker;

typedef struct nix_waker_vtable
{
    nix_waker (*clone)(const void * data);
    void (*wake)(const void * data);
    void (*wake_by_ref)(const void * data);
    void (*drop)(const void * data);
} nix_waker_vtable;

struct nix_waker
{
    const void * data;
    const nix_waker_vtable * vtable;
};

typedef enum nix_poll {
    NIX_POLL_READY = 0,
    NIX_POLL_PENDING = 1,
} nix_poll;

/**
 * A C++ exception that crosses the language boundary: an owned,
 * heap-allocated `std::exception_ptr`.
 *
 * The holder of the pointer owns the exception. Rust code holds it
 * opaquely and gives it back to C++, which rethrows it with its dynamic
 * type. An error that starts in Rust becomes an exception through
 * `nix_exception_from_rust`.
 */
typedef struct nix_exception nix_exception;

/**
 * Release an exception without rethrowing it.
 */
void nix_exception_free(nix_exception * exception) NIX_FUTURE_ABI_NOEXCEPT;

/**
 * The `what()` string of the exception, or a fixed text if it is not a
 * `std::exception`. The string is valid as long as the exception is.
 */
const char * nix_exception_what(const nix_exception * exception) NIX_FUTURE_ABI_NOEXCEPT;

typedef enum nix_rust_error_kind {
    /** A Rust `Err` value. */
    NIX_RUST_ERROR = 0,
    /** A Rust panic. */
    NIX_RUST_PANIC = 1,
} nix_rust_error_kind;

/**
 * Make an exception from a Rust error message. The message is UTF-8 and
 * need not be null-terminated.
 *
 * The exception has the type that the registered factory gives it
 * (`nix::Error` once libutil registered its factory), and
 * `std::runtime_error` otherwise.
 */
nix_exception *
nix_exception_from_rust(nix_rust_error_kind kind, const char * message, size_t length) NIX_FUTURE_ABI_NOEXCEPT;

/**
 * A factory for `nix_exception_from_rust`. It must not throw.
 */
typedef nix_exception * (*nix_rust_error_factory)(nix_rust_error_kind kind, const char * message, size_t length);

/**
 * Set the function that `nix_exception_from_rust` uses. A library above
 * libnixrust registers it at run time, so that libnixrust does not refer
 * to the library's exception types at link time.
 */
void nix_set_rust_error_factory(nix_rust_error_factory factory) NIX_FUTURE_ABI_NOEXCEPT;

/**
 * A type-erased future.
 *
 * `poll` drives the future. It returns `NIX_POLL_PENDING` after it
 * arranged for `waker` (or a clone of it) to be woken when progress is
 * possible, or `NIX_POLL_READY` when the future completed. The `waker`
 * pointer is valid only for the duration of the call. A completed future
 * must not be polled again.
 *
 * `take` retrieves the result of a completed future, once. On success it
 * moves the output into `out` and returns null. On failure it returns the
 * exception and leaves `out` unchanged. `out` points to uninitialized
 * storage for one value of the output type.
 *
 * `output_type` names the output type, so that both sides can check that
 * they agree on it before `take` writes into `out`. The names are those
 * of the Rust types, for example `String` or `()`. Both sides compare the
 * strings, not the pointers.
 *
 * `drop` destroys the future. Dropping a future that is not complete
 * cancels it: the future stops, releases its resources and wakes nobody.
 */
typedef struct nix_future_vtable
{
    nix_poll (*poll)(void * self, const nix_waker * waker);
    nix_exception * (*take)(void * self, void * out);
    void (*drop)(void * self);
    const char * output_type;
} nix_future_vtable;

typedef struct nix_future
{
    void * self;
    const nix_future_vtable * vtable;
} nix_future;

#ifdef __cplusplus
}
#endif
