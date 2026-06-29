#!/usr/bin/env bash

# Stress test for the pre-flush-substitution fix (#49): nested
# higher-order callbacks where the cached body constructs a function
# that itself receives an inner-constructed function.
#
# `{ apply2 }: apply2 (innerLambda: innerLambda 5)` — cb body passes a
# 1-arg function to apply2. apply2 (outer-supplied) chooses what to
# pass to that 1-arg function.
#
# Outer: `apply2 = midfn: midfn (n: n + 100)`. midfn receives
# (n: n + 100), applies that lambda to 5 → 105.
#
# This routes through TWO apply Q's in the inner trace: the outer→
# inner apply for `apply2 (innerLambda: ...)`, and the second-level
# apply when innerLambda is invoked. Each should get its old→new hash
# substitution persisted, otherwise late-flushed observation facts
# whose `from` references either apply's placeholder will fail to
# substitute and replay will serve stale.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/nested-ho.nix" << 'NIX'
{ apply2 }: apply2 (innerLambda: innerLambda 5)
NIX

# Cold paths now work — fixed by routing seed-self queryApply through
# `applyOn` (= using captured outerArgObj directly instead of
# `resolveOuter(rootId)`, which boundary discipline keeps
# unregistered).
echo "=== cold: apply2 = midfn: midfn (n: n+100) (expect 105) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested-ho.nix; }) { apply2 = midfn: midfn (n: n + 100); }')
echo "Got: $result"
[[ "$result" == 105 ]]

echo "=== replay (expect 105) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested-ho.nix; }) { apply2 = midfn: midfn (n: n + 100); }')
echo "Got: $result"
[[ "$result" == 105 ]]

echo "=== outer change to (n + 200) (expect 205) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested-ho.nix; }) { apply2 = midfn: midfn (n: n + 200); }')
echo "Got: $result"
[[ "$result" == 205 ]]

echo "=== restore (expect 105) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested-ho.nix; }) { apply2 = midfn: midfn (n: n + 100); }')
echo "Got: $result"
[[ "$result" == 105 ]]
