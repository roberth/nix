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
# Hit count progression:
#   6 (original) → 8 (multi-root apply-path observations land at
#   d1) → 12 (per-Q Asks edges land — each Q's recorded chain has
#   one Asks edge per writer logResult, principles 3/5/7, so the
#   walker visits multiple Asks edges per Q's lookup instead of one
#   whole-remaining edge) → 4 (lambda-primop firing: per the design
#   memo's Change C, the apply-result observations on the standin's
#   synthetic — getType / getInt on the recursive cb-apply's result
#   — now read from LocalResponseMap as d=2 lookups, which are not
#   counted as v13Walk hits. The remaining 4 hits are the outer
#   evalFile/import Qs plus the outer expression's d=0 getType /
#   getInt at the top level).
echo "=== warm (expect 3 hits, 0 misses, 0 fallbacks) ==="
assertCacheStats 3 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }'

# Result correctness: warm replay returns the same value as cold.
echo "=== warm result correctness (expect 6) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/ho.nix; }) { f = g: g 5; }')
[[ "$result" == 6 ]]
