#!/usr/bin/env bash

# SourceRoot preservation across a builtins.cache boundary.
#
# `builtins.makePath` produces a path value whose SourceRoot is a
# Copyable-kinded virtual accessor with no stamped unpinnedId
# (anonymous). Passing that path into a `builtins.cache`-wrapped
# function and importing a child from it exercises three Phase 1
# properties at once:
#
# - Recording stamps a per-EvalState `anon#<n>` identifier for the
#   anonymous outer SourceRoot.
# - The inner state delegates identifier lookup to the outer via
#   `parentState`, prefixing with `_outer_:` so wire identifiers
#   stay disjoint per layer.
# - Warm reconstruction resolves `_outer_:anon#<n>` back to the
#   outer's SourceRoot via the parent chain, so `import` reaches
#   the virtual accessor rather than falling back to the system
#   root (which would fail — the tree only exists behind the
#   ValueSourceAccessor built by makePath).
#
# Pre-Phase-1 or with delegation missing, this test would fail
# with either a wrong-root import or a "sourceRootId not admitted"
# error.

source common.sh

enableFeatures "tracing-eval-cache"

cacheDir="$TEST_HOME/.cache/nix/eval-tracing-decision-graph"

clearCache() {
    rm -rf "$cacheDir"
}

clearCache

cat > "$TEST_ROOT/fn.nix" << 'NIX'
{ p }: import (p + "/child.nix")
NIX

expr='
  let
    f = builtins.cache { import = '"$TEST_ROOT"'/fn.nix; };
    input = builtins.makePath {
      root = { type = "directory"; entries = {
        "child.nix" = { type = "regular"; contents = "\"hello from makePath\""; };
      }; };
    };
  in f { p = input; }'

echo "=== cold ==="
cold=$(nix eval --impure --expr "$expr")
echo "cold: $cold"
[[ "$cold" == '"hello from makePath"' ]]

echo "=== warm ==="
warm=$(nix eval --impure --expr "$expr")
echo "warm: $warm"
[[ "$warm" == '"hello from makePath"' ]]
