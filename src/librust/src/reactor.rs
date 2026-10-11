//! The reactor: one tokio runtime for the whole process, with the
//! `current_thread` scheduler, on a thread of its own (`nix-reactor`).
//!
//! The reactor gives C++ and Rust code three things:
//!
//! - A place to run a future: [`reactor_spawn`] runs a future on the reactor
//!   thread, and gives back a future of its completion. C++ uses it to run a
//!   `nix::Task` on the reactor (`nix::runOnReactor`).
//! - Timers and readiness of file descriptors, as futures
//!   ([`reactor_sleep`], [`reactor_wait_fd`]) and as callbacks on the reactor
//!   thread ([`ReactorTimer`], [`FdWatch`]). The callbacks are for C
//!   libraries that report the timers and descriptors that they need, such as
//!   the curl multi interface.
//! - A cleanup scope for drops that must wait ([`CleanupScope`]): a drop on
//!   the reactor thread must not block it, so it gives its wait to the scope
//!   of the run that owns it, and the run completes only after the wait.
//!
//! One thread drives the runtime, so the callbacks of the reactor never run
//! at the same time as each other. A C library that is not thread-safe can
//! therefore run all its calls on the reactor thread.
//!
//! The runtime lives as long as the process. Nothing shuts it down: a
//! shutdown would have to cancel the work of other threads at exit, which
//! the C++ static destructors cannot order.

use std::future::{Future, poll_fn};
use std::pin::{Pin, pin};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::task::{Context, Poll, Waker};
use std::thread::{self, ThreadId};
use std::time::{Duration, Instant};

use cxx::UniquePtr;
use tokio::runtime::Handle;
use tokio::sync::{Notify, mpsc};
use tokio::task::AbortHandle;

use crate::future::{CxxFuture, Error, NixFuture, export_future};

#[cxx::bridge(namespace = "nix::rs")]
mod ffi {
    unsafe extern "C++" {
        include!("nix/rust/reactor.hh");

        #[namespace = ""]
        type nix_future = crate::future::NixFuture;

        type ReactorCallback;

        fn run(self: &ReactorCallback);

        type FdCallback;

        fn ready(self: &FdCallback, readable: bool, writable: bool);
    }

    extern "Rust" {
        /// Run `future`, a future of `()`, on the reactor, and return a
        /// future of `()` that completes after it and after the cleanups
        /// that its drops deferred. The returned future is lazy: the
        /// spawn happens on its first poll.
        ///
        /// Dropping the returned future cancels the spawned one, and waits
        /// until it is dropped and its cleanups completed. On the reactor
        /// thread, that wait is itself deferred, to the scope of the run
        /// that polled the returned future.
        fn reactor_spawn(future: nix_future) -> nix_future;

        /// Run `callback` once on the reactor thread. Callbacks run in the
        /// order of the calls, from all threads together.
        fn reactor_post(callback: UniquePtr<ReactorCallback>);

        /// A future of `()` that completes `millis` milliseconds after its
        /// first poll. It can be polled on any thread.
        fn reactor_sleep(millis: u64) -> nix_future;

        /// A future of `u64` that completes when `fd` is ready for one of
        /// the directions, with the ready directions: bit 0 for reading,
        /// bit 1 for writing. It can be polled on any thread. While it
        /// exists, `fd` must stay open and must have no other registration
        /// with the reactor.
        fn reactor_wait_fd(fd: i32, readable: bool, writable: bool) -> nix_future;

        /// Whether the current thread is the reactor thread.
        fn on_reactor_thread() -> bool;

        /// Runs a callback on the reactor thread when it expires. Use and
        /// drop it on the reactor thread only: then its callback never runs
        /// after `set(-1)` or after the drop.
        type ReactorTimer;

        fn reactor_timer(callback: UniquePtr<ReactorCallback>) -> Box<ReactorTimer>;

        /// Expire `millis` milliseconds from now, instead of at the
        /// previous time. A negative value disarms the timer.
        fn set(self: &ReactorTimer, millis: i64);

        /// Reports the readiness of a file descriptor to a callback on the
        /// reactor thread, like a level-triggered `poll`: the callback is
        /// called again while the descriptor stays ready. Use and drop it
        /// on the reactor thread only. The descriptor must stay open until
        /// the drop.
        type FdWatch;

        fn reactor_watch_fd(fd: i32, callback: UniquePtr<FdCallback>) -> Result<Box<FdWatch>>;

        /// The directions to report. Without any, nothing is reported.
        #[cxx_name = "setInterest"]
        fn set_interest(self: &FdWatch, readable: bool, writable: bool);

        /// The cleanup scope of the run on the reactor that polls the
        /// caller, or none (see `CleanupScope::current`). For C++ code
        /// whose drop must wait.
        type CleanupScopeRef;

        fn cleanup_scope_current() -> Box<CleanupScopeRef>;

        /// Whether there is a scope. Without one, the drop waits in place.
        #[cxx_name = "isSome"]
        fn is_some(self: &CleanupScopeRef) -> bool;

        /// Run `cleanup`, a future of `()`, as part of the scope. Without a
        /// scope, drop `cleanup` at once, which cancels it.
        fn defer(self: &CleanupScopeRef, cleanup: nix_future);
    }
}

