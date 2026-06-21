#!/usr/bin/env bash

# TracingLocalObject (recording) absorbs observations into argScope.
# ReplayLocalObject (replay) doesn't. For top-level Locals this is fine
# because replay reads pool by final substituted localId. For descendant
# Locals (children navigated via maybeGetAttr/getListElem on a parent
# Local), recording-side child shares parent's cell — observations
# evolve parent. Replay-side derives child IDs from parent's settled
# hash; if the asymmetry caused a hash mismatch the pool lookup would
# fail and replay would fall through.
#
# This test exercises descendant Local navigation: inner constructs a
# nested attrset and passes it to an outer-supplied function that
# reads through several levels.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/nested-local.nix" << 'NIX'
{ f }: f { outer = { middle = { inner = 42; }; }; }
NIX

echo "=== cold: outer reads outer.middle.inner (expect 42) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested-local.nix; }) { f = a: a.outer.middle.inner; }')
echo "Got: $result"
[[ "$result" == 42 ]]

echo "=== replay (expect 42) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested-local.nix; }) { f = a: a.outer.middle.inner; }')
echo "Got: $result"
[[ "$result" == 42 ]]

# Outer fn navigates two distinct deep paths in the inner-supplied
# attrset. The second path's child Local descends from the same root
# as the first — exercises the descendant CDI chain for siblings.
cat > "$TEST_ROOT/nested-local2.nix" << 'NIX'
{ f }: f { left = { v = 10; }; right = { v = 20; }; }
NIX

echo "=== cold: outer reads .left.v + .right.v (expect 30) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested-local2.nix; }) { f = a: a.left.v + a.right.v; }')
echo "Got: $result"
[[ "$result" == 30 ]]

echo "=== replay (expect 30) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested-local2.nix; }) { f = a: a.left.v + a.right.v; }')
echo "Got: $result"
[[ "$result" == 30 ]]
