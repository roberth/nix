#!/usr/bin/env bash

# M4 T-file-1: editing the cached expression's file itself invalidates
# recordings whose Selector chain includes SelectorImport for that file.
#
# The `import` Selector at the trace root has the file's content hash
# folded in (via evalFile's FileReadRequest). A byte-level edit of the
# file — even semantic-preserving — changes the content hash → the root
# import Selector's cur diverges → walker misses cleanly, falls back to
# inner re-eval.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/edited.nix" << 'NIX'
{ x }: x + 1
NIX

echo "=== cold: x = 5 (expect 6) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/edited.nix; }) { x = 5; }')
echo "Got: $result"
[[ "$result" == 6 ]]

echo "=== warm same (expect 6) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/edited.nix; }) { x = 5; }')
echo "Got: $result"
[[ "$result" == 6 ]]

# Semantic-preserving edit: add whitespace / comment. Content hash
# changes, so the recorded trace is unreachable. Walker misses; inner
# re-eval works because we're not under DISALLOW_PARSE.
cat > "$TEST_ROOT/edited.nix" << 'NIX'
# Same semantics, different bytes.
{ x }:  x + 1
NIX

echo "=== after semantic-preserving edit (expect 6, via re-eval) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/edited.nix; }) { x = 5; }')
echo "Got: $result"
[[ "$result" == 6 ]]

# Semantic-changing edit: change body. Result should differ.
cat > "$TEST_ROOT/edited.nix" << 'NIX'
{ x }: x + 100
NIX

echo "=== after semantic-changing edit (expect 105) ==="
result=$(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/edited.nix; }) { x = 5; }')
echo "Got: $result"
[[ "$result" == 105 ]]

# Warm replay of the second version.
echo "=== warm replay after edit (expect 105) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/edited.nix; }) { x = 5; }')
echo "Got: $result"
[[ "$result" == 105 ]]