use ffi::{FdCallback, ReactorCallback};

// SAFETY: the callbacks run on the reactor thread, so they move there. The
// C++ implementations (`nix/util/reactor.hh`) hold a `std::function` and
// nothing that belongs to the thread that made them.
unsafe impl Send for ReactorCallback {}
// SAFETY: as for `ReactorCallback`.
unsafe impl Send for FdCallback {}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|e| e.into_inner())
}

struct Reactor {
    handle: Handle,
    posts: mpsc::UnboundedSender<UniquePtr<ReactorCallback>>,
    thread: ThreadId,
}

static REACTOR: OnceLock<Reactor> = OnceLock::new();

/// The reactor. Starts it on the first call.
fn reactor() -> &'static Reactor {
    REACTOR.get_or_init(start_reactor)
}

fn start_reactor() -> Reactor {
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_io()
        .enable_time()
        .build()
        .unwrap_or_else(|e| panic!("cannot start the reactor: {e}"));
    let handle = runtime.handle().clone();
    let (posts, mut received) = mpsc::unbounded_channel::<UniquePtr<ReactorCallback>>();
    let (started, thread) = std::sync::mpsc::channel();
    thread::Builder::new()
        .name("nix-reactor".to_owned())
        .spawn(move || {
            unshare_filesystem();
            let _ = started.send(thread::current().id());
            // The sender is in a static, so this never returns.
            runtime.block_on(async move {
                while let Some(callback) = received.recv().await {
                    callback.run();
                }
            });
        })
        .unwrap_or_else(|e| panic!("cannot start the reactor thread: {e}"));
    Reactor {
        handle,
        posts,
        thread: thread.recv().expect("the reactor thread starts"),
    }
}

/// Give the reactor thread its own file system state, like the other
/// long-lived threads of Nix (`tryUnshareFilesystem` in libutil). The main
/// thread can then unshare its mount namespace, which Linux allows only
/// for a thread that shares its file system state with no other thread.
#[cfg(target_os = "linux")]
fn unshare_filesystem() {
    // EPERM and ENOSYS leave the state shared, as in libutil. There is no
    // caller to report another error to; the mount namespace unshare
    // reports it later.
    // SAFETY: `unshare` takes no pointers; `CLONE_FS` affects only the
    // calling thread.
    unsafe { libc::unshare(libc::CLONE_FS) };
}

#[cfg(not(target_os = "linux"))]
fn unshare_filesystem() {}

fn on_reactor_thread() -> bool {
    REACTOR.get().is_some_and(|r| r.thread == thread::current().id())
}

fn reactor_post(callback: UniquePtr<ReactorCallback>) {
    // The receiver lives as long as the process, so this does not fail.
    let _ = reactor().posts.send(callback);
}

