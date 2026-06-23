#!/usr/bin/env bash

# Cross-process distinct-(f, x) invocations sharing the same cache.
#
# The fixture: `{ f, x }: f x`. Three nix-eval invocations against the
# same shared on-disk trie, with distinct (f, x) combinations. Each
# must produce the correct value at cold record.
#
# Originally this test asserted on a `pre_flush_substitution_collisions`
# metric from the substitution-machinery era; that metric was removed
# under the via-Asks design (= per design principles, identity is
# observation-derived, not substitution-tracked). The fixture remains
# useful as a cross-process behavioral integration check: distinct
# (f, x) combinations write distinct recordings into a shared trie.
#
# Warm replay across the three recordings is gated on task #82 (=
# sibling cb-applies share seed cdi at flush; per-apply Asks edges
# would isolate their factSet positions). For now, single-recording
# warm replay is covered by other tests (cb-curried-state-creep,
# cb-higher-order, cb-stats-sidecar-baseline).

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

echo '{ f, x }: f x' > "$TEST_ROOT/call-fn.nix"

# Three invocations, distinct (f, x), against the same trie:
#   1: f = (n: n + 1),   x = 10  →  11
#   2: f = (n: n + 100), x = 10  → 110
#   3: f = (n: n + 1),   x = 50  →  51

echo "=== cold #1 (expect 11) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/call-fn.nix; }) { f = n: n + 1; x = 10; }')
echo "Got: $result"
[[ "$result" == 11 ]]

echo "=== cold #2 (expect 110) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/call-fn.nix; }) { f = n: n + 100; x = 10; }')
echo "Got: $result"
[[ "$result" == 110 ]]

echo "=== cold #3 (expect 51) ==="
result=$(nix eval --impure --expr \
    '(builtins.cache { import = '"$TEST_ROOT"'/call-fn.nix; }) { f = n: n + 1; x = 50; }')
echo "Got: $result"
[[ "$result" == 51 ]]
