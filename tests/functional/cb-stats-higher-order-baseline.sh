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
# Depth-2 facts (= the outer's probes on the inner-supplied lambda)
# now live in AmbientAsks rather than the depth-1 v13FactSet, so the
# warm walk reports 2 fewer hits than before depth-2 landed (= the
# two depth-2 observations the outer made on the local) — those
# observations still validate, just outside the depth-1 hit counter.
echo "=== warm (expect 6 hits, 0 misses, 0 fallbacks) ==="
assertCacheStats 6 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }'

# Result correctness: warm replay returns the same value as cold.
echo "=== warm result correctness (expect 6) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }')
[[ "$result" == 6 ]]
