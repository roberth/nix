#!/usr/bin/env bash

# `nix eval --apply 'f: f { ... }' --file FILE` with tracing-eval-cache
# enabled triggers a SIGSEGV (regardless of whether `builtins.cache`
# appears anywhere in the input). The file's function gets wrapped via
# ExprFromObject → makeCachedFnPrimOp because TracingObject carries an
# innerEvaluator and the function-typed Object branch routes that way.
#
# This is a regression test: each scenario below must NOT crash.

source common.sh

enableFeatures "tracing-eval-cache"

echo '{ x }: x.inner + 1' > "$TEST_ROOT/main.nix"

# Scenario 1: --apply with a plain attrset arg, no builtins.cache involved
echo "=== Scenario 1: --apply with plain attrset (expect 43) ==="
result=$(nix eval --impure --apply 'f: f { x = { inner = 42; }; }' --file "$TEST_ROOT/main.nix")
echo "Got: $result"
[[ "$result" == 43 ]]

# Scenario 2: --apply with a cache result as the arg
echo '{ inner = 42; }' > "$TEST_ROOT/cached-val.nix"
echo "=== Scenario 2: --apply with cache result (expect 43) ==="
result=$(nix eval --impure --apply 'f: f { x = builtins.cache { import = '"$TEST_ROOT"'/cached-val.nix; }; }' --file "$TEST_ROOT/main.nix")
echo "Got: $result"
[[ "$result" == 43 ]]
