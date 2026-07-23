#!/usr/bin/env bash

# Two-sibling correctness: two `builtins.cache` invocations of the
# same cached function with DIFFERENT callback closures. Sibling
# discrimination for cb-apply results happens at the QCA queryHash
# level (different obsSet content → different QCA reqHash). The
# cache must return each sibling's own recorded result on warm
# replay.
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
