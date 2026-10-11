//! The Rust side of the C ABI futures in `nix/rust/future-abi.h`.
//!
//! C++ tasks (`nix::Task`) and Rust futures drive each other through this
//! ABI. Both sides have the semantics of a Rust future: a future is lazy,
//! has one owner, is driven by `poll` with a waker, and is cancelled by
//! dropping it.
//!
//! - [`export_future`] turns a Rust future into a [`NixFuture`] that C++
//!   awaits with `nix::awaitCFuture<T>()`.
//! - [`CxxFuture`] is a Rust future over a [`NixFuture`] that C++ made with
//!   `nix::toCFuture()`.
//! - [`waker_from_nix`] and [`nix_waker_from_rust`] convert wakers. A
//!   waker that crosses the boundary twice is unwrapped, not wrapped twice.
//! - [`Error`] carries a C++ exception, a Rust error, or a Rust panic
//!   across the boundary, and gives back the same value when it crosses
//!   back.
//!
//! The output of a future crosses the boundary by value, in storage that
//! the receiver provides. Only types with an [`AbiType`] name can cross;
//! cxx guarantees that their layout is the same in both languages.
//!
//! The crate refers to the C functions `nix_exception_*` of
//! `nix/rust/future-abi.h`, which `libnixrust` defines. It contains no C++
//! code itself, so that a second Rust library, such as
//! `nix-rust-test-support`, can use it and still share the C functions of
//! `libnixrust`.

use std::any::Any;
use std::ffi::{CStr, c_char, c_void};
use std::fmt;
use std::future::Future;
use std::marker::PhantomData;
use std::mem::{ManuallyDrop, MaybeUninit};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::pin::Pin;
use std::ptr::NonNull;
use std::sync::atomic::AtomicU8;
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

/// `nix_waker_vtable`.
#[repr(C)]
pub struct NixWakerVTable {
    pub clone: unsafe extern "C" fn(data: *const c_void) -> NixWaker,
    pub wake: unsafe extern "C" fn(data: *const c_void),
    pub wake_by_ref: unsafe extern "C" fn(data: *const c_void),
    pub drop: unsafe extern "C" fn(data: *const c_void),
}

/// `nix_waker`: a waker in the C ABI. As a value of this type, it is owned.
#[repr(C)]
pub struct NixWaker {
    pub data: *const c_void,
    pub vtable: *const NixWakerVTable,
}

// SAFETY: `nix_waker` in `future-abi.h` is a C struct with the same two
// pointer fields, and it is trivially movable: its owner is whoever holds
// the bits.
#[cfg(feature = "cxx")]
unsafe impl cxx::ExternType for NixWaker {
    type Id = cxx::type_id!("nix_waker");
    type Kind = cxx::kind::Trivial;
}

/// `nix_poll`.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum NixPoll {
    Ready = 0,
    Pending = 1,
}

/// `nix_exception`: an owned `std::exception_ptr`, opaque to Rust.
#[repr(C)]
pub struct NixException {
    _private: [u8; 0],
}

/// `nix_rust_error_kind`.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
enum NixRustErrorKind {
    Error = 0,
    Panic = 1,
}

/// `nix_rust_value`: a Rust value that a C++ exception owns.
#[repr(C)]
struct NixRustValue {
    kind: NixRustErrorKind,
    data: *mut c_void,
    drop: unsafe extern "C" fn(data: *mut c_void),
    owner: *const c_void,
}

/// `nix_future_vtable`.
#[repr(C)]
pub struct NixFutureVTable {
    pub poll: unsafe extern "C" fn(this: *mut c_void, waker: *const NixWaker) -> NixPoll,
    pub take: unsafe extern "C" fn(this: *mut c_void, out: *mut c_void) -> *mut NixException,
    pub drop: unsafe extern "C" fn(this: *mut c_void),
    pub output_type: *const c_char,
}

/// `nix_future`: a type-erased future in the C ABI. As a value of this
/// type, it is owned, but nothing drops it automatically. Wrap it in a
/// [`CxxFuture`], or return it to C++.
#[repr(C)]
pub struct NixFuture {
    pub this: *mut c_void,
    pub vtable: *const NixFutureVTable,
}

