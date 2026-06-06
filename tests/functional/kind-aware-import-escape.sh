#!/usr/bin/env bash

# Pin the kind-aware `resolveSymlinks` wrapper's behaviour through
# the three Part 2 read sites: `import` (via `resolveExprPath`),
# `readFile` (via `realisePath`), and `"${path}"` interpolation
# (via `copyPathToStore`'s ancestor walk). For each site, exercise:
#
# - Symlink-target `..` escape — the path itself has no literal
#   `..`, but resolving an in-tree symlink whose target pops past
#   the accessor root raises `AccessorBoundaryEscape`. Pre-fix the
#   spliced target was silently followed out of the tree.
# - Absolute-target symlink — same shape, but the symlink target
#   is `/sibling` instead of `../escape`. Copyable trees must be
#   position-independent (post-materialisation, `/` shifts meaning
#   from accessor root to system root) so the
#   `StrictCopyableBoundary` arm rejects absolute symlinks too.
# - Non-escaping sanity case — pins that the wrapper isn't
#   rejecting all Copyable reads, only the escape cases.
#
# Mechanism for each: `builtins.fetchTree { type = "path"; lazy =
# true; }` returns a path-typed `outPath` admitted under a
# Copyable SourceRoot. `outPath + "/<some-suffix>"` builds a path
# Value with the fetcher's accessor; the relevant read site (
# import / readFile / interpolation) routes through the kind-aware
# wrapper.

source common.sh

root="$TEST_ROOT/import-escape"
rm -rf "$root"
mkdir -p "$root/tree/sub"

echo "{ a = 1; }" > "$root/tree/good.nix"
echo "content" > "$root/tree/good.txt"
# `..`-escaping symlink: target pops past root from /.
ln -s ../escape "$root/tree/escape-link"
# Absolute symlink: target shifts meaning at materialisation.
ln -s /sibling "$root/tree/abs-link"

xpFlags="flakes nix-command fetch-tree"

# Tight pattern for the bare libutil-level escape (propagated when
# the read site doesn't catch + rewrap). Includes `'..'` so a
# regression that drops the literal `..` mention surfaces.
dotdotEscapePattern="'\.\.' would escape the source tree at"
# Tight pattern for the absolute-symlink rejection (Copyable-only,
# from `StrictCopyableBoundary`). The "position-independent" tail
# is the load-bearing rationale phrase; pin it to catch a
# regression that softens the message.
absSymlinkPattern="absolute symlink '.*' .* is not allowed; Copyable trees must be position-independent"

# ----- import / parseExprFromFile via resolveExprPath ------------------

# Symlink-target `..` escape: import through `/escape-link/foo.nix`.
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in import (t.outPath + \"/escape-link/foo.nix\")
" | grepQuiet "$dotdotEscapePattern"

# Absolute-symlink rejection: import through `/abs-link/foo.nix`.
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in import (t.outPath + \"/abs-link/foo.nix\")
" | grepQuiet "$absSymlinkPattern"

# Sanity: a non-escaping import through the same Copyable tree works.
[[ $(nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in toString (import (t.outPath + \"/good.nix\")).a
") = 1 ]]

# ----- readFile via realisePath ----------------------------------------

# Symlink-target `..` escape: readFile through `/escape-link/file.txt`.
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in builtins.readFile (t.outPath + \"/escape-link/file.txt\")
" | grepQuiet "$dotdotEscapePattern"

# Absolute-symlink rejection: readFile through `/abs-link/file.txt`.
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in builtins.readFile (t.outPath + \"/abs-link/file.txt\")
" | grepQuiet "$absSymlinkPattern"

# Sanity: readFile of a non-escaping path works.
[[ $(nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in builtins.readFile (t.outPath + \"/good.txt\")
") = "content" ]]

# ----- ${path} interpolation via copyPathToStore -----------------------

# Symlink-target `..` escape: interpolating a path whose ancestor
# is `/escape-link` triggers copyPathToStore's ancestor walk.
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in \"\${t.outPath + \"/escape-link/file.txt\"}\"
" | grepQuiet "$dotdotEscapePattern"

# Absolute-symlink rejection: interpolation through `/abs-link/`.
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in \"\${t.outPath + \"/abs-link/file.txt\"}\"
" | grepQuiet "$absSymlinkPattern"
