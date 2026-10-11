{
  lib,
  stdenv,
  buildPackages,
  mkMesonLibrary,

  cargo,
  rustc,
  rustPlatform,

  # Configuration Options

  version,
}:

let
  inherit (lib) fileset;

  # cargo builds build scripts and proc macros for the build platform and the
  # staticlib for the host platform. It needs a C/C++ compiler and a linker for
  # each. These are the variables that Nixpkgs' `rust.envVars.setEnv` sets.
  buildRust = stdenv.buildPlatform.rust;
  hostRust = stdenv.hostPlatform.rust;
  buildCc = buildPackages.stdenv.cc;
  ccFor = cc: name: "${cc}/bin/${cc.targetPrefix}${name}";
in

mkMesonLibrary (finalAttrs: {
  pname = "nix-rust";
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
    ./libnixrust.map
    ./libnixrust.exp
    ./src
    ./future
    ./include
  ];

  # The crates that `Cargo.lock` names, from the Nix store, so that cargo needs
  # no network access.
  cargoDeps = rustPlatform.importCargoLock {
    lockFile = ./Cargo.lock;
  };

  nativeBuildInputs = [
    cargo
    rustc
    rustPlatform.cargoSetupHook
  ];

  mesonFlags = [
    (lib.mesonOption "rust-target" hostRust.rustcTargetSpec)
  ];

  # In a native build, both platforms have the same variable names, and the
  # host values win. They are the same compilers then.
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

  # `cargo clippy` on the source and the crates of the package, as the flake
  # check `nix-rust-clippy`. The warnings are errors.
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
    # The `dev` profile compiles the C++ of the build script without
    # optimization, and `_FORTIFY_SOURCE` warns about that.
    hardeningDisable = [ "fortify" ];
    dontConfigure = true;
    buildPhase = ''
      runHook preBuild
      cargo clippy --offline --locked --workspace --all-targets -- --deny warnings
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
