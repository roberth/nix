//! Rust futures that the unit tests of the C ABI futures drive from C++.
//!
//! The tests in `libutil-tests/rust-future.cc` and
//! `libstore-tests/filetransfer-cancel.cc` use them to show that values,
//! errors, panics, wakers and cancellation cross the boundary in both
//! directions. See `future.rs` for the layer under test.

use crate::future::{
    CxxFuture, Error, NixFuture, NixWaker, export_future, nix_waker_from_rust, waker_from_nix,
    with_borrowed_nix_waker,
};
use std::future::poll_fn;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::task::{Poll, Waker};
use std::thread;
use std::time::Duration;

#[cxx::bridge(namespace = "nix::rs")]
mod ffi {
    unsafe extern "C++" {
        include!("nix/rust/future-probe.hh");

        #[namespace = ""]
        type nix_future = crate::future::NixFuture;

        #[namespace = ""]
        type nix_waker = crate::future::NixWaker;

        /// An event log that the test owns.
        type ProbeLog;

        fn push(self: &ProbeLog, event: &str);
    }

    extern "Rust" {
        /// A future that completes with `value`.
        fn probe_future_ready(value: String) -> nix_future;

        /// A future that fails with a Rust error.
        fn probe_future_error(message: &str) -> nix_future;

        /// A future that panics.
        fn probe_future_panic(message: &str) -> nix_future;

        /// A future that awaits `inner`, a C++ future of `rust::String`, and
        /// completes with `Rust saw: <value>`, or with the error of `inner`
        /// unchanged. Logs `rust: dropped` to `log` when it is dropped.
        unsafe fn probe_future_forward(inner: nix_future, log: &ProbeLog) -> nix_future;

        /// A future that never completes. Logs `rust: polled` on each poll
        /// and `rust: dropped` when it is dropped.
        unsafe fn probe_future_pending(log: &ProbeLog) -> nix_future;

        /// A future that never completes, and that a thread wakes every
        /// `interval_ms`. Logs `rust: started` on the first poll, and
        /// `rust: dropped` after it stopped the thread on drop. The number
        /// of polls is in `probe_ticking_polls()`.
        unsafe fn probe_future_ticking(log: &ProbeLog, interval_ms: u64) -> nix_future;

        /// The number of polls of all `probe_future_ticking` futures.
        fn probe_ticking_polls() -> usize;

        /// A future that a thread wakes after `delay_ms`, and that then
        /// completes with `woken from a Rust thread`. The thread wakes the
        /// waker again `late_wake_ms` after that, when the future is
        /// already complete, and then drops the waker.
        fn probe_future_woken_from_thread(delay_ms: u64, late_wake_ms: u64) -> nix_future;

        /// The number of threads of `probe_future_woken_from_thread` that
        /// have not finished.
        fn probe_wake_threads_running() -> usize;

        /// Clone, wake and drop `waker` in all the ways that the ABI
        /// allows, and convert wakers back and forth. Describes whether
        /// the conversions unwrapped the wakers instead of wrapping them
        /// again.
        unsafe fn probe_waker_roundtrip(waker: &nix_waker) -> String;
    }
}

/// A pointer to a `ProbeLog` that a future keeps. The test keeps the log
/// alive for longer than the future, and calls into it only from the
/// thread that polls or drops the future.
#[derive(Clone, Copy)]
struct LogRef(*const ffi::ProbeLog);

unsafe impl Send for LogRef {}

impl LogRef {
    fn push(self, event: &str) {
        unsafe { &*self.0 }.push(event);
    }
}

/// Logs `rust: dropped` when the future that owns it is dropped.
struct DropLogger(LogRef);

impl Drop for DropLogger {
    fn drop(&mut self) {
        self.0.push("rust: dropped");
    }
}

fn probe_future_ready(value: String) -> NixFuture {
    export_future(async move { Ok::<_, Error>(value) })
}

fn probe_future_error(message: &str) -> NixFuture {
    let message = message.to_owned();
    export_future(async move { Err::<(), _>(Error::msg(message)) })
}

fn probe_future_panic(message: &str) -> NixFuture {
    let message = message.to_owned();
    export_future(async move {
        // `resume_unwind` does not run the panic hook, so the test output
        // has no panic message.
        std::panic::resume_unwind(Box::new(message));
        #[allow(unreachable_code)]
        Ok::<(), Error>(())
    })
}

unsafe fn probe_future_forward(inner: NixFuture, log: &ffi::ProbeLog) -> NixFuture {
    let inner = unsafe { CxxFuture::<String>::new(inner) };
    let logger = DropLogger(LogRef(log));
    export_future(async move {
        let _logger = logger;
        let value = inner.await?;
        Ok(format!("Rust saw: {value}"))
    })
}

