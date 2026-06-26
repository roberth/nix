#!/usr/bin/env bash

# Principle #7 (= laziness end-to-end): forcing is initiated by the
# value's consumer, never by the cache itself. The cb arg must not
# be forced unless the cached body actually probes it. This includes
# the case where the *entire argument* is unobserved — a non-strict
# cached body should accept `throw`-thunked args without firing the
# throw.
#
# Companion to cb-irrelevant-fields-lazy.sh, which covers the
# "throw in unused field of attrset arg" case. Here the entire arg
# is the throw, exercising the apply-boundary's own laziness:
#   1. Computing applyReqHash must not force the arg (= it's a hash
#      over structural identifiers, not the arg's value).
#   2. The d=2 chain rooted at applyReqHash must not require probing
#      the arg to enumerate.
#   3. Warm replay must not force the arg either — the recorded
#      Terminal carries the cached result without consulting the
#      live arg's value.
#
# Failure modes this test catches:
#  - applyReqHash computation forcing the arg via getCdiHex →
#    cold record fires the throw.
#  - bridgedLocals or ExprFromObject eagerly probing the arg →
#    cold record fires the throw.
#  - warm walker forcing the arg via standin reconstruction or
#    apply-result probing → warm fires the throw.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

# Body completely ignores its argument: `_: 42`.
cat > "$TEST_ROOT/ignore-arg.nix" << 'NIX'
_: 42
NIX

clearCache
echo "=== cold (arg = throw, body ignores arg, expect 42) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/ignore-arg.nix; }) (throw "must not be used")')
[[ "$result" == 42 ]]

echo "=== warm DISALLOW_PARSE (expect 42) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/ignore-arg.nix; }) (throw "must not be used")')
[[ "$result" == 42 ]]

# Variation: body returns a function that itself ignores everything.
# Forces the apply result to nFunction without ever needing the arg.
cat > "$TEST_ROOT/ignore-arg-fn.nix" << 'NIX'
_: (_: 100)
NIX

clearCache
echo "=== cold (apply result is a fn, neither side touches arg, expect 100) ==="
result=$(nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/ignore-arg-fn.nix; }) (throw "outer")) (throw "inner")')
[[ "$result" == 100 ]]

echo "=== warm (expect 100) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/ignore-arg-fn.nix; }) (throw "outer")) (throw "inner")')
[[ "$result" == 100 ]]

# Variation: cached body returns an attrset whose values don't depend on
# the arg. Probing a result attr (`.a`) must record only the result-side
# query; the arg must stay unforced cold and warm.
cat > "$TEST_ROOT/ignore-arg-attrs.nix" << 'NIX'
_: { a = 10; b = 20; }
NIX

clearCache
echo "=== cold (arg = throw, result is attrset, force .a, expect 10) ==="
result=$(nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/ignore-arg-attrs.nix; }) (throw "must not be used")).a')
[[ "$result" == 10 ]]

echo "=== warm (force .a, expect 10) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/ignore-arg-attrs.nix; }) (throw "must not be used")).a')
[[ "$result" == 10 ]]
