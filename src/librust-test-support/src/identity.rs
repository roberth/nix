//! Rust errors and panics that cross into C++ and come back.
//!
//! The tests in `libutil-tests/rust-identity.cc` pass the error or the
//! panic of a probe future to C++, give the C++ exception back to Rust as
//! the error of a C++ future, and ask Rust whether it got the same value:
//! the same allocation, not only the same message.

use nix_rust_future::{CxxFuture, Error, NixException, NixFuture, bridge, export_future, take_pending_exception};
use std::ffi::{c_char, c_void};
use std::fmt;
use std::future::{Future, poll_fn};
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::pin::Pin;
use std::sync::Mutex;
use std::task::Poll;

/// A Rust error with an identity.
#[derive(Debug)]
struct ProbeError {
    id: u64,
}

impl fmt::Display for ProbeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "probe error {}", self.id)
    }
}

impl std::error::Error for ProbeError {}

/// The addresses of the last `ProbeError` and of the bytes of the last
/// panic message that the probes made. Rust moves the boxes, not the
/// values in them, so an address identifies a value across the round trip.
static LAST_ERROR: Mutex<usize> = Mutex::new(0);
static LAST_PANIC: Mutex<usize> = Mutex::new(0);

/// A future that fails with a `ProbeError` with `id`.
#[unsafe(no_mangle)]
pub extern "C" fn nix_rust_test_future_probe_error(id: u64) -> NixFuture {
    export_future(async move {
        let error = Box::new(ProbeError { id });
        *LAST_ERROR.lock().expect("not poisoned") = &*error as *const ProbeError as usize;
        Err::<(), _>(Error::from_rust(error))
    })
}

/// A future that panics with the payload `probe panic <id>`, a `String`.
#[unsafe(no_mangle)]
pub extern "C" fn nix_rust_test_future_probe_panic(id: u64) -> NixFuture {
    export_future(async move {
        let message = format!("probe panic {id}");
        *LAST_PANIC.lock().expect("not poisoned") = message.as_ptr() as usize;
        // `resume_unwind` does not run the panic hook, so the test output
        // has no panic message.
        resume_unwind(Box::new(message));
        #[allow(unreachable_code)]
        Ok::<(), Error>(())
    })
}

/// The Rust half of a synchronous bridge function that panics with the
/// payload `probe panic <id>`: its body runs in `bridge`. Writes the message
/// that cxx would throw as a `rust::Error` to `sink`. The test support
/// library has no cxx, so the test plays the part of the cxx shim and of
/// `nix::rs::call`, with `nix_rust_test_take_pending_exception`.
///
/// # Safety
///
/// `sink` must accept `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_bridge_panic(id: u64, sink: crate::StringSink, context: *mut c_void) {
    let result = bridge(|| -> Result<(), Error> {
        let message = format!("probe panic {id}");
        *LAST_PANIC.lock().expect("not poisoned") = message.as_ptr() as usize;
        resume_unwind(Box::new(message))
    });
    let message = match result {
        Ok(()) => "no error".to_owned(),
        Err(error) => error.to_string(),
    };
    // SAFETY: The caller guarantees that `sink` accepts `context`.
    unsafe { crate::send_string(sink, context, &message) }
}

/// `nix_rust_take_pending_exception` of this copy of the Rust code.
///
/// # Safety
///
/// `message` must be valid for `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_take_pending_exception(
    message: *const c_char,
    length: usize,
) -> *mut NixException {
    // SAFETY: The caller guarantees that `message` is valid for `length`
    // bytes.
    let message = unsafe { std::slice::from_raw_parts(message.cast::<u8>(), length) };
    take_pending_exception(message)
}

/// Panics when it is dropped.
struct PanicOnDrop;

impl Drop for PanicOnDrop {
    fn drop(&mut self) {
        resume_unwind(Box::new("probe panic in drop"));
    }
}

/// A future that completes with `value`, and panics when the completed
/// future is dropped.
#[unsafe(no_mangle)]
pub extern "C" fn nix_rust_test_future_panic_on_drop() -> NixFuture {
    let guard = PanicOnDrop;
    export_future(poll_fn(move |_| {
        let _guard = &guard;
        Poll::Ready(Ok::<_, Error>("value".to_owned()))
    }))
}

/// A future that awaits `inner`, a C++ future without a value, and
/// describes how it failed: `same Rust error <id>` if the error is the last
/// `ProbeError`, `same panic: <message>` if the panic resumed with the
/// last payload, and otherwise the message of what it got.
///
/// # Safety
///
/// `inner` must be a valid, owned `nix_future` whose output type is `()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_test_future_identify(inner: NixFuture) -> NixFuture {
    // SAFETY: The caller passes the ownership of a valid `nix_future`.
    let mut inner = unsafe { CxxFuture::<()>::new(inner) };
    export_future(poll_fn(move |cx| {
        let polled = catch_unwind(AssertUnwindSafe(|| Pin::new(&mut inner).poll(cx)));
        let description = match polled {
            Ok(Poll::Pending) => return Poll::Pending,
            Ok(Poll::Ready(Ok(()))) => "no error".to_owned(),
            Ok(Poll::Ready(Err(error))) => describe_error(&error),
            Err(payload) => describe_panic(payload.as_ref()),
        };
        Poll::Ready(Ok::<_, Error>(description))
    }))
}

fn describe_error(error: &Error) -> String {
    let Some(probe) = error.rust_error().and_then(|e| e.downcast_ref::<ProbeError>()) else {
        return format!("other error: {error}");
    };
    let address = probe as *const ProbeError as usize;
    if address == *LAST_ERROR.lock().expect("not poisoned") {
        format!("same Rust error {}", probe.id)
    } else {
        format!("a copy of Rust error {}", probe.id)
    }
}

fn describe_panic(payload: &(dyn std::any::Any + Send)) -> String {
    let Some(message) = payload.downcast_ref::<String>() else {
        return "a panic with another payload".to_owned();
    };
    if message.as_ptr() as usize == *LAST_PANIC.lock().expect("not poisoned") {
        format!("same panic: {message}")
    } else {
        format!("another panic: {message}")
    }
}
