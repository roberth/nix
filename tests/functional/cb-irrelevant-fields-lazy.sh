#!/usr/bin/env bash

# Principle #7 (= laziness end-to-end) and principle #6 (= no deep
# hashing of values), combined.
#
# Body: `{ x, ... }: x + 100`. It only observes `x`. Two cb calls:
#   A: `{ x = 13; unused = "literal"; }`
#   B: `{ x = 13; unused = (throw "evaluated unused"); }`
#
# Required behaviors:
#  1. Variant B must not fire the throw — the body never probes
#     `unused`, so the cache mustn't force it for "deep hashing" or
#     similar. Failure mode: a `getAttrNames` or deepSeq somewhere in
#     the cache path that fires the throw → variant B aborts.
#  2. Variant A and variant B must collapse to the same cache state.
#     The body's observation history is identical (= one getAttr on
#     "x", one getInt on the result). The trace records only those
#     observations. Different unused fields are extensional fluff that
#     can't perturb identity.
#
# If either invariant breaks, the cache is leaking interpreter-level
# observations into cache state.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

echo '{ x, ... }: x + 100' > "$TEST_ROOT/fn.nix"

# --- Variant A: harmless extra field ---

clearCache
echo "=== variant A cold ==="
assertCacheStats 0 2 1 -- \
    nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 13; unused = "literal"; }'

echo "=== variant A warm ==="
assertCacheStats 2 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 13; unused = "literal"; }'

# --- Variant B: throw-on-eval in unused field (laziness probe) ---

clearCache
echo "=== variant B cold (throw in unused field — must not fire) ==="
assertCacheStats 0 2 1 -- \
    nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 13; unused = throw "evaluated unused"; }'

echo "=== variant B warm ==="
assertCacheStats 2 0 0 -- \
    env _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
        '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 13; unused = throw "evaluated unused"; }'

# Result sanity.
resultA=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 13; unused = "literal"; }')
[[ "$resultA" == 113 ]]
resultB=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 13; unused = throw "evaluated unused"; }')
[[ "$resultB" == 113 ]]
