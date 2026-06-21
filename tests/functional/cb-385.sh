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

# --- deep-indep: mirrors builtins-cache.sh lines 586-610 ---
clearCache

cat > "$TEST_ROOT/deep-indep.nix" << 'NIX'
{ args }:
{ a = args.x.val; b = args.y.val; }
NIX

echo "=== deep-indep test 1: a=1 b=2 (record) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 1; }; y = { val = 2; }; }; }')
echo "Got: $result"
[[ "$result" == '{ a = 1; b = 2; }' ]]

echo "=== deep-indep test 2: replay (expect {a=1;b=2}) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 1; }; y = { val = 2; }; }; }')
echo "Got: $result"
[[ "$result" == '{ a = 1; b = 2; }' ]]

echo "=== deep-indep test 3: change y to 99 (expect {a=1;b=99}) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 1; }; y = { val = 99; }; }; }')
echo "Got: $result"
[[ "$result" == '{ a = 1; b = 99; }' ]]

echo "=== deep-indep test 4: replay (expect {a=1;b=99}) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 1; }; y = { val = 99; }; }; }')
echo "Got: $result"
[[ "$result" == '{ a = 1; b = 99; }' ]]

echo "=== deep-indep test 5: change x to 77 (expect {a=77;b=99}) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 77; }; y = { val = 99; }; }; }')
echo "Got: $result"
[[ "$result" == '{ a = 77; b = 99; }' ]]
