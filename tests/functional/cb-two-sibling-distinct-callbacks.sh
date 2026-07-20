#!/usr/bin/env bash

# Two-sibling correctness: two `builtins.cache` invocations of the
# same cached function with DIFFERENT callback closures. Their
# outer-probe queryHashes collide (matching-until-divergence at
# Arg{0} initial state), so cold records two Terminals under the
# same queryHash at distinct session-cumulative curs.
#
# Regression bug (fixed by "try outgoing Asks before Terminal"):
# warm's second-sibling walk would find the first-sibling's
# Terminal at envCur and return its Result silently → wrong hit,
# not a miss.
#
# Correct behaviour: warm's second-sibling walk tries outgoing Asks
# at envCur first. Under matching-until-divergence, the wrong
# sibling's chain has extensions from that cur — warm dispatches
# them under its own live cell chain and gets responses that match
# cold's b-recording, advancing to b's Terminal.
#
# Test asserts warm returns the correct value (42 + 99 = 141), not
# the wrong-hit value (42 + 42 = 84).

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/pass-attrset.nix" << 'NIX'
{ f }: f { a = 42; b = 99; }
NIX

EXPR='let
  c = builtins.cache { import = '"$TEST_ROOT"'/pass-attrset.nix; };
  a = c { f = x: x.a; };
  b = c { f = x: x.b; };
in a + b'

echo "=== cold (expect 141) ==="
result=$(nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 141 ]]

echo "=== warm (expect 141, not 84 wrong-hit) ==="
result=$(nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 141 ]]
