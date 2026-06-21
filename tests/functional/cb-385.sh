#!/usr/bin/env bash

source common.sh

enableFeatures "tracing-eval-cache"

# Self-traced repro: each nix invocation prints CACHE_TRACE-guarded
# diagnostics in libexpr. Unset CACHE_TRACE here to silence.
export CACHE_TRACE=1

cacheDir="$TEST_HOME/.cache/nix/eval-tracing-decision-graph"

clearCache() {
    rm -rf "$cacheDir"
}

clearCache

# --- Nested builtins.cache with function calls ---

echo '{ f = x: x * 10; base = 1; }' > "$TEST_ROOT/inner-mod.nix"

cat > "$TEST_ROOT/outer-mod.nix" <<OUTER
let inner = builtins.cache { import = $TEST_ROOT/inner-mod.nix; };
in inner.f inner.base + inner.f 2
OUTER

echo "=== Cold (expect 30) ==="
result=$(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }')
echo "Got: $result"
[[ "$result" == 30 ]]

echo "=== Warm same (expect 30) ==="
result=$(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }')
echo "Got: $result"
[[ "$result" == 30 ]]

sleep 1
echo '{ f = x: x * 100; base = 1; }' > "$TEST_ROOT/inner-mod.nix"

echo "=== Warm with changed inner-mod (expect 300) ==="
result=$(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }')
echo "Got: $result"
[[ "$result" == 300 ]]

# Mirror lines 388-402 of builtins-cache.sh
sleep 1
echo '{ f = x: x * 100; base = 5; }' > "$TEST_ROOT/inner-mod.nix"

echo "=== inner-mod base changed (expect 700) ==="
result=$(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }')
echo "Got: $result"
[[ "$result" == 700 ]]

sleep 1
cat > "$TEST_ROOT/outer-mod.nix" <<OUTER
let inner = builtins.cache { import = $TEST_ROOT/inner-mod.nix; };
in inner.f inner.base
OUTER

echo "=== outer-mod simplified (expect 500) ==="
result=$(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }')
echo "Got: $result"
[[ "$result" == 500 ]]
