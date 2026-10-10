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
  The waker is two pointers with the layout of Rust's `RawWaker` (`future-abi.h`, a C header).
  `toCFuture` and `awaitCFuture` cross that boundary in either direction.

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
