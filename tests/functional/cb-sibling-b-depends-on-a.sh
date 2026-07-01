#!/usr/bin/env bash

# Sibling B's cb-arg depends on sibling A's result (`b.x = a.whatever`).
# The evaluation order is naturally A-then-B — forcing `b.whatever`
# forces `a.whatever` first. This is the shape most real-world uses of
# `builtins.cache` take: earlier cached results feed later ones as
# data. Cache discrimination must work even when the two sibling
# invocations aren't independent.
#
# Cached body: `{ f, x }: f x`.
#   sibling A: f = x: { whatever = x * 100; }, x = 1
#     → a.whatever = 100
#   sibling B: f = x: { whatever = x * 1000; }, x = a.whatever = 100
#     → b.whatever = 100000
# Total: 100100.
#
# Warm must produce the same 100100 without falling through to the
# inner interpreter (`_NIX_DISALLOW_CACHE_INTERPRET_INNER=1`). Any
# discrimination scheme that can't handle "sibling B legitimately runs
# after sibling A" is broken: this isn't cache poisoning, it's data
# flow.
#
# Red until cb-sibling's discrimination lands.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/fn.nix" << 'NIX'
{ f, x }: f x
NIX

EXPR="let
  cached = builtins.cache { import = $TEST_ROOT/fn.nix; };
  a = cached { f = x: { whatever = x * 100; }; x = 1; };
  b = cached { f = x: { whatever = x * 1000; }; x = a.whatever; };
in a.whatever + b.whatever"

echo "=== cold (expect 100100) ==="
result=$(nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 100100 ]]

echo "=== warm DISALLOW_CACHE_INTERPRET_INNER (expect 100100) ==="
result=$(_NIX_DISALLOW_CACHE_INTERPRET_INNER=1 nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 100100 ]]
