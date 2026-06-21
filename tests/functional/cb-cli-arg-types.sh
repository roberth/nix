#!/usr/bin/env bash

# TracingEvaluator's mkString/mkInt/mkBool/mkPath/mkAttrs/
# getInternalPrimOp construct Objects without opening a root cell.
# Per the audit in this branch, these are constructed values rather
# than "arguments," so probably benign — but CLI auto-args
# (--argstr / --arg-from-file / --arg-from-stdin) flow through
# mkString and end up as actual arguments to the main expression.
#
# This test exercises that flow with a cached function so any cell-
# wiring assumption inside the cb apply path gets stressed by an
# argScope-less TracingObject sitting in the proxy graph.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/use-str.nix" << 'NIX'
{ s }: builtins.stringLength s
NIX

# --argstr binds `s` to a string Value built via mkString. The cb
# applies its body to {s = <that string>}.
echo "=== --argstr cold (expect 5) ==="
result=$(nix eval --impure --apply 'f: f { s = "hello"; }' --file "$TEST_ROOT/use-str.nix")
echo "Got: $result"
[[ "$result" == 5 ]]

echo "=== cached + --argstr cold (expect 5) ==="
result=$(nix eval --impure --apply 'f: (builtins.cache { import = '"$TEST_ROOT"'/use-str.nix; }) { s = "hello"; }' --file "$TEST_ROOT/use-str.nix")
echo "Got: $result"
[[ "$result" == 5 ]]

# Pass a cb-arg whose value originates from a CLI string arg via
# mkString (autoArgs path inside common-eval-args.cc).
echo '{ s }: builtins.stringLength s' > "$TEST_ROOT/main.nix"
echo "=== --argstr through cb (expect 5) ==="
result=$(nix eval --impure --apply 'f: (builtins.cache { import = '"$TEST_ROOT"'/main.nix; }) { s = "world"; }' --file "$TEST_ROOT/main.nix")
echo "Got: $result"
[[ "$result" == 5 ]]
