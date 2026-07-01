#!/usr/bin/env bash

# Sibling A carries a value that would abort *if forced*, placed
# where a cache mechanism might plausibly reach for it in the name of
# discrimination — one level below the top of the cb-arg — and the
# cached body never forces it. Cold succeeds because the body doesn't
# touch that value; warm must succeed the same way, without any
# cache-induced force.
#
# The test teases cache implementations that try to fingerprint or
# hash the outer arg tree to disambiguate siblings. Any such
# implementation would walk into `x.inner` and trip the abort. The
# correct discipline is: the cache observes what the user's expression
# observes, nothing more — even in service of making warm's Q
# computation converge with cold's recording site.
#
# Cached body: `{ f, x }: f x`. `f` ignores its argument, so `x` is
# never forced by the body — `x.inner` doubly so.
#   sibling A: f = _: { whatever = 42; },
#              x = { inner = abort "cache must not deep-force x"; }
#     → a.whatever = 42
#   sibling B: f = _: { whatever = 43; },
#              x = { inner = "safe"; }
#     → b.whatever = 43
# Total: 85.
#
# If any discrimination scheme reaches into `x.inner` — to hash it,
# to compute a per-sibling id, to normalise recording context, any
# reason — the abort fires, nix exits nonzero, `set -e` fails the
# test. The abort message names the violated discipline.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/fn.nix" << 'NIX'
{ f, x }: f x
NIX

EXPR="let cached = builtins.cache { import = $TEST_ROOT/fn.nix; }; in (cached { f = _: { whatever = 42; }; x = { inner = abort \"cache must not deep-force x\"; }; }).whatever + (cached { f = _: { whatever = 43; }; x = { inner = \"safe\"; }; }).whatever"

echo "=== cold (expect 85) ==="
result=$(nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 85 ]]

echo "=== warm (expect 85) ==="
result=$(nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 85 ]]
