#pragma once
/**
 * @file
 *
 * The C ABI of a poll-based future and its waker.
 *
 * The layout follows Rust's `core::task::RawWaker` and `Future::poll`, so
 * that C++ tasks (`nix::Task`) and Rust futures can drive each other
 * through plain function pointers. This header is valid C.
 *
 * Ownership rules (the same as Rust's `RawWakerVTable`):
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
 * A type-erased future.
 *
 * `poll` drives the future. It returns `NIX_POLL_PENDING` after it
 * arranged for `waker` (or a clone of it) to be woken when progress is
 * possible, or `NIX_POLL_READY` when the future completed. The `waker`
 * pointer is valid only for the duration of the call. A completed future
 * must not be polled again.
 *
 * `drop` destroys the future. Dropping a future that is not complete
 * cancels it: the future stops, releases its resources and wakes nobody.
 *
 * The result of a completed future is not part of this vtable. A typed
 * accessor next to the function that created the future retrieves it,
 * because the C ABI cannot express the result type generically.
 */
typedef struct nix_future_vtable
{
    nix_poll (*poll)(void * self, const nix_waker * waker);
    void (*drop)(void * self);
} nix_future_vtable;

typedef struct nix_future
{
    void * self;
    const nix_future_vtable * vtable;
} nix_future;

#ifdef __cplusplus
}
#endif
