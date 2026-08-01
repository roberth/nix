#!/usr/bin/env bash

# M4 T-file-2: editing a transitively-imported file from within the
# cached expression invalidates the cache.
#
# The cached expression `import ./helper.nix` records the imported
# file's content as an env-file observation. Editing helper.nix
# changes its content hash → walker misses at that observation.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/helper.nix" << 'NIX'
x: x * 2
NIX

cat > "$TEST_ROOT/uses-helper.nix" << 'NIX'
{ multiplier }:
let helper = import ./helper.nix;
in helper multiplier
NIX

echo "=== cold: multiplier = 7 (expect 14) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/uses-helper.nix; }) { multiplier = 7; }')
echo "Got: $result"
[[ "$result" == 14 ]]

echo "=== warm same (expect 14) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/uses-helper.nix; }) { multiplier = 7; }')
echo "Got: $result"
[[ "$result" == 14 ]]

# Edit helper.nix — the transitive dependency. Cache should miss and
# re-eval.
cat > "$TEST_ROOT/helper.nix" << 'NIX'
x: x * 5
NIX

echo "=== after helper.nix edit (expect 35, cache miss + re-eval) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/uses-helper.nix; }) { multiplier = 7; }')
echo "Got: $result"
[[ "$result" == 35 ]]

echo "=== warm replay of edited version (expect 35) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/uses-helper.nix; }) { multiplier = 7; }')
echo "Got: $result"
[[ "$result" == 35 ]]
