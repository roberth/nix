#!/usr/bin/env bash

# A cached call is made twice with the same `f` (which ignores its
# argument) but a different `x`. In the second call `x.inner` is
# `abort "…"`. The cached body never forces `x`, so neither call
# has any reason to touch `x.inner` — cold or warm.
#
# The test teases cache implementations that would reach into the
# outer arg to fingerprint, hash, or otherwise re-inspect it for
# discrimination. Any such reach walks into `x.inner` and trips the
# abort. The correct discipline is: the cache observes what the
# user's expression observed, nothing more.
#
# fn.nix: `{ f, x }: f x`, with `f = _: { whatever = 42; }`.
#   1st eval: x = "safe"           → 42, populates the cache
#   2nd eval: x = { inner = abort } → 42, cache must not force x.inner
#
# If any cache-induced force reaches `x.inner`, the 2nd eval aborts,
# nix exits nonzero, `set -e` fails the test with a stack trace
# naming the violated discipline.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/fn.nix" << 'NIX'
{ f, x }: f x
NIX

echo "=== 1st eval: populate cache (expect 42) ==="
EXPR1="let cached = builtins.cache { import = $TEST_ROOT/fn.nix; }; in (cached { f = _: { whatever = 42; }; x = \"safe\"; }).whatever"
result=$(nix eval --impure --expr "$EXPR1")
echo "Got: $result"
[[ "$result" == 42 ]]

echo "=== 2nd eval: updated x, cache must not force x.inner (expect 42) ==="
EXPR2="let cached = builtins.cache { import = $TEST_ROOT/fn.nix; }; in (cached { f = _: { whatever = 42; }; x = { inner = abort \"cache must not deep-force x\"; }; }).whatever"
result=$(nix eval --impure --expr "$EXPR2")
echo "Got: $result"
[[ "$result" == 42 ]]