unsafe fn probe_future_pending(log: &ffi::ProbeLog) -> NixFuture {
    let log = LogRef(log);
    let logger = DropLogger(log);
    export_future(async move {
        let _logger = logger;
        poll_fn(move |_| {
            log.push("rust: polled");
            Poll::<()>::Pending
        })
        .await;
        Ok::<(), Error>(())
    })
}

static TICKING_POLLS: AtomicUsize = AtomicUsize::new(0);

struct Ticker {
    stop: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
    log: LogRef,
}

impl Drop for Ticker {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        self.log.push("rust: dropped");
    }
}

unsafe fn probe_future_ticking(log: &ffi::ProbeLog, interval_ms: u64) -> NixFuture {
    let log = LogRef(log);
    export_future(async move {
        let mut ticker: Option<Ticker> = None;
        poll_fn(move |cx| {
            TICKING_POLLS.fetch_add(1, Ordering::SeqCst);
            if ticker.is_none() {
                log.push("rust: started");
                let stop = Arc::new(AtomicBool::new(false));
                let waker = cx.waker().clone();
                let thread = {
                    let stop = stop.clone();
                    thread::spawn(move || {
                        while !stop.load(Ordering::SeqCst) {
                            thread::sleep(Duration::from_millis(interval_ms));
                            waker.wake_by_ref();
                        }
                    })
                };
                ticker = Some(Ticker {
                    stop,
                    thread: Some(thread),
                    log,
                });
            }
            Poll::<()>::Pending
        })
        .await;
        Ok::<(), Error>(())
    })
}

fn probe_ticking_polls() -> usize {
    TICKING_POLLS.load(Ordering::SeqCst)
}

static WAKE_THREADS_RUNNING: AtomicUsize = AtomicUsize::new(0);

struct WokenShared {
    ready: AtomicBool,
    waker: Mutex<Option<Waker>>,
}

fn probe_future_woken_from_thread(delay_ms: u64, late_wake_ms: u64) -> NixFuture {
    let shared = Arc::new(WokenShared {
        ready: AtomicBool::new(false),
        waker: Mutex::new(None),
    });
    let mut started = false;
    export_future(poll_fn(move |cx| {
        if shared.ready.load(Ordering::SeqCst) {
            return Poll::Ready(Ok::<_, Error>("woken from a Rust thread".to_owned()));
        }
        *shared.waker.lock().unwrap() = Some(cx.waker().clone());
        if !started {
            started = true;
            WAKE_THREADS_RUNNING.fetch_add(1, Ordering::SeqCst);
            let shared = shared.clone();
            thread::spawn(move || {
                thread::sleep(Duration::from_millis(delay_ms));
                shared.ready.store(true, Ordering::SeqCst);
                let waker = shared.waker.lock().unwrap().clone();
                if let Some(waker) = &waker {
                    waker.wake_by_ref();
                }
                thread::sleep(Duration::from_millis(late_wake_ms));
                // The future is complete now, and its task may be gone too.
                if let Some(waker) = waker {
                    waker.wake();
                }
                drop(shared);
                WAKE_THREADS_RUNNING.fetch_sub(1, Ordering::SeqCst);
            });
        }
        Poll::Pending
    }))
}

fn probe_wake_threads_running() -> usize {
    WAKE_THREADS_RUNNING.load(Ordering::SeqCst)
}

unsafe fn probe_waker_roundtrip(waker: &NixWaker) -> String {
    let unwrapped_nix = unsafe {
        with_borrowed_nix_waker(waker as *const NixWaker, |borrowed| {
            let a = borrowed.clone();
            let b = a.clone();
            let c = b.clone();
            a.wake();
            b.wake_by_ref();
            drop(b);
            // `c` wraps a clone of the C++ waker. Going back to C++ unwraps
            // it: the result is the C++ waker itself.
            let back = nix_waker_from_rust(c);
            let unwrapped = back.data == waker.data && std::ptr::eq(back.vtable, waker.vtable);
            drop(waker_from_nix(back));
            unwrapped
        })
    };

    // `Waker::noop()` has a vtable in a constant, which need not have one
    // address, so `will_wake` is unreliable for it. An `Arc` waker has a
    // vtable in a static.
    struct NoWake;
    impl std::task::Wake for NoWake {
        fn wake(self: Arc<Self>) {}
    }
    let rust = Waker::from(Arc::new(NoWake));
    let back = unsafe { waker_from_nix(nix_waker_from_rust(rust.clone())) };
    let unwrapped_rust = back.will_wake(&rust);

    format!("C++ waker unwrapped: {unwrapped_nix}, Rust waker unwrapped: {unwrapped_rust}")
}
