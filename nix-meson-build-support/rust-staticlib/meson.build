# The common part of the components that build a Rust staticlib with cargo,
# such as `nix-rust` (`libnixrust`).
#
# Expects `cxx`, the C++ compiler, and the option `rust-target`.
#
# Defines:
# - `cargo` and `cargo_profile`, for `cargo-build.sh`;
# - `rust_target`, the value of the option;
# - `native_static_libs`, the system libraries that the Rust standard
#   library needs on the host;
# - `cargo_build_script`, see `cargo-build.sh`;
# - `localize_rust_symbols`, whether the host supports
#   `localize-rust-symbols.sh`, and `localize_rust_symbols_command`, the
#   command to run it with, followed by the pattern of the C exports, the
#   input and the output.

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
# Linking a shared library that contains a Rust staticlib, or linking a
# program against the static library, needs them.
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

cargo_build_script = files('cargo-build.sh')

# Only ELF has the tools that `localize-rust-symbols.sh` uses: a relocatable
# link with `-r`, and `objcopy --localize-symbols`.
localize_rust_symbols = host_machine.system() not in [
  'darwin',
  'windows',
  'cygwin',
]
localize_rust_symbols_command = [
  find_program('sh'),
  files('localize-rust-symbols.sh'),
  ' '.join(cxx.cmd_array()),
]
