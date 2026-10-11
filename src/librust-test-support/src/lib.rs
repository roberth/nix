//! Rust functions for the unit tests of the boundary between C++ and Rust.
//!
//! The tests in `libutil-tests/rust.cc`, `libutil-tests/rust-future.cc` and
//! `libstore-tests/filetransfer-cancel.cc` call them. They show that values,
//! errors, panics, wakers and cancellation cross the C ABI of
//! `nix/rust/future-abi.h` in both directions, and that a panic unwinds
//! through Rust frames in the link layout of the build.
//!
//! This crate is a second Rust staticlib, separate from `libnixrust`, so
//! that the shipped library does not contain test code. Each test program
//! therefore has two copies of the Rust standard library: the one in
//! `libnixrust`, and the one here. That only works because neither library
//! exports its Rust symbols, and the tests show that it does. The two copies
//! share no Rust types; they meet only at the C ABI:
//!
//! - The futures here are `nix_future` values from this copy of
//!   `nix-rust-future`. The C functions `nix_exception_*` resolve to
//!   `libnixrust`, so that a Rust error becomes the same `nix::Error` as an
//!   error from `libnixrust`.
//! - An output of type `String` is written by one copy of the Rust standard
//!   library and freed by another (through `rust::String` in C++). Both use
//!   the default global allocator, the C library's `malloc`.
//!
//! The functions are `extern "C"` without `catch_unwind`. A panic that
//! would leave one of them aborts the process, as for every `extern "C"`
//! function since Rust 1.81. The futures turn a panic into an error.

use nix_rust_future::{
    CxxFuture, Error, NixFuture, NixWaker, export_future, nix_waker_from_rust, waker_from_nix,
    with_borrowed_nix_waker,
};
use std::ffi::{c_char, c_void};
use std::future::poll_fn;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::task::{Poll, Waker};
use std::thread;
use std::time::Duration;

/// `nix_rust_test_log`: an event log that a test owns.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NixRustTestLog {
    context: *mut c_void,
    push: unsafe extern "C" fn(context: *mut c_void, event: *const c_char, length: usize),
}

/// `nix_rust_test_string_sink`.
type StringSink = unsafe extern "C" fn(context: *mut c_void, data: *const c_char, length: usize);

/// A log that a future keeps.
///
/// The test keeps the log alive for longer than the future, and the log
/// is called only from the thread that polls or drops the future; see
/// `nix/rust/tests/probes.hh`.
#[derive(Clone, Copy)]
struct LogRef(NixRustTestLog);

// SAFETY: The test calls the log only from one thread at a time, the thread
// that polls or drops the future.
unsafe impl Send for LogRef {}

impl LogRef {
    /// # Safety
    ///
    /// `log` must stay valid for as long as the `LogRef` exists.
    unsafe fn new(log: *const NixRustTestLog) -> Self {
        // SAFETY: The caller guarantees that `log` is valid.
        LogRef(unsafe { *log })
    }

    fn push(self, event: &str) {
        // SAFETY: `new` requires the log to outlive this value, and `event`
        // is valid for `event.len()` bytes.
        unsafe { (self.0.push)(self.0.context, event.as_ptr().cast(), event.len()) }
    }
}

/// Logs `rust: dropped` when the future that owns it is dropped.
struct DropLogger(LogRef);

impl Drop for DropLogger {
    fn drop(&mut self) {
        self.0.push("rust: dropped");
    }
}

/// # Safety
///
/// `data` must be valid for `length` bytes of UTF-8.
unsafe fn str_from_c<'a>(data: *const c_char, length: usize) -> &'a str {
    // SAFETY: The caller guarantees that `data` is valid for `length` bytes.
    let bytes = unsafe { std::slice::from_raw_parts(data.cast::<u8>(), length) };
    std::str::from_utf8(bytes).expect("a probe argument is UTF-8")
}

