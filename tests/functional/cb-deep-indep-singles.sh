#!/usr/bin/env bash

# Probes that two independent single-attr warmups for the same cached
# value compose into a combined warm hit.
#
# Cached fn: `{args}: {a = args.x.val; b = args.y.val;}` — observations
# on `x` and on `y` are structurally independent. Set commutativity in
# the factSet means the order in which the two warmups were recorded
# must not matter: `{[x], [y]}` and `{[y], [x]}` produce the same trie
# state. A subsequent warm replay that forces BOTH `.a` and `.b` must
# hit (each child Q has its own Asks chain rooted at the apply's
# anchor; live env can answer both).
#
# Today this fails: warm xy and warm yx miss on the second-forced
# attr. The diagnostic is the asymmetry between matched-order
# (which hits) and divergent-order (which misses).

source common.sh

enableFeatures "tracing-eval-cache"

cacheDir="$TEST_HOME/.cache/nix/eval-tracing-decision-graph"

clearCache() {
    rm -rf "$cacheDir"
}

cat > "$TEST_ROOT/deep-indep.nix" << 'NIX'
{ args }:
{ a = args.x.val; b = args.y.val; }
NIX

ARGS='{ args = { x = { val = 1; }; y = { val = 99; }; }; }'
EXPR_PFX='let r = (builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) '"$ARGS"
ONLY_A_EXPR="$EXPR_PFX; in r.a"
ONLY_B_EXPR="$EXPR_PFX; in r.b"
XY_EXPR="$EXPR_PFX; in builtins.seq r.a r"
YX_EXPR="$EXPR_PFX; in builtins.seq r.b r"

expectedAB='{ a = 1; b = 99; }'

# Run a cold warmup that forces a single attr, then a combined warm
# replay (DISALLOW_PARSE) and check it produced the full result.
probe() {
    local label=$1 warmupA=$2 warmupB=$3 combined=$4
    clearCache
    echo "=== $label: warmup A ==="
    a=$(nix eval --impure --expr "$warmupA")
    echo "Got: $a"
    [[ "$a" == "1" || "$a" == "99" ]]
    echo "=== $label: warmup B ==="
    b=$(nix eval --impure --expr "$warmupB")
    echo "Got: $b"
    [[ "$b" == "1" || "$b" == "99" ]]
    echo "=== $label: combined warm (DISALLOW_PARSE) ==="
    c=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "$combined")
    echo "Got: $c"
    [[ "$c" == "$expectedAB" ]]
}

probe "{[a],[b]} -> xy"  "$ONLY_A_EXPR"  "$ONLY_B_EXPR"  "$XY_EXPR"
probe "{[a],[b]} -> yx"  "$ONLY_A_EXPR"  "$ONLY_B_EXPR"  "$YX_EXPR"
probe "{[b],[a]} -> xy"  "$ONLY_B_EXPR"  "$ONLY_A_EXPR"  "$XY_EXPR"
probe "{[b],[a]} -> yx"  "$ONLY_B_EXPR"  "$ONLY_A_EXPR"  "$YX_EXPR"