// SAFETY: `nix_future` in `future-abi.h` is a C struct with the same two
// pointer fields, and it is trivially movable, like `nix_waker`.
#[cfg(feature = "cxx")]
unsafe impl cxx::ExternType for NixFuture {
    type Id = cxx::type_id!("nix_future");
    type Kind = cxx::kind::Trivial;
}

// The C++ functions in `src/future-abi.cc`. They are in libnixrust, so these
// are not references to the libraries above it.
unsafe extern "C" {
    fn nix_exception_free(exception: *mut NixException);
    fn nix_exception_what(exception: *const NixException) -> *const c_char;
    fn nix_exception_from_rust(message: *const c_char, length: usize, value: NixRustValue) -> *mut NixException;
    fn nix_exception_take_rust_value(
        exception: *const NixException,
        owner: *const c_void,
        out: *mut NixRustValue,
    ) -> bool;
}

/// Identifies this copy of the crate in `nix_rust_value::owner`. A process
/// can contain more than one copy of the Rust standard library, for
/// example `libnixrust` and `libnix-rust-test-support`; a value that one
/// copy made must not be taken apart by another, whose types are not the
/// same. An atomic, so that the compiler gives it an address of its own.
static OWNER: AtomicU8 = AtomicU8::new(0);

fn owner() -> *const c_void {
    &OWNER as *const AtomicU8 as *const c_void
}

/// The name of the Rust type of a future's output, in
/// `nix_future_vtable::output_type`. The C++ trait `nix::rs::AbiType`
/// gives the same names.
///
/// # Safety
///
/// The type must have the same layout and the same ownership semantics as
/// its C++ counterpart, and `NAME` must be the same string on both sides.
pub unsafe trait AbiType: Send + 'static {
    const NAME: &'static CStr;
}

// SAFETY: C++ uses `void`, and passes no storage for it.
unsafe impl AbiType for () {
    const NAME: &'static CStr = c"()";
}

// SAFETY: `bool` has the same layout in Rust and C++ on all platforms of
// Nix, and cxx relies on that too.
unsafe impl AbiType for bool {
    const NAME: &'static CStr = c"bool";
}

// SAFETY: `uint64_t` in C++.
unsafe impl AbiType for u64 {
    const NAME: &'static CStr = c"u64";
}

// SAFETY: `int64_t` in C++.
unsafe impl AbiType for i64 {
    const NAME: &'static CStr = c"i64";
}

// SAFETY: `rust::String` in C++, which cxx defines with the layout of
// `String` and frees with the Rust allocator.
unsafe impl AbiType for String {
    const NAME: &'static CStr = c"String";
}

// SAFETY: `rust::Vec<uint8_t>` in C++, as for `String`.
unsafe impl AbiType for Vec<u8> {
    const NAME: &'static CStr = c"Vec<u8>";
}

/// The error of Rust code, as [`Error`] holds it.
pub type RustError = Box<dyn std::error::Error + Send + Sync + 'static>;

/// The payload of a Rust panic, as `catch_unwind` returns it.
pub type PanicPayload = Box<dyn Any + Send + 'static>;

/// An error that crosses the boundary in either direction.
///
/// The value keeps its identity on the way through the other language:
///
/// - A C++ exception stays a C++ exception: Rust holds it opaquely and
///   gives it back unchanged, so C++ rethrows the same exception object,
///   with its dynamic type.
/// - A Rust error goes to C++ inside an exception that owns it
///   (`nix::RustError`), and comes back out as the same Rust value.
/// - A Rust panic goes to C++ inside an exception that owns the payload
///   (`nix::RustPanic`). When that exception comes back into Rust,
///   [`Error::from_exception`] resumes the panic with the same payload.
pub struct Error(Repr);

enum Repr {
    Cxx(NonNull<NixException>),
    Rust(RustError),
    Panic(PanicPayload),
}

// SAFETY: A `std::exception_ptr` refers to an immutable exception object,
// and libstdc++ and libc++ count its references atomically, so it can move
// to another thread. The Rust error and the panic payload are `Send`.
unsafe impl Send for Error {}

impl Error {
    /// An error with a message.
    pub fn msg(message: impl Into<String>) -> Self {
        Error(Repr::Rust(message.into().into()))
    }

    /// A Rust error, which keeps its type across the boundary.
    pub fn new(error: impl std::error::Error + Send + Sync + 'static) -> Self {
        Error(Repr::Rust(Box::new(error)))
    }

