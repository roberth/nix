#!/bin/sh
# Turn a Rust staticlib into a static library that exports only C++ symbols
# and the C functions whose names match a pattern.
#
# Usage: localize-rust-symbols.sh CC C_EXPORTS INPUT OUTPUT
#
# CC is the compiler driver, as one word list. C_EXPORTS is an extended
# regular expression for the C functions to keep global, for example
# `^nix_`. AR, NM, OBJCOPY and READELF come from the environment, as
# Nixpkgs sets them for the host, and default to the unprefixed tools.
#
# A Rust staticlib contains the Rust standard library, every crate, and the
# compiler runtime as global symbols. Two of them in one static link, for
# example `libnixrust.a` and the staticlib of a Rust program that links
# `libnixstore.a`, fail on duplicate symbols. The shared libraries solve
# this with a version script; an archive has no such thing. Instead, this
# script links all members of the archive into one relocatable object, so
# that the references between them are resolved, and then makes the global
# symbols local to that object, except:
#
# - C++ symbols. They are the interface, for example `nix::rs` and the cxx
#   runtime `rust::cxxbridge1`, and the inline functions and templates of
#   the C++ standard library. Rust symbols with the legacy mangling look
#   like C++ symbols, but end in a hash.
# - The C functions that match C_EXPORTS.
# - The signatures of COMDAT groups, for example `DW.ref.rust_eh_personality`.
#   The final link merges the groups by name with the same groups of other
#   objects, and a local symbol in a discarded group would break that.
#
# The weak symbols of the compiler runtime, for example `sqrt` and
# `__udivti3`, become local too, so that they do not replace the functions
# of the C library and of libgcc in the program.

set -eu

cc=$1
c_exports=$2
input=$3
output=$4

ar=${AR:-ar}
nm=${NM:-nm}
objcopy=${OBJCOPY:-objcopy}
readelf=${READELF:-readelf}

work=$output.localize
rm -rf "$work"
mkdir -p "$work"

# shellcheck disable=SC2086 # `cc` is a word list.
$cc -r -nostdlib \
    -Wl,--whole-archive "$input" -Wl,--no-whole-archive \
    -o "$work/prelinked.o"

# The lines of `readelf -g` look like
# COMDAT group section [    3] `.group' [<signature>] contains 2 sections:
"$readelf" -gW "$work/prelinked.o" \
    | sed -n "s/^COMDAT group section \[[ 0-9]*\] \`[^']*' \[\(.*\)\] contains .*/\1/p" \
    > "$work/group-signatures"

# A Rust symbol has the v0 mangling (`_R`), or the legacy mangling, which
# ends in a hash `17h<hex digits>E`, sometimes with a suffix
# `.llvm.<digits>` after it.
"$nm" -g --defined-only "$work/prelinked.o" \
    | awk -v c_exports="$c_exports" -v signatures="$work/group-signatures" '
        BEGIN {
            while ((getline line < signatures) > 0)
                signature[line] = 1
        }
        NF == 3 {
            name = $3
            rust = name ~ /^_R/ || name ~ /17h[0-9a-f]+E(\.llvm\.[0-9]+)?$/
            cxx = name ~ /^_Z/ && !rust
            if (!cxx && name !~ c_exports && !(name in signature))
                print name
        }' \
    > "$work/localize"

"$objcopy" --localize-symbols="$work/localize" "$work/prelinked.o"

rm -f "$output"
"$ar" rcs "$output" "$work/prelinked.o"
rm -rf "$work"
