#!/bin/sh
# Build the `nix-rust` staticlib with cargo for Meson.
#
# Meson calls this from a `custom_target`. The script copies the artifacts to
# the paths that Meson declares as outputs, and rewrites cargo's dep-info file
# so that Ninja rebuilds the target when a Rust source changes.
#
# Usage: cargo-build.sh CARGO MANIFEST TARGET_DIR PROFILE RUST_TARGET OUTDIR DEPFILE
#
# RUST_TARGET is empty for a build for the machine that runs cargo.
#
# OUTDIR receives:
# - `libnix_rust.a`, the staticlib;
# - the generated bridge headers (`<module>.rs.h`) and `cxx.h`, which are the
#   outputs that Meson installs;
# - the same headers under `include/nix/rust/` and `include/rust/`, the paths
#   by which C++ code includes them inside the build tree.

set -eu

cargo=$1
manifest=$2
target_dir=$3
profile=$4
rust_target=$5
outdir=$6
depfile=$7

if [ -n "$rust_target" ]; then
    set -- --target "$rust_target"
    artifact_dir=$target_dir/$rust_target
else
    set --
    artifact_dir=$target_dir
fi

case $profile in
    dev) artifact_dir=$artifact_dir/debug ;;
    *) artifact_dir=$artifact_dir/$profile ;;
esac

# Meson strips and splits the debug information of the final library.
export CARGO_PROFILE_RELEASE_STRIP=false

mkdir -p "$target_dir"
messages=$target_dir/nix-rust-messages.json
"$cargo" build \
    --manifest-path "$manifest" \
    --target-dir "$target_dir" \
    --profile "$profile" \
    --locked \
    --message-format=json-render-diagnostics \
    "$@" > "$messages"

cp "$artifact_dir/libnix_rust.a" "$outdir/libnix_rust.a"

# Cargo names its own artifact as the target of the dependency rule. Ninja
# checks that the rule names the first output that Meson declared.
sed "1s|^[^:]*:|$outdir/libnix_rust.a:|" "$artifact_dir/libnix_rust.d" > "$depfile"

# cxx-build writes the generated headers to the `OUT_DIR` of the build script
# of nix-rust. Cargo reports that directory in its `build-script-executed`
# message, also when the build script does not run again. (cxx-build also
# publishes the headers as links under `TARGET_DIR/cxxbridge`, but not in every
# configuration: the links are absent in a Nix sandbox build.)
out_dir=$(
    grep '"reason":"build-script-executed"' "$messages" \
        | grep '#nix-rust@' \
        | sed 's/.*"out_dir":"\([^"]*\)".*/\1/'
)
generated=$out_dir/cxxbridge/include
if [ ! -f "$generated/rust/cxx.h" ]; then
    echo "cargo-build.sh: no generated headers in '$generated'" >&2
    exit 1
fi

mkdir -p "$outdir/include/rust" "$outdir/include/nix/rust"
cp -L "$generated/rust/cxx.h" "$outdir/cxx.h"
cp -L "$generated/rust/cxx.h" "$outdir/include/rust/cxx.h"
for header in "$generated"/nix-rust/src/*.rs.h; do
    cp -L "$header" "$outdir/"
    cp -L "$header" "$outdir/include/nix/rust/"
done