    /// A boxed Rust error, which keeps its type and its box across the
    /// boundary.
    pub fn from_rust(error: RustError) -> Self {
        Error(Repr::Rust(error))
    }

    /// An error for the payload of a panic that `catch_unwind` caught.
    pub fn from_panic(payload: PanicPayload) -> Self {
        Error(Repr::Panic(payload))
    }

    /// Adopt an exception that C++ gave up.
    ///
    /// If the exception carries a Rust error that this copy of the crate
    /// made, the result is that Rust error. If it carries the payload of a
    /// Rust panic that this copy made, the panic resumes here, with the
    /// original payload: a panic stays a panic in Rust.
    ///
    /// # Safety
    ///
    /// `exception` must be a non-null `nix_exception` that nothing else
    /// owns.
    pub unsafe fn from_exception(exception: *mut NixException) -> Self {
        let exception = NonNull::new(exception).expect("a nix_exception is not null");
        let mut value = MaybeUninit::<NixRustValue>::uninit();
        // SAFETY: The caller passes a valid exception, and `value` is
        // storage for the result.
        if !unsafe { nix_exception_take_rust_value(exception.as_ptr(), owner(), value.as_mut_ptr()) } {
            return Error(Repr::Cxx(exception));
        }
        // SAFETY: The function returned true, so it wrote the value.
        let value = unsafe { value.assume_init() };
        // SAFETY: The caller owns the exception, which no longer owns the
        // value.
        unsafe { nix_exception_free(exception.as_ptr()) };
        match value.kind {
            // SAFETY: This copy made the value in `into_exception`, as a box
            // of a `RustError`, and the exception gave up its ownership.
            NixRustErrorKind::Error => Error(Repr::Rust(*unsafe { Box::from_raw(value.data as *mut RustError) })),
            NixRustErrorKind::Panic => {
                // SAFETY: As above, for a box of a `PanicPayload`.
                let payload = *unsafe { Box::from_raw(value.data as *mut PanicPayload) };
                std::panic::resume_unwind(payload)
            }
        }
    }

    /// Whether this error is a C++ exception, as opposed to an error that
    /// started in Rust.
    pub fn is_cxx_exception(&self) -> bool {
        matches!(self.0, Repr::Cxx(_))
    }

    /// The Rust error, if this error started in Rust and is not a panic.
    pub fn rust_error(&self) -> Option<&(dyn std::error::Error + Send + Sync + 'static)> {
        match &self.0 {
            Repr::Rust(error) => Some(error.as_ref()),
            _ => None,
        }
    }

    /// The panic payload, if this error is a caught panic.
    pub fn into_panic(self) -> Result<PanicPayload, Self> {
        let this = ManuallyDrop::new(self);
        // SAFETY: `this` is never dropped, so the field is moved out once.
        match unsafe { std::ptr::read(&this.0) } {
            Repr::Panic(payload) => Ok(payload),
            repr => Err(Error(repr)),
        }
    }

    /// Give the error to C++, as an exception. A Rust error or panic
    /// payload goes inside the exception, so that it can come back.
    pub fn into_exception(self) -> *mut NixException {
        let this = ManuallyDrop::new(self);
        // SAFETY: `this` is never dropped, so the field is moved out once.
        match unsafe { std::ptr::read(&this.0) } {
            Repr::Cxx(exception) => exception.as_ptr(),
            Repr::Rust(error) => {
                let message = error.to_string();
                wrap_rust_value(&message, NixRustErrorKind::Error, Box::new(error))
            }
            Repr::Panic(payload) => {
                let message = panic_message(&payload);
                wrap_rust_value(&message, NixRustErrorKind::Panic, Box::new(payload))
            }
        }
    }
}

/// The message of a panic payload, as `std` prints it.
fn panic_message(payload: &PanicPayload) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_owned()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "a panic with a payload that is not a string".to_owned()
    }
}

/// Make a C++ exception with `message` that owns `value`.
fn wrap_rust_value<T: Send + 'static>(message: &str, kind: NixRustErrorKind, value: Box<T>) -> *mut NixException {
    let value = NixRustValue {
        kind,
        data: Box::into_raw(value) as *mut c_void,
        drop: drop_rust_value::<T>,
        owner: owner(),
    };
    // SAFETY: The message is valid for `len()` bytes during the call, and
    // `value` follows the rules of `nix_rust_value`.
    unsafe { nix_exception_from_rust(message.as_ptr().cast(), message.len(), value) }
}

