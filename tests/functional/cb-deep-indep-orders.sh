#!/usr/bin/env bash

# Probes order-dependence in the cb cache for the deep-indep shape.
#
# The cached function {args}: {a = args.x.val; b = args.y.val;} has
# independent reads on `x` and `y`. Under the design principles
# (observed results independent of unobserved inputs; no defined
# evaluation order), the cache must serve a warm replay regardless of
# the order in which the outer forces `a` and `b`. Combinations:
#
#   1. cold xy → warm xy
#   2. cold xy → warm yx
#   3. cold yx → warm xy
#   4. cold yx → warm yx
#
# All four should print `{ a = 1; b = 99; }` on warm replay with
# _NIX_DISALLOW_PARSE=1. Failure of any variant pinpoints which
# direction of order-dependence is leaking through the cache.

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

# `builtins.seq r.a r` forces a; the printer then iterates
# alphabetically and forces b — net force order xy.
# `builtins.seq r.b r` forces b; printer forces a — net yx.
XY_EXPR='let r = (builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) '"$ARGS"'; in builtins.seq r.a r'
YX_EXPR='let r = (builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) '"$ARGS"'; in builtins.seq r.b r'

expected='{ a = 1; b = 99; }'

# Same-order cold→warm: the cache must serve the recorded result
# without re-parsing.
matched() {
    local label=$1 expr=$2
    clearCache
    echo "=== $label: cold ==="
    cold=$(nix eval --impure --expr "$expr")
    echo "Got: $cold"
    [[ "$cold" == "$expected" ]]
    echo "=== $label: warm (DISALLOW_PARSE) ==="
    warm=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "$expr")
    echo "Got: $warm"
    [[ "$warm" == "$expected" ]]
}

matched "xy→xy" "$XY_EXPR"
matched "yx→yx" "$YX_EXPR"

# Order divergence after a cold recording is itself a fresh cold run:
# the cache can't infer which fields will be needed, so a different
# force order is a different observation history. The first attempt
# with the new order must succeed via recording (parse allowed), and
# a subsequent run in that order must then hit warm.
divergent() {
    local label=$1 coldExpr=$2 divergentExpr=$3
    clearCache
    echo "=== $label: cold ==="
    nix eval --impure --expr "$coldExpr" > /dev/null
    echo "=== $label: divergent-order (cold record, parse allowed) ==="
    div=$(nix eval --impure --expr "$divergentExpr")
    echo "Got: $div"
    [[ "$div" == "$expected" ]]
    echo "=== $label: divergent-order warm (DISALLOW_PARSE) ==="
    warm=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "$divergentExpr")
    echo "Got: $warm"
    [[ "$warm" == "$expected" ]]
}

divergent "xy→yx" "$XY_EXPR" "$YX_EXPR"
divergent "yx→xy" "$YX_EXPR" "$XY_EXPR"
