#!/usr/bin/env bash

# Fibonacci via a single flattened cached callback.
#
# `fib.nix` receives the outer `fib` function and n. Recursion goes:
# outer's fib → cachedFib(fib, n) → cached body computes fib(n-1)+fib(n-2)
# → outer's fib for each sub-problem → cachedFib again.
#
# CORRECTNESS: fib(N) returns the right value.
#
# MEMOIZATION: does NOT emerge naturally from the tracing cache. Each
# outer→inner callback firing has content-addressed identity that
# depends on what inner probes about `fib` during THAT firing. The
# same `fib 3` call from `fib 5` vs `fib 4` records different SCAs
# because the deeper recursion trees differ, so cross-context reuse
# doesn't happen. Timing is O(fib(N)) ~ O(1.618^N) even with the
# cache engaged. Bounded n only.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/fib.nix" << 'NIX'
{ fib, n }: if n < 2 then n else fib (n - 1) + fib (n - 2)
NIX

FIB_EXPR='let cachedFib = builtins.cache { import = '"$TEST_ROOT"'/fib.nix; };
              fib = n: cachedFib { inherit fib n; };
          in'

echo "=== fib(0)=0 ==="
result=$(nix eval --impure --expr "$FIB_EXPR fib 0")
[[ "$result" == 0 ]]

echo "=== fib(1)=1 ==="
result=$(nix eval --impure --expr "$FIB_EXPR fib 1")
[[ "$result" == 1 ]]

echo "=== fib(5)=5 ==="
result=$(nix eval --impure --expr "$FIB_EXPR fib 5")
echo "Got: $result"
[[ "$result" == 5 ]]

echo "=== fib(10)=55 ==="
result=$(nix eval --impure --expr "$FIB_EXPR fib 10")
echo "Got: $result"
[[ "$result" == 55 ]]

echo "=== warm fib(10)=55 ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "$FIB_EXPR fib 10")
echo "Got: $result"
[[ "$result" == 55 ]]
