#!/usr/bin/env bash

# Fibonacci via `builtins.cache` called from WITHIN the cached file.
# The recursive calls take only an int arg — no callback, no
# outer→inner→outer round-trip carrying closure state through
# observations. Cross-session (and within-session) memoization
# naturally kicks in because each fib(k) invocation's cache Selector
# depends only on `{ n = k }`, which is stable across every caller.
#
# Result: O(N) execution, not O(fib(N)). Contrast with cb-fib-flat.sh
# and cb-fib-mutual.sh where fib is passed as a callback and identity
# includes the observed recursion tree, defeating memoization.
#
# Also demonstrates: obsSet nesting depth stays at 0 because no
# callbacks are involved — the outer→inner argument is a plain
# attrset with an int.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/fib-self.nix" << NIX
{ n }:
let cachedFib = builtins.cache { import = $TEST_ROOT/fib-self.nix; };
in if n < 2 then n else (cachedFib { n = n - 1; }) + (cachedFib { n = n - 2; })
NIX

echo "=== fib(10)=55 ==="
result=$(nix eval --impure --expr "(builtins.cache { import = $TEST_ROOT/fib-self.nix; }) { n = 10; }")
echo "Got: $result"
[[ "$result" == 55 ]]

echo "=== fib(20)=6765 ==="
result=$(nix eval --impure --expr "(builtins.cache { import = $TEST_ROOT/fib-self.nix; }) { n = 20; }")
echo "Got: $result"
[[ "$result" == 6765 ]]

echo "=== fib(30)=832040 (would be ~10^6 recursive calls without memoization) ==="
result=$(nix eval --impure --expr "(builtins.cache { import = $TEST_ROOT/fib-self.nix; }) { n = 30; }")
echo "Got: $result"
[[ "$result" == 832040 ]]

echo "=== warm fib(30)=832040 ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "(builtins.cache { import = $TEST_ROOT/fib-self.nix; }) { n = 30; }")
echo "Got: $result"
[[ "$result" == 832040 ]]

# Stats: expect hits > 0 (memoization firing) and depth == 0 (no callbacks).
echo "=== stats: memoization + no-callback shape ==="
clearCache
statsFile=$(mktemp)
NIX_CACHE_STATS_FILE="$statsFile" nix eval --impure --expr \
  "(builtins.cache { import = $TEST_ROOT/fib-self.nix; }) { n = 20; }" > /dev/null
echo "fib(20) stats: $(cat "$statsFile")"
hits=$(sed 's/.*"hits":\([0-9]*\).*/\1/' "$statsFile")
depth=$(sed 's/.*"maxCallbackObsSetNestingDepth":\([0-9]*\).*/\1/' "$statsFile")
[[ "$hits" -gt 0 ]]
[[ "$depth" == 0 ]]
rm -f "$statsFile"
