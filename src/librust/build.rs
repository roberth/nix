use std::process::Command;

/// The bridge modules. Each one gets a generated C++ header and source file.
/// Keep this list in sync with `bridge_headers` in `meson.build`.
const BRIDGES: &[&str] = &[
    "src/lib.rs",
    "src/probe.rs",
    "src/future_probe.rs",
    "src/tarball_cache.rs",
];

fn main() {
    // cxx-build compiles the generated C++ side of the bridges into this
    // staticlib. That keeps the generated code and the `cxx` crate at the same
    // version, which the separate `cxxbridge` command does not guarantee.
    cxx_build::bridges(BRIDGES)
        .file("src/boundary.cc")
        .file("src/future-abi.cc")
        .file("src/future-probe.cc")
        .include("include")
        .std("c++23")
        .compile("nix-rust-bridge");

    for bridge in BRIDGES {
        println!("cargo:rerun-if-changed={bridge}");
    }
    println!("cargo:rerun-if-changed=src/boundary.cc");
    println!("cargo:rerun-if-changed=src/future-abi.cc");
    println!("cargo:rerun-if-changed=src/future-probe.cc");
    println!("cargo:rerun-if-changed=include/nix/rust/boundary.hh");
    println!("cargo:rerun-if-changed=include/nix/rust/future-abi.h");
    println!("cargo:rerun-if-changed=include/nix/rust/future-abi.hh");
    println!("cargo:rerun-if-changed=include/nix/rust/future-probe.hh");
    println!("cargo:rerun-if-changed=include/nix/rust/tarball-cache.hh");

    let rustc = std::env::var("RUSTC").expect("cargo sets RUSTC for build scripts");
    let output = Command::new(rustc)
        .arg("--version")
        .output()
        .expect("rustc --version runs");
    // The output is `rustc 1.95.0 (59807616e 2026-04-14)`. Keep the version.
    let version = String::from_utf8(output.stdout).expect("rustc --version prints UTF-8");
    let version = version
        .split_whitespace()
        .nth(1)
        .expect("rustc --version prints a version");
    println!("cargo:rustc-env=NIX_RUST_RUSTC_VERSION={version}");
}
