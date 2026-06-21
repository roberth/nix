#!/usr/bin/env bash

# Higher-order callback: cached body constructs an inner lambda and
# passes it to the outer-supplied callback, which applies it.
# Design doc's "higher-order callback" example (lines 988-1052).
#
# Exercises ReplayLocalObject standin construction at the apply branch
# of resolveAmbientId (tracing-replay-evaluator.cc:273) when the walker
# dispatches a recorded apply whose arg id refers to an inner-supplied
# local value that's no longer live on replay.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/higher-order.nix" << 'NIX'
{ f }: f (x: x + 1)
NIX

# Cold record: outer-side g (= f) applies inner lambda (x:x+1) to 5
echo "=== cold record (expect 6) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/higher-order.nix; }) { f = g: g 5; }')
echo "Got: $result"
[[ "$result" == 6 ]]

# Replay (DISALLOW_PARSE): walker dispatches the apply Fact; the
# inner lambda has no live representation, so resolveAmbientId
# materialises a ReplayLocalObject standin keyed by the lambda's CDI.
echo "=== replay (expect 6) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/higher-order.nix; }) { f = g: g 5; }')
echo "Got: $result"
[[ "$result" == 6 ]]

# Outer-fn change: f now applies the inner lambda to 10 instead of 5.
# The recorded apply-result observation (= 6 for (x:x+1) 5) differs
# from the live observation (= 11 for (x:x+1) 10). Live dispatch through
# the standin must observe this divergence and invalidate.
echo "=== outer change f to apply 10 (expect 11) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/higher-order.nix; }) { f = g: g 10; }')
echo "Got: $result"
[[ "$result" == 11 ]]

# Restore: with the original outer f, replay must hit the recorded
# trace and return 6.
echo "=== restore (expect 6) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/higher-order.nix; }) { f = g: g 5; }')
echo "Got: $result"
[[ "$result" == 6 ]]
