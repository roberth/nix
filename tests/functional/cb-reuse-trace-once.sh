#!/usr/bin/env bash

# Verifies SCA-firing cell-reuse: on warm, two probes into the same
# callback-produced attrset (`x.a + x.b`) should dispatch through
# the same SelectorApply once — the second probe reuses the first
# firing's RCA (extended obsSet, same cell), so the callback body
# runs once per warm session, not once per probe.
#
# The callback traces its input; counting trace lines on stderr is
# the observable proxy for "how many times did the body run".
#
# Cached body returns the callback's applyResult unchanged (so the
# outer probes drive the SelectorApply dispatches on warm):
#   f: f 42
# Outer:
#   let x = (cache body) (arg: trace "cb-fired-${toString arg}" {a=1,b=2});
#   in x.a + x.b
#
# Cold: body runs, callback fires once during body eval, trace once, result 3.
# Warm: expected exactly one trace (reuse collapses the two SelectorApply
# dispatches from x.a and x.b into a single body invocation).
#
# If reuse doesn't fire, warm traces twice — one per attribute probe's walk.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

cat > "$TEST_ROOT/cb-once-body.nix" << 'NIX'
f: f 42
NIX

expr='let x = (builtins.cache { import = '"$TEST_ROOT"'/cb-once-body.nix; }) (arg: builtins.trace "cb-fired-${toString arg}" { a = 1; b = 2; }); in x.a + x.b'

clearCache
echo "=== cold ==="
result=$(nix eval --impure --expr "$expr" 2> "$TEST_ROOT/cold.err")
cold_traces=$(grep -c "cb-fired-42" "$TEST_ROOT/cold.err" || true)
echo "cold: result=$result, traces=$cold_traces"
[[ "$result" == 3 ]]
[[ "$cold_traces" == 1 ]]

echo "=== warm ==="
result=$(nix eval --impure --expr "$expr" 2> "$TEST_ROOT/warm.err")
warm_traces=$(grep -c "cb-fired-42" "$TEST_ROOT/warm.err" || true)
echo "warm: result=$result, traces=$warm_traces"
[[ "$result" == 3 ]]
[[ "$warm_traces" == 1 ]]