// Cleanup scopes.

/// The pending work of one run on the reactor: the future that
/// [`reactor_spawn`] runs, and the cleanups that drops in it deferred. The
/// run completes when none is left.
#[derive(Clone)]
pub struct CleanupScope(Arc<ScopeInner>);

struct ScopeInner {
    state: Mutex<ScopeState>,
    idle: Condvar,
}

struct ScopeState {
    pending: usize,
    waker: Option<Waker>,
}

tokio::task_local! {
    static SCOPE: CleanupScope;
}

/// Pending work in a scope, until it is dropped.
struct Hold(Arc<ScopeInner>);

impl Drop for Hold {
    fn drop(&mut self) {
        let waker = {
            let mut state = lock(&self.0.state);
            state.pending -= 1;
            if state.pending > 0 {
                return;
            }
            self.0.idle.notify_all();
            state.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

impl CleanupScope {
    fn new() -> Self {
        CleanupScope(Arc::new(ScopeInner {
            state: Mutex::new(ScopeState {
                pending: 0,
                waker: None,
            }),
            idle: Condvar::new(),
        }))
    }

    /// The scope of the run on the reactor that is polling the caller, or
    /// nothing outside of such a run, for example under C++'s
    /// `runBlocking`.
    ///
    /// A future whose drop must wait calls this in its first poll and
    /// keeps the result: the drop can happen in a context without the
    /// scope, for example when a C++ task that owns the future is
    /// destroyed.
    pub fn current() -> Option<Self> {
        SCOPE.try_with(Clone::clone).ok()
    }

    fn hold(&self) -> Hold {
        lock(&self.0.state).pending += 1;
        Hold(self.0.clone())
    }

    /// Run `cleanup` on the reactor as part of this scope. The run of the
    /// scope completes only after `cleanup`.
    pub fn defer(&self, cleanup: impl Future<Output = ()> + Send + 'static) {
        let hold = self.hold();
        reactor().handle.spawn(SCOPE.scope(self.clone(), async move {
            cleanup.await;
            drop(hold);
        }));
    }

    fn poll_idle(&self, cx: &mut Context<'_>) -> Poll<()> {
        let mut state = lock(&self.0.state);
        if state.pending == 0 {
            return Poll::Ready(());
        }
        if !state.waker.as_ref().is_some_and(|w| w.will_wake(cx.waker())) {
            state.waker = Some(cx.waker().clone());
        }
        Poll::Pending
    }

    fn wait_idle(&self) {
        let mut state = lock(&self.0.state);
        while state.pending > 0 {
            state = self.0.idle.wait(state).unwrap_or_else(|e| e.into_inner());
        }
    }
}

/// A [`CleanupScope`], or none, for C++ (`nix::CleanupScope`).
pub struct CleanupScopeRef(Option<CleanupScope>);

fn cleanup_scope_current() -> Box<CleanupScopeRef> {
    Box::new(CleanupScopeRef(CleanupScope::current()))
}

impl CleanupScopeRef {
    fn is_some(&self) -> bool {
        self.0.is_some()
    }

    fn defer(&self, cleanup: NixFuture) {
        // SAFETY: the bridge passes ownership of `cleanup`, a `nix_future`
        // of `()` that C++ made with `toCFuture`.
        let cleanup = unsafe { CxxFuture::<()>::new(cleanup) };
        match &self.0 {
            Some(scope) => scope.defer(async move {
                // The error of a cleanup has nobody to go to.
                let _ = cleanup.await;
            }),
            None => drop(cleanup),
        }
    }
}

// Runs.

/// The future that [`reactor_spawn`] spawns. The fields drop in order: the
/// future first, which can defer cleanups to the scope, then the hold of
/// the future itself.
struct Main {
    future: CxxFuture<()>,
    result: Arc<Mutex<Option<Result<(), Error>>>>,
    _hold: Hold,
}

impl Future for Main {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let this = self.get_mut();
        let result = std::task::ready!(Pin::new(&mut this.future).poll(cx));
        *lock(&this.result) = Some(result);
        Poll::Ready(())
    }
}

struct Spawned {
    scope: CleanupScope,
    result: Arc<Mutex<Option<Result<(), Error>>>>,
    abort: AbortHandle,
    /// The scope of the run that polled this one, for the wait of the drop.
    outer: Option<CleanupScope>,
}

struct Run {
    future: Option<CxxFuture<()>>,
    spawned: Option<Spawned>,
}

impl Future for Run {
    type Output = Result<(), Error>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        if let Some(future) = this.future.take() {
            let scope = CleanupScope::new();
            let result = Arc::new(Mutex::new(None));
            let main = Main {
                future,
                result: result.clone(),
                _hold: scope.hold(),
            };
            let abort = reactor().handle.spawn(SCOPE.scope(scope.clone(), main)).abort_handle();
            this.spawned = Some(Spawned {
                scope,
                result,
                abort,
                outer: CleanupScope::current(),
            });
        }
        let spawned = this.spawned.as_ref().expect("a run is spawned on its first poll");
        std::task::ready!(spawned.scope.poll_idle(cx));
        Poll::Ready(
            lock(&spawned.result)
                .take()
                .unwrap_or_else(|| Err(Error::msg("the future on the reactor panicked"))),
        )
    }
}

impl Drop for Run {
    fn drop(&mut self) {
        let Some(spawned) = self.spawned.take() else {
            return;
        };
        spawned.abort.abort();
        if on_reactor_thread() {
            // Waiting here would block the thread that has to do the work.
            if let Some(outer) = &spawned.outer {
                let scope = spawned.scope.clone();
                outer.defer(poll_fn(move |cx| scope.poll_idle(cx)));
            }
            return;
        }
        spawned.scope.wait_idle();
    }
}

fn reactor_spawn(future: NixFuture) -> NixFuture {
    // SAFETY: the bridge passes ownership of `future`, a `nix_future` of
    // `()` that C++ made with `toCFuture`.
    let future = unsafe { CxxFuture::<()>::new(future) };
    export_future(Run {
        future: Some(future),
        spawned: None,
    })
}

// Timers.

fn reactor_sleep(millis: u64) -> NixFuture {
    export_future(async move {
        // Made on the first poll, which makes the future lazy, and in the
        // context of the reactor, whose timer wheel it registers with.
        let sleep = {
            let _context = reactor().handle.enter();
            tokio::time::sleep(Duration::from_millis(millis))
        };
        sleep.await;
        Ok::<(), Error>(())
    })
}

struct TimerShared {
    deadline: Mutex<Option<Instant>>,
    changed: Notify,
}

pub struct ReactorTimer {
    shared: Arc<TimerShared>,
    task: AbortHandle,
}

fn reactor_timer(callback: UniquePtr<ReactorCallback>) -> Box<ReactorTimer> {
    let shared = Arc::new(TimerShared {
        deadline: Mutex::new(None),
        changed: Notify::new(),
    });
    let task = {
        let shared = shared.clone();
        reactor().handle.spawn(async move {
            loop {
                // Made before the deadline is read: a `set` after the read
                // leaves a permit that this future takes.
                let mut changed = pin!(shared.changed.notified());
                let Some(deadline) = *lock(&shared.deadline) else {
                    changed.await;
                    continue;
                };
                let mut sleep = pin!(tokio::time::sleep_until(deadline.into()));
                let expired = poll_fn(|cx| {
                    if changed.as_mut().poll(cx).is_ready() {
                        return Poll::Ready(false);
                    }
                    sleep.as_mut().poll(cx).map(|()| true)
                })
                .await;
                if !expired {
                    continue;
                }
                {
                    let mut current = lock(&shared.deadline);
                    if *current != Some(deadline) {
                        continue;
                    }
                    *current = None;
                }
                callback.run();
            }
        })
    };
    Box::new(ReactorTimer {
        shared,
        task: task.abort_handle(),
    })
}

impl ReactorTimer {
    fn set(&self, millis: i64) {
        *lock(&self.shared.deadline) = u64::try_from(millis)
            .ok()
            .map(|millis| Instant::now() + Duration::from_millis(millis));
        self.shared.changed.notify_one();
    }
}

impl Drop for ReactorTimer {
    fn drop(&mut self) {
        *lock(&self.shared.deadline) = None;
        self.task.abort();
    }
}

// Readiness of file descriptors.

#[cfg(unix)]
mod fd {
    use super::*;
    use std::os::fd::{AsRawFd, RawFd};
    use tokio::io::Interest;
    use tokio::io::unix::AsyncFd;

