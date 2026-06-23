#!/usr/bin/env bash

# Cb body iterates over a list-valued arg. Two fixtures:
#
#  - list-len.nix:  `{ xs }: builtins.length xs`
#    Tests list-Subject + getListSize observation through the cache.
#
#  - list-sum.nix:  `{ xs }: builtins.foldl' (a: b: a + b) 0 xs`
#    Tests list-Subject + getListSize + per-element getListElem +
#    integer observations through the cache. foldl' is a fundamental
#    list-iteration pattern in Nix.
#
# Each fixture is exercised in three modes:
#  1. Cold record → expected result.
#  2. Warm replay with the same arg under DISALLOW_PARSE → result
#     reproduced from cache, no inner re-parse.
#  3. Warm replay with a *different* list under DISALLOW_PARSE → miss
#     → fallback → DISALLOW_PARSE blocks. The cache must NOT silently
#     return the recorded result for a different list.
#
# Why: list-valued args go through a code path (getListSize +
# getListElem on a list-typed Subject) distinct from attrset args.
# A bug in list-Subject CDI composition would surface as either
# wrong-result-on-warm or cross-list collapse — the DISALLOW_PARSE
# probe in step 3 catches the latter.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

# --- Fixture 1: builtins.length over a list arg ---

clearCache
echo '{ xs }: builtins.length xs' > "$TEST_ROOT/list-len.nix"

echo "=== length [1 2 3] cold ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/list-len.nix; }) { xs = [1 2 3]; }')
[[ "$result" == 3 ]]

echo "=== length [1 2 3] warm DISALLOW_PARSE ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/list-len.nix; }) { xs = [1 2 3]; }')
[[ "$result" == 3 ]]

echo "=== length [10 20] warm DISALLOW_PARSE — must error, not reuse cached 3 ==="
if _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/list-len.nix; }) { xs = [10 20]; }' \
    2>"$TEST_ROOT/err.log"; then
    echo "FAIL: novel-list warm succeeded — cache silently reused [1 2 3]'s length" >&2
    exit 1
fi
grep -q "parsing disallowed by _NIX_DISALLOW_PARSE" "$TEST_ROOT/err.log"

# --- Fixture 2: builtins.foldl' (list iteration with element observation) ---

clearCache
echo "{ xs }: builtins.foldl' (a: b: a + b) 0 xs" > "$TEST_ROOT/list-sum.nix"

echo "=== foldl' + 0 [1 2 3] cold (= 6) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/list-sum.nix; }) { xs = [1 2 3]; }')
[[ "$result" == 6 ]]

echo "=== foldl' + 0 [1 2 3] warm DISALLOW_PARSE ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/list-sum.nix; }) { xs = [1 2 3]; }')
[[ "$result" == 6 ]]

echo "=== foldl' + 0 [10 20] warm DISALLOW_PARSE — must error, not reuse 6 ==="
if _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/list-sum.nix; }) { xs = [10 20]; }' \
    2>"$TEST_ROOT/err.log"; then
    echo "FAIL: novel-list warm succeeded — cache silently reused [1 2 3]'s sum" >&2
    exit 1
fi
grep -q "parsing disallowed by _NIX_DISALLOW_PARSE" "$TEST_ROOT/err.log"

# Sanity: novel list under fallback (no DISALLOW_PARSE) produces correct value.
echo "=== foldl' + 0 [10 20] live fallback (= 30) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/list-sum.nix; }) { xs = [10 20]; }')
[[ "$result" == 30 ]]
