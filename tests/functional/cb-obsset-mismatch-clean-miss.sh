#!/usr/bin/env bash

# Verify clean miss when the outer callback probes an attribute
# not in the recorded obsSet, AND that the fallback path serves
# the correct value when allowed.
#
# Setup: cached body applies the outer's callback f to an
# inner-constructed attrset. Under matching-until-divergence, fn's
# state hash reflects Arg{0} at the cache boundary — same for two
# outers with different callback closures. So a warm session with a
# DIFFERENT callback f may reach cold's callbackApply Q (same
# ref.fn hash), invoke its own live fn on a ReplayCallbackArg
# backed by cold's obsSet, and probe an attr cold didn't record.
#
# Two properties verified independently, each starting from an
# untouched cold cache (so warm's DB state matches exactly what
# cold wrote):
#
# 1. Fallback works: under normal warm, obsSet-miss triggers
#    ReplayCallbackArg's throw, dispatchAmbientQuery catches and
#    returns std::nullopt (walker miss), walker falls back to
#    inner, inner re-eval produces the correct value.
#
# 2. Miss is clean: under
#    `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1` the same setup
#    refuses the fallback with the specific guardCacheRecording
#    error, confirming the miss cascaded cleanly rather than
#    producing a wrong result, crashing, or silently activating
#    inner behind DISALLOW's back.
#
# TDD regression coverage for task #105.

source common.sh

enableFeatures "tracing-eval-cache"

cacheDir="$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
coldBackup="$TEST_ROOT/cold-cache-backup"

clearCache() {
    rm -rf "$cacheDir"
}

primeColdCache() {
    clearCache
    result=$(nix eval --impure --expr "$COLD_EXPR")
    [[ "$result" == 42 ]]
    # snapshot the cache state after cold so each warm run starts
    # from the same untouched DB
    rm -rf "$coldBackup"
    cp -r "$cacheDir" "$coldBackup"
}

restoreCold() {
    clearCache
    cp -r "$coldBackup" "$cacheDir"
}

cat > "$TEST_ROOT/pass-attrset.nix" << 'NIX'
{ f }: f { a = 42; b = 99; }
NIX

COLD_EXPR='let r = (builtins.cache { import = '"$TEST_ROOT"'/pass-attrset.nix; }) { f = x: x.a; }; in r'
WARM_DIFF_EXPR='let r = (builtins.cache { import = '"$TEST_ROOT"'/pass-attrset.nix; }) { f = x: x.b; }; in r'

echo "=== priming cold cache with f = x: x.a ==="
primeColdCache
echo "Cold produced 42; DB snapshot captured."

echo "=== scenario 1: warm with different callback, fallback ALLOWED (expect 99) ==="
# Warm's fn probes .b — not in cold's obsSet {.a}. Expected:
# obsSet-miss triggers ReplayCallbackArg throw → dispatch catches
# → walker miss → inner fallback fires → inner produces 99.
restoreCold
result=$(nix eval --impure --expr "$WARM_DIFF_EXPR")
echo "Got: $result"
[[ "$result" == 99 ]]

echo "=== scenario 2: warm with different callback, fallback DISALLOWED (expect clean miss error) ==="
# Same setup, but with inner fallback refused. Expected: the
# miss cascades to guardCacheRecording throwing the specific
# "boundary was asked to evalFile" error. If the miss weren't
# clean (wrong response silently returned, exception swallowed,
# inner activated without triggering DISALLOW), the assertion
# would fail differently.
restoreCold
set +e
result=$(_NIX_DISALLOW_CACHE_INTERPRET_INNER=1 nix eval --impure --expr "$WARM_DIFF_EXPR" 2>&1)
exit_code=$?
set -e
echo "Exit: $exit_code"
[[ $exit_code -ne 0 ]]
echo "$result" | grep -q "recording layer of a .builtins.cache. boundary was asked to"
echo "Clean miss confirmed."
