#!/usr/bin/env bash

# CDI #9: higher-order callback warm-replay hit-rate baseline.
#
# `{ f }: f (x: x + 1)` cached, outer `{ f = g: g 5; }`. Warm replay
# exercises the path where the walker resolves the apply Q for
# f(innerLambda) — `bridgedLocals` memoization (AmbientResolver::apply
# line ~230) keys on argObj.get() pointer, and fresh standins per
# walker dispatch never share argThunk. If a future change either fixes
# the memoization to key on argId (lower thunk creation, possibly fewer
# missed hits) or introduces additional dispatch passes, hit count will
# shift and this baseline trips.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

echo '{ f }: f (x: x + 1)' > "$TEST_ROOT/ho.nix"

# Cold record.
echo "=== cold (expect 0 hits, 5 misses, 2 fallbacks) ==="
assertCacheStats 0 5 2 -- \
    nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }'

# Warm replay. Every Q dispatched live; everything hits.
echo "=== warm (expect 8 hits, 0 misses, 0 fallbacks) ==="
assertCacheStats 8 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }'

# TODO(depth-2): the `pre_flush_substitution_collisions` metric was
# removed when the substitution machinery was stripped (the via-Asks
# design replaces it). The check below asserts the metric reads 0,
# but `cacheStatsField` now returns the JSON string "null" for the
# missing field and `[[ null == 0 ]]` fails. Restore a meaningful
# equivalent invariant when the depth-2 walker lands.
# collisions=$(cacheStatsField pre_flush_substitution_collisions -- \
#     nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }')
# [[ "$collisions" == 0 ]]