/// `nix_rust_value::drop` for a value of type `T`.
unsafe extern "C" fn drop_rust_value<T>(data: *mut c_void) {
    // SAFETY: `wrap_rust_value` made `data` from a `Box<T>`, and C++ drops
    // it once.
    let value = unsafe { Box::from_raw(data as *mut T) };
    if let Err(payload) = catch_unwind(AssertUnwindSafe(move || drop(value))) {
        report_lost_panic("dropping a Rust value that a C++ exception owned", &payload);
    }
}

/// Report a panic that has nowhere to go: it happened while Rust code
/// dropped a value for C++, and no caller waits for a result. The panic
/// hook of `std` already printed the panic, unless it was resumed with
/// `resume_unwind`, so say where it was lost.
fn report_lost_panic(context: &str, payload: &PanicPayload) {
    eprintln!(
        "nix: a Rust panic while {context} was ignored: {}",
        panic_message(payload)
    );
}

impl From<String> for Error {
    fn from(message: String) -> Self {
        Error::msg(message)
    }
}

// Synchronous bridge functions.
//
// cxx turns the `Err` of an `extern "Rust"` function into a `rust::Error`
// that holds only the message, and aborts the process on a panic. To keep
// the value, `bridge` catches the panic, and keeps the error, or the panic,
// in `PENDING` of the thread. The C++ caller catches the `rust::Error` and
// takes the original from there (`nix::rs::call`), on the same thread,
// before any other bridge function can fail.

thread_local! {
    static PENDING: std::cell::RefCell<Option<(String, Error)>> = const { std::cell::RefCell::new(None) };
}

/// The error of an `extern "Rust"` bridge function, as cxx sees it: only
/// its message. The error itself waits for the C++ caller; see [`bridge`].
#[derive(Debug)]
pub struct BridgeError(String);

impl fmt::Display for BridgeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// The body of every `extern "Rust"` bridge function.
///
/// It runs `f`, catches a panic, and keeps an error or a panic for the C++
/// caller, which takes it back with `nix::rs::call` and rethrows the
/// original: a C++ exception that `f` got from C++, or a `nix::RustError` or
/// `nix::RustPanic` that owns the Rust value. Without it, cxx aborts the
/// process on a panic.
///
/// Declare each bridge function as returning `Result<T>`, and write its
/// body as `bridge(|| ...)`. A test in `nix-rust` checks that every one does.
pub fn bridge<T>(f: impl FnOnce() -> Result<T, Error>) -> Result<T, BridgeError> {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(value)) => return Ok(value),
        Ok(Err(error)) => error,
        Err(payload) => Error::from_panic(payload),
    };
    let message = error.to_string();
    PENDING.with(|pending| *pending.borrow_mut() = Some((message.clone(), error)));
    Err(BridgeError(message))
}

/// Take the error that the last failed bridge function on this thread kept,
/// as an exception for C++, if its message is `message`. Otherwise drop it
/// and return null: a C++ caller that did not take it is gone.
pub fn take_pending_exception(message: &[u8]) -> *mut NixException {
    match PENDING.with(|pending| pending.borrow_mut().take()) {
        Some((kept, error)) if kept.as_bytes() == message => error.into_exception(),
        _ => std::ptr::null_mut(),
    }
}

