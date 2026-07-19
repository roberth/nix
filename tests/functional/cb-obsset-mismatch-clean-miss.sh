#!/usr/bin/env bash

# Verify clean miss when the outer callback probes an attribute
# not in the recorded obsSet.
#
# Setup: cached body applies the outer's callback f to an
# inner-constructed attrset. Under matching-until-divergence, fn's
# state hash reflects Arg{0} at the cache boundary — same for two
# outers with different callback closures. So a warm session with a
# DIFFERENT callback f may reach cold's callbackApply Q (same
# ref.fn hash), invoke its own live fn on a ReplayCallbackArg
# backed by cold's obsSet, and probe an attr cold didn't record.
#
# Expected: warm's obsSet-miss triggers ReplayCallbackArg's
# `throw Error("no recorded response")` (`replay-callback-arg.cc:101`);
# dispatchAmbientQuery catches (std::exception) and returns
# std::nullopt (walker miss); the walker then falls back cleanly
# to inner. Under `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1` the
# fallback is refused with the specific guardCacheRecording
# error, confirming the miss cascaded cleanly rather than
# producing a wrong result or crash.
#
# TDD regression coverage for task #105.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/pass-attrset.nix" << 'NIX'
{ f }: f { a = 42; b = 99; }
NIX

COLD_EXPR='let r = (builtins.cache { import = '"$TEST_ROOT"'/pass-attrset.nix; }) { f = x: x.a; }; in r'
WARM_DIFF_EXPR='let r = (builtins.cache { import = '"$TEST_ROOT"'/pass-attrset.nix; }) { f = x: x.b; }; in r'

echo "=== cold: f = x: x.a (expect 42) ==="
result=$(nix eval --impure --expr "$COLD_EXPR")
echo "Got: $result"
[[ "$result" == 42 ]]

echo "=== warm ALLOWED, different callback (expect 99 via inner fallback) ==="
# With inner fallback allowed, warm should get 99 (b value) via inner
# re-eval after the obsSet-miss on `.b`. Assertion: result is correct,
# no crash.
result=$(nix eval --impure --expr "$WARM_DIFF_EXPR")
echo "Got: $result"
[[ "$result" == 99 ]]

echo "=== warm DISALLOW, different callback (expect clean miss error) ==="
# Under DISALLOW, the miss cascades to a specific guardCacheRecording
# error. If the miss WEREN'T clean — if ReplayCallbackArg produced a
# wrong response, or if the throw wasn't caught, or if inner was
# activated silently — this assertion would fail differently.
set +e
result=$(_NIX_DISALLOW_CACHE_INTERPRET_INNER=1 nix eval --impure --expr "$WARM_DIFF_EXPR" 2>&1)
exit_code=$?
set -e
echo "Exit: $exit_code"
echo "Output: $result"
[[ $exit_code -ne 0 ]]
echo "$result" | grep -q "recording layer of a .builtins.cache. boundary was asked to"
