#!/usr/bin/env bash

# Sibling cb-applies of the same cached fn whose pre-apply
# observations match but whose apply-result observations diverge
# must be discriminated by the cache.
#
# The cached body is `{ f, x }: f x`. Both applications pass the
# same `x = 1` but different `f` lambdas:
#   sibling A: f = x: { whatever = x * 100; }   → (f x).whatever == 100
#   sibling B: f = x: { whatever = x * 1000; }  → (f x).whatever == 1000
#
# The two lambdas are observationally indistinguishable until the
# outer reads `.whatever` on the apply result. At that point the
# responses diverge (100 vs 1000) and the cache must route each
# sibling's warm-replay to its own recorded value.
#
# Cold: 100 + 1000 = 1100. Warm: also 1100.
#
# Today warm returns 200 (= 100 + 100): the recorder stores both
# `.whatever` responses under the same requestHash in
# LocalResponseMap, the first writer wins, and sibling B's warm
# replay reads sibling A's stored 100. This is the discrimination
# gap the cidasks-with-paths redesign needs to close.
#
# This test is intentionally red until that redesign lands.

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

echo "=== warm DISALLOW_PARSE (expect 1100) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 1100 ]]
