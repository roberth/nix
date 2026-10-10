//! Minimal functions that exercise the C++/Rust boundary in both directions.
//!
//! The unit tests in `libutil-tests` call these functions. They show that
//! values, errors and C++ exceptions cross the boundary, and that the link
//! layout holds: Rust code calls up into C++ without a link-time reference
//! to the libraries above `libnixrust`.

#[cxx::bridge(namespace = "nix::rs")]
mod ffi {
    unsafe extern "C++" {
        include!("nix/rust/boundary.hh");

        type Callback;

        fn call(self: &Callback) -> Result<String>;
    }

    extern "Rust" {
        /// Return a greeting for `name`.
        fn probe_greet(name: &str) -> String;

        /// Return an `Err` with `message`. C++ receives it as a
        /// `rust::Error` exception.
        fn probe_fail(message: &str) -> Result<()>;

        /// Call `callback` and describe the result: `ok: <value>`, or
        /// `err: <what()>` when the C++ code throws.
        fn probe_call(callback: &Callback) -> String;
    }
}

fn probe_greet(name: &str) -> String {
    format!("Hello, {name}, from Rust!")
}

fn probe_fail(message: &str) -> Result<(), String> {
    Err(message.to_owned())
}

fn probe_call(callback: &ffi::Callback) -> String {
    match callback.call() {
        Ok(value) => format!("ok: {value}"),
        Err(exception) => format!("err: {}", exception.what()),
    }
}
