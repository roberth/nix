# Rust

Nix contains Rust code.
All of it is in one component, `nix-rust` (`src/librust`), which builds one library, `libnixrust`.
Every other Nix library links `libnixrust`, and `libnixrust` links no other Nix library.

## Why one library at the bottom

A process must contain exactly one copy of the Rust standard library, and of each Rust crate.
A Rust `staticlib` contains its own copy of the standard library.
Two of them in one process do not work:

- A static link of two Rust staticlibs into one program fails on duplicate symbols.
- A shared build gets two independent copies of each crate, for example two allocators for one type.

Nix therefore has one Rust staticlib.
Meson wraps it into `libnixrust`: a shared library in the default build, and a static library in a static build.

The C++ libraries call Rust, and Rust calls back up into C++, for example to check for interrupts or to write into a sink.
The upward calls must not become link-time references from `libnixrust` to the libraries above it.
Such a reference makes a cycle in the library graph: a static link then depends on the link order, and a shared build on Darwin needs `-undefined dynamic_lookup`.

Rust code therefore calls upward only through virtual functions of abstract C++ classes that `libnixrust` declares, for example `nix::rs::Callback` in `nix/rust/boundary.hh`, or `nix::rs::TarballUnpacker` in `nix/rust/tarball-cache.hh`, which libfetchers implements with libarchive.
The libraries above implement the classes and pass instances down.
The call goes through the vtable at run time, so `libnixrust` has no undefined symbols of other Nix libraries.
Two other kinds of upward call follow the same rule:

- A C ABI future (`nix/rust/future-abi.h`) carries its own vtable, so Rust polls a C++ task through function pointers.
- A library above registers a function pointer at run time, for example the factory that turns Rust errors into `nix::Error` (`nix_set_rust_error_factory`).

Rejected alternatives:

- A Rust staticlib in each Nix library that needs Rust: two copies of the standard library, as described above.
- Rust code inside an existing library, for example `libnixutil`: the Rust code of all layers, such as gitoxide for `libfetchers`, then sits in `libnixutil`, and its upward calls into `libfetchers` are a cycle.
- cxx `extern "C++"` functions that the libraries above define: a link-time reference upward, as described above.
- The `cxxbridge` command, run by Meson, to generate the C++ side of the bridges: its version can differ from the version of the `cxx` crate in `Cargo.lock`. `build.rs` uses `cxx-build` instead, which is always the same version.
- Rust `dylib` crates that share one `libstd.so`: the Rust ABI is not stable, and the arrangement binds every library to one exact compiler build.
- Meson's own Rust support with Cargo subprojects: Meson marks it as experimental, and a dependency such as gitoxide has more than 100 crates.

## Layout

- `Cargo.toml`, `Cargo.lock`: the crate `nix-rust`, crate type `staticlib`.
  Further Rust code goes into this crate, or into crates that it depends on.
- `build.rs`: compiles the C++ side of the `#[cxx::bridge]` modules, and the handwritten C++ in `src/*.cc`, into the staticlib.
- `src/*.rs`: the Rust code. Each bridge module has a generated header `nix/rust/<module>.rs.h`.
- `include/nix/rust/*.hh`: handwritten C++ headers, such as the abstract classes for upward calls.
- `cargo-build.sh`: runs cargo for Meson, and copies the staticlib and the generated headers to the outputs that Meson declares.
- `meson.build`: wraps the staticlib into `libnixrust`, installs the headers, and generates `nix-rust.pc`.

The C++ namespace of the bridges is `nix::rs`.
A namespace `nix::rust` would hide cxx's own namespace `::rust` (`rust::String`, `rust::Box`) from all code inside `namespace nix`.

## Adding a bridge module

1. Add `src/<module>.rs` with a `#[cxx::bridge(namespace = "nix::rs")]` module, and declare it in `src/lib.rs`.
2. Add the file to `BRIDGES` in `build.rs`.
3. Add `<module>.rs.h` to `bridge_headers` in `meson.build`.
4. Include `nix/rust/<module>.rs.h` from C++, and add `dependency('nix-rust')` to the Meson dependencies of the library that includes it.

## Asynchronous functions

cxx has no support for `async fn`.
An asynchronous function crosses the boundary as a C ABI future instead, which a bridge passes by value:

```rust
#[cxx::bridge(namespace = "nix::rs")]
mod ffi {
    unsafe extern "C++" {
        include!("nix/rust/future-abi.hh");
        #[namespace = ""]
        type nix_future = crate::future::NixFuture;
    }
    extern "Rust" {
        fn fetch_something(url: &str) -> nix_future;
    }
}

fn fetch_something(url: &str) -> NixFuture {
    let url = url.to_owned();
    export_future(async move { Ok::<String, Error>(format!("fetched {url}")) })
}
```

C++ awaits it with `co_await awaitCFuture<rust::String>(rs::fetch_something(url))`.

An upward call whose C++ exception must keep its type returns it as a `nix_exception *` instead of using cxx's `Result`, which keeps only `what()`.
`TarballUnpacker::run` does so: it catches the exception, returns `rs::currentException()`, and Rust adopts it with `Error::from_exception`, so that the exception reaches the C++ code that awaits the future with its dynamic type.
In the other direction, a bridge function takes a `nix_future` that C++ made with `toCFuture(task)`, and Rust awaits `CxxFuture::<T>::new(future)`.
See [Asynchronous code](./async.md#tasks-and-rust-futures) for the semantics.

## Adding a crate dependency

Add the dependency to `Cargo.toml` and run `cargo update -p <crate>` (or `cargo generate-lockfile`) in `src/librust`.
The packaging reads `Cargo.lock` with `rustPlatform.importCargoLock`, which takes the checksum of each crate from the lock file.
There is no separate vendor hash to update.
A Git dependency is the exception: it needs an entry in `outputHashes` in `src/librust/package.nix`.

## Building

cargo builds the build scripts and proc macros for the build platform, and the staticlib for the host platform.
`package.nix` gives cargo a C and C++ compiler and a linker for each, through the same variables that Nixpkgs' `rust.envVars.setEnv` sets, and passes the Rust target of the host to Meson as `-Drust-target`.

At configure time, `meson.build` asks rustc which system libraries the Rust standard library needs on the host (`--print=native-static-libs`).
It links them into the shared `libnixrust`, and lists them in `Libs.private` of `nix-rust.pc` for a static link.

The cargo profile follows the Meson build type: `dev` for `debug`, `release` otherwise.

## Known limitations

- The shared `libnixrust` exports all its symbols, including those of the Rust standard library. A plugin that brings its own Rust code can bind to them.
- `rust/cxx.h` is installed at the top of the include directory, where other packages that use cxx install it too.
- The cross-compilation dev shells do not set the per-target compiler variables for cargo. Only the Nix package builds set them.