impl Drop for Error {
    fn drop(&mut self) {
        if let Repr::Cxx(exception) = self.0 {
            // SAFETY: The error owns the exception, and drops it once.
            unsafe { nix_exception_free(exception.as_ptr()) }
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.0 {
            Repr::Cxx(exception) => {
                // SAFETY: The error owns the exception, and the string lives
                // as long as the exception does.
                let what = unsafe { CStr::from_ptr(nix_exception_what(exception.as_ptr())) };
                f.write_str(&what.to_string_lossy())
            }
            Repr::Rust(error) => fmt::Display::fmt(error, f),
            Repr::Panic(payload) => write!(f, "Rust panic: {}", panic_message(payload)),
        }
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.0 {
            Repr::Cxx(_) => write!(f, "Error::Cxx({self})"),
            Repr::Rust(_) => write!(f, "Error::Rust({self})"),
            Repr::Panic(_) => write!(f, "Error::Panic({self})"),
        }
    }
}

impl std::error::Error for Error {}

#[cfg(feature = "cxx")]
impl From<cxx::Exception> for Error {
    fn from(exception: cxx::Exception) -> Self {
        Error::msg(exception.what())
    }
}

// Wakers.
//
// A `nix_waker` is two pointers and a `RawWaker` is two pointers, but the
// vtables differ, so each direction wraps the waker of the other language
// in a box. A waker that comes back to its own language is unwrapped
// instead of wrapped again, so that a chain of crossings stays one level
// deep and `Waker::will_wake` keeps working.

/// A Rust `Waker` in a box, presented as an owned `nix_waker`.
static RUST_WAKER_VTABLE: NixWakerVTable = NixWakerVTable {
    clone: rust_waker_clone,
    wake: rust_waker_wake,
    wake_by_ref: rust_waker_wake_by_ref,
    drop: rust_waker_drop,
};

/// A borrowed Rust `Waker`, presented as a borrowed `nix_waker` for the
/// duration of a `poll` call. C++ never wakes or drops a borrowed waker;
/// it only clones it.
static BORROWED_RUST_WAKER_VTABLE: NixWakerVTable = NixWakerVTable {
    clone: rust_waker_clone,
    wake: rust_waker_wake_by_ref,
    wake_by_ref: rust_waker_wake_by_ref,
    drop: borrowed_rust_waker_drop,
};

// The functions of the two vtables above. For both, `data` points to a
// `Waker`: in a box that the `nix_waker` owns for `RUST_WAKER_VTABLE`, and
// borrowed for `BORROWED_RUST_WAKER_VTABLE`. The ABI rules of
// `future-abi.h` say which function consumes the `nix_waker`.
//
// A panic in the waker of a Rust executor aborts the process here: these
// functions are `extern "C"`.

unsafe extern "C" fn rust_waker_clone(data: *const c_void) -> NixWaker {
    // SAFETY: `data` points to a live `Waker`, owned or borrowed.
    let waker = unsafe { &*(data as *const Waker) };
    nix_waker_from_rust(waker.clone())
}

unsafe extern "C" fn rust_waker_wake(data: *const c_void) {
    // SAFETY: Only `RUST_WAKER_VTABLE` has this function as `wake`, so
    // `data` is the box, and `wake` consumes the `nix_waker`.
    let waker = unsafe { Box::from_raw(data as *mut Waker) };
    waker.wake();
}

unsafe extern "C" fn rust_waker_wake_by_ref(data: *const c_void) {
    // SAFETY: `data` points to a live `Waker`, owned or borrowed.
    let waker = unsafe { &*(data as *const Waker) };
    waker.wake_by_ref();
}

unsafe extern "C" fn rust_waker_drop(data: *const c_void) {
    // SAFETY: Only `RUST_WAKER_VTABLE` has this function as `drop`, so
    // `data` is the box, and `drop` consumes the `nix_waker`.
    drop(unsafe { Box::from_raw(data as *mut Waker) });
}

unsafe extern "C" fn borrowed_rust_waker_drop(_data: *const c_void) {}

/// A `nix_waker` in a box, presented as a Rust `Waker`.
static NIX_WAKER_VTABLE: RawWakerVTable =
    RawWakerVTable::new(nix_waker_clone, nix_waker_wake, nix_waker_wake_by_ref, nix_waker_drop);

/// A borrowed `nix_waker`, presented as a Rust `Waker` for the duration of
/// a `poll` call. Rust code receives it as `&Waker` and can only clone it
/// or wake it by reference.
static BORROWED_NIX_WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    nix_waker_clone,
    nix_waker_wake_by_ref,
    nix_waker_wake_by_ref,
    borrowed_nix_waker_drop,
);

// The functions of the two vtables above. For both, `data` points to a
// valid `nix_waker`: in a box that the `Waker` owns for `NIX_WAKER_VTABLE`,
// and borrowed for `BORROWED_NIX_WAKER_VTABLE`. The functions of a valid
// `nix_waker` follow the ABI rules of `future-abi.h`.

