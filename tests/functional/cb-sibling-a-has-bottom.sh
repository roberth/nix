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
# The invariant we assert here is orthogonal to whether warm
# actually produces the right value. Warm may currently gate on
# DISALLOW_CACHE_INTERPRET_INNER (= same failure mode as
# cb-sibling-discrimination-via-observation, which owns *that*
# assertion). What this test uniquely asserts is: no cache-induced
# force of `x.inner` occurs. If any discrimination scheme reaches
# for `x.inner` (to hash it, to compute a per-sibling id, to
# normalise recording context — any reason), the abort fires with
# our specific message and this test fails loud, naming the
# violated discipline.
#
# Passes today (abort doesn't fire; the gate does). Will keep
# passing when the discrimination fix lands *if* the fix stays
# lazy on `x`. Fails hard if any implementation deep-forces.

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

# Warm: the only invariant this test asserts is that the abort is
# NOT triggered. It does not assert warm produces 85 — that's the
# concern of cb-sibling-discrimination-via-observation, which fails
# for a separate reason today (missing discrimination). This test
# stays green as long as no cache-induced force reaches `x.inner`,
# whether warm succeeds cleanly, gates on
# DISALLOW_CACHE_INTERPRET_INNER, or produces any other error not
# involving the abort message.
echo "=== warm DISALLOW_CACHE_INTERPRET_INNER (abort must not fire) ==="
result=$(_NIX_DISALLOW_CACHE_INTERPRET_INNER=1 nix eval --impure --expr "$EXPR" 2>&1 || true)
echo "Got: $result"
if [[ "$result" == *"cache must not deep-force x"* ]]; then
    echo "FAIL: cache-induced force of x.inner triggered the abort"
    exit 1
fi
