#!/usr/bin/env bash

# M4 T-comb-5: mutually recursive callback nesting. Cached body threads
# two DIFFERENT outer callbacks (f, g) through alternating deeper
# layers, each recursion adding a callback firing. Scalability test
# for M2's recursive nested-SCA replay (O17).
#
# `{ f, g }: f (n1: g (n2: f (n3: g (n4: n1+n2+n3+n4))))` at depth 4,
# then depth 8 and depth 10 to confirm scaling.
#
# Outer: f cb = cb 1, g cb = cb 10 (or 20 for divergence).
# Distinct callback fns at each layer verify the mechanism doesn't
# collapse or shadow across layers of the same shape.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/mut-nested-4.nix" << 'NIX'
{ f, g }: f (n1: g (n2: f (n3: g (n4: n1 + n2 + n3 + n4))))
NIX

echo "=== depth 4 cold (expect 22 = 2*1 + 2*10) ==="
result=$(nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/mut-nested-4.nix; }) {
    f = cb: cb 1;
    g = cb: cb 10;
  }')
echo "Got: $result"
[[ "$result" == 22 ]]

echo "=== depth 4 warm (expect 22) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/mut-nested-4.nix; }) {
    f = cb: cb 1;
    g = cb: cb 10;
  }')
echo "Got: $result"
[[ "$result" == 22 ]]

echo "=== depth 4 outer change: g gives 20 (expect 42 = 2*1 + 2*20) ==="
result=$(nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/mut-nested-4.nix; }) {
    f = cb: cb 1;
    g = cb: cb 20;
  }')
echo "Got: $result"
[[ "$result" == 42 ]]

echo "=== depth 4 restore, warm (expect 22) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/mut-nested-4.nix; }) {
    f = cb: cb 1;
    g = cb: cb 10;
  }')
echo "Got: $result"
[[ "$result" == 22 ]]

# --- Depth 8 to confirm scalability. Fresh cache slot per depth. ---

cat > "$TEST_ROOT/mut-nested-8.nix" << 'NIX'
{ f, g }: f (n1: g (n2: f (n3: g (n4: f (n5: g (n6: f (n7: g (n8:
  n1 + n2 + n3 + n4 + n5 + n6 + n7 + n8))))))))
NIX

echo "=== depth 8 cold (expect 44 = 4*1 + 4*10) ==="
result=$(nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/mut-nested-8.nix; }) {
    f = cb: cb 1;
    g = cb: cb 10;
  }')
echo "Got: $result"
[[ "$result" == 44 ]]

echo "=== depth 8 warm (expect 44) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/mut-nested-8.nix; }) {
    f = cb: cb 1;
    g = cb: cb 10;
  }')
echo "Got: $result"
[[ "$result" == 44 ]]

# --- Depth 10 to confirm no arbitrary cutoff. ---

cat > "$TEST_ROOT/mut-nested-10.nix" << 'NIX'
{ f, g }: f (n1: g (n2: f (n3: g (n4: f (n5: g (n6: f (n7: g (n8: f (n9: g (n10:
  n1 + n2 + n3 + n4 + n5 + n6 + n7 + n8 + n9 + n10))))))))))
NIX

echo "=== depth 10 cold (expect 55 = 5*1 + 5*10) ==="
result=$(nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/mut-nested-10.nix; }) {
    f = cb: cb 1;
    g = cb: cb 10;
  }')
echo "Got: $result"
[[ "$result" == 55 ]]

echo "=== depth 10 warm (expect 55) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/mut-nested-10.nix; }) {
    f = cb: cb 1;
    g = cb: cb 10;
  }')
echo "Got: $result"
[[ "$result" == 55 ]]
