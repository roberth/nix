#!/usr/bin/env bash

# CDI #8: preFlushSubstitutions overwrite invariant.
#
# Two sibling cb invocations in one process whose deferred apply Qs
# share an oldHash (because args' initial CDI is the same empty-cell
# hash) trigger the invariant. The warning fires loudly via
# tracingCacheLog when _NIX_TRACING_CACHE_LOGGING=1; this test asserts
# it's visible.
#
# When #63 is fixed (so the collision stops happening at all), this
# test should be re-evaluated: either the warning should escalate to a
# throw and this test become a negative-assertion ("no collision
# warning"), or the warning text changes.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

echo '{ f, x }: f x' > "$TEST_ROOT/call-fn.nix"

# Trigger the collision scenario from builtins-cache.sh's call-fn
# pattern. Multiple `nix eval` calls each record a distinct trace into
# the shared on-disk decisionGraph. The third invocation's warm walker
# falls through to inner re-evaluation, which defers an apply Q whose
# substitution collides with an apply-Q substitution already established
# in the same process by an earlier flush cycle (likely a walker
# dispatch that itself invoked queryApply).
echo "=== priming: invocation 1 records (no collision) ==="
collisions=$(cacheStatsField pre_flush_substitution_collisions -- \
    nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/call-fn.nix; }) { f = x: x + 1; x = 10; }')
[[ "$collisions" == 0 ]]

echo "=== priming: invocation 2 records different f (no collision) ==="
collisions=$(cacheStatsField pre_flush_substitution_collisions -- \
    nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/call-fn.nix; }) { f = x: x + 100; x = 10; }')
[[ "$collisions" == 0 ]]

echo "=== invocation 3 with new x — collision counter must be > 0 ==="
collisions=$(cacheStatsField pre_flush_substitution_collisions -- \
    nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/call-fn.nix; }) { f = x: x + 1; x = 50; }')
echo "Collisions: $collisions"
[[ "$collisions" -gt 0 ]]
