#!/usr/bin/env bash

# Two real-world cb body patterns that previously had no coverage:
#
#  1. `with arg; x + y`. Brings the arg's scope into the body via
#     `with`. Tests that the cache observes the body's actual probes
#     (= getAttr "x" then getAttr "y") rather than treating `with`
#     specially. Cross-value soundness: novel input must not collapse
#     onto a recorded one.
#
#  2. `(builtins.tryEval (f 0)).success`. Inner apply runs inside
#     tryEval, which catches throws. Tests that a recorded cb result
#     of `true` for a non-throwing `f` warm-replays correctly. We do
#     NOT cross-test with a throwing `f` here — that exercises the
#     function-equality wart (= cache treats lambdas as opaque) which
#     is explicitly out-of-scope per the design.
#
# Both fixtures cover the same skeleton: cold → warm hit (DISALLOW_PARSE)
# → if applicable, warm with a novel value (= structural change in
# observable scalars, not functions) → DISALLOW_PARSE error.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

# --- Fixture 1: `with arg` scope ---

clearCache
echo 'arg: with arg; x + y' > "$TEST_ROOT/with-fn.nix"

echo "=== with arg cold: { x=3; y=4 } → 7 ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/with-fn.nix; }) { x = 3; y = 4; }')
[[ "$result" == 7 ]]

echo "=== warm hit DISALLOW_PARSE — must replay 7 ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/with-fn.nix; }) { x = 3; y = 4; }')
[[ "$result" == 7 ]]

echo "=== warm DISALLOW_PARSE with novel scalars { x=10; y=20 } — must error, not reuse 7 ==="
if _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/with-fn.nix; }) { x = 10; y = 20; }' \
    2>"$TEST_ROOT/err.log"; then
    echo "FAIL: novel-scalar warm succeeded — cache reused 7" >&2
    exit 1
fi
grep -q "parsing disallowed by _NIX_DISALLOW_PARSE" "$TEST_ROOT/err.log"

# --- Fixture 2: tryEval around an inner apply (non-throwing) ---

clearCache
echo '{ f }: (builtins.tryEval (f 0)).success' > "$TEST_ROOT/tryeval.nix"

echo "=== tryEval cold: f = (n: n+1), apply succeeds → true ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/tryeval.nix; }) { f = n: n + 1; }')
[[ "$result" == true ]]

echo "=== tryEval warm DISALLOW_PARSE — must replay true ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/tryeval.nix; }) { f = n: n + 1; }')
[[ "$result" == true ]]
