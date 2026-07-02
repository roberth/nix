#!/usr/bin/env bash

# Repeated cb-apply with DIFFERENT arguments — a real-world pattern
# (map cb [x y z], callPackageWith, self-referential attrsets, etc.)
# that the existing suite doesn't exercise.
#
# `cb-xor-evolution-repeated-cb-apply.sh` covers the same-args case
# `(f x) + (f x)` and passes today. This test extends coverage to
# different-args, chained (reentrant into own prior results), and
# attrset-shaped-return variants.
#
# All variants:
#   1. Cold: expected value. ✓
#   2. Warm no-change, `_NIX_DISALLOW_PARSE=1`: expected value from
#      cache without fall-through to inner re-parse. Failing this
#      means the cache silently falls through and inner re-parse
#      would have been needed — mask the gap under normal mode but
#      surface it under DISALLOW.
#   3. Warm with changed callback, no DISALLOW: expected NEW value
#      (fall-through works correctly for the semantic-change case).
#
# The value assertions under DISALLOW_PARSE are what distinguish
# "warm hit" from "warm fall-through". Without them the standard
# `nix eval` value check accepts both — hiding hit-rate regressions.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

# --- Variant 1: two cb-applies with different literal args, scalar sum ---

clearCache
echo '{ cb }: (cb 10) + (cb 20)' > "$TEST_ROOT/sum2.nix"

echo "=== variant 1: (cb 10) + (cb 20), cb = x: x + 1, cold — expect 32 ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/sum2.nix; }) { cb = x: x + 1; }')
[[ "$result" == 32 ]]

echo "=== variant 1: warm no-change DISALLOW_PARSE — expect 32 from cache ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/sum2.nix; }) { cb = x: x + 1; }')
[[ "$result" == 32 ]]

echo "=== variant 1: warm changed cb, no DISALLOW — expect 60 (cb = x*2) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/sum2.nix; }) { cb = x: x * 2; }')
[[ "$result" == 60 ]]

# --- Variant 2: three cb-applies, attrset return ---

clearCache
echo '{ cb }: { a = cb 10; b = cb 20; c = cb 30; }' > "$TEST_ROOT/attrs3.nix"

echo "=== variant 2: attrset { a=cb 10; b=cb 20; c=cb 30 }, cold — expect { 11 21 31 } ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/attrs3.nix; }) { cb = x: x + 1; }')
[[ "$result" == '{ a = 11; b = 21; c = 31; }' ]]

echo "=== variant 2: warm no-change DISALLOW_PARSE, force each attr — all from cache ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/attrs3.nix; }) { cb = x: x + 1; }')
[[ "$result" == '{ a = 11; b = 21; c = 31; }' ]]

# --- Variant 3: reentrant — later cb calls receive earlier cb results ---

clearCache
echo '{ cb }: let a = cb 10; b = cb a; c = cb b; in { a = a; b = b; c = c; }' > "$TEST_ROOT/chain.nix"

echo "=== variant 3: reentrant chain cb 10 → cb a → cb b, cold — expect { 11 12 13 } ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/chain.nix; }) { cb = x: x + 1; }')
[[ "$result" == '{ a = 11; b = 12; c = 13; }' ]]

echo "=== variant 3: warm no-change DISALLOW_PARSE — all three from cache ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/chain.nix; }) { cb = x: x + 1; }')
[[ "$result" == '{ a = 11; b = 12; c = 13; }' ]]

# --- Variant 4: map cb over a list (idiomatic pattern) ---

clearCache
echo '{ cb, xs }: map cb xs' > "$TEST_ROOT/mapcb.nix"

echo "=== variant 4: map cb [1 2 3 4 5], cold — expect [2 3 4 5 6] ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/mapcb.nix; }) { cb = x: x + 1; xs = [ 1 2 3 4 5 ]; }')
[[ "$result" == '[ 2 3 4 5 6 ]' ]]

echo "=== variant 4: warm no-change DISALLOW_PARSE — full list from cache ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/mapcb.nix; }) { cb = x: x + 1; xs = [ 1 2 3 4 5 ]; }')
[[ "$result" == '[ 2 3 4 5 6 ]' ]]