    /// A descriptor that the reactor watches but does not own: dropping it
    /// does not close the descriptor.
    pub struct Unowned(pub RawFd);

    impl AsRawFd for Unowned {
        fn as_raw_fd(&self) -> RawFd {
            self.0
        }
    }

    pub fn register(fd: RawFd, interest: Interest) -> std::io::Result<AsyncFd<Unowned>> {
        let _context = reactor().handle.enter();
        AsyncFd::with_interest(Unowned(fd), interest)
    }

    /// The readiness of `fd` now, from `poll(2)`. An error or a hangup
    /// counts as readiness in both directions.
    pub fn poll_now(fd: RawFd) -> (bool, bool) {
        let mut pollfd = libc::pollfd {
            fd,
            events: libc::POLLIN | libc::POLLOUT,
            revents: 0,
        };
        // SAFETY: `pollfd` is one valid `struct pollfd`, and the timeout of
        // 0 makes the call return at once.
        if unsafe { libc::poll(&mut pollfd, 1, 0) } != 1 {
            return (false, false);
        }
        let failed = pollfd.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0;
        (
            failed || pollfd.revents & libc::POLLIN != 0,
            failed || pollfd.revents & libc::POLLOUT != 0,
        )
    }

    /// Poll the readiness of `afd` for the directions of the interest.
    /// Leaves the readiness in place.
    pub fn poll_ready(afd: &AsyncFd<Unowned>, cx: &mut Context<'_>, readable: bool, writable: bool) -> (bool, bool) {
        let r = readable && afd.poll_read_ready(cx).is_ready();
        let w = writable && afd.poll_write_ready(cx).is_ready();
        (r, w)
    }

