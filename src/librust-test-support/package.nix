{
  lib,
  stdenv,
  buildPackages,
  mkMesonLibrary,

  cargo,
  rustc,
  rustPlatform,

  nix-rust,

  # Configuration Options

  version,
}:

let
  inherit (lib) fileset;

  # See `src/librust/package.nix`.
  buildRust = stdenv.buildPlatform.rust;
  hostRust = stdenv.hostPlatform.rust;
  buildCc = buildPackages.stdenv.cc;
  ccFor = cc: name: "${cc}/bin/${cc.targetPrefix}${name}";
in

mkMesonLibrary (finalAttrs: {
  pname = "nix-rust-test-support";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../nix-meson-build-support
    ./nix-meson-build-support
    ../../.version
    ./.version
    ./meson.build
    ./meson.options
    ./Cargo.toml
    ./Cargo.lock
    ./build.rs
    ./libnix-rust-test-support.map
    ./libnix-rust-test-support.exp
    ./src
    ./include
    # The path dependency `nix-rust-future`.
    ../librust/future
  ];

  # `Cargo.lock` names only the local crates, but `cargoSetupHook` expects
  # a vendor directory all the same.
  cargoDeps = rustPlatform.importCargoLock {
    lockFile = ./Cargo.lock;
  };

  nativeBuildInputs = [
    cargo
    rustc
    rustPlatform.cargoSetupHook
  ];

  propagatedBuildInputs = [
    nix-rust
  ];

  mesonFlags = [
    (lib.mesonOption "rust-target" hostRust.rustcTargetSpec)
  ];

  env = {
    "CC_${buildRust.cargoEnvVarTarget}" = ccFor buildCc "cc";
    "CXX_${buildRust.cargoEnvVarTarget}" = ccFor buildCc "c++";
    "CARGO_TARGET_${buildRust.cargoEnvVarTarget}_LINKER" = ccFor buildCc "cc";
  }
  // {
    "CC_${hostRust.cargoEnvVarTarget}" = ccFor stdenv.cc "cc";
    "CXX_${hostRust.cargoEnvVarTarget}" = ccFor stdenv.cc "c++";
    "CARGO_TARGET_${hostRust.cargoEnvVarTarget}_LINKER" = ccFor stdenv.cc "cc";
  };

  # See `src/librust/package.nix`.
  passthru.tests.clippy = stdenv.mkDerivation {
    pname = "${finalAttrs.pname}-clippy";
    inherit (finalAttrs)
      version
      src
      sourceRoot
      cargoDeps
      env
      ;
    nativeBuildInputs = finalAttrs.nativeBuildInputs ++ [ buildPackages.clippy ];
    dontConfigure = true;
    buildPhase = ''
      runHook preBuild
      cargo clippy --offline --locked --all-targets -- --deny warnings
      runHook postBuild
    '';
    installPhase = ''
      touch $out
    '';
  };

  meta = {
    platforms = lib.platforms.unix ++ lib.platforms.windows;
  };

})
