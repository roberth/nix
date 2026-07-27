#!/usr/bin/env bash

# CDI #6: Pass-2 sidecar persistence baseline.
#
# `{ f, x }: f x` cached. cb body invokes the outer-supplied f with
# the outer-supplied x (a covariant callback). AmbientResolver::apply
# defers BOTH a Pass-1 apply Q request AND a Pass-2 sidecar
# (localArg with keyPlaceholder = the local's placeholder hex).
#
# #49's fix persists Pass 1 and Pass 3 substitutions across flush
# cycles. Pass 2 sidecar substitution is NOT persisted (it inserts at
# its substituted key directly). If a future scenario emerges where a
# sidecar's key needs to survive across flushes — e.g. a fact in a
# later cycle whose `from` is the local's INITIAL placeholder and the
# local hasn't been re-observed in that cycle — this baseline would
# shift and prompt extending persistence to sidecars too.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

echo '{ f, x }: f x' > "$TEST_ROOT/call.nix"

echo "=== cold (expect 0 hits, 3 misses, 1 fallback, 0 collisions) ==="
assertCacheStats 0 2 1 -- \
    nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) { f = x: x + 100; x = 7; }'

echo "=== warm replay (expect 3 hits, 0 misses, 0 fallbacks) ==="
assertCacheStats 2 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) { f = x: x + 100; x = 7; }'

# Sanity: result is correct (= 107) and no collisions.
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) { f = x: x + 100; x = 7; }')
[[ "$result" == 107 ]]
