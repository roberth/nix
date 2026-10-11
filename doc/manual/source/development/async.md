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
- `runOnReactor(task)` runs a task on the [reactor](#the-reactor) thread, and blocks the current thread until the task and its [cleanups](#asynchronous-drop) are done.
  A user interrupt destroys the task on the reactor thread, waits for the cleanups, and throws `Interrupted`.
  It must not be called on the reactor thread, which would wait for itself.
- `waitForInterrupt()` is a task that throws `Interrupted` on a user interrupt, or when the `runInThread` task of its thread is dropped.
  `race(work, waitForInterrupt())` makes an interrupt cancel `work` under any executor.
  Under `runBlocking` on the thread of a `runInThread` task, a drop of that task also wakes the loop, and `runBlocking` unwinds and drops its task on that thread.
- The build `Worker` runs goals in a task (`Worker::runGoals`); see [Goals and the Worker](#goals-and-the-worker).
- `asyncRunTask(task, token)` and `taskToAwaitable(task)` in `nix/util/task-asio.hh` run a task as a Boost.Asio operation, and `awaitableToTask(executor, awaitable)` awaits a Boost.Asio awaitable from a task.
  No code of Nix uses Boost.Asio any more; only the tests of the adapters do.

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
  Dropping the task makes `checkInterrupt()` on that thread throw `Interrupted`, calls the callbacks that the thread registered with `onThreadCancelled`, and then joins the thread, on the reactor as a cleanup of the run (see [Asynchronous drop](#asynchronous-drop)).
  A blocking wait registers such a callback to wake up; `FileTransfer::download(request, sink)` cancels its transfer in it.
  The cancellation is per task: it does not set the global interrupt flag.

There is no detached spawn.
A task always has an owner, so its lifetime is visible in the code.
The reactor spawns a task only for a run (`runOnReactor`), whose caller owns it, and for a deferred cleanup, which the scope of a run owns.
The build `Worker` is the one place where several owners share a task; see [Goals and the Worker](#goals-and-the-worker).

## Goals and the Worker

The build `Worker` (`src/libstore/build/`) runs goals: substitutions, builds, and goals that only wait for other goals.
The work of a goal is a `Task<Goal::ExitCode>`, and goal code is ordinary task code.
It awaits other tasks, for example `Store::queryPathInfoAsync`, and the leaves below.

`Worker::runGoals` is the executor of the goals, as a task.
Each poll runs the goals that woke, in the order of `CompareGoalPtrs`, and then wakes the goals that wait for a slot, in FIFO order.
The waker of a goal puts the goal in a queue of the `Worker`, from any thread, and wakes the task that runs the goals.
`Worker::run` is `runBlocking(runGoals(...))`.

The goals run on the thread that polls `runGoals`, and never on the reactor thread:

- Goal code still makes blocking store calls: SQLite queries, the registration and hashing of outputs, the build hook protocol, `copyClosure`.
  A blocked reactor thread would stop all transfers and timers of the process.
- Goal code keeps thread-local state across suspensions: the current activity of the logger, and the interrupt check.
  So one thread creates, runs and drops the goals of a build.

The waits for the outside world go to the [reactor](#the-reactor), which wakes the goals on their thread:

| Leaf | What wakes it |
|------|---------------|
| `waitForChildEvent()` | The reactor: `waitFd` on the channels of the goal's child, and `sleepFor` until its first deadline (`max-silent-time`, `timeout`). |
| `waitForAWhile()` | The reactor: `sleepFor(poll-interval)`. |
| `waitForBuildSlot()` | A free build or substitution slot. The goal waits in a `WaitQueue` of the `Worker`. |
| `yield()` | The goal itself, so that the other awake goals run first. |
| `await(goals)` | The completion of the goals. Without `--keep-going`, the first failure ends the wait. |

When a wait for a child fires, the goal reads once from every channel of the child that is ready, in the order of the descriptors, as the poll loop of the `Worker` did.
Reading only the channel that woke gave the EOF of the builder's channel before the log that waited on the build hook's channel.

The entry of a `WaitQueue` lives in the leaf, so a cancelled goal leaves the queue and does not take the turn of a goal that still waits.

A task awaits a build with `Worker::buildPathsWithResultsAsync`.
It runs `buildPathsWithResultsTask`, which makes the goals in its frame and awaits `runGoals`, on a thread of its own (`runInThread`), so that a task on the reactor can await it.
Dropping it cancels the thread: `runBlocking` there unwinds and drops the goals, which kills and reaps the builders and releases the locks, and the join goes to the cleanup scope of the run on the reactor.
The functional test `build-drop` checks this with a task under `runOnReactor` that drops a build once its builder runs.

### Where the contract does not fit

- A goal is shared.
  The `Worker` deduplicates goals by store path, and several goals can wait for the same goal.
  A task has one owner, so a goal is a shared object (`GoalPtr`) that owns its task.
  The last `GoalPtr` to go destroys the goal, and with it the task: that is the cancellation by drop.
  To wait for goals is therefore a leaf that holds `GoalPtr`s, not an awaited task.
  Without `--keep-going`, the first failure releases the other goals, which cancels those that nothing else holds.
- The `Worker` reports a deadlock when no goal can continue.
  A waker is opaque, so the `Worker` cannot see who will wake a goal.
  It counts the goals that suspended on something other than another goal or a slot: the reactor (the output of a child, a timer, a transfer) or a thread (`Worker::nrGoalsSuspendedElsewhere`).
  Only such a goal can make progress without the help of another goal.
  When no goal is awake and the count is 0, `runGoals` throws.
  The reactor is shared with other tasks of the process, so "the reactor has no work" says nothing about the goals; the count does.
- The goals cannot run on the reactor, so a task that awaits a build costs a thread.

### Cancellation

- Without `--keep-going`, a failed goal releases the goals that its waiters waited for, and a failed top-level goal ends `Worker::run`.
  The goals that nothing holds any more are destroyed, and their tasks are cancelled.
- A user interrupt wins the race of `runGoals` with `waitForInterrupt()`, which drops the goals; `Interrupted` reaches the caller.
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

- The goals run on a thread that may block, not on the reactor, because goal code still calls blocking store operations, which stop all goals of the build while they run:
  `isValidPath`, `queryPartialDerivationOutputMap` and other SQLite queries, the registration of outputs, the hashing in `pathContentsGood`, the build hook protocol (`tryBuildHook`, the reads of the machine name), `copyClosure`, `writeDerivation`, and `LocalStore::autoGC`.
  Each needs a task version, or `runInThread`, before the goals can run on a shared executor.
  `computeFSClosure` has a task version now (`computeFSClosureAsync`, virtual, because `LegacySSHStore` computes a closure in one request that blocks), and goal code awaits it.
  The local `queryRealisation` of `DrvOutputSubstitutionGoal` goes through `callbackToTask`; for the stores of Nix today, its callback version still completes before it returns.
- Only `FileTransfer::download` registers with `onThreadCancelled`.
  A substitution from an SSH store or a daemon blocks in a socket read, which a drop does not interrupt; the drop then waits until the read returns.
- `queryRealisation` has no cancellable version: `callbackToTask` discards its result on a drop but does not stop it.
- A wait for a child registers its descriptors with the reactor again for each chunk of output (`waitFd` per wait).
  The measurements below show no difference beyond the noise; a persistent registration (`FdWatch`) would have to run its callback on the reactor thread and wake the goal from there.
- Windows has no `waitFd`, so a goal there polls the completion port of the children every 50 ms.
  It compiles and links in the cross build `nix-store-x86_64-w64-mingw32`, but no test ran on Windows.

Measured with the release builds of `nix-cli` at 6bbb87ad7 (before) and of this branch (after), in a private store without sandbox, on a machine with 32 cores and a load average of 11 to 30 from other work, interleaved, 6 runs each.
"many" builds 1000 trivial derivations and one that depends on them, with `max-jobs = 16`; "log" builds one derivation that writes 50 MB of log.
The times are wall, user and system seconds of `nix build`, builders included.

| Case | Before: median wall (range) | After: median wall (range) | Lowest user + sys, before / after |
|------|------|------|------|
| many | 2.53 s (2.38–4.08) | 2.85 s (2.44–5.60) | 3.91 / 4.02 s |
| log  | 7.22 s (6.46–10.87) | 7.25 s (6.78–7.92) | 6.37 / 6.63 s |

The ranges overlap; the noise of the machine is larger than any difference.

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
  A C++ exception that travels through Rust and back is the same exception object, because Rust holds it without looking inside.
  A Rust `Err` becomes a `nix::RustError`, and a Rust panic a `nix::RustPanic`; both are `nix::Error` and own the Rust value, so that it comes back unchanged into Rust.
  libutil makes these exceptions through a factory that it registers with libnixrust at run time, because libnixrust cannot refer to libutil at link time.
  A panic never unwinds into C++; see [Panics, exceptions and `unsafe`](./rust.md#panics-exceptions-and-unsafe).

A `CxxFuture` is `Send`, so a C++ task that crosses into Rust can be polled on any thread, one thread at a time.
A task that keeps thread-local state across a `co_await`, such as a `PushActivity`, is therefore not fit to cross.

### Example: fetching a tarball

A tarball fetch is a task from end to end; [The tarball cache on gitoxide](./tarball-cache.md#fetching-a-tarball-as-a-task) describes it.
It shows the patterns that a migration of other code can use:

- Blocking work behind a future.
  The Rust future starts a thread on its first poll, and the thread wakes the future when it is done.
  Dropping the future sets a cancellation token, wakes the thread from a blocking read, and joins it, on the reactor as a cleanup of the run (see [Asynchronous drop](#asynchronous-drop)).
- A stream between two concurrent tasks.
  `FileTransfer::transferStreaming` writes the body of a transfer into a `TransferStream`, which a blocking reader on another thread reads.
  The owner closes the stream to wake the reader when it cancels both.
- A cancellation token per operation, instead of the global interrupt flag.
  `runOnReactor` and `runBlocking` turn a user interrupt into the destruction of the task, and the destruction reaches the operation through its owner.
- An exception that a C library swallows.
  libarchive turns an exception of its input into a message.
  The code that calls libarchive remembers the exception and rethrows it, so that it keeps its type.

## The reactor

The reactor is one tokio runtime for the whole process, with the `current_thread` scheduler, on a thread of its own (`nix-reactor`).
It is in libnixrust (`src/librust/src/reactor.rs`), and `nix/util/reactor.hh` gives it to C++:

- `runOnReactor(task)` runs a task on it (see [Running tasks](#running-tasks)).
  Rust code does the same with `reactor_spawn`, which returns a future of the completion.
- `sleepFor(duration)` and `waitFd(fd, readable, writable)` are tasks that the reactor wakes.
  Any thread can poll them, also under `runBlocking`.
- `postToReactor(f)`, `ReactorTimer` and `FdWatch` run callbacks on the reactor thread.
  They are for C libraries that report the timers and descriptors that they need, such as the curl multi interface.
  `FdWatch` reports readiness like a level-triggered `poll`, which curl expects, although tokio registers descriptors edge-triggered.

One thread drives the runtime, so its callbacks never run at the same time.
A C library that is not thread-safe, such as a curl multi handle, can therefore make all its calls on the reactor thread without a lock.
The thread is the reactor's own, and not the thread of a `runBlocking` caller, because synchronous code also starts transfers, and something must drive curl while no task waits.

curl runs on the reactor (`_NIX_FILE_TRANSFER_BACKEND=reactor`, the default except on Windows).
Its socket and timer callbacks register with the reactor, and a transfer completes through its callback on the reactor thread, which wakes the task that waits for it.
With `_NIX_FILE_TRANSFER_BACKEND=thread`, curl runs on a thread of its own that polls, as before the reactor.
That backend stays during the migration: Windows needs it, because tokio cannot watch arbitrary sockets there, and it gives a way back without a new build.

A tarball fetch runs with `runOnReactor` when curl runs on the reactor, so that the transfer and the task share a thread.

The rules for code that runs on the reactor thread:

- Do not block.
  A blocked reactor thread stops all transfers and timers of the process.
  Blocking work, such as libarchive's reads, goes to a thread of its own behind a future (see [Example: fetching a tarball](#example-fetching-a-tarball)).
- Do not call `runOnReactor`; await the task instead.
- A drop must not block either; see [Asynchronous drop](#asynchronous-drop).

## Asynchronous drop

Some drops must wait: a future that owns a thread joins it, a writer flushes a pack, a process must be killed and reaped.
Under `runBlocking`, the drop can wait in place: it blocks only the thread of the caller, which waits for the task anyway.
On the reactor thread, a drop that waits stops everything else.

The rule:

- A drop that must wait first starts the cancellation without waiting: it sets the cancellation token, closes the stream, or sends the signal.
- Then it gives the wait to the cleanup scope of the run that polled it, as a future: `CleanupScope::defer` in Rust, `nix::CleanupScope::defer` in C++ (`nix/util/reactor.hh`), which takes a task.
  The drop returns at once.
- A run on the reactor (`runOnReactor`, `reactor_spawn`) completes only after all cleanups in its scope are done, and its drop waits for them too.
  The caller of `runOnReactor` therefore sees the same state as with a drop that waits: no thread of the task runs any more, and no temporary file is left.
- A future gets the scope with `CleanupScope::current()` on its first poll, and keeps it.
  The scope is the run that the future belongs to; the drop can happen where tokio's task-local scope is not visible.
- Without a scope, for example under `runBlocking`, the drop waits in place, as before.

Two leaves follow this rule:

- The unpacking of a tarball (`UnpackJob` in `tarball_cache.rs`): its drop sets the token, closes the `TransferStream`, and defers a future that waits for the last message of the thread and then joins it.
- `runInThread` (`ThreadLeaf` in `nix/util/task-thread.hh`): its drop cancels the thread and calls its `onThreadCancelled` callbacks, and defers a task that waits for the result of the thread and then joins it.
  In the `Worker`, which has no scope, the drop joins in place as before.

Other drops that may wait, for example of a goal with a builder process that must be killed and reaped, have not been audited; the goals run and are dropped outside the reactor, on the thread of `runGoals`.

Rejected alternatives:

- An explicit `close()`, or `co_await shutdown()`, before the drop.
  A cancellation is a drop at any suspension point, where no code of the owner runs that could call `close()`.
  A `close()` helps where the owner completes normally, but cancellation is the case that needs the wait.
- A detached cleanup task, which the executor owns and nothing awaits.
  The drop does not block, but the caller loses the guarantee that the work stopped: a new fetch could start while the old thread still writes to the cache, and the process could exit before the thread removed its temporary files.
  The scope keeps the guarantee, because the run waits for its cleanups.
- A cleanup scope that the drop finds through tokio's task-local storage.
  Whether a drop sees it depends on how tokio drops an aborted task, and on which thread destroys the owner.
  The scope of the first poll is explicit.

A cleanup that never ends makes its run never end, as a drop that waits forever does under `runBlocking`.
