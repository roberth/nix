#!/usr/bin/env bash

# Outer's callback body returns a *lazy attrset* whose children each
# reference the contra-arg through a distinct thunk. The outer then
# forces multiple children separately (via `r.rr + r.ss + r.tt`),
# so each forcing triggers a fresh fn firing at replay with its own
# ambient probe on the contra-arg.
#
# Body: `{ f }: f { a = 42; b = 99; c = 100; }`
# Outer: `f = x: { rr = x.a; ss = x.b; tt = x.c; }`
# Consumer: `let r = ... in r.rr + r.ss + r.tt`
#
# Unlike cb-local-descendants and cb-with-scope-and-tryeval, which
# do multi-attr access on the contra-arg but within a single eager
# body evaluation (one fn firing at replay), this test exercises
# distinct outer probes on the applyResult that each drive a
# separate fresh fn firing at replay. Under a design that stamps
# contra-arg ambient probes with progressive walkFacts, cold's
# cumulative-history probe queryHashes wouldn't match warm's
# fresh-firing-per-outer-probe queryHashes and the obsSet lookup
# would miss. The contra-arg has no state hash to evolve — its
# ambient probes must be stamped at the invariant structural id
# (Arg{depth}'s `positional-<depth>` XOR argAncestry).
#
# Correct answer: 42 + 99 + 100 = 241 on both cold and warm.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/lazy-result.nix" << 'NIX'
{ f }: f { a = 42; b = 99; c = 100; }
NIX

EXPR='let r = (builtins.cache { import = '"$TEST_ROOT"'/lazy-result.nix; }) { f = x: { rr = x.a; ss = x.b; tt = x.c; }; }; in r.rr + r.ss + r.tt'

echo "=== cold (expect 241) ==="
result=$(nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 241 ]]

echo "=== warm DISALLOW_PARSE (expect 241) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "$EXPR")
echo "Got: $result"
[[ "$result" == 241 ]]
