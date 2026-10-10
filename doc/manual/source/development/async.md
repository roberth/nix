# Asynchronous code

New asynchronous code in Nix uses `nix::Task<T>` (`src/libutil/include/nix/util/task.hh`).
A task is a C++20 coroutine with the semantics of a Rust future.
This page states the contract and the reasons for it.

## The contract

- A task is lazy.
  Creating a task does not run its body.
  The body runs when the task is first polled or awaited.
  A task that nobody awaits therefore does no work, and building a list of tasks before awaiting them all is cheap.

- A task has one owner.
  It is move-only.
  Exactly one object owns it: a variable, a combinator, or the task that awaits it.

- Destroying a task that is not complete cancels it.
  The coroutine frame is destroyed at its suspension point, which runs the destructors of its locals.
  The task that it awaits is one of those locals, so the cancellation reaches the bottom of the chain.
  The operation at the bottom (a *leaf*, for example an HTTP transfer) stops in its destructor.
  Nothing resumes a cancelled task.

  Boost.Asio, the previous model, cancels only through per-operation slots, and destroying an awaitable does not stop the operation under it.
  Rust has the same model as `Task`, so code on both sides of an FFI boundary agrees on what cancellation means.

- An exception that escapes the body is stored as an `std::exception_ptr` and rethrown to the awaiter, so it keeps its dynamic type.

- A task is driven by `poll(waker)`.
  `poll` returns `NIX_POLL_READY` when the task is complete.
  Otherwise it returns `NIX_POLL_PENDING`, after the task arranged for the waker to be woken when it can make progress.
  The waker is two pointers with the layout of Rust's `RawWaker` (`nix/rust/future-abi.h`, a C header).
  `toCFuture` and `awaitCFuture` cross that boundary in either direction; see [Tasks and Rust futures](#tasks-and-rust-futures).

  When a task awaits another task, control transfers directly between the two coroutines.
  Only the outermost task of a chain is polled.
  `poll` polls the leaf that the chain is suspended on and resumes the chain only when that leaf is ready, so a spurious poll does not resume anything.

## The cancel-safety rule

Every `co_await` is a point where the task can be destroyed.
Destruction does not run `catch` blocks, and code after the `co_await` does not run.
Cleanup must therefore be in destructors (RAII), not in `catch (...)` blocks or at the end of the function.

Two consequences:

- Do not write to an output parameter in several steps across a `co_await`.
  A cancelled task can leave the output half-written.
  Return a value instead.
  This is why `Store::querySubstitutablePathInfos` returns `SubstitutablePathInfos` instead of filling a map.
- An exchange that must not be interrupted, such as a request and its response on a daemon connection, must not contain a `co_await`.
  `RemoteStore::querySubstitutablePathInfos` does the whole exchange before its task suspends for the first time.

## Running tasks

- `runBlocking(task)` runs a task on the current thread.
  A user interrupt wakes it, throws `Interrupted`, and destroys the task.
- `asyncRunTask(task, token)` and `taskToAwaitable(task)` run a task as a Boost.Asio operation.
  All polls run on the handler's executor.
  An Asio cancellation destroys the task.
- `awaitableToTask(executor, awaitable)` awaits a Boost.Asio awaitable from a task.
  Destroying the task emits a terminal cancellation to the awaitable.
  Asio cancellation is cooperative, so the awaitable can run until its next cancellable operation.

## Leaves and combinators

- `callbackToTask<T>(initiate)` adapts an operation that reports through `Callback<T>`.
  If `initiate` returns a callable, destroying the task calls it to cancel the operation.
  `FileTransfer::transfer` uses this to remove the transfer from curl.
- `whenAll(tasks)` awaits tasks concurrently.
  The first error cancels the others.
  Each child has its own waker, so a wake polls only the child that woke.
- `race(a, b)` returns the result of the first task to complete and cancels the other.

There is no detached spawn.
A task always has an owner, so its lifetime is visible in the code.

## Tasks and Rust futures

A C++ task and a Rust future can await each other, through the C ABI in `nix/rust/future-abi.h`.
The ABI is in libnixrust, the lowest library, because both languages use it.

- `toCFuture(task)` boxes a task as a `nix_future`.
  Rust awaits it as a `CxxFuture<T>` (`src/librust/src/future.rs`).
- Rust boxes a future with `export_future`.
  C++ awaits the `nix_future` with `awaitCFuture<T>(future)`.

Both sides have the same semantics, so a chain of `co_await` and `.await` can cross the boundary at any point:

- Dropping the owner cancels the chain on both sides.
  Dropping a task drops the Rust future that it awaits, and dropping a `CxxFuture` destroys the task that it awaits, down to the leaf operation.
- A waker crosses as two pointers.
  A waker that comes back to its own language is unwrapped, so a chain that crosses the boundary several times wakes through one level of indirection.
- The output crosses by value, in storage that the receiver provides (`nix_future_vtable::take`).
  Only types whose layout cxx guarantees to be the same in both languages can cross: `void`/`()`, `bool`, `uint64_t`/`u64`, `int64_t`/`i64`, `rust::String`/`String` and `rust::Vec<uint8_t>`/`Vec<u8>`.
  Both sides name the type (`nix::rs::AbiType`, the Rust trait `AbiType`), and the receiver checks the name before it accepts the value.
  A mismatch aborts, instead of corrupting memory.
  To let a future carry another cxx type, for example `rust::Box<T>` of an opaque Rust type, add the same name on both sides.
- An error crosses as a `nix_exception`, which owns an `std::exception_ptr`.
  A C++ exception that travels through Rust and back keeps its dynamic type, because Rust holds it without looking inside.
  A Rust `Err` and a Rust panic become a `nix::Error`.
  libutil makes these exceptions through a factory that it registers with libnixrust at run time, because libnixrust cannot refer to libutil at link time.
  A panic never unwinds into C++.

A `CxxFuture` is `Send`, so a C++ task that crosses into Rust can be polled on any thread, one thread at a time.
A task that keeps thread-local state across a `co_await`, such as a `PushActivity`, is therefore not fit to cross.

### What remains for a Rust executor

Today C++ drives every chain: `runBlocking` or the Asio runner polls the outermost task, and Rust futures run inside it.
No Rust code spawns a task or owns a reactor.
To run a chain on tokio, for example for network code in Rust, the following is necessary and does not exist yet:

- A tokio runtime with a `LocalSet` for futures that contain C++ tasks, or an argument that the C++ tasks are thread-safe, so that a multi-threaded runtime can poll them.
- A C++ entry point that submits a `nix_future` to the runtime and returns a task that completes with it, so that C++ code can wait for tokio without blocking an Asio thread.
- Interrupt handling: `runBlocking` turns a user interrupt into a drop of the task, and a tokio-driven chain needs the same, for example by racing it against an interrupt future.
- A decision about which reactor owns the curl multi handle, which is the same question for C++ code with or without Rust.
