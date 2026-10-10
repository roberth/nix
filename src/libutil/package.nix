{
  lib,
  stdenv,
  mkMesonLibrary,

  nix-rust,
  boost,
  brotli,
  libarchive,
  libblake3,
  libcpuid,
  libsodium,
  nlohmann_json,
  openssl,
  zstd,

  # Configuration Options

  version,
}:

let
  inherit (lib) fileset;
in

mkMesonLibrary (finalAttrs: {
  pname = "nix-util";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../nix-meson-build-support
    ./nix-meson-build-support
    ../../.version
    ./.version
    ./widecharwidth
    ./meson.build
    ./meson.options
    ./include/nix/util/meson.build
    ./linux/meson.build
    ./linux/include/nix/util/meson.build
    ./freebsd/meson.build
    ./freebsd/include/nix/util/meson.build
    ./unix/meson.build
    ./unix/include/nix/util/meson.build
    ./windows/meson.build
    ./windows/include/nix/util/meson.build
    (fileset.fileFilter (file: file.hasExt "cc") ./.)
    (fileset.fileFilter (file: file.hasExt "hh") ./.)
  ];

  buildInputs = [
    brotli
    libblake3
    libsodium
    openssl
    zstd
  ]
  ++ lib.optional stdenv.hostPlatform.isx86_64 libcpuid;

  # `nix-util.pc` names `nix-rust` in `Requires`, because the public header
  # `nix/util/task.hh` includes `nix/rust/future-abi.h`.
  propagatedBuildInputs = [
    nix-rust
    boost
    libarchive
    nlohmann_json
  ];

  mesonFlags = [
    (lib.mesonEnable "cpuid" stdenv.hostPlatform.isx86_64)
  ];

  meta = {
    platforms = lib.platforms.unix ++ lib.platforms.windows;
  };

})