unsafe fn nix_waker_clone(data: *const ()) -> RawWaker {
    // SAFETY: `data` points to a valid `nix_waker`, owned or borrowed.
    let waker = unsafe { &*(data as *const NixWaker) };
    // SAFETY: `clone` of a valid `nix_waker` returns an owned clone.
    let clone = unsafe { ((*waker.vtable).clone)(waker.data) };
    RawWaker::new(Box::into_raw(Box::new(clone)) as *const (), &NIX_WAKER_VTABLE)
}

unsafe fn nix_waker_wake(data: *const ()) {
    // SAFETY: Only `NIX_WAKER_VTABLE` has this function as `wake`, so `data`
    // is the box, and `wake` consumes the `Waker`.
    let waker = unsafe { Box::from_raw(data as *mut NixWaker) };
    // SAFETY: The box owned the `nix_waker`, and `wake` consumes it.
    unsafe { ((*waker.vtable).wake)(waker.data) }
}

unsafe fn nix_waker_wake_by_ref(data: *const ()) {
    // SAFETY: `data` points to a valid `nix_waker`, owned or borrowed.
    let waker = unsafe { &*(data as *const NixWaker) };
    // SAFETY: `wake_by_ref` does not consume the `nix_waker`.
    unsafe { ((*waker.vtable).wake_by_ref)(waker.data) }
}

unsafe fn nix_waker_drop(data: *const ()) {
    // SAFETY: Only `NIX_WAKER_VTABLE` has this function as `drop`, so `data`
    // is the box, and `drop` consumes the `Waker`.
    let waker = unsafe { Box::from_raw(data as *mut NixWaker) };
    // SAFETY: The box owned the `nix_waker`, and `drop` consumes it.
    unsafe { ((*waker.vtable).drop)(waker.data) }
}

unsafe fn borrowed_nix_waker_drop(_data: *const ()) {}

fn is_rust_waker(waker: &NixWaker) -> bool {
    std::ptr::eq(waker.vtable, &RUST_WAKER_VTABLE) || std::ptr::eq(waker.vtable, &BORROWED_RUST_WAKER_VTABLE)
}

/// Take ownership of a `nix_waker` as a Rust `Waker`.
///
/// # Safety
///
/// `waker` must be a valid, owned `nix_waker`.
pub unsafe fn waker_from_nix(waker: NixWaker) -> Waker {
    if std::ptr::eq(waker.vtable, &RUST_WAKER_VTABLE) {
        // A Rust waker that went to C++ and comes back: unwrap it.
        // SAFETY: The vtable says that `data` is the box of a `Waker`, which
        // the `nix_waker` owns, and the caller passes that ownership.
        return *unsafe { Box::from_raw(waker.data as *mut Waker) };
    }
    assert!(
        !std::ptr::eq(waker.vtable, &BORROWED_RUST_WAKER_VTABLE),
        "a borrowed nix_waker cannot be owned"
    );
    // SAFETY: The functions of `NIX_WAKER_VTABLE` expect a box with an owned
    // `nix_waker`, and each of them is thread-safe, as `future-abi.h`
    // requires of the functions of a `nix_waker`.
    unsafe {
        Waker::from_raw(RawWaker::new(
            Box::into_raw(Box::new(waker)) as *const (),
            &NIX_WAKER_VTABLE,
        ))
    }
}

/// Give a Rust `Waker` to C++ as an owned `nix_waker`.
pub fn nix_waker_from_rust(waker: Waker) -> NixWaker {
    if std::ptr::eq(waker.vtable(), &NIX_WAKER_VTABLE) {
        // A C++ waker that came to Rust and goes back: unwrap it.
        let waker = ManuallyDrop::new(waker);
        // SAFETY: The vtable says that the data is the box of an owned
        // `nix_waker`. The `Waker` is not dropped, so the box is freed once.
        return *unsafe { Box::from_raw(waker.data() as *mut NixWaker) };
    }
    NixWaker {
        data: Box::into_raw(Box::new(waker)) as *const c_void,
        vtable: &RUST_WAKER_VTABLE,
    }
}

