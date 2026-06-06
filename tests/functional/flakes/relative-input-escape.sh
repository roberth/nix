#!/usr/bin/env bash

# `follow-paths.sh` tests `resolveRelativePath`'s `..`-escape
# rejection only from the top-level flake (parent at tree root →
# any `..` escapes immediately). The non-trivial branch where the
# parent flake lives at a non-root subdir, `..` pops validly
# within the tree, and further `..` then escapes wasn't exercised.
# Added here:
#
# - subflake at /sub with a relative input that pops `..` validly
#   to a sibling under the tree root (success — depth never goes
#   negative).
# - same shape but with `..` that pops past root via
#   `/sub/../../escapedoy` (rejected).
#
# Decoy `/escapedoy/flake.nix` under the same tree makes a silent-
# clamp regression observable: without the boundary check, the
# misroute would land in the decoy and the test would see its
# output. With the check, lock errors out.

source ./common.sh

requireGit

root="$TEST_ROOT/relative-input-escape"
rm -rf "$root"
createGitRepo "$root"

mkdir -p "$root/sub" "$root/other" "$root/escapedoy"

cat > "$root/other/flake.nix" <<EOF
{
    description = "sibling target reached via ..";
    outputs = { ... }: { tag = "other"; };
}
EOF

cat > "$root/escapedoy/flake.nix" <<EOF
{
    description = "decoy: would-be landing spot if escape silently clamps";
    outputs = { ... }: { tag = "decoy"; };
}
EOF

# ----- subflake-with-valid-parent-pop ----------------------------------

cat > "$root/sub/flake.nix" <<EOF
{
    description = "subflake with valid relative input ../other";
    inputs = {
        sibling.url = "path:../other";
    };
    outputs = { sibling, ... }: { tag = sibling.tag; };
}
EOF
cat > "$root/flake.nix" <<EOF
{
    description = "root";
    inputs = {
        sub.url = "path:./sub";
    };
    outputs = { sub, ... }: { tag = sub.tag; };
}
EOF
git -C "$root" add flake.nix sub/flake.nix other/flake.nix escapedoy/flake.nix
nix flake lock "$root"
[[ $(nix eval --raw "$root#tag") = "other" ]]

# ----- subflake-with-escape --------------------------------------------

cat > "$root/sub/flake.nix" <<EOF
{
    description = "subflake with escaping relative input ../../escapedoy";
    inputs = {
        sibling.url = "path:../../escapedoy";
    };
    outputs = { sibling, ... }: { tag = sibling.tag; };
}
EOF
git -C "$root" add sub/flake.nix
rm -f "$root/flake.lock"
expect 1 nix flake lock "$root" 2>&1 \
    | grep "relative flake input path '../../escapedoy' escapes the source tree at"
