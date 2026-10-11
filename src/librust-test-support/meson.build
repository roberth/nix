project(
  'nix-rust-test-support',
  'cpp',
  version : files('.version'),
  default_options : [
    'cpp_std=c++23',
    'warning_level=1',
    'errorlogs=true', # Please print logs for tests that fail
  ],
  meson_version : '>= 1.8',
  license : 'LGPL-2.1-or-later',
)

# `libnix-rust-test-support` contains the Rust functions that the unit tests
# of the boundary between C++ and Rust call. It is a second Rust staticlib,
# apart from `libnixrust`, so that the shipped `libnixrust` contains no test
# code. See `doc/manual/source/development/rust.md`.

cxx = meson.get_compiler('cpp')

subdir('nix-meson-build-support/export-all-symbols')
subdir('nix-meson-build-support/rust-staticlib')

nix_rust = dependency('nix-rust')

cargo_build = custom_target(
  'nix-rust-test-support-cargo',
  input : files(
    'Cargo.lock',
    'Cargo.toml',
  ),
  output : [ 'libnix_rust_test_support.a' ],
  depfile : 'libnix_rust_test_support.a.d',
  command : [
    find_program('sh'),
    cargo_build_script,
    cargo,
    meson.current_source_dir() / 'Cargo.toml',
    meson.current_build_dir() / 'cargo-target',
    cargo_profile,
    rust_target,
    meson.current_build_dir(),
    '@DEPFILE@',
    'nix-rust-test-support',
  ],
  console : true,
  build_by_default : true,
)
cargo_staticlib = cargo_build[0]

install_headers(
  'include/nix/rust/tests/probes.hh',
  subdir : 'nix/rust/tests',
)

include_dirs = [ include_directories('include') ]

if get_option('default_library') == 'static'
  # The Rust symbols must be local: the test programs link `libnixrust.a`
  # too, which has its own copy of the Rust standard library.
  if localize_rust_symbols
    static_library_command = localize_rust_symbols_command + [
      '^nix_rust_test_',
      '@INPUT@',
      '@OUTPUT@',
    ]
  else
    static_library_command = [ 'cp', '@INPUT@', '@OUTPUT@' ]
  endif
  this_library = custom_target(
    'nix-rust-test-support',
    input : cargo_staticlib,
    output : 'libnix-rust-test-support.a',
    command : static_library_command,
    install : true,
    install_dir : get_option('libdir'),
  )
else
  # See `meson.build` of `libnixrust`.
  windows_link_args = []
  if host_machine.system() == 'windows'
    windows_link_args += '-Wl,--allow-multiple-definition'
  endif

  # Export only the C functions; see `libnix-rust-test-support.map`.
  export_link_args = []
  export_link_depends = []
  if host_machine.system() == 'darwin'
    exported_symbols = files('libnix-rust-test-support.exp')
    export_link_args += '-Wl,-exported_symbols_list,' + exported_symbols[0].full_path()
    export_link_depends += exported_symbols
  elif host_machine.system() not in [ 'windows', 'cygwin' ]
    version_script = files('libnix-rust-test-support.map')
    export_link_args += '-Wl,--version-script,' + version_script[0].full_path()
    export_link_depends += version_script
  endif

  this_library = shared_library(
    'nix-rust-test-support',
    link_whole : cargo_staticlib,
    dependencies : [ nix_rust ],
    link_args : linker_export_flags + native_static_libs + windows_link_args + export_link_args,
    link_depends : export_link_depends,
    soversion : meson.project_version().split('+')[0].split('pre')[0],
    install : true,
  )
endif

import('pkgconfig').generate(
  filebase : meson.project_name(),
  name : 'Nix',
  description : 'Nix Package Manager (Rust test support)',
  requires : [ 'nix-rust' ],
  libraries : [ '-L${libdir}', '-lnix-rust-test-support' ],
  libraries_private : native_static_libs,
)

meson.override_dependency(
  meson.project_name(),
  declare_dependency(
    include_directories : include_dirs,
    link_with : this_library,
    dependencies : [ nix_rust ],
    link_args : get_option('default_library') == 'static' ? native_static_libs : [],
  ),
)
