#!/usr/bin/env bash

# Pin that `import` on a Copyable-rooted path that traverses an
# in-tree symlink whose target escapes the accessor root raises an
# EvalError, instead of silently following the spliced target out of
# the tree (the historical behaviour without the kind-aware
# `resolveExprPath` migration).
#
# Mechanism: `builtins.fetchTree { type = "path"; lazy = true; }`
# returns a path-typed `outPath` admitted under a Copyable
# SourceRoot. `(outPath + "/escape-link/foo.nix")` builds a path
# Value with the fetcher's accessor and canon path
# `/escape-link/foo.nix`. `import` triggers `evalFile` →
# `resolveExprPath`, whose ancestor walk now uses the kind-aware
# `resolveSymlinks` wrapper. Following `/escape-link` splices its
# target `../escape` into the walk; processing from `/` (the
# parent), `..` pops past root and the boundary fires.

source common.sh

root="$TEST_ROOT/import-escape"
rm -rf "$root"
mkdir -p "$root/tree"

echo "{ a = 1; }" > "$root/tree/good.nix"
ln -s ../escape "$root/tree/escape-link"

xpFlags="flakes nix-command fetch-tree"

# `import` through an escaping in-tree symlink: should fail with the
# kind-aware wrapper's diagnostic.
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in import (t.outPath + \"/escape-link/foo.nix\")
" | grepQuiet "escape the source tree"

# Sanity: a non-escaping import through the same Copyable tree works.
# Pins that the wrapper isn't rejecting all Copyable imports — only
# the symlink-escape case.
[[ $(nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in toString (import (t.outPath + \"/good.nix\")).a
") = 1 ]]

# `builtins.readFile` through the same escape: goes through
# `realisePath`, whose kind-aware wrapper rejects the same way.
echo "content" > "$root/tree/good.txt"
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in builtins.readFile (t.outPath + \"/escape-link/file.txt\")
" | grepQuiet "escape the source tree"

# Sanity: readFile of a non-escaping path works.
[[ $(nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in builtins.readFile (t.outPath + \"/good.txt\")
") = "content" ]]

# `"${path}"` interpolation through the same escape: goes through
# `copyPathToStore`'s ancestor walk, whose kind-aware wrapper
# rejects via the same mechanism.
expectStderr 1 nix --extra-experimental-features "$xpFlags" eval --impure --raw --expr "
let
  t = builtins.fetchTree { type = \"path\"; path = $root/tree; lazy = true; };
in \"\${t.outPath + \"/escape-link/file.txt\"}\"
" | grepQuiet "escape the source tree"
