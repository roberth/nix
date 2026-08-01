#!/usr/bin/env bash

# M4 T-comb-3: two independent contra-args in a single cached-fn firing.
#
# `{ f, g }: f 1 + g 2` — cb body invokes two separate callbacks.
# Each callback gets its own contra-arg from inner; the two firings
# should be recorded independently, and warm should replay both.
#
# This exercises whether SCAs for sibling callback firings within one
# cached-fn invocation are correctly attributed to their respective
# cells and don't collide.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/two-cb.nix" << 'NIX'
{ f, g }: f 1 + g 2
NIX

echo "=== cold: f = x: x*10, g = y: y*100 (expect 210) ==="
result=$(nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/two-cb.nix; }) {
    f = x: x * 10;
    g = y: y * 100;
  }')
echo "Got: $result"
[[ "$result" == 210 ]]

echo "=== warm replay (expect 210) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/two-cb.nix; }) {
    f = x: x * 10;
    g = y: y * 100;
  }')
echo "Got: $result"
[[ "$result" == 210 ]]

echo "=== change f (expect 2010) ==="
result=$(nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/two-cb.nix; }) {
    f = x: x * 2000;
    g = y: y * 100;
  }')
echo "Got: $result"
[[ "$result" == 2200 ]]

echo "=== restore to original — warm replay (expect 210) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/two-cb.nix; }) {
    f = x: x * 10;
    g = y: y * 100;
  }')
echo "Got: $result"
[[ "$result" == 210 ]]
