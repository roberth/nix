#!/usr/bin/env bash

# Cb body returns a "module-shaped" attrset: `{ x }: { add = n: n+x;
# sub = n: n-x; mul = n: n*x; }`. This is a common real-world pattern
# (= config builders, utility modules). The outer probes one attr at a
# time and applies the resulting lambda.
#
# Three behaviors must hold:
#  1. Cold: `result.add 5` with x=10 produces 15.
#  2. Warm replay (DISALLOW_PARSE): same probe reproduces 15 from
#     cache — the recorded chain (getAttr "add", apply with n=5) all
#     replays without re-parsing the cb source.
#  3. Warm probe of a *different* attr that was never recorded
#     (= `.sub 3`): miss → fallback → DISALLOW_PARSE blocks re-parse
#     with a clear error. The cache must NOT silently reuse `.add 5`'s
#     recorded response for `.sub 3`.
#
# The third behavior is the soundness check. A bug where the walker
# matched recorded apply queries by structural shape alone (= not by
# the qualified attr name) would silently return 15 for `.sub 3`.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/module.nix" <<'EOF'
{ x }: {
  add = n: n + x;
  sub = n: n - x;
  mul = n: n * x;
}
EOF

# (1) cold record
echo "=== cold: (cache module) { x = 10 }.add 5 ==="
result=$(nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/module.nix; }) { x = 10; }).add 5')
[[ "$result" == 15 ]]

# (2) warm hit on same probe
echo "=== warm hit (DISALLOW_PARSE) — .add 5 must replay 15 ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/module.nix; }) { x = 10; }).add 5')
[[ "$result" == 15 ]]

# (3) novel attr probe — must miss, NOT replay .add's response
echo "=== warm miss (DISALLOW_PARSE) — .sub 3 must error, not reuse 15 ==="
if _NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/module.nix; }) { x = 10; }).sub 3' \
    2>"$TEST_ROOT/err.log"; then
    echo "FAIL: .sub 3 succeeded under DISALLOW_PARSE — cache silently reused a foreign response" >&2
    exit 1
fi
grep -q "parsing disallowed by _NIX_DISALLOW_PARSE" "$TEST_ROOT/err.log" || {
    echo "FAIL: expected DISALLOW_PARSE error, got:" >&2
    cat "$TEST_ROOT/err.log" >&2
    exit 1
}

# (4) sanity: live fallback yields the right value (= 3 - 10 = -7)
echo "=== fallback re-eval (.sub 3, no DISALLOW_PARSE) — expect -7 ==="
result=$(nix eval --impure --expr \
    '((builtins.cache { import = '"$TEST_ROOT"'/module.nix; }) { x = 10; }).sub 3')
[[ "$result" == -7 ]]
