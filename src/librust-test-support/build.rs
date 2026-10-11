use std::process::Command;

fn main() {
    // The same version as `rustc_version()` of `libnixrust` reports, which
    // a test compares with.
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
