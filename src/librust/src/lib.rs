//! The Rust part of Nix.
//!
//! This crate is the only Rust staticlib in Nix. Meson wraps it into
//! `libnixrust`, which every other Nix library links. All Rust code of Nix
//! goes into this crate or into crates that it depends on, so that a process
//! contains exactly one copy of the Rust standard library.
//!
//! Each `#[cxx::bridge]` module has its own generated C++ header,
//! `nix/rust/<module>.rs.h`. The headers are listed in `meson.build`.

pub use nix_rust_future as future;
pub mod boundary;
pub mod reactor;
pub mod tarball_cache;

use future::{BridgeError, bridge};

#[cxx::bridge(namespace = "nix::rs")]
mod ffi {
    extern "Rust" {
        /// The version of the Rust compiler that built `libnixrust`, for
        /// example `1.95.0`.
        #[cxx_name = "rustc_version"]
        fn bridge_rustc_version() -> Result<&'static str>;
    }
}

fn bridge_rustc_version() -> Result<&'static str, BridgeError> {
    bridge(|| Ok(env!("NIX_RUST_RUSTC_VERSION")))
}
