#!/usr/bin/env bash

# Property: sibling cb-applies discriminate when their traces
# diverge only after the callback has returned.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/fn.nix" << 'NIX'
{ f, x }: f x
NIX

EXPR="let cached = builtins.cache { import = $TEST_ROOT/fn.nix; }; in (cached { f = x: { whatever = x * 100; }; x = 1; }).whatever + (cached { f = x: { whatever = x * 1000; }; x = 1; }).whatever"

echo "=== cold (expect 1100) ==="
result=$(nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 1100 ]]

echo "=== warm DISALLOW_CACHE_INTERPRET_INNER (expect 1100) ==="
result=$(_NIX_DISALLOW_CACHE_INTERPRET_INNER=1 nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 1100 ]]