/// # Safety
///
/// `sink` must accept `context`.
unsafe fn send_string(sink: StringSink, context: *mut c_void, value: &str) {
    // SAFETY: `value` is valid for `value.len()` bytes, and the caller
    // guarantees that `sink` accepts `context`.
    unsafe { sink(context, value.as_ptr().cast(), value.len()) }
}

/// The version of the Rust compiler that built this library, as a C
/// string, for example `1.95.0`.
#[unsafe(no_mangle)]
pub extern "C" fn nix_rust_test_rustc_version() -> *const c_char {
    concat!(env!("NIX_RUST_RUSTC_VERSION"), "\0").as_ptr().cast()
}

#[inline(never)]
fn unwind() {
    // `resume_unwind` does not call the panic hook, so the test output
    // stays clean.
    std::panic::resume_unwind(Box::new("probe panic"));
}

/// Unwind a panic through Rust frames, catch it again, and describe the
/// payload. This needs a working unwinder for Rust frames, which in a
/// static build is the unwinder of the C++ runtime.
///
/// # Safety
///
/// `sink` must accept `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_catch_panic(sink: StringSink, context: *mut c_void) {
    let description = match std::panic::catch_unwind(unwind) {
        Ok(()) => "not caught".to_owned(),
        Err(payload) => match payload.downcast_ref::<&str>() {
            Some(message) => format!("caught: {message}"),
            None => "caught: unknown payload".to_owned(),
        },
    };
    // SAFETY: The caller guarantees that `sink` accepts `context`.
    unsafe { send_string(sink, context, &description) }
}

/// A future that completes with `value`.
///
/// # Safety
///
/// `value` must be valid for `length` bytes of UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_future_ready(value: *const c_char, length: usize) -> NixFuture {
    // SAFETY: The caller guarantees that `value` is valid.
    let value = unsafe { str_from_c(value, length) }.to_owned();
    export_future(async move { Ok::<_, Error>(value) })
}

/// A future that fails with a Rust error.
///
/// # Safety
///
/// `message` must be valid for `length` bytes of UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_future_error(message: *const c_char, length: usize) -> NixFuture {
    // SAFETY: The caller guarantees that `message` is valid.
    let message = unsafe { str_from_c(message, length) }.to_owned();
    export_future(async move { Err::<(), _>(Error::msg(message)) })
}

/// A future that panics.
///
/// # Safety
///
/// `message` must be valid for `length` bytes of UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_future_panic(message: *const c_char, length: usize) -> NixFuture {
    // SAFETY: The caller guarantees that `message` is valid.
    let message = unsafe { str_from_c(message, length) }.to_owned();
    export_future(async move {
        // `resume_unwind` does not run the panic hook, so the test output
        // has no panic message.
        std::panic::resume_unwind(Box::new(message));
        #[allow(unreachable_code)]
        Ok::<(), Error>(())
    })
}

/// A future that awaits `inner`, a C++ future of `rust::String`, and
/// completes with `Rust saw: <value>`, or with the error of `inner`
/// unchanged. Logs `rust: dropped` to `log` when it is dropped.
///
/// # Safety
///
/// `inner` must be a valid, owned `nix_future` whose output type is
/// `String`. `log` must outlive the returned future.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_future_forward(
    inner: NixFuture,
    log: *const NixRustTestLog,
) -> NixFuture {
    // SAFETY: The caller passes the ownership of a valid `nix_future`.
    let inner = unsafe { CxxFuture::<String>::new(inner) };
    // SAFETY: The caller guarantees that `log` outlives the future.
    let logger = DropLogger(unsafe { LogRef::new(log) });
    export_future(async move {
        let _logger = logger;
        let value = inner.await?;
        Ok(format!("Rust saw: {value}"))
    })
}

/// A future that never completes. Logs `rust: polled` on each poll and
/// `rust: dropped` when it is dropped.
///
/// # Safety
///
/// `log` must outlive the returned future.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_future_pending(log: *const NixRustTestLog) -> NixFuture {
    // SAFETY: The caller guarantees that `log` outlives the future.
    let log = unsafe { LogRef::new(log) };
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

