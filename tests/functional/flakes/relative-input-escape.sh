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

# ----- relative-input-via-escaping-symlink -----------------------------

# `inputs.X.url = "path:./sub/symlink-escape"` exercises the
# symlink-target case at the relative-input layer. Note:
# `resolveRelativePath` walks with `SymlinkResolution::Ancestors`,
# which does *not* resolve the trailing component — so the
# escape isn't detected at lock time by Part 3A. Instead, the
# resolved SourcePath flows through `readFlake → evalFile →
# resolveExprPath` (Part 2B), which uses `Full` mode and follows
# the symlink. The wrapper's `StrictCopyableBoundary` then
# catches the escape with the bare libutil message.
#
# This is the "two layers compose" property from Part 2/3:
# Part 3A catches escapes whose `..` is in the input path's
# *ancestor* chain (so the Ancestors-mode walk hits them);
# Part 2 catches escapes via symlinks at the input's trailing
# position (Full-mode walk at the actual read site).
ln -s ../../escape "$root/sub/symlink-escape"
cat > "$root/flake.nix" <<EOF
{
    description = "root with relative input via escaping symlink";
    inputs = {
        viaLink.url = "path:./sub/symlink-escape";
    };
    outputs = { viaLink, ... }: { tag = viaLink.tag; };
}
EOF
git -C "$root" add flake.nix sub/symlink-escape
rm -f "$root/flake.lock"
expect 1 nix flake lock "$root" 2>&1 \
    | grep "'\.\.' would escape the source tree at"

# Absolute-symlink variant: `abs-link -> /sibling`. Same
# layering — caught at the read site (Part 2B) with the
# StrictCopyableBoundary's `Copyable trees must be position-
# independent` message.
ln -s /sibling "$root/abs-link"
cat > "$root/flake.nix" <<EOF
{
    description = "root with relative input via absolute symlink";
    inputs = {
        viaAbs.url = "path:./abs-link";
    };
    outputs = { viaAbs, ... }: { tag = viaAbs.tag; };
}
EOF
git -C "$root" add flake.nix abs-link
rm -f "$root/flake.lock"
expect 1 nix flake lock "$root" 2>&1 \
    | grep "absolute symlink '.*' .* is not allowed; Copyable trees must be position-independent"

# ----- relative-input-with-escaping-?dir= ------------------------------

# `inputs.X.url = "path:./sub?dir=../../escapedoy"` exercises the
# `?dir=` escape on a relative input. `resolveRelativePath` itself
# succeeds (the relative path `./sub` is in-tree); the subdir
# composition `resolvedPath + "/" + ref.subdir` is what walks past
# the accessor root. For a flake input that goes through
# `readFlake`, Part 3B catches it first (readFlake composes the
# ?dir= subdir against rootDir.path with the wrapper). The Part
# 3A.2 site is a backstop for the lockFlake-internal subdir
# composition that doesn't go through readFlake.
cat > "$root/sub/flake.nix" <<EOF
{
    description = "subflake (target — not the actual loaded flake)";
    outputs = { ... }: { tag = "loaded-sub"; };
}
EOF
cat > "$root/flake.nix" <<EOF
{
    description = "root with relative input + escaping ?dir=";
    inputs = {
        sub.url = "path:./sub?dir=../../escapedoy";
    };
    outputs = { sub, ... }: { tag = sub.tag; };
}
EOF
git -C "$root" add flake.nix sub/flake.nix
rm -f "$root/flake.lock"
expect 1 nix flake lock "$root" 2>&1 \
    | grep "flake input subdir '../../escapedoy' escapes the source tree"

# ----- hand-edited-lockfile defence-in-depth ---------------------------

# Bypass `resolveRelativePath` entirely: lock against a benign input,
# then hand-edit `flake.lock` to point its `locked.path` at
# `../escapedoy`. At eval time, `call-flake.nix` builds
# `flakePath = parentNode.flakePath + "/" + node.locked.path` (path-
# concat). Part 1's lexical strict-`..` check on Copyable-rooted
# concat fires: the joined string `/(parent)/../escapedoy`'s `..`
# would pop past the accessor root, so concat raises EvalError.
#
# This is the defence-in-depth layer the call-flake.nix path relies
# on. Without it, hand-edited (or corrupted) lockfiles could
# silently clamp the escape and eval would happily import the
# wrong tree.
cat > "$root/sub/flake.nix" <<EOF
{
    description = "subflake (benign for the initial lock)";
    outputs = { ... }: { tag = "benign"; };
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
git -C "$root" add flake.nix sub/flake.nix
rm -f "$root/flake.lock"
nix flake lock "$root"
[[ $(nix eval --raw "$root#tag") = "benign" ]]

# Tamper with the lockfile: rewrite the `sub` node's locked.path
# from `sub` to `../escapedoy`. The lockfile-loading path in
# `call-flake.nix` trusts the locked attrs verbatim, so this
# reaches the path-concat site without going through
# `resolveRelativePath`'s lock-time check again. Part 1's lexical
# check at the concat site is what catches the escape.
jq '.nodes.sub.locked.path = "../escapedoy"' "$root/flake.lock" \
    > "$root/flake.lock.tmp"
mv "$root/flake.lock.tmp" "$root/flake.lock"

# Tightened: pin Part 1's specific wording so a regression in
# the path-concat error message doesn't silently weaken this
# defence-in-depth assertion.
expect 1 nix eval --raw "$root#tag" 2>&1 \
    | grep "concatenated path '.*' would escape the source tree at"
