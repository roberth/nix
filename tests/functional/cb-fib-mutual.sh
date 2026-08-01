#!/usr/bin/env bash

# Fibonacci via TWO mutually recursive cached functions. fibA and
# fibB both compute fib, but each delegates to the OTHER for the
# smaller sub-problem. Two distinct cached inner files; two distinct
# outer wrapper functions; each recursive step crosses through the
# opposite cache.
#
# fibA(n) = fibB(n-1) + fibA(n-2)  (delegates fibB for -1, fibA for -2)
# fibB(n) = fibA(n-1) + fibB(n-2)
#
# Same asymptotics as cb-fib-flat: cache doesn't memoize across
# call-context, so O(fib(N)). Correctness holds.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/fibA.nix" << 'NIX'
{ fibA, fibB, n }: if n < 2 then n else fibB (n - 1) + fibA (n - 2)
NIX

cat > "$TEST_ROOT/fibB.nix" << 'NIX'
{ fibA, fibB, n }: if n < 2 then n else fibA (n - 1) + fibB (n - 2)
NIX

FIB_EXPR='let cachedA = builtins.cache { import = '"$TEST_ROOT"'/fibA.nix; };
              cachedB = builtins.cache { import = '"$TEST_ROOT"'/fibB.nix; };
              fibA = n: cachedA { inherit fibA fibB n; };
              fibB = n: cachedB { inherit fibA fibB n; };
          in'

echo "=== fibA(0)=0 ==="
result=$(nix eval --impure --expr "$FIB_EXPR fibA 0")
[[ "$result" == 0 ]]

echo "=== fibA(5)=5 ==="
result=$(nix eval --impure --expr "$FIB_EXPR fibA 5")
echo "Got: $result"
[[ "$result" == 5 ]]

echo "=== fibB(5)=5 (mutual delegation makes both produce fib) ==="
result=$(nix eval --impure --expr "$FIB_EXPR fibB 5")
echo "Got: $result"
[[ "$result" == 5 ]]

echo "=== fibA(8)=21 ==="
result=$(nix eval --impure --expr "$FIB_EXPR fibA 8")
echo "Got: $result"
[[ "$result" == 21 ]]

echo "=== warm fibA(8)=21 ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "$FIB_EXPR fibA 8")
echo "Got: $result"
[[ "$result" == 21 ]]
