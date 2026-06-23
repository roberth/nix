#!/usr/bin/env bash

# Regression guard for cidasks own-fold (= Component G in the XOR
# audit, see design doc § Technical requirements). Cb body invokes the
# same outer-supplied callback twice with the same arg, producing two
# cb-apply boundaries that record observations against the same
# (subject, scope) into a shared depth-2 trie chain.
#
# Each fact's `from` field must be cidasks-evolved per its position in
# the chain — without that evolution the two cb-applies' identical
# probe sequences (getType, getInt) would log facts with identical
# `from` values, identical reqHashes, and the per-fact XOR-fold into
# the AmbientAsks chain would cancel them in pairs. The factSet would
# silently lose the second cb-apply's contributions and the warm
# walker would traverse a chain that doesn't match what it dispatches.
#
# Visible failure mode if cidasks-evolution regresses:
#   - the warm walker can't find an AmbientAsks edge for one of the
#     duplicate probes → DISALLOW_PARSE blocks the fallback re-eval
#     and the test exits with the parsing-disallowed error;
#   - or, the chain's terminal factSet differs from what the walker
#     computes, the depth-1 dispatch can't find the cb apply's
#     terminal, fallback fires, same error;
#   - either way the test fails with a clearly-wrong outcome.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/fn.nix" << 'NIX'
{ f, x }: (f x) + (f x)
NIX

# Cold: f=(n: n+100), x=7. (f x) = 107. Sum = 214.
echo "=== cold (expect 214) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { f = n: n + 100; x = 7; }')
[[ "$result" == 214 ]]

echo "=== warm DISALLOW_PARSE (expect 214) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { f = n: n + 100; x = 7; }')
[[ "$result" == 214 ]]

# A second cold record with a different scalar value must give a
# different warm result. If XOR evolution broke and the trie's final
# state was wrong, this would warm to the wrong cached value (= e.g.,
# 214 for x=13 too, which would be the symptom of cross-recording
# leakage at the depth-2 factSet level).
clearCache
echo "=== cold (x=13, expect 226) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { f = n: n + 100; x = 13; }')
[[ "$result" == 226 ]]

echo "=== warm DISALLOW_PARSE (x=13, expect 226) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { f = n: n + 100; x = 13; }')
[[ "$result" == 226 ]]
