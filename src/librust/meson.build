project(
  'nix-rust',
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

# `libnixrust` contains all the Rust code of Nix, and the C++ side of the cxx
# bridges. It is the bottom of the library graph: every other Nix library links
# it, and it links no Nix library. See `doc/manual/source/development/rust.md`.

cxx = meson.get_compiler('cpp')
fs = import('fs')

subdir('nix-meson-build-support/deps-lists')
subdir('nix-meson-build-support/export-all-symbols')

cargo = find_program('cargo')
rustc = find_program('rustc')
rust_target = get_option('rust-target')

# The cargo profile follows the Meson build type, so that an optimized Nix has
# optimized Rust code and a debug build of Nix has debuggable Rust code.
if get_option('buildtype') == 'debug'
  cargo_profile = 'dev'
else
  cargo_profile = 'release'
endif

# Ask rustc which system libraries the Rust standard library needs on the host.
# Linking `libnixrust` (shared), or linking a program against it (static),
# needs them.
native_static_libs_args = [
  '--crate-type=staticlib',
  '--crate-name=nix_rust_native_static_libs',
  '--edition=2024',
  '--print=native-static-libs',
  '--out-dir',
  meson.current_build_dir() / 'native-static-libs',
]
if rust_target != ''
  native_static_libs_args += [ '--target', rust_target ]
endif
native_static_libs_probe = run_command(
  rustc,
  native_static_libs_args,
  files('native-static-libs.rs'),
  check : true,
)
native_static_libs = []
foreach line : native_static_libs_probe.stderr().split('\n')
  if line.contains('native-static-libs: ')
    foreach lib : line.split('native-static-libs: ')[1].strip().split(' ')
      if lib != ''
        native_static_libs += lib
      endif
    endforeach
  endif
endforeach

# The Rust standard library for musl asks for LLVM's libunwind. A GCC
# toolchain has no libunwind; it unwinds C++ with libgcc_eh, which provides
# the same `_Unwind_*` interface. The C++ runtime then unwinds the Rust frames
# too, so that one unwinder handles both languages. Two unwinders in one
# program would each see only the frames that they registered.
if '-lunwind' in native_static_libs and not cxx.find_library(
  'unwind',
  required : false,
).found()
  message(
    'No libunwind for the host; the unwinder of the C++ runtime unwinds Rust frames too.',
  )
  native_static_libs_without_unwind = []
  foreach lib : native_static_libs
    if lib != '-lunwind'
      native_static_libs_without_unwind += lib
    endif
  endforeach
  native_static_libs = native_static_libs_without_unwind
endif
message('Rust native static libraries: ' + ' '.join(native_static_libs))

# The generated C++ headers, one per `#[cxx::bridge]` module, and the cxx
# runtime header. Keep in sync with `BRIDGES` in `build.rs`.
bridge_headers = [
  'lib.rs.h',
  'probe.rs.h',
  'future_probe.rs.h',
  'tarball_cache.rs.h',
]

# The installation directory of each output of `cargo_build`. The staticlib
# itself is installed only as part of `this_library`.
cargo_install_dirs = [ false, get_option('includedir') / 'rust' ]
foreach header : bridge_headers
  cargo_install_dirs += get_option('includedir') / 'nix/rust'
endforeach

cargo_build = custom_target(
  'nix-rust-cargo',
  input : files(
    'Cargo.lock',
    'Cargo.toml',
  ),
  output : [ 'libnix_rust.a', 'cxx.h' ] + bridge_headers,
  depfile : 'libnix_rust.a.d',
  command : [
    find_program('sh'),
    files('cargo-build.sh'),
    cargo,
    meson.current_source_dir() / 'Cargo.toml',
    meson.current_build_dir() / 'cargo-target',
    cargo_profile,
    rust_target,
    meson.current_build_dir(),
    '@DEPFILE@',
  ],
  console : true,
  build_by_default : true,
  install : true,
  install_dir : cargo_install_dirs,
)

install_headers(
  'include/nix/rust/boundary.hh',
  'include/nix/rust/future-abi.h',
  'include/nix/rust/future-abi.hh',
  'include/nix/rust/future-probe.hh',
  'include/nix/rust/tarball-cache.hh',
  subdir : 'nix/rust',
)

# The handwritten headers are in the source directory under `include/`, and
# `cargo-build.sh` puts the generated headers in the build directory under
# `include/`. Meson adds the build directory counterpart of an include
# directory only when the directory exists at configure time, so the
# generated one is given as a compiler argument.
include_dirs = [ include_directories('include') ]
generated_include_args = [ '-I' + (meson.current_build_dir() / 'include') ]

cargo_staticlib = cargo_build[0]
cargo_headers = []
foreach i : range(1, 2 + bridge_headers.length())
  cargo_headers += cargo_build[i]
endforeach

if get_option('default_library') == 'static'
  # The cargo staticlib is the static library. Copy it to the name that the
  # other Nix libraries link.
  this_library = custom_target(
    'nixrust',
    input : cargo_staticlib,
    output : 'libnixrust.a',
    command : [ 'cp', '@INPUT@', '@OUTPUT@' ],
    install : true,
    install_dir : get_option('libdir'),
  )
else
  # On Windows, Rust generates import stubs for `raw-dylib` imports in each
  # crate that declares them, for example `ProcessPrng` in both the standard
  # library and `getrandom`. The stubs of one function are identical, but
  # `link_whole` puts all of them into the link, and GNU ld then reports
  # multiple definitions. Allow them for this one library.
  windows_link_args = []
  if host_machine.system() == 'windows'
    windows_link_args += '-Wl,--allow-multiple-definition'
  endif

  # Export only the interface of `libnixrust`, and keep the Rust standard
  # library and the Rust crates local; see `libnixrust.map`. A Windows DLL
  # binds each import to the DLL that it names, so a second copy of the
  # Rust standard library in another DLL does not clash there, and
  # `linker_export_flags` still exports everything.
  export_link_args = []
  export_link_depends = []
  if host_machine.system() == 'darwin'
    exported_symbols = files('libnixrust.exp')
    export_link_args += '-Wl,-exported_symbols_list,' + exported_symbols[0].full_path()
    export_link_depends += exported_symbols
  elif host_machine.system() not in [ 'windows', 'cygwin' ]
    version_script = files('libnixrust.map')
    export_link_args += [
      '-Wl,--version-script,' + version_script[0].full_path(),
      # Calls from `libnixrust` to its own exported functions, such as the
      # cxx runtime, stay inside `libnixrust`. Another library with the
      # same cxx runtime cannot interpose them.
      '-Wl,-Bsymbolic-functions',
    ]
    export_link_depends += version_script
  endif

  this_library = shared_library(
    'nixrust',
    link_whole : cargo_staticlib,
    link_args : linker_export_flags + native_static_libs + windows_link_args + export_link_args,
    link_depends : export_link_depends,
    soversion : meson.project_version().split('+')[0].split('pre')[0],
    install : true,
  )
endif

import('pkgconfig').generate(
  filebase : meson.project_name(),
  name : 'Nix',
  description : 'Nix Package Manager (Rust components)',
  libraries : [ '-L${libdir}', '-lnixrust' ],
  libraries_private : native_static_libs,
)

meson.override_dependency(
  meson.project_name(),
  declare_dependency(
    include_directories : include_dirs,
    compile_args : generated_include_args,
    link_with : this_library,
    # Order the compilation of C++ code that includes the generated headers
    # after the cargo build.
    sources : cargo_headers,
    link_args : get_option('default_library') == 'static' ? native_static_libs : [],
  ),
)
