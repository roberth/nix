#!/usr/bin/env bash

# CDI #53: sanity test for assertCacheStats + the NIX_CACHE_STATS_FILE
# counter mechanism (#52). Locks in exact (hits, misses, fallbacks)
# counts for a simple cb to catch silent regressions.
#
# Calibration: counts are taken from observed behaviour as of the
# commit that introduced this test. If a future change shifts them
# without breaking output, this test surfaces it.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

echo '{ x = 1; y = 2; }' > "$TEST_ROOT/simple.nix"

# Cold record: no recorded trace yet → first lookup misses, primop
# falls through to inner evaluator. No TracingReplayObject means no
# ensureInner fallback path.
echo "=== cold record (no hits, but a miss, no fallbacks) ==="
assertCacheStats 0 1 0 -- \
    nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/simple.nix; }).x'

# Warm replay: cache hit on evalFile → TracingReplayObject wraps the
# result. Outer's .x access + getType/getInt forces all hit the trie.
# The exact count depends on how many Q's the trie has for this path
# (file hash dispatch + outer's reads on the attrset + reads on the
# int child). Calibrated against current behaviour.
echo "=== warm replay (hits, no misses, no fallbacks) ==="
# One fewer hit than before the cell-migration: evalFile now
# pre-populates the root wrapper's cachedWHNF from SelectorImport's
# Terminal (Phase C), so the previously-separate SelectorGetWHNF walk
# on the root is elided. Walker sees:
#   1. lookup(SelectorImport) — hit.
#   2. lookup(SelectorGetAttr{"x", from=root}) — hit.
assertCacheStats 2 0 0 -- \
    nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/simple.nix; }).x'
