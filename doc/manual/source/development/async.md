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
- `TaskSet<T>` is a set of tasks that complete in any order, like `FuturesUnordered` in Rust.
  The owner pushes tasks at any time and awaits `next()` for the result of the task that completes first.
  It is for work that grows while it runs, which `whenAll` cannot take.
  Destroying the set cancels the tasks in it.
- `computeClosure(start, getEdges, known, maxConcurrent)` traverses a graph with a `TaskSet`, and returns the closure.
  An error in one edge, or a drop of the traversal, cancels the edges that run.
  An edge task can refer to its element by reference: the element is in the result set, whose nodes do not move.
- `runInThread<T>(fn)` runs blocking code on a thread of its own, as a task.
  The thread starts on the first poll.
  Dropping the task makes `checkInterrupt()` on that thread throw `Interrupted`, calls the callbacks that the thread registered with `onThreadCancelled`, and then joins the thread.
  A blocking wait registers such a callback to wake up; `FileTransfer::download(request, sink)` cancels its transfer in it.
  The cancellation is per task: it does not set the global interrupt flag.

There is no detached spawn.
A task always has an owner, so its lifetime is visible in the code.
The build `Worker` is the one place where several owners share a task; see [Goals and the Worker](#goals-and-the-worker).

## Goals and the Worker

The build `Worker` (`src/libstore/build/`) runs goals: substitutions, builds, and goals that only wait for other goals.
The work of a goal is a `Task<Goal::ExitCode>`, and goal code is ordinary task code.
It awaits other tasks, for example `Store::queryPathInfoAsync`, and the leaves below.

The `Worker` is the executor of the goals.
It polls the task of a goal when the goal's waker wakes.
The waker puts the goal in a queue of the `Worker`, from any thread; a wake from another thread also wakes the `Worker` from its wait for input.
The `Worker` is also the reactor of the goals: it waits for the output of child processes and for timeouts, and wakes the goals.

| Leaf | What wakes it |
|------|---------------|
| `waitForChildEvent()` | Output, EOF or a timeout of a child process of the goal. |
| `waitForBuildSlot()` | A free build or substitution slot. The goal waits in a `WaitQueue` of the `Worker`. |
| `waitForAWhile()` | The poll interval for locks, in a `WaitQueue`. |
| `yield()` | The goal itself, so that the other awake goals run first. |
| `await(goals)` | The completion of the goals. Without `--keep-going`, the first failure ends the wait. |

The entry of a `WaitQueue` lives in the leaf, so a cancelled goal leaves the queue and does not take the turn of a goal that still waits.

### Where the contract does not fit

- A goal is shared.
  The `Worker` deduplicates goals by store path, and several goals can wait for the same goal.
  A task has one owner, so a goal is a shared object (`GoalPtr`) that owns its task.
  The last `GoalPtr` to go destroys the goal, and with it the task: that is the cancellation by drop.
  To wait for goals is therefore a leaf that holds `GoalPtr`s, not an awaited task.
  Without `--keep-going`, the first failure releases the other goals, which cancels those that nothing else holds.
- The `Worker` reports a deadlock when no goal can continue.
  A waker is opaque, so the `Worker` cannot see who will wake a goal.
  It counts the goals that suspended on a leaf that it does not wake itself, for example a transfer or a thread (`Worker::nrGoalsSuspendedElsewhere`).
  While such goals exist, it waits for wakes instead of reporting a deadlock.
- `Worker::run` blocks the thread.
  The `Worker` is an executor and a reactor of its own, not a task: it waits in `poll()` for child processes and for its wakeup pipe.
  A user interrupt wakes it through an interrupt callback.

### Cancellation

- Without `--keep-going`, a failed goal releases the goals that its waiters waited for, and a failed top-level goal ends `Worker::run`.
  The goals that nothing holds any more are destroyed, and their tasks are cancelled.
- A user interrupt makes `Worker::run` throw `Interrupted`, which destroys the goals while it unwinds.
- A substitution copies the store path with `runInThread`.
  The drop of the goal cancels the download of the NAR through `onThreadCancelled`, also when the substituter stalls.
  Before, the goal joined a thread that downloaded and imported the whole NAR.
  The functional test `substitute-cancel` checks this.
  In three runs, the build ended 165 to 176 ms after it started.
  With the previous `Worker` and `stalled-download-timeout` set to 20 s, it ended after 26 s.

## Migrating code to tasks

The closure traversal and the build `Worker` moved from Boost.Asio and from a coroutine type of their own (`Goal::Co`) to `Task`.
These steps worked:

1. Replace output parameters with return values, and assign the output of a blocking wrapper only on success.
   A cancelled task then leaves nothing half-written.
2. Replace callbacks and Asio awaitables with the `Task` methods of the store, or with `callbackToTask`.
   `Store::queryPathInfoAsync` replaces `queryPathInfo(path, callback)`.
3. Replace a custom suspension point with a leaf that stores the waker that it gets, and wakes it when the event happens.
   Keep the bodies of the coroutines; only the suspension points change.
4. Move blocking code that a drop must stop to `runInThread`, and make its waits register with `onThreadCancelled`.
5. Rename the coroutine type to `Task` in a separate, mechanical commit.

Three things change for the code that moves:

- An exception now unwinds the coroutines that await the one that throws.
  `Goal::Co` passed it to the `Worker` and left the awaiting coroutines suspended, so their locals (for example output locks) were released only when the goal was destroyed.
- A coroutine parameter that is a reference must outlive the task.
  A lambda that takes `const T &` and is called on an element of a queue that the caller pops refers to freed memory when it first runs.
- In the package builds, the source files of a component, its tests included, are one unity build, so names in anonymous namespaces must not repeat across files.

The cost, in lines added and removed per component, for the commits of this migration:

| Component | Change | Lines |
|-----------|--------|-------|
| `nix/util/task.hh` | `TaskSet` | +161 |
| `nix/util/closure.hh` | Task form, Asio form removed | +59 −166 |
| `nix/util/async.hh` | Asio helpers removed, no user left | −122 |
| `nix/util/task-thread.hh`, `task.cc` | `runInThread`, `onThreadCancelled` | +267 −7 |
| Callers of `computeClosure` (`misc.cc`, `http-binary-cache-store.cc`) | Task edges | +23 −40 |
| `Goal`, `Worker` (`goal.hh`, `goal.cc`, `worker.hh`, `worker.cc`, `goal-impl.hh`) | Executor, leaves, wait queues | +592 −764 |
| `PathSubstitutionGoal` | Task queries, `runInThread`, slot guard | +37 −79 |
| `DrvOutputSubstitutionGoal` | `callbackToTask` | +4 −4 |
| `DerivationBuildingGoal` | `waitForChildEvent()` | +4 −4 |
| `DerivationGoal`, `DerivationResolutionGoal`, `DerivationTrampolineGoal` | None | 0 |
| `FileTransfer::download` | Cancel on drop | +6 |
| All goal classes | Rename `Goal::Co` to `Task` | +54 −60 |
| `DerivationBuildingGoal` | Guard for the post-build hook on cancellation; the bug is older than the migration | +39 −24 |

The tests are not in the table: they add 741 lines and remove 28 lines of the old closure test.

### What remains for the Worker

- The `Worker` is not a task, so a task cannot await a build.
  To make `Worker::run` a task needs a reactor for file descriptors and timers that tasks share, which does not exist yet; `runBlocking` only waits for wakes.
  It is the same question as which reactor owns the curl multi handle.
- Goal code still calls blocking store operations on the `Worker` thread, which stops all goals while they run:
  `computeFSClosure` (a nested `runBlocking`), `queryRealisation` in `DrvOutputSubstitutionGoal`, `isValidPath`, the registration of outputs, the hashing in `pathContentsGood`, the build hook protocol, and `copyClosure`.
  Each needs a task version, or `runInThread`.
  `computeFSClosure` is virtual (`LegacySSHStore` overrides it), so its task version needs a virtual task method.
- Only `FileTransfer::download` registers with `onThreadCancelled`.
  A substitution from an SSH store or a daemon blocks in a socket read, which a drop does not interrupt; the drop then waits until the read returns.
- `queryRealisation` has no task version: `callbackToTask` discards its result on a drop but does not stop it.
- The Windows code of the `Worker` keeps its structure. It compiles and links in the cross build `nix-store-x86_64-w64-mingw32`, but no test ran on Windows.

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

### Example: fetching a tarball

A tarball fetch is a task from end to end; [The tarball cache on gitoxide](./tarball-cache.md#fetching-a-tarball-as-a-task) describes it.
It shows the patterns that a migration of other code can use:

- Blocking work behind a future.
  The Rust future starts a thread on its first poll, and the thread wakes the future when it is done.
  Dropping the future sets a cancellation token, wakes the thread from a blocking read, and joins it.
- A stream between two concurrent tasks.
  `FileTransfer::transferStreaming` writes the body of a transfer into a `TransferStream`, which a blocking reader on another thread reads.
  The owner closes the stream to wake the reader when it cancels both.
- A cancellation token per operation, instead of the global interrupt flag.
  `runBlocking` turns a user interrupt into the destruction of the task, and the destruction reaches the operation through its owner.
- An exception that a C library swallows.
  libarchive turns an exception of its input into a message.
  The code that calls libarchive remembers the exception and rethrows it, so that it keeps its type.

### What remains for a Rust executor

Today C++ drives every chain: `runBlocking` or the Asio runner polls the outermost task, and Rust futures run inside it.
No Rust code spawns a task or owns a reactor.
To run a chain on tokio, for example for network code in Rust, the following is necessary and does not exist yet:

- A tokio runtime with a `LocalSet` for futures that contain C++ tasks, or an argument that the C++ tasks are thread-safe, so that a multi-threaded runtime can poll them.
- A C++ entry point that submits a `nix_future` to the runtime and returns a task that completes with it, so that C++ code can wait for tokio without blocking an Asio thread.
- Interrupt handling: `runBlocking` turns a user interrupt into a drop of the task, and a tokio-driven chain needs the same, for example by racing it against an interrupt future.
- A decision about which reactor owns the curl multi handle, which is the same question for C++ code with or without Rust.
- An asynchronous drop for futures that own a thread.
  The tarball unpacking joins its thread when it is dropped, which blocks the thread that drops it until the unpacking stops.
  Under `runBlocking` that is acceptable, but an executor thread must not block; a drop would have to hand the thread to a task that joins it.