/// Call `f` with a borrowed `nix_waker` as a `&Waker`.
///
/// # Safety
///
/// `waker` must be valid for the duration of the call.
pub unsafe fn with_borrowed_nix_waker<R>(waker: *const NixWaker, f: impl FnOnce(&Waker) -> R) -> R {
    // SAFETY: The caller guarantees that `waker` is valid during the call.
    let waker = unsafe { &*waker };
    if is_rust_waker(waker) {
        // SAFETY: The vtable says that `data` points to a `Waker`, which
        // lives as long as the `nix_waker`.
        return f(unsafe { &*(waker.data as *const Waker) });
    }
    let raw = RawWaker::new(waker as *const NixWaker as *const (), &BORROWED_NIX_WAKER_VTABLE);
    // SAFETY: `BORROWED_NIX_WAKER_VTABLE` expects a pointer to a valid
    // `nix_waker`, which `waker` is during the call. `f` receives only a
    // reference, and the `Waker` is not dropped, so nothing consumes the
    // borrowed `nix_waker`.
    let waker = ManuallyDrop::new(unsafe { Waker::from_raw(raw) });
    f(&waker)
}

/// Call `f` with a borrowed Rust `Waker` as a borrowed `nix_waker`.
pub fn with_borrowed_rust_waker<R>(waker: &Waker, f: impl FnOnce(&NixWaker) -> R) -> R {
    if std::ptr::eq(waker.vtable(), &NIX_WAKER_VTABLE) || std::ptr::eq(waker.vtable(), &BORROWED_NIX_WAKER_VTABLE) {
        // SAFETY: The vtable says that the data points to a valid
        // `nix_waker`, which lives as long as `waker`.
        return f(unsafe { &*(waker.data() as *const NixWaker) });
    }
    let nix = NixWaker {
        data: waker as *const Waker as *const c_void,
        vtable: &BORROWED_RUST_WAKER_VTABLE,
    };
    f(&nix)
}

// Rust futures for C++.

type BoxedFuture<T> = Pin<Box<dyn Future<Output = Result<T, Error>> + Send>>;

struct ExportedFuture<T> {
    future: Option<BoxedFuture<T>>,
    output: Option<Result<T, Error>>,
}

/// Drop a value, and turn a panic during the drop into nothing. A panic
/// must not unwind into C++, and there is nobody to report it to.
fn drop_quietly<V>(value: V) {
    let _ = catch_unwind(AssertUnwindSafe(move || drop(value)));
}

impl<T: AbiType> ExportedFuture<T> {
    const VTABLE: NixFutureVTable = NixFutureVTable {
        poll: Self::poll,
        take: Self::take,
        drop: Self::drop,
        output_type: T::NAME.as_ptr(),
    };

    // The functions of `VTABLE`. `this` is the box that `export_future`
    // made, which the `nix_future` owns; C++ calls them one at a time, as
    // `future-abi.h` requires. A panic of the future becomes an error. A
    // panic here otherwise is a broken contract, for example a second
    // `take`; it aborts the process, because these functions are
    // `extern "C"`.

    unsafe extern "C" fn poll(this: *mut c_void, waker: *const NixWaker) -> NixPoll {
        // SAFETY: `this` is the box of `export_future`, and nothing else
        // uses it during the call.
        let this = unsafe { &mut *(this as *mut Self) };
        let future = this
            .future
            .as_mut()
            .expect("a completed nix_future is not polled again");
        // SAFETY: `waker` is valid for the duration of `poll`.
        let result = unsafe {
            with_borrowed_nix_waker(waker, |waker| {
                let mut cx = Context::from_waker(waker);
                catch_unwind(AssertUnwindSafe(|| future.as_mut().poll(&mut cx)))
            })
        };
        let output = match result {
            Ok(Poll::Pending) => return NixPoll::Pending,
            Ok(Poll::Ready(output)) => output,
            Err(payload) => Err(Error::from_panic(payload)),
        };
        // Drop the future as soon as it completed, as Rust executors do, so
        // that its resources are released before the result is taken.
        drop_quietly(this.future.take());
        this.output = Some(output);
        NixPoll::Ready
    }

    unsafe extern "C" fn take(this: *mut c_void, out: *mut c_void) -> *mut NixException {
        // SAFETY: As in `poll`.
        let this = unsafe { &mut *(this as *mut Self) };
        match this
            .output
            .take()
            .expect("take is called once, after poll returned ready")
        {
            Ok(value) => {
                if size_of::<T>() == 0 {
                    // C++ passes no storage for `void`.
                    std::mem::forget(value);
                } else {
                    // SAFETY: `out` is uninitialized storage for one value of
                    // the output type, which C++ checked through
                    // `output_type`, and which has the layout of `T`
                    // (`AbiType`).
                    unsafe { std::ptr::write(out as *mut T, value) };
                }
                std::ptr::null_mut()
            }
            Err(error) => error.into_exception(),
        }
    }

