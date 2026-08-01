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
echo "=== cold (no hits, but misses, fallbacks) ==="
assertCacheStats 0 3 2 -- \
    nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }'

# Warm replay. Every Q dispatched live; everything hits.
# 2 hits (post-#217): dropping the fnIsTlo placeholder Terminal removed
# a spurious hit — the layer-2 SelectorApply Terminal isn't consulted
# any more; warm routes through the SelectorCallbackApply dispatch,
# which produces one applyResult look-up per apply instead of two.
echo "=== warm (hits, no misses, no fallbacks) ==="
assertCacheStats 2 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }'

# Result correctness: warm replay returns the same value as cold.
echo "=== warm result correctness (expect 6) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }')
[[ "$result" == 6 ]]
