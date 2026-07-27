#!/usr/bin/env bash

# Principle #8 — cache behavior is independent of the argument's
# forcedness state. Identity is derived only from observations the
# function body makes through the value, never from observations the
# interpreter made incidentally. A refactor that changes evaluation
# order (= a `seq` upstream of the cb call, an eagerly-evaluated
# adjacent primop) must NOT perturb cache layout.
#
# Test plan: the cb body is `{ f, x }: f x` with f = (n: n + 100) and
# x = 13. Variant A passes the attrset plainly (= fields arrive as
# unforced thunks at the cb-apply boundary). Variant B forces x via
# `builtins.seq` in the *caller's* scope before the cb call. Both
# variants must produce identical cache stats (cold and warm) and
# identical results.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

echo '{ f, x }: f x' > "$TEST_ROOT/call.nix"

# --- Variant A: arg fields arrive unforced ---

clearCache

echo "=== variant A cold (= unforced arg) ==="
assertCacheStats 0 2 1 -- \
    nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) { f = n: n + 100; x = 13; }'

echo "=== variant A warm ==="
assertCacheStats 2 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) { f = n: n + 100; x = 13; }'

resultA=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) { f = n: n + 100; x = 13; }')
[[ "$resultA" == 113 ]]

# --- Variant B: caller pre-forces arg.x via seq ---
#
# `seq` is the canonical "interpreter forces this incidentally"
# operation: it evaluates its first argument to WHNF for its side
# effect, then yields the second. After the seq, `arg.x` (shared
# through `let arg = …`) is already an int rather than a thunk when
# the cb-apply boundary sees it.

clearCache

echo "=== variant B cold (= pre-forced arg via seq) ==="
assertCacheStats 0 2 1 -- \
    nix eval --impure --expr \
        'let arg = { f = n: n + 100; x = 13; };
         in builtins.seq arg.x ((builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) arg)'

echo "=== variant B warm ==="
assertCacheStats 2 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
        'let arg = { f = n: n + 100; x = 13; };
         in builtins.seq arg.x ((builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) arg)'

resultB=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    'let arg = { f = n: n + 100; x = 13; };
     in builtins.seq arg.x ((builtins.cache { import = '"$TEST_ROOT"'/call.nix; }) arg)')
[[ "$resultB" == 113 ]]
