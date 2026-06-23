#!/usr/bin/env bash

# CDI #10: registerOuterAt derived-id collision probe.
#
# Sibling cb invocations whose navigation produces the same derived
# childId — qH(getAttr{name, from=rootId}) is deterministic — will
# insert_or_assign-collide on AmbientResolver's shared outerValues map.
# This SHOULD be benign because same-shape outer args produce same-
# observation children (extensionality), but the invariant isn't
# obvious. Test it directly: two cb invocations navigating to
# `args.deep.nested.value`, each with same shape, verify the result
# is correct.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/deep.nix" << 'NIX'
{ args }: args.outer.middle.inner
NIX

# Two cb invocations with same-shape deep args. Each navigates
# args.outer.middle.inner via 3 derived childIds. Second invocation's
# registerOuterAt calls overwrite the first's. If the overwrite is
# benign (same-shape collapse), result is correct.
echo "=== cold: two same-shape cb invocations (expect 42 + 42 = 84) ==="
result=$(nix eval --impure --expr \
    'let c = builtins.cache { import = '"$TEST_ROOT"'/deep.nix; };
         args = { outer = { middle = { inner = 42; }; }; };
     in c { inherit args; } + c { inherit args; }')
echo "Got: $result"
[[ "$result" == 84 ]]

# Warm: must hit and still produce the right answer.
echo "=== warm same (expect 84) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    'let c = builtins.cache { import = '"$TEST_ROOT"'/deep.nix; };
         args = { outer = { middle = { inner = 42; }; }; };
     in c { inherit args; } + c { inherit args; }')
echo "Got: $result"
[[ "$result" == 84 ]]

# Two SYNTACTICALLY-DISTINCT but observationally-identical arg
# constructions, to confirm collapse still applies regardless of how
# the args were built.
clearCache
echo "=== cold: two independently-constructed same-shape args (expect 84) ==="
result=$(nix eval --impure --expr \
    'let c = builtins.cache { import = '"$TEST_ROOT"'/deep.nix; };
     in c { args = { outer = { middle = { inner = 42; }; }; }; }
      + c { args = { outer = { middle = { inner = 42; }; }; }; }')
echo "Got: $result"
[[ "$result" == 84 ]]
