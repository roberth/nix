#!/usr/bin/env bash

# M4 T-div: string-typed argument. Verify cache correctly distinguishes
# different string values and hits on identical strings.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/str.nix" << 'NIX'
{ prefix }: "${prefix}-suffix"
NIX

echo "=== cold: prefix='hello' (expect 'hello-suffix') ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/str.nix; }) { prefix = "hello"; }')
echo "Got: $result"
[[ "$result" == '"hello-suffix"' ]]

echo "=== warm same (expect 'hello-suffix') ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/str.nix; }) { prefix = "hello"; }')
echo "Got: $result"
[[ "$result" == '"hello-suffix"' ]]

echo "=== change to 'world' (expect 'world-suffix') ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/str.nix; }) { prefix = "world"; }')
echo "Got: $result"
[[ "$result" == '"world-suffix"' ]]

echo "=== restore 'hello' warm (expect 'hello-suffix') ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/str.nix; }) { prefix = "hello"; }')
echo "Got: $result"
[[ "$result" == '"hello-suffix"' ]]
