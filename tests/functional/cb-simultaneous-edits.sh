#!/usr/bin/env bash

# M4 T-file-5: simultaneous outer + cached edits. Both the cached
# expression's file AND the outer args change between recordings.
# Cache must correctly reflect the combined new state.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/sim.nix" << 'NIX'
{ multiplier }: multiplier * 2
NIX

echo "=== cold v1: multiplier=5 (expect 10) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/sim.nix; }) { multiplier = 5; }')
echo "Got: $result"
[[ "$result" == 10 ]]

echo "=== warm v1 (expect 10) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/sim.nix; }) { multiplier = 5; }')
echo "Got: $result"
[[ "$result" == 10 ]]

# Both change: cached expression AND outer arg.
cat > "$TEST_ROOT/sim.nix" << 'NIX'
{ multiplier }: multiplier * 10
NIX

echo "=== v2 (cached edit + outer arg change): multiplier=7, cache*=10 (expect 70) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/sim.nix; }) { multiplier = 7; }')
echo "Got: $result"
[[ "$result" == 70 ]]

echo "=== warm v2 (expect 70) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/sim.nix; }) { multiplier = 7; }')
echo "Got: $result"
[[ "$result" == 70 ]]

# Original state restore.
cat > "$TEST_ROOT/sim.nix" << 'NIX'
{ multiplier }: multiplier * 2
NIX

echo "=== warm restore v1 (expect 10) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/sim.nix; }) { multiplier = 5; }')
echo "Got: $result"
[[ "$result" == 10 ]]