    /// Clear the readiness of the directions that `poll(2)` no longer
    /// reports. tokio registers descriptors edge-triggered, and keeps a
    /// direction ready until it is cleared. A user that does not read
    /// until `EAGAIN`, such as curl, needs the readiness of a
    /// level-triggered `poll`: ready again while data is left.
    ///
    /// No readiness event is lost: the reactor reads events only when its
    /// thread parks, which cannot happen between the `poll(2)` and the
    /// clear, and data that arrives after the `poll(2)` makes a new edge.
    pub fn clear_stale(afd: &AsyncFd<Unowned>, cx: &mut Context<'_>, readable: bool, writable: bool) {
        let (still_r, still_w) = poll_now(afd.get_ref().0);
        if readable
            && !still_r
            && let Poll::Ready(Ok(mut guard)) = afd.poll_read_ready(cx)
        {
            guard.clear_ready();
        }
        if writable
            && !still_w
            && let Poll::Ready(Ok(mut guard)) = afd.poll_write_ready(cx)
        {
            guard.clear_ready();
        }
    }

    pub fn interest(readable: bool, writable: bool) -> Option<Interest> {
        match (readable, writable) {
            (true, true) => Some(Interest::READABLE | Interest::WRITABLE),
            (true, false) => Some(Interest::READABLE),
            (false, true) => Some(Interest::WRITABLE),
            (false, false) => None,
        }
    }
}

#[cfg(unix)]
fn reactor_wait_fd(fd: i32, readable: bool, writable: bool) -> NixFuture {
    export_future(async move {
        let interest = fd::interest(readable, writable)
            .ok_or_else(|| Error::msg("waiting for a file descriptor needs a direction"))?;
        let afd = fd::register(fd, interest)
            .map_err(|e| Error::msg(format!("registering file descriptor {fd} with the reactor: {e}")))?;
        let (r, w) = poll_fn(|cx| match fd::poll_ready(&afd, cx, readable, writable) {
            (false, false) => Poll::Pending,
            ready => Poll::Ready(ready),
        })
        .await;
        Ok::<u64, Error>(u64::from(r) | u64::from(w) << 1)
    })
}

#[cfg(not(unix))]
fn reactor_wait_fd(_fd: i32, _readable: bool, _writable: bool) -> NixFuture {
    export_future(async move {
        Err::<u64, Error>(Error::msg(
            "waiting for a file descriptor is not supported on this platform",
        ))
    })
}

#[cfg(unix)]
struct FdShared {
    /// The registration. The drop of the watch takes it, which removes
    /// the descriptor from the reactor at once, before the owner closes
    /// it.
    afd: Mutex<Option<tokio::io::unix::AsyncFd<fd::Unowned>>>,
    interest: Mutex<(bool, bool)>,
    changed: Notify,
}

pub struct FdWatch {
    #[cfg(unix)]
    shared: Arc<FdShared>,
    #[cfg(unix)]
    task: AbortHandle,
}

#[cfg(unix)]
fn reactor_watch_fd(fd: i32, callback: UniquePtr<FdCallback>) -> Result<Box<FdWatch>, String> {
    use tokio::io::Interest;

    let afd = fd::register(fd, Interest::READABLE | Interest::WRITABLE)
        .map_err(|e| format!("registering file descriptor {fd} with the reactor: {e}"))?;
    let shared = Arc::new(FdShared {
        afd: Mutex::new(Some(afd)),
        interest: Mutex::new((false, false)),
        changed: Notify::new(),
    });
    let task = {
        let shared = shared.clone();
        reactor().handle.spawn(async move {
            loop {
                let mut changed = pin!(shared.changed.notified());
                let (readable, writable) = *lock(&shared.interest);
                // `None`: the watch is dropped. `Some(None)`: the interest
                // changed.
                let ready = poll_fn(|cx| {
                    if changed.as_mut().poll(cx).is_ready() {
                        return Poll::Ready(Some(None));
                    }
                    let afd = lock(&shared.afd);
                    let Some(afd) = afd.as_ref() else {
                        return Poll::Ready(None);
                    };
                    match fd::poll_ready(afd, cx, readable, writable) {
                        (false, false) => Poll::Pending,
                        ready => Poll::Ready(Some(Some(ready))),
                    }
                })
                .await;
                let Some(ready) = ready else { return };
                let Some((r, w)) = ready else { continue };
                // Not under the lock: the callback can drop the watch.
                callback.ready(r, w);
                poll_fn(|cx| {
                    if let Some(afd) = lock(&shared.afd).as_ref() {
                        fd::clear_stale(afd, cx, r, w);
                    }
                    Poll::Ready(())
                })
                .await;
            }
        })
    };
    Ok(Box::new(FdWatch {
        shared,
        task: task.abort_handle(),
    }))
}

#[cfg(not(unix))]
fn reactor_watch_fd(_fd: i32, _callback: UniquePtr<FdCallback>) -> Result<Box<FdWatch>, String> {
    Err("watching file descriptors is not supported on this platform".to_owned())
}

impl FdWatch {
    #[cfg(unix)]
    fn set_interest(&self, readable: bool, writable: bool) {
        *lock(&self.shared.interest) = (readable, writable);
        self.shared.changed.notify_one();
    }

    #[cfg(not(unix))]
    fn set_interest(&self, _readable: bool, _writable: bool) {}
}

#[cfg(unix)]
impl Drop for FdWatch {
    fn drop(&mut self) {
        drop(lock(&self.shared.afd).take());
        self.task.abort();
    }
}
