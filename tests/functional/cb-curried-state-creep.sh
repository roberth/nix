#!/usr/bin/env bash

# CDI state-creep test: a curried cached function `{ x }: y: x + y`.
# The second apply (depth 1) opens a cell whose parent is the first
# apply's cell. That parent cell evolves via TracingObject's absorb
# on outer reads of the depth-0 apply result, so depth-1's seedCell
# contentId() XOR-folds those evolutions in.
#
# If apply opened cells "redundantly" (same as arg's existing scope),
# OR if the first apply's cell didn't evolve, two different depth-0
# args would still produce the same depth-1 CDI — collisions in the
# trie that don't show up as wrong answers when input tracing catches
# them, but DO when responses happen to align.
#
# The test verifies the simpler input-tracing invalidation works for
# curried values. CDI distinctness is a stricter sub-property; harder
# to construct a stale-hit case in pure Nix without contrivance.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/curried.nix" << 'NIX'
x: y: x + y
NIX

echo "=== cold: (c 10) 20 (expect 30) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/curried.nix; } 10) 20')
echo "Got: $result"
[[ "$result" == 30 ]]

# Change depth-1 arg only
echo "=== change y to 25 (expect 35) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/curried.nix; } 10) 25')
echo "Got: $result"
[[ "$result" == 35 ]]

# Change depth-0 arg only: state creep must distinguish
echo "=== change x to 100 (expect 120) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/curried.nix; } 100) 20')
echo "Got: $result"
[[ "$result" == 120 ]]

# TODO: replay coverage for curried cached values is blocked on a
# separate gap — `_NIX_DISALLOW_PARSE=1 (c 10) 20` falls through to
# inner re-eval instead of replaying from the trie. State-creep CDI
# distinctness can't be fully verified until that's fixed.
#   echo "=== replay same (expect 30) ==="
#   result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/curried.nix; } 10) 20')
#   [[ "$result" == 30 ]]
