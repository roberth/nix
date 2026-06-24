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
# Hit count went from 6 to 8 after task #87 landed: observations on
# apply-result descendants now also produce depth-1 facts (= path
# carries an Apply step plus the trailing GetAttr) that the warm walk
# dispatches and counts. The two extra hits are the apply-result's
# getType and getInt — the cost of routing apply-result observations
# back through the per-arg root for sibling discrimination.
echo "=== warm (expect 8 hits, 0 misses, 0 fallbacks) ==="
assertCacheStats 8 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }'

# Result correctness: warm replay returns the same value as cold.
echo "=== warm result correctness (expect 6) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }')
[[ "$result" == 6 ]]