    unsafe extern "C" fn drop(this: *mut c_void) {
        // SAFETY: `this` is the box of `export_future`, and `drop` consumes
        // the `nix_future`.
        drop_quietly(unsafe { Box::from_raw(this as *mut Self) });
    }
}

/// Give a Rust future to C++ as a `nix_future`, which C++ awaits with
/// `nix::awaitCFuture<T>()`.
///
/// C++ may poll the future on any thread, hence `Send`. A panic in the
/// future becomes an error. Dropping the `nix_future` drops the future.
pub fn export_future<T, F>(future: F) -> NixFuture
where
    T: AbiType,
    F: Future<Output = Result<T, Error>> + Send + 'static,
{
    let exported = Box::new(ExportedFuture::<T> {
        future: Some(Box::pin(future)),
        output: None,
    });
    NixFuture {
        this: Box::into_raw(exported) as *mut c_void,
        vtable: &ExportedFuture::<T>::VTABLE,
    }
}

// C++ futures for Rust.

/// A Rust future over a `nix_future` from C++, typically a `nix::Task`
/// that `nix::toCFuture()` boxed. Dropping it drops the C++ future, which
/// cancels a task that is not complete.
pub struct CxxFuture<T> {
    raw: NixFuture,
    complete: bool,
    output: PhantomData<fn() -> T>,
}

// SAFETY: C++ futures that cross the boundary must tolerate being polled on
// any thread, one thread at a time. `nix::Task` does: a waker posts the
// resumption to whichever executor drives the outermost task.
unsafe impl<T> Send for CxxFuture<T> {}

impl<T: AbiType> CxxFuture<T> {
    /// Adopt a `nix_future` whose output type is `T`.
    ///
    /// # Panics
    ///
    /// If the output type of the future is not `T`.
    ///
    /// # Safety
    ///
    /// `raw` must be a valid `nix_future` that nothing else owns.
    pub unsafe fn new(raw: NixFuture) -> Self {
        // SAFETY: The caller guarantees a valid `nix_future`, whose vtable
        // has a null-terminated `output_type`.
        let actual = unsafe { CStr::from_ptr((*raw.vtable).output_type) };
        let this = CxxFuture {
            raw,
            complete: false,
            output: PhantomData,
        };
        assert_eq!(
            actual,
            T::NAME,
            "the output type of a nix_future from C++ is not the expected one"
        );
        this
    }
}

impl<T: AbiType> Future for CxxFuture<T> {
    type Output = Result<T, Error>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        // `CxxFuture` is `Unpin`: the C++ future lives behind a pointer.
        let this = self.get_mut();
        assert!(!this.complete, "a completed CxxFuture is not polled again");
        // SAFETY: `new` adopted a valid `nix_future`, whose vtable lives as
        // long as the future.
        let vtable = unsafe { &*this.raw.vtable };
        let polled = with_borrowed_rust_waker(cx.waker(), |waker| {
            // SAFETY: The future is not complete, and `waker` is valid
            // during the call.
            unsafe { (vtable.poll)(this.raw.this, waker) }
        });
        if polled == NixPoll::Pending {
            return Poll::Pending;
        }
        this.complete = true;
        let mut out = MaybeUninit::<T>::uninit();
        // SAFETY: `poll` returned ready, and `take` runs once, because
        // `complete` stops a second poll. `out` is storage for a `T`, the
        // output type that `new` checked.
        let exception = unsafe { (vtable.take)(this.raw.this, out.as_mut_ptr() as *mut c_void) };
        if exception.is_null() {
            // SAFETY: `take` returned no exception, so it wrote the value.
            Poll::Ready(Ok(unsafe { out.assume_init() }))
        } else {
            // SAFETY: `take` gives up the ownership of the exception.
            Poll::Ready(Err(unsafe { Error::from_exception(exception) }))
        }
    }
}

impl<T> Drop for CxxFuture<T> {
    fn drop(&mut self) {
        // SAFETY: The `CxxFuture` owns the `nix_future`, and `drop` consumes
        // it once.
        unsafe { ((*self.raw.vtable).drop)(self.raw.this) }
    }
}
