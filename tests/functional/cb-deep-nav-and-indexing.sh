#!/usr/bin/env bash

# Two cb body shapes that exercise Subject-derivation chains the
# other tests don't cover:
#
#  1. `{ a }: a.b.c` — three-level attrset navigation through the cb
#     arg. Each step constructs a DerivedSubject{GetAttr}; the chain
#     `a → b → c` must round-trip correctly via the cache.
#
#  2. `{ xs, i }: builtins.elemAt xs i` — list indexing with an
#     index that's *also* a cb-arg field. Tests that the walker
#     discriminates on the integer scalar `i` while the list `xs` is
#     observed identically across runs. This is the canonical
#     multi-arg-axis discrimination case.
#
# Both: cold → warm hit (DISALLOW_PARSE) → warm with a single scalar
# changed (the deepest leaf in case 1, just the index in case 2)
# under DISALLOW_PARSE → must error, not reuse the recorded value.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

# --- Fixture 1: three-level attrset navigation ---

clearCache
echo '{ a }: a.b.c' > "$TEST_ROOT/deep-nav.nix"

echo "=== deep nav cold: { a = { b = { c = 42; }; }; } → 42 ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/deep-nav.nix; }) { a = { b = { c = 42; }; }; }')
[[ "$result" == 42 ]]

echo "=== deep nav warm DISALLOW_PARSE ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/deep-nav.nix; }) { a = { b = { c = 42; }; }; }')
[[ "$result" == 42 ]]

echo "=== deep nav warm DISALLOW_PARSE with c=99 — must error, not reuse 42 ==="
if _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/deep-nav.nix; }) { a = { b = { c = 99; }; }; }' \
    2>"$TEST_ROOT/err.log"; then
    echo "FAIL: deep-leaf change went unobserved — cache reused 42" >&2
    exit 1
fi
grep -q "parsing disallowed by _NIX_DISALLOW_PARSE" "$TEST_ROOT/err.log"

# --- Fixture 2: list indexing with cb-arg index ---

clearCache
echo '{ xs, i }: builtins.elemAt xs i' > "$TEST_ROOT/elemat.nix"

echo "=== elemAt cold: xs=[10 20 30], i=1 → 20 ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/elemat.nix; }) { xs = [10 20 30]; i = 1; }')
[[ "$result" == 20 ]]

echo "=== elemAt warm DISALLOW_PARSE ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/elemat.nix; }) { xs = [10 20 30]; i = 1; }')
[[ "$result" == 20 ]]

echo "=== elemAt warm DISALLOW_PARSE with i=2 — must error, not reuse 20 ==="
if _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/elemat.nix; }) { xs = [10 20 30]; i = 2; }' \
    2>"$TEST_ROOT/err.log"; then
    echo "FAIL: index-axis change went unobserved — cache reused 20" >&2
    exit 1
fi
grep -q "parsing disallowed by _NIX_DISALLOW_PARSE" "$TEST_ROOT/err.log"

# Sanity: fallback re-eval with i=2 yields 30.
echo "=== elemAt fallback re-eval i=2 (= 30) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/elemat.nix; }) { xs = [10 20 30]; i = 2; }')
[[ "$result" == 30 ]]
