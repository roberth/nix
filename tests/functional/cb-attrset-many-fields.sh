#!/usr/bin/env bash

# M4 T-comb-4b (simpler variant of "callback returns attrset"):
# cached body returns an attrset; outer accesses only some attrs.
#
# `{ x }: { a = x + 1; b = x * 2; c = x - 1; }` — outer reads `.a`
# and `.c` only. `.b` is unprobed.
#
# Warm should hit for the probed attrs; changing outer's `x` should
# miss because `.a` and `.c` change.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/many-fields.nix" << 'NIX'
{ x }: { a = x + 1; b = x * 2; c = x - 1; }
NIX

echo "=== cold: x=10 → .a + .c (expect 20) ==="
result=$(nix eval --impure --expr '
  let r = (builtins.cache { import = '"$TEST_ROOT"'/many-fields.nix; }) { x = 10; };
  in r.a + r.c')
echo "Got: $result"
[[ "$result" == 20 ]]

echo "=== warm same (expect 20) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  let r = (builtins.cache { import = '"$TEST_ROOT"'/many-fields.nix; }) { x = 10; };
  in r.a + r.c')
echo "Got: $result"
[[ "$result" == 20 ]]

echo "=== change x=100 (expect 200, cache miss) ==="
result=$(nix eval --impure --expr '
  let r = (builtins.cache { import = '"$TEST_ROOT"'/many-fields.nix; }) { x = 100; };
  in r.a + r.c')
echo "Got: $result"
[[ "$result" == 200 ]]

echo "=== restore x=10, warm (expect 20) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  let r = (builtins.cache { import = '"$TEST_ROOT"'/many-fields.nix; }) { x = 10; };
  in r.a + r.c')
echo "Got: $result"
[[ "$result" == 20 ]]
