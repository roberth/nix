# Rust

Nix contains Rust code.
All of it is in one component, `nix-rust` (`src/librust`), which builds one library, `libnixrust`.
Every other Nix library links `libnixrust`, and `libnixrust` links no other Nix library.
The unit tests also link a second Rust library, `libnix-rust-test-support`; see [Test support](#test-support).

## Why one library at the bottom

A Rust `staticlib` contains its own copy of the Rust standard library and of each crate that it uses.
Rust code that passes Rust values to other Rust code needs one copy of the types and functions involved: a `String` that one copy allocates, another copy may free with a different allocator, and a type of one copy is a different type in the other.
All Rust code of Nix shares Rust types, so it is in one staticlib.
Meson wraps it into `libnixrust`: a shared library in the default build, and a static library in a static build.

A second Rust staticlib in the same process works only if it meets `libnixrust` at a C or C++ interface, and if the two copies of the standard library do not bind to each other's symbols.
`libnixrust` therefore exports only its C and C++ interface; see [Exported symbols](#exported-symbols).
A plugin or a program that brings its own Rust code, and that links Nix, has such a second copy.

The C++ libraries call Rust, and Rust calls back up into C++, for example to check for interrupts or to write into a sink.
The upward calls must not become link-time references from `libnixrust` to the libraries above it.
Such a reference makes a cycle in the library graph: a static link then depends on the link order, and a shared build on Darwin needs `-undefined dynamic_lookup`.

Rust code therefore calls upward only through virtual functions of abstract C++ classes that `libnixrust` declares, for example `nix::rs::TarballUnpacker` in `nix/rust/tarball-cache.hh`, which libfetchers implements with libarchive.
`nix/rust/boundary.hh` states the rules.
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

In `src/librust`:

- `Cargo.toml`, `Cargo.lock`: the crate `nix-rust`, crate type `staticlib`, and the root of a Cargo workspace with the crates below.
  Further Rust code goes into this crate, or into crates that it depends on.
- `future/`: the crate `nix-rust-future`, the Rust side of the C ABI futures.
  It is a crate of its own so that the test support library can use it without the rest of `nix-rust`; see [Test support](#test-support).
- `build.rs`: compiles the C++ side of the `#[cxx::bridge]` modules, and the handwritten C++ in `src/*.cc`, into the staticlib.
- `src/*.rs`: the Rust code. Each bridge module has a generated header `nix/rust/<module>.rs.h`.
- `include/nix/rust/*.hh`: handwritten C++ headers, such as the abstract classes for upward calls.
- `libnixrust.map`, `libnixrust.exp`: the exported symbols of the shared library, for ELF and for Darwin.
- `meson.build`: wraps the staticlib into `libnixrust`, installs the headers, and generates `nix-rust.pc`.

In `nix-meson-build-support/rust-staticlib`, shared with `src/librust-test-support`:

- `cargo-build.sh`: runs cargo for Meson, and copies the staticlib and the generated headers to the outputs that Meson declares.
- `native-static-libs.rs`: an empty crate, which `meson.build` compiles to ask rustc which system libraries the Rust standard library needs.
- `localize-rust-symbols.sh`: makes the Rust symbols of a static library local; see [Exported symbols](#exported-symbols).

The C++ namespace of the bridges is `nix::rs`.
A namespace `nix::rust` would hide cxx's own namespace `::rust` (`rust::String`, `rust::Box`) from all code inside `namespace nix`.

## Adding a bridge module

1. Add `src/<module>.rs` with a `#[cxx::bridge(namespace = "nix::rs")]` module, and declare it in `src/lib.rs`.
2. Add the file to `BRIDGES` in `build.rs`.
3. Add `<module>.rs.h` to `bridge_headers` in `meson.build`.
4. Include `nix/rust/<module>.rs.h` from C++, and add `dependency('nix-rust')` to the Meson dependencies of the library that includes it.

The exported symbols of `libnixrust` include all of `nix::rs`, so a new bridge in that namespace needs no change to `libnixrust.map` or `libnixrust.exp`.

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

Rust code that needs timers, readiness of file descriptors, or a place to run a future uses the reactor (`src/reactor.rs`), a tokio runtime with one thread.
Code that runs on the reactor thread must not block, also not in a drop; see [The reactor](./async.md#the-reactor) and [Asynchronous drop](./async.md#asynchronous-drop).
Do not start another tokio runtime: a tokio timer or descriptor belongs to the runtime that registered it, and fires only while a thread drives that runtime.

## Adding a crate dependency

Add the dependency to `Cargo.toml` and run `cargo update -p <crate>` (or `cargo generate-lockfile`) in `src/librust`.
The packaging reads `Cargo.lock` with `rustPlatform.importCargoLock`, which takes the checksum of each crate from the lock file.
There is no separate vendor hash to update.
A Git dependency is the exception: it needs an entry in `outputHashes` in `src/librust/package.nix`.

The license of the crate must allow its use in an LGPL-2.1-or-later library.
`cargo metadata --format-version 1 | jq -r '.packages[] | "\(.name) \(.license)"'` lists the licenses of all crates.

## Building

cargo builds the build scripts and proc macros for the build platform, and the staticlib for the host platform.
`package.nix` gives cargo a C and C++ compiler and a linker for each, through the same variables that Nixpkgs' `rust.envVars.setEnv` sets, and passes the Rust target of the host to Meson as `-Drust-target`.

At configure time, `meson.build` asks rustc which system libraries the Rust standard library needs on the host (`--print=native-static-libs`).
It links them into the shared `libnixrust`, and lists them in `Libs.private` of `nix-rust.pc` for a static link.

The cargo profile follows the Meson build type: `dev` for `debug`, `release` otherwise.

## Exported symbols

`libnixrust` exports its C and C++ interface, and nothing of the Rust standard library or of the crates:

- the C functions of `nix/rust/future-abi.h`;
- the namespace `nix::rs`: the C++ side of the bridges, and the vtables and type information of the classes for upward calls;
- the namespace `rust::cxxbridge1`: the C++ runtime of cxx, which the inline code of `rust/cxx.h` in the libraries above calls.

The shared library gets this list as a version script (`libnixrust.map`) on ELF platforms, and as an exported symbols list (`libnixrust.exp`) on Darwin.
On x86_64-linux, the list takes the dynamic symbol table of `libnixrust.so` from 5148 symbols, 4138 of them Rust symbols, to about 310, none of them a Rust symbol.
The library also links with `-Bsymbolic-functions`, so that its calls to its own exported functions stay inside it.
Windows keeps `--export-all-symbols`: a PE import names its DLL, so a second copy of the Rust standard library in another DLL does not clash, and a DEF file cannot express the patterns.

An archive has no version script.
In a static build on ELF platforms, `localize-rust-symbols.sh` links all members of the cargo staticlib into one relocatable object, so that the references between them are resolved, and then makes the global symbols local, except the C++ symbols, the C functions of `future-abi.h`, and the signatures of COMDAT groups.
The C++ symbols include the inline functions and templates of the C++ standard library, which the final link merges with the same functions of other objects.
On x86_64-linux-musl, this takes `libnixrust.a` from 12412 global definitions, 9003 of them Rust symbols, to about 430, none of them a Rust symbol.

To check the result, compare `nm -D --defined-only libnixrust.so` (or `nm -g --defined-only libnixrust.a`) with the patterns.

## Test support

The unit tests need Rust functions that the shipped library must not contain: futures that fail, panic, or never complete, and a panic that unwinds through Rust frames.
They are in `libnix-rust-test-support` (`src/librust-test-support`), which only the unit tests of libutil and libstore link, like the other `*-test-support` libraries.

The library is a second Rust staticlib, with its own copy of the Rust standard library.
That makes each test program an instance of the case that [Exported symbols](#exported-symbols) is for, in each link mode.
The two copies share no Rust types, so the test functions are C functions over the C ABI of `future-abi.h` (`nix/rust/tests/probes.hh`), not cxx bridges.
The crate uses `nix-rust-future` without its feature `cxx`, so it contains neither a copy of the cxx runtime nor a copy of `future-abi.cc`.
Its calls to `nix_exception_*` therefore resolve to `libnixrust`, and a Rust error from a test future becomes the same `nix::Error` as one from `libnixrust`.
A future with a `String` output passes a `String` from one copy of the standard library to the other; both use the default global allocator, the `malloc` of the C library.

The tarball cache reports the phases of a write through a hook (`setTarballCachePhaseHook` in libfetchers), and a test that interrupts a write in a phase sets a hook that blocks until the write is cancelled.
The functional test `tarball-cancel.sh` sets the hook from a plugin, `tests/functional/tarball-cache-pause-plugin`.

## Panics, exceptions and `unsafe`

Neither a Rust panic nor a C++ exception may unwind across the boundary.
The rules:

- A panic that leaves an `extern "C"` function aborts the process (Rust 1.81 and later).
  The vtable functions of an exported future catch the panics of the future, which then completes with an error (`Rust panic: ...`).
  A panic elsewhere in them means a broken contract, for example a second `take`, and aborts.
- cxx aborts on a panic that leaves an `extern "Rust"` function, and makes its C++ shims for `extern "C++"` functions `noexcept`.
  An exception that a C++ implementation of an upward call does not catch therefore terminates the process.
- The C functions of `future-abi.h`, and the C++ functions in the vtables of C ABI futures and wakers, are `noexcept`.
  An upward call that must report an exception returns it as a `nix_exception *`.

Each `unsafe` block and `unsafe impl` has a `SAFETY` comment.
The crates enforce that with the lint `clippy::undocumented_unsafe_blocks`, and treat `unsafe_op_in_unsafe_fn` as an error.

## Formatting and lints

- rustfmt formats the Rust code, with the settings in `rustfmt.toml` at the top of the repository.
  The pre-commit hook `rustfmt` runs it, like `clang-format` for C++ (`./maintainers/format.sh`).
- The flake checks `nix-rust-clippy` and `nix-rust-test-support-clippy` run `cargo clippy` with warnings as errors.
  In the dev shell, run `cargo clippy --workspace` in `src/librust`, and `cargo clippy` in `src/librust-test-support`.

## Known limitations

- `libnixrust` exports the C++ runtime of cxx (`rust::cxxbridge1`), because the C++ code of the libraries above calls it.
  Another library with its own copy of the cxx runtime can interpose those symbols for the callers.
  Where the C++ runtime compares type information by address, a `catch` of `rust::Error` does not match an exception from the other copy.
  In a static link, the two copies of the cxx runtime are duplicate symbols.
- The Windows DLL exports all its symbols.
- `rust/cxx.h` is installed at the top of the include directory, where other packages that use cxx install it too.
- The cross-compilation dev shells do not set the per-target compiler variables for cargo. Only the Nix package builds set them.