/// A future that never completes, and that a thread wakes every
/// `interval_ms`. Logs `rust: started` on the first poll, and
/// `rust: dropped` after it stopped the thread on drop. The number of
/// polls is in `nix_rust_test_ticking_polls()`.
///
/// # Safety
///
/// `log` must outlive the returned future.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_future_ticking(log: *const NixRustTestLog, interval_ms: u64) -> NixFuture {
    // SAFETY: The caller guarantees that `log` outlives the future.
    let log = unsafe { LogRef::new(log) };
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

/// The number of polls of all `nix_rust_test_future_ticking` futures.
#[unsafe(no_mangle)]
pub extern "C" fn nix_rust_test_ticking_polls() -> usize {
    TICKING_POLLS.load(Ordering::SeqCst)
}

static WAKE_THREADS_RUNNING: AtomicUsize = AtomicUsize::new(0);

struct WokenShared {
    ready: AtomicBool,
    waker: Mutex<Option<Waker>>,
}

/// A future that a thread wakes after `delay_ms`, and that then completes
/// with `woken from a Rust thread`. The thread wakes the waker again
/// `late_wake_ms` after that, when the future is already complete, and
/// then drops the waker.
#[unsafe(no_mangle)]
pub extern "C" fn nix_rust_test_future_woken_from_thread(delay_ms: u64, late_wake_ms: u64) -> NixFuture {
    let shared = Arc::new(WokenShared {
        ready: AtomicBool::new(false),
        waker: Mutex::new(None),
    });
    let mut started = false;
    export_future(poll_fn(move |cx| {
        if shared.ready.load(Ordering::SeqCst) {
            return Poll::Ready(Ok::<_, Error>("woken from a Rust thread".to_owned()));
        }
        *shared.waker.lock().expect("the waker lock is not poisoned") = Some(cx.waker().clone());
        if !started {
            started = true;
            WAKE_THREADS_RUNNING.fetch_add(1, Ordering::SeqCst);
            let shared = shared.clone();
            thread::spawn(move || {
                thread::sleep(Duration::from_millis(delay_ms));
                shared.ready.store(true, Ordering::SeqCst);
                let waker = shared
                    .waker
                    .lock()
                    .expect("the waker lock is not poisoned")
                    .clone();
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

/// The number of threads of `nix_rust_test_future_woken_from_thread` that
/// have not finished.
#[unsafe(no_mangle)]
pub extern "C" fn nix_rust_test_wake_threads_running() -> usize {
    WAKE_THREADS_RUNNING.load(Ordering::SeqCst)
}

/// Clone, wake and drop `waker` in all the ways that the ABI allows, and
/// convert wakers back and forth. Describes whether the conversions
/// unwrapped the wakers instead of wrapping them again.
///
/// # Safety
///
/// `waker` must be a valid `nix_waker` for the duration of the call, and
/// `sink` must accept `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_waker_roundtrip(
    waker: *const NixWaker,
    sink: StringSink,
    context: *mut c_void,
) {
    // SAFETY: The caller guarantees that `waker` is valid during the call.
    let unwrapped_nix = unsafe {
        with_borrowed_nix_waker(waker, |borrowed| {
            let a = borrowed.clone();
            let b = a.clone();
            let c = b.clone();
            a.wake();
            b.wake_by_ref();
            drop(b);
            // `c` wraps a clone of the C++ waker. Going back to C++ unwraps
            // it: the result is the C++ waker itself.
            let back = nix_waker_from_rust(c);
            let unwrapped = back.data == (*waker).data && std::ptr::eq(back.vtable, (*waker).vtable);
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
    // SAFETY: `nix_waker_from_rust` returns an owned, valid `nix_waker`.
    let back = unsafe { waker_from_nix(nix_waker_from_rust(rust.clone())) };
    let unwrapped_rust = back.will_wake(&rust);

    let description = format!("C++ waker unwrapped: {unwrapped_nix}, Rust waker unwrapped: {unwrapped_rust}");
    // SAFETY: The caller guarantees that `sink` accepts `context`.
    unsafe { send_string(sink, context, &description) }
}
