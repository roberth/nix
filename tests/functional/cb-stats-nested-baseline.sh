#!/usr/bin/env bash

# CDI #5: hit-rate regression baseline for nested-cache.
#
# Outer cache imports a file that itself imports a cache. The cb in the
# outer calls into the inner. Locks in the exact (hits, misses,
# fallbacks) for cold and warm. Catches silent CDI regressions where a
# future change shifts the trie shape without breaking output.
#
# Calibrated against current behaviour. Re-calibrate the counts if a
# correct change shifts them — but the act of re-calibrating forces
# explicit acknowledgement.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

echo '{ f = x: x * 10; base = 1; }' > "$TEST_ROOT/inner-mod.nix"
cat > "$TEST_ROOT/outer-mod.nix" <<OUTER
let inner = builtins.cache { import = $TEST_ROOT/inner-mod.nix; };
in inner.f inner.base + inner.f 2
OUTER

echo "=== cold (no hits, but misses, fallbacks) ==="
assertCacheStats 0 4 2 -- \
    nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }'

echo "=== warm DISALLOW_PARSE (hits, no misses, no fallbacks) ==="
assertCacheStats 1 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }'
