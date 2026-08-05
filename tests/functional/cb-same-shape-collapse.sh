#!/usr/bin/env bash

# CDI #3: same-shape collapse positive test.
#
# Two cb invocations with extensionally-equivalent (but syntactically
# distinct) args must share trie entries. The second invocation should
# HIT the trace the first one recorded, because the observation
# history is identical → CDIs collapse → same trie position.
#
# If CDIs were over-discriminating (e.g. tagging by allocation order),
# the cold path would record both invocations independently and the
# hit count would be 0 — this test would fail loudly.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

echo '{ x }: x + 100' > "$TEST_ROOT/fn.nix"

# Cold: c is applied twice with two `{ x = 1; }` literals. These are
# syntactically distinct in the AST but observationally identical.
# Expected: first invocation records, second invocation hits the
# recorded trace via CDI collapse. (Exact counts calibrated.)
echo "=== cold record: c{x=1} + c{x=1} ==="
# Wrapping-stack update (2026-08-05): TRE::evalFile now always
# returns a TRO wrapping the lazy inner. Cold's walker probe misses
# on empty DB (+1 miss) and the ensureInner activation ticks the
# fallback stat (+1 fallback). Hit unchanged.
assertCacheStats 1 3 2 -- \
    nix eval --impure --expr \
        'let c = builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }; in c { x = 1; } + c { x = 1; }'

# Warm replay: everything must hit, no fallbacks.
echo "=== warm replay ==="
assertCacheStats 3 0 0 -- \
    nix eval --impure --expr \
        'let c = builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }; in c { x = 1; } + c { x = 1; }'

# Stronger collapse probe: three independent constructions with the
# same shape. The 2nd and 3rd should both hit the 1st's trace.
clearCache
echo "=== cold record: c{x=1} + c{x=1} + c{x=1} (expect more hits via further collapse) ==="
# +1 miss, +1 fallback for wrapping-stack (see above).
assertCacheStats 2 3 2 -- \
    nix eval --impure --expr \
        'let c = builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }; in c { x = 1; } + c { x = 1; } + c { x = 1; }'
