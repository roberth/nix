#!/usr/bin/env bash

# M4 T-div-2: attrset argument — same probed subset, different unprobed
# attrs. Cache identity depends only on what inner probes, not on what
# inner ignores. Two outer args with the same `.foo` (probed) but
# different `.bar` (unprobed) should both hit the same recording.
#
# Verifies Foundational 7 (laziness end-to-end): cache never touches
# unprobed attributes, so their values don't participate in identity.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/probed.nix" << 'NIX'
{ args }: args.foo + 100
NIX

echo "=== cold: args = { foo = 5; bar = 10; } (expect 105) ==="
result=$(nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/probed.nix; }) {
    args = { foo = 5; bar = 10; };
  }')
echo "Got: $result"
[[ "$result" == 105 ]]

echo "=== warm replay same args (expect 105) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/probed.nix; }) {
    args = { foo = 5; bar = 10; };
  }')
echo "Got: $result"
[[ "$result" == 105 ]]

echo "=== warm replay: change UNPROBED .bar (expect 105, cache hit) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/probed.nix; }) {
    args = { foo = 5; bar = 999; };
  }')
echo "Got: $result"
[[ "$result" == 105 ]]

echo "=== change PROBED .foo (expect 107, cache miss + fallback) ==="
result=$(nix eval --impure --expr '
  (builtins.cache { import = '"$TEST_ROOT"'/probed.nix; }) {
    args = { foo = 7; bar = 10; };
  }')
echo "Got: $result"
[[ "$result" == 107 ]]
