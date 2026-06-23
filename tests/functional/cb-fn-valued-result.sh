#!/usr/bin/env bash

# Cb body returns a function value: `{ x }: (n: n + x)`. The outer
# then applies that returned function. Three behaviors must hold:
#
#  1. Cold path produces the correct value (= 15 for x=10, n=5).
#  2. Warm replay with the same outer-applied arg (= n=5) hits the
#     cache and reproduces 15 without re-parsing the cb source. The
#     `apply` observation on the returned lambda was recorded as a
#     depth-1 fact during cold; warm walker replays it.
#  3. Warm replay with a *novel* outer-applied arg (= n=7, never
#     recorded) is a cache miss that falls back to live re-eval.
#     Under DISALLOW_PARSE the fallback must surface as a clean
#     parsing-disallowed error — not a stale `15` or a crash. This is
#     the depth-1 walker correctly bailing on an unrecorded apply.
#
# Why it matters: function-valued cb results are common in real Nix
# code (config builders, currying helpers). If the cache silently
# returned the recorded `15` for any apply on the returned lambda, it
# would be unsound. The DISALLOW_PARSE probe is the cleanest way to
# distinguish "cache hit returned the right value" from "live re-eval
# happened to produce the right value."

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

echo '{ x }: (n: n + x)' > "$TEST_ROOT/fn.nix"

# (1) cold record with x=10, n=5 → 15
echo "=== cold (x=10, n=5) ==="
result=$(nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 10; }) 5')
[[ "$result" == 15 ]]

# (2) warm hit with same outer arg
echo "=== warm hit (n=5, DISALLOW_PARSE) — must return 15 from cache ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 10; }) 5')
[[ "$result" == 15 ]]

# (3) warm miss with novel outer arg → fallback → DISALLOW_PARSE error
echo "=== warm miss (n=7, DISALLOW_PARSE) — must error, NOT return stale 15 ==="
if _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 10; }) 7' \
    2>"$TEST_ROOT/err.log"; then
    echo "FAIL: warm with novel arg succeeded — cache returned a value for an unrecorded apply" >&2
    exit 1
fi
grep -q "parsing disallowed by _NIX_DISALLOW_PARSE" "$TEST_ROOT/err.log" || {
    echo "FAIL: expected DISALLOW_PARSE error, got:" >&2
    cat "$TEST_ROOT/err.log" >&2
    exit 1
}

# (4) sanity: without DISALLOW_PARSE the novel arg works via fallback
echo "=== warm miss (n=7, no DISALLOW_PARSE) — fallback re-eval returns 17 ==="
result=$(nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 10; }) 7')
[[ "$result" == 17 ]]
