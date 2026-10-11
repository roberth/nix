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
- `spawnOnReactor(task)` is the same run on the reactor as a task, which any executor can await.
  Its drop cancels the run and waits for its cleanups.
- `runOnFiber(fn)` runs blocking code, such as the evaluator, on a fiber, as a task; see [Evaluator on fibers](#evaluator-on-fibers).
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
  On Unix, `nix::read` and `nix::write` on such a thread `poll` the descriptor together with a pipe that the cancellation closes, so a read from a stalled connection (`FdSource`, the daemon and SSH protocols) ends at once on a drop.
  A write to a pipe or a socket is then limited to `PIPE_BUF` bytes per call, because `poll` promises room for no more; writes to files skip the wait.
  The cancellation is per task: it does not set the global interrupt flag.
  The thread ends with the function, and Linux kills a child that the thread forked with `dieWithParent` (`PR_SET_PDEATHSIG`) then: the signal follows the thread, not the process.
- `BlockingPool` runs blocking functions as tasks on threads that it keeps until it is destroyed, with the cancellation of `runInThread`; a function that did not start when its task is dropped does not run.
  Use it for a function that forks a process which must outlive the function, such as the start of a builder.

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

- Goal code still makes blocking calls: SQLite queries, `copyClosure`, `writeDerivation`, the hashing of `pathContentsGood`, and drops that kill and reap processes.
  A blocked reactor thread would stop all transfers and timers of the process.
  [Blocking calls of the goals](#blocking-calls-of-the-goals) lists them, and says which ones moved off the thread of the goals.
- Goal code keeps thread-local state across suspensions: the current activity of the logger, and the interrupt check.
  So one thread creates, runs and drops the goals of a build.
- The blocking steps of a build refer to the frame of their goal (see below), which a deferred drop would destroy too early.

The waits for the outside world go to the [reactor](#the-reactor), which wakes the goals on their thread:

| Leaf | What wakes it |
|------|---------------|
| `waitForChildEvent()` | The reactor: `waitFd` on the channels of the goal's child, and `sleepFor` until its first deadline (`max-silent-time`, `timeout`). |
| `waitForAWhile()` | The reactor: `sleepFor(poll-interval)`. |
| `waitForBuildSlot()` | A free build or substitution slot. The goal waits in a `WaitQueue` of the `Worker`. |
| `waitForBuildHook()` | The end of the turn of the goal that talks to the build hook (`Worker::wantingBuildHook`). |
| `yield()` | The goal itself, so that the other awake goals run first. |
| `await(goals)` | The completion of the goals. Without `--keep-going`, the first failure ends the wait. |
| `waitFd` in `tryBuildHook`, `buildWithHook` | The reactor: the pipes to and from the build hook. |
| A step on `Worker::builderStartPool` or `Worker::blockingPool` | A thread of the worker: the start of a builder, its reaping, the registration of its outputs. |

The exchange with the build hook is a task: it writes to a non-blocking pipe and reads the reply one byte at a time, and waits with `waitFd` whenever the pipe is full or empty.
One goal at a time talks to the hook, as when the exchange blocked; otherwise the goals that ask together start a hook each.
A goal that is dropped in the middle of the exchange destroys the hook, whose state is then unknown.

The blocking steps of a local build run on threads of the worker (`inThread` in `derivation-building-goal.cc`):

- `startBuild` forks the builder and waits until it has set up its sandbox, on one thread (`builderStartPool`).
  Starting several builders at once multiplied the system time (below) and did not make the build faster.
- `unprepareBuild`, `registerOutputs` and `cleanupBuild` run on up to `max-jobs` threads (`blockingPool`).
- The threads stay alive as long as the `Worker` (`BlockingPool`), because a builder dies with the thread that forked it.
- A step refers to the frame of the goal, for example to the builder.
  A drop cancels the step and waits for it in place, which is safe only while the goals run without a cleanup scope; `inThread` asserts that.
- The `Worker` is not thread-safe.
  `unprepareBuild` reports the end of the child through a callback, which does nothing off the thread of the goals; the goal frees the build slot after the step, or `ChildRegistration` does if the goal is dropped.

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
- The goals do not run on the reactor (see [Should the goals run on the reactor?](#should-the-goals-run-on-the-reactor)), so a task that awaits a build costs a thread.

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
- A substitution from an SSH store (`ssh-ng://`, the daemon protocol, and `ssh://`) reads the connection in `nix::read`, which the drop wakes.
  The functional test `substitute-cancel-ssh` stalls the remote program after 100 kB of the NAR; the build ended 89 to 98 ms (`ssh-ng`) and 192 to 202 ms (`ssh`) after it started.
  Before, it waited until the remote program exited: the test did not end within 60 s.
- A goal that waits for the reply of the build hook is dropped at once.
  The functional test `build-hook-cancel` uses a hook that never answers; the build ended 64 ms after it started, and the hook was killed.
  Before, the whole `Worker` waited in the read of the reply, and the test did not end within 60 s.

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

### Blocking calls of the goals

The calls that goal code makes on the thread of the goals, and what happens to them.
Paths are relative to `src/libstore/build/`.

| Call | Where | Kind | Now |
|------|-------|------|-----|
| `isValidPath`, `queryPartialDerivationOutputMap`, `queryDerivationOutputMap`, `queryRealisation`, `readDerivation` | `derivation-goal.cc:256,348,373,379,442,452`, `derivation-building-goal.cc:399,1509,1539,1546`, `derivation-trampoline-goal.cc:104,141`, `substitution-goal.cc:40,196` | SQLite reads, a file read | On the thread of the goals. Tens of microseconds; a read waits for the lock of the database while another thread writes. |
| `registerDrvOutput`, `writeDerivation` | `derivation-goal.cc:109,289,461`, `derivation-building-goal.cc:1556` | SQLite writes | On the thread of the goals. Rare: CA derivations and resolved derivations. |
| `addTempRoot` | `substitution-goal.cc:37`, `derivation-goal.cc:73`, `derivation-trampoline-goal.cc:131`, `derivation-building-goal.cc:54` | A write to the file of temporary roots | On the thread of the goals; short. |
| `startBuild`: fork, sandbox setup, wait for the builder | `derivation-building-goal.cc:1121` | Processes, waits | `Worker::builderStartPool`. |
| `unprepareBuild`, `registerOutputs`, `cleanupBuild` | `derivation-building-goal.cc:1170,1180,1205` | Waits, hashing, SQLite writes, deletion | `Worker::blockingPool`. |
| The build hook protocol (`tryBuildHook`, the machine name, the inputs) | `derivation-building-goal.cc` (`tryBuildHook`, `buildWithHook`) | Protocol I/O on pipes | A task with `waitFd`. |
| The start of the hook (`HookInstance`), the post-build hook (`startProcess`), `Pid::kill` of the hook | `derivation-building-goal.cc:860,1304,1391` | Fork, wait for an exiting process | On the thread of the goals: about a millisecond. |
| `copyClosure` from the eval store | `derivation-goal.cc:285`, `derivation-building-goal.cc:395` | Copies NARs | On the thread of the goals. Only with `--eval-store`. Not converted. |
| `pathContentsGood` | `worker.cc:660` | Hashing | On the thread of the goals. Only with `--repair` and `--check`. Not converted. |
| `lockPaths(…, wait = false)`, `deletePath` of an invalid output | `derivation-building-goal.cc:555,594` | File locks, deletion | On the thread of the goals. The lock does not wait. |
| `LocalStore::autoGC(false)` | `worker.cc:424` | `statvfs` every 5 s; the collection itself runs on a thread of its own | On the thread of the goals. |
| `getSubstituters()` | `substitution-goal.cc:51`, `drv-output-substitution-goal.cc:28` | Opens the substituters on first use, which can fetch `nix-cache-info` | On the thread of the goals. On the reactor thread this would wait for the reactor itself. |
| The copy of a substitution | `substitution-goal.cc` | Network and disk | `runInThread`; a drop wakes its reads (above). |
| `queryRealisation` of a substituter | `drv-output-substitution-goal.cc:38` | Network | `callbackToTask`; blocks for a daemon or an SSH store, whose callback version is synchronous. |
| The drop of a goal: `DerivationBuilder` kills and reaps the builder, `HookInstance` kills the hook (up to `build-hook-kill-timeout`), a step on a pool is waited for | Destructors | Waits | In place, on the thread of the goals. |

The SQLite reads stay synchronous.
A thread hop per query would cost more than the query, and a dedicated thread for the database would serialize the reads behind the writes that now run on the pools, which is what makes the rare long poll today.

### Measurements

Release builds of `nix-cli`, private stores, a machine with 32 cores and a load average of 7 to 28 from other work.
"base" is 49264de7a, "head" this branch.

1000 trivial derivations and one that depends on them, in a chroot store (which builds in namespaces), `max-jobs = 16`, 6 interleaved rounds with the order rotated each round.
"before" is fae4111a0, before the goals were tasks; "s11-base" is 5a45e1e97, which another round measured about 5 % slower than "before".
Wall, user and system seconds of `nix-store -r`, builders included:

| Variant | Median wall (range) | Median sys |
|---------|---------------------|------------|
| before | 9.15 s (8.84–9.70) | 8.40 s |
| s11-base | 9.26 s (8.95–9.71) | 8.65 s |
| base | 9.21 s (8.98–9.58) | 8.46 s |
| head | 9.28 s (8.11–9.81) | 8.82 s |

The 5 % did not show again: the ranges overlap.
The reactor was busy for 0.19 to 0.35 s of each build, so the registration of the descriptors of a child for each wait cannot account for more than that; it is not changed.
The system time is close to the wall time, with or without a free thread of the goals: the kernel work for the namespaces of the builders bounds this build.

The polls of the goals in the same build, from `_NIX_REACTOR_STATS`, with a `debugoptimized` build of the branch at each step (one run each):

| Step | Busy | Polls ≥ 1 / 10 / 50 ms | Longest poll |
|------|------|------------------------|--------------|
| Before the pools | 10.3 s of 11.4 s | 1492 / 116 / 0 | 27 ms |
| Starts on 16 threads | 4.2 s of 12.3 s | 500 / 88 / 4 | 63 ms |
| Starts on 1 thread, the rest on 16 | 0.9 s of 8.6 s | 12 / 3 / 0 | 26 ms |

Before the pools, the goals were the bottleneck: about 10 ms per build in blocking calls (`strace` of the thread: 4.1 s in `read` waiting for the builders to set up, 2.2 s in `wait4`, 0.9 s in `clone`).
Starting builders on several threads multiplied the system time (9 s with one start at a time, 18 s with 4, 31 s with 16).

Substitution of 3008 paths (3000 small, 8 of 32 MiB) from a local HTTP binary cache with `nix-store -r`, 3 interleaved rounds:

| `http-connections`, `max-substitution-jobs` | base | head | Polls of the goals (head) |
|------|------|------|------|
| 25, 16 | 13.06 s (13.06–14.81) | 13.04 s (12.96–14.81) | busy 0.11 to 0.73 s; longest 0.2 to 2 ms, once 230 ms at a load of 28 |
| 100, 64 | 3.42 s (3.39–3.45) | 3.48 s (3.40–3.49) | busy 0.12 s; longest 0.9 to 1.2 ms |

In one more run of each, the reactor was busy for 715 ms of 3.4 s and 717 ms of 12.9 s, with a longest poll of 7 and 1.8 ms and no poll over 10 ms.

A remote build (the functional test `build-remote-input-addressed`, through `ssh-ng://localhost`): the longest poll of a goal was 25 to 27 ms, from `Pid::kill` of the hook, whose kill timer slept in steps of 25 ms.
With the timer on a condition variable it is below 2 ms, and the busy time of the goals went from 113 ms to 9 to 15 ms per build.

### Should the goals run on the reactor?

Not yet; the numbers do not ask for it, and the move is not safe:

- The thread of the goals is now idle for most of a build (busy 0.1 to 0.9 s in the runs above), so it costs a thread, not throughput.
  None of the three workloads waits for it: the namespaces of the builders bound the many small builds, the transfers bound the substitution.
- Most polls are short, but not all: a few polls of 10 to 26 ms remain in the many small builds, behind the lock of the database or a fork, and one 230 ms poll under load.
  On the reactor, each would stop all transfers of the process.
- Drops wait in place: a builder is killed and reaped, the hook is killed with a timeout, a step on a pool is waited for.
  On the reactor these must become cleanups of the run, and a step that refers to the frame of its goal must then own what it uses; `inThread` asserts that there is no cleanup scope until then.
- `getSubstituters()` can fetch `nix-cache-info` synchronously through the reactor, and the `queryRealisation` of a daemon or SSH store blocks; both would wait for the reactor thread on itself.
- Goal code keeps thread-local state of the logger across suspensions.

### What remains for the Worker

- The table above: `copyClosure`, `writeDerivation`, `pathContentsGood`, `getSubstituters()`, the drops.
- `queryRealisation` has no cancellable version: `callbackToTask` discards its result on a drop but does not stop it.
- A wait for a child registers its descriptors with the reactor again for each chunk of output (`waitFd` per wait); see the measurements.
- Windows has no `waitFd`, so a goal there polls the completion port of the children every 50 ms.
  It compiles and links in the cross build `nix-store-x86_64-w64-mingw32`, but no test ran on Windows.
  On Windows, `unprepareBuild` of the Unix builder does not run, but the pools do; no test covers them there.
- Only `ssh-ng://` and `ssh://` substituters have a drop test; a daemon socket uses the same `RemoteStore` code and `nix::read`.

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

### Measuring the reactor

The reactor times each poll of its tasks and each posted function (`reactorStats()` in `nix/util/reactor.hh`):
the busy time, the longest single poll, and the busy time of the busiest second.
A watchdog logs each poll that is longer than `_NIX_REACTOR_SLOW_POLL_MS` (50 ms by default) at the debug level, with its kind (task, posted function, timer, fd watch, cleanup).
It reports after the fact; to find the code inside a slow task, use a profiler.
`_NIX_REACTOR_STATS` prints the numbers to stderr when the process exits, with details that cost a system call per poll, so that only the variable turns them on:

- For each kind of poll: the polls, the busy time, and the CPU time of the thread (`CLOCK_THREAD_CPUTIME_ID`).
  Busy time without CPU time is time off the CPU inside a poll: the thread was preempted, or waited for a lock or a page fault.
- The `poll(2)` rechecks of the level-triggered emulation of `FdWatch`, which run outside the polls.
- On Linux, the user and system time and the context switches of the whole reactor thread, which include `epoll_wait` and the wakes of other threads.

The busy time alone does not show what limits a run.
With 200 tarball fetches at once, the reactor thread was busy for about half of the run, but most of the run was spent in the unpacking threads, which the reactor does not see; see [Many fetches at once](./tarball-cache.md#many-fetches-at-once).

The `Worker` times the polls of its goals in the same way, because a poll of a goal is what would block the reactor if the goals ran there.
With `_NIX_REACTOR_STATS`, it prints the number of polls, their busy time, the longest poll with its goal, and the number of polls of at least 1, 10 and 50 ms (`nix: goals:`).
A poll longer than `_NIX_REACTOR_SLOW_POLL_MS` is logged at the debug level with the name of the goal.

`nix-fetchers-tarball-bench`, in the tests of libfetchers, fetches many tarballs at once through the task path, and `maintainers/tarball-cancel-stress.sh` interrupts it with `CONCURRENT=N`.
[The tarball cache on gitoxide](./tarball-cache.md#many-fetches-at-once) has the measurements under load.

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

## Evaluator on fibers

The evaluator is recursive C++.
A primop that waits for a store query or a fetch is many C++ frames below the top of the evaluation, and each of those frames would have to become a task before the wait could be a `co_await`.
The evaluator instead runs on a fiber: a stack of its own, which it can leave at any depth and come back to later.
At the boundary, a fiber is a task like any other.

- `runOnFiber(fn)` (`nix/util/fiber.hh`) is a task.
  Its poll switches onto the fiber, which runs `fn` until `fn` suspends or returns.
- `awaitOnFiber(task)`, called by `fn` at any depth, polls `task` on the fiber with the waker of the outer poll.
  While `task` is pending, the fiber suspends, and the outer poll returns `NIX_POLL_PENDING`.
  A wake of `task` wakes the outer task, whose next poll resumes the fiber, which polls `task` again.
- The result of `fn`, or its exception, is the result of the task.
  An exception crosses in both directions as the same object: from `task` to the code on the fiber, and from `fn` to the awaiter of the outer task.
- A drop of the outer task while the fiber is suspended cancels it.
  The fiber resumes once more, and `awaitOnFiber` throws `Interrupted` instead of a poll.
  The stack unwinds like for any exception: the destructors of the frames on it run, and `task`, a local of `awaitOnFiber`, is dropped, which cancels the operation below it.
  Then the stack returns to the pool.
  A `catch` on the fiber can stop the unwinding, but the next `awaitOnFiber` throws again, so the drop waits only until `fn` returns.
- The stacks come from a pool of the process, so a fiber after the first one costs no `mmap` and no page faults.
  A stack has 60 MiB of address space, as much as the evaluator asks for on the main thread; only the pages that a fiber touches use memory.

`awaitOnFiber` outside of a fiber is `runBlocking`.
Code that can run in both places, such as a fetcher, calls it unconditionally.

### Where an evaluation suspends

`downloadIntoTarballCache`, which `builtins.fetchTarball`, `builtins.fetchTree` and the tarball inputs of flakes reach, awaits its fetch task with `awaitOnFiber` when it runs on a fiber, through `spawnOnReactor` when curl runs on the reactor.
Off a fiber, it blocks as before.

Nothing runs an evaluation on a fiber by default.
`_NIX_EVAL_ON_FIBER=1` makes `nix-instantiate` evaluate under `runBlocking(runOnFiber(...))`.
A user interrupt then wakes `runBlocking` while the evaluation is suspended in a fetch, and `runBlocking` drops it.
`tests/functional/tarball-cancel.sh` checks that this stops the transfer and leaves no pack in the tarball cache.

The unit tests in `src/libexpr-tests/fiber-fetch.cc` evaluate `builtins.fetchTarball` against a server that holds each response until the test releases it.
They show that the evaluation suspends in the fetch and continues with the right result; that a drop closes the connection, runs the destructors of the evaluator's frames, and leaves a thunk that can be forced again; and that two evaluations, each with its own `EvalState`, wait in their fetches at the same time on one thread and complete in the order of the responses.

### The garbage collector

A suspended fiber keeps `Value`s on its stack, and while a fiber runs, the stack of its thread keeps the `Value`s of the frames below the switch.
The Boehm GC scans only the stack that a stopped thread executes on.
The fork of bdwgc of Determinate Systems (`packaging/dependencies.nix`) adds stacks that the client registers (`GC_register_stack`), each with a saved stack pointer, and scans each registered stack that has one.
DeterminateSystems/nix-src uses the same fork for its parallel evaluator.

The pool registers each stack when it allocates it, through the `FiberStackHooks` that `initGC` installs.
On each switch, the hooks save the stack pointer of the stack that the thread leaves, and clear it on the stack that the thread enters.
The saved stack pointer is lower than the real one by 512 bytes, for the registers that the switch stores below it, which can hold pointers of the callers.
A switch from a stack other than the one that the hooks expect, for example from a `sinkToSource` coroutine, panics, because the collector would scan from one stack to the base of another.
A free stack of the pool has no saved stack pointer, so the collector ignores it.

The tests in `src/libexpr-tests/fiber-gc.cc` keep 1000 `Value`s only on the stack of a suspended fiber, collect twice from outside while allocating 200000 other `Value`s, and check that the 1000 survive with their contents.
Without the hooks, none survive, and their memory holds other `Value`s.
The tests also pass with a bdwgc that is built with `--enable-gc-assertions`.

A build whose bdwgc has no registered stacks disables the fibers: `runOnFiber` then runs `fn` in place, on its first poll, and `awaitOnFiber` blocks.
The evaluator then behaves as before the fibers.
Windows does the same: Nix builds without the GC there, but no test of the fibers ran on Windows.

### Rules for code on a fiber

- A fiber runs only on the thread that started it; a resumption on another thread panics.
  The compiler can keep the address of a `thread_local` across a call, because a function never changes threads in C++, and a stack switch is a call.
  Fibers that move between threads would need every thread-local on their stacks to be fiber-local instead: `GC_current_stack`, the state of the C++ runtime for exceptions, the current activity of the logger, the interrupt check and the cancellation of `runInThread`, and the allocation cache of `Value`s.
  DeterminateSystems/nix-src pins its fibers to their threads for the same reason.
- The fibers on one thread share its thread-locals.
  The current activity of the logger is fiber-local: `Fiber::resume` swaps it, because `PushActivity` restores the previous activity, and two fibers would restore each other's.
  The other thread-locals of Nix are correct to share: the interrupt check and the cancellation of `runInThread` belong to the thread, and the allocation cache of `Value`s is a free list.
- `awaitOnFiber` in a `catch` block, or during unwinding, blocks instead of suspending.
  The C++ runtime keeps the exceptions that are caught as a stack per thread, and a second fiber on the thread would push and pop its own exceptions on it.
  While the owner of a fiber unwinds and drops it, the code on the fiber sees the exception of the owner in `std::uncaught_exceptions()`.
- A lock that is held across `awaitOnFiber` deadlocks when another fiber on the same thread takes it.
- Two evaluations that run at the same time need an `EvalState` each.
  The state of an `EvalState` assumes one evaluation at a time: a thunk that a suspended evaluation forces is a black hole, which another evaluation reports as infinite recursion, and the call depth counter adds up the depths of all suspended evaluations.
  The parallel evaluator of DeterminateSystems/nix-src has a fiber-aware call depth and waits for thunks that another fiber forces; that is out of the scope of this design.

### Measurements

Measured with the release builds of this branch (`nix-everything`, `nix-expr-tests`), on a machine with 32 cores and a load average of about 14 from other work.

A whole evaluation on a fiber costs no more than the noise.
`nix-instantiate --eval --readonly-mode` of the `drvPath` of a minimal NixOS system, with and without `_NIX_EVAL_ON_FIBER=1`, interleaved, 6 runs each, against the dummy store:

| Mode | Median wall (range) | Lowest user + sys | Max RSS |
|------|------|------|------|
| Direct | 2.14 s (2.07–2.61) | 2.54 s | 635–638 MiB |
| On a fiber | 2.15 s (2.11–3.86) | 2.58 s | 635–638 MiB |

Both give the same `.drv` path.

`nix-expr-fiber-bench` (`src/libexpr-tests/fiber-bench.cc`) measures the rest without I/O:

| Measurement | Result |
|------|------|
| Resident memory per suspended fiber, at a depth of 0 Nix calls | 8 KiB |
| The same, at a depth of 100 (1000 fibers) | 236 KiB |
| The same, at a depth of 1000 (100 fibers) | 2240 KiB |
| Memory mappings per fiber | 2 (the stack and its guard page) |
| Drop of a suspended evaluation, at a depth of 0 (median of 200) | 10.6 µs |
| The same, at a depth of 1000 | 2.19 ms |
| Suspension and resumption of a fiber, per round trip | 115 ns |
| The same wait in a coroutine task | 18 ns |

The memory of a suspended evaluation grows by about 2.2 KiB for each Nix call that it is deep in.
The benchmark does not separate stack from heap; a call of the benchmark allocates an environment and a few values on the heap, so most of it is probably stack, which the same evaluation also uses on the main thread.
The fiber itself adds 8 KiB and two mappings.
With a `vm.max_map_count` of 65530, the default of Linux, a process can hold about 32000 suspended fibers.

The drop of a deep evaluation is slow because it is a C++ exception that unwinds through the frames of the evaluator, which catch and rethrow it at each forced thunk, as a user interrupt does without fibers.

The drop of an evaluation that waits in `builtins.fetchTarball` (`fiber-fetch.cc`, which cancels the transfer and the unpacking too) took 1.5 ms in the package build of the tests.
In `tarball-cancel.sh`, `nix-instantiate` exited 3 to 9 ms after `SIGINT` when it evaluated on a fiber, and 3 to 5 ms when it did not (debug-optimised build, 8 interrupts each).

### What remains

- Only the tarball fetch suspends.
  Import from derivation, store queries, and the fetchers other than tarballs still block the fiber, and with it the thread.
  `Worker::buildPathsWithResultsAsync` is the task for import from derivation.
- Concurrent evaluations on one `EvalState`, and evaluations that move between threads; see [Rules for code on a fiber](#rules-for-code-on-a-fiber).
- No test of the fibers ran on Darwin or Windows.
  The flake output for `aarch64-darwin` evaluates, but it was not built.
- The bdwgc fork reads and writes the saved stack pointers without atomics, and a FIXME in it notes that a thread inside `GC_do_blocking`, which the collector does not stop, must not switch stacks.
  Nix does not call `GC_do_blocking`, and a fiber must run on a thread that is registered with the collector, which the threads that evaluate are.
