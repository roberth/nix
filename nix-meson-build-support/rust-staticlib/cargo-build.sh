#!/bin/sh
# Build a Rust staticlib with cargo for Meson.
#
# Meson calls this from a `custom_target`. The script copies the artifacts to
# the paths that Meson declares as outputs, and rewrites cargo's dep-info file
# so that Ninja rebuilds the target when a Rust source changes.
#
# Usage: cargo-build.sh CARGO MANIFEST TARGET_DIR PROFILE RUST_TARGET OUTDIR DEPFILE CRATE [cxx]
#
# RUST_TARGET is empty for a build for the machine that runs cargo. CRATE is
# the name of the package, for example `nix-rust`; its staticlib is
# `lib<CRATE with underscores>.a`.
#
# OUTDIR receives the staticlib. With the last argument `cxx`, it also
# receives:
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
crate=$8
cxx_headers=${9:-}

staticlib=lib$(printf '%s' "$crate" | tr - _).a

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
messages=$target_dir/$crate-messages.json
"$cargo" build \
    --manifest-path "$manifest" \
    --target-dir "$target_dir" \
    --profile "$profile" \
    --locked \
    --message-format=json-render-diagnostics \
    "$@" > "$messages"

cp "$artifact_dir/$staticlib" "$outdir/$staticlib"

# Cargo names its own artifact as the target of the dependency rule. Ninja
# checks that the rule names the first output that Meson declared.
sed "1s|^[^:]*:|$outdir/$staticlib:|" "$artifact_dir/${staticlib%.a}.d" > "$depfile"

if [ "$cxx_headers" != cxx ]; then
    exit 0
fi

# cxx-build writes the generated headers to the `OUT_DIR` of the build script
# of the crate. Cargo reports that directory in its `build-script-executed`
# message, also when the build script does not run again. (cxx-build also
# publishes the headers as links under `TARGET_DIR/cxxbridge`, but not in every
# configuration: the links are absent in a Nix sandbox build.)
out_dir=$(
    grep '"reason":"build-script-executed"' "$messages" \
        | grep "#$crate@" \
        | sed 's/.*"out_dir":"\([^"]*\)".*/\1/'
)
generated=$out_dir/cxxbridge/include
if [ ! -f "$generated/rust/cxx.h" ]; then
    echo "cargo-build.sh: no generated headers in '$generated'" >&2
    exit 1
fi

# The copies under `include/` are the ones that the compiler reads. Ninja does
# not know them as outputs of this script, so it cannot order a compilation,
# or the build of a precompiled header, after the copy. Copy them only if
# their contents changed: a new modification time on an unchanged header,
# such as `cxx.h`, makes the precompiled headers that include it stale in the
# middle of a build, and the compiler then rejects them. The outputs that
# Meson declares are copied every time, so that Ninja sees them as up to
# date.
copy_if_changed() {
    if ! cmp -s "$1" "$2"; then
        cp -L "$1" "$2"
    fi
}

mkdir -p "$outdir/include/rust" "$outdir/include/nix/rust"
cp -L "$generated/rust/cxx.h" "$outdir/cxx.h"
copy_if_changed "$generated/rust/cxx.h" "$outdir/include/rust/cxx.h"
for header in "$generated/$crate"/src/*.rs.h; do
    cp -L "$header" "$outdir/"
    copy_if_changed "$header" "$outdir/include/nix/rust/$(basename "$header")"
done
