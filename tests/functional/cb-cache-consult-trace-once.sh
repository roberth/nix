#!/usr/bin/env bash

# Cache boundary observes the same callback probe twice. Callback
# body traces on stderr; trace count = how many times the body ran.
#
# Why dedup is required: FactSet's XOR-fold is self-inverse. Two
# identical (req, resp) Facts on one chain XOR-cancel — the second
# fold takes cur back to before the first, chain becomes cyclic.
# Failing to dedup gives one of two broken shapes: a cyclic Ask
# structure (catastrophic), or a duplicate live callback invocation
# the outer scenario didn't ask for a second time (unprompted
# callback = outer-request discipline violation).
#
# Cold: 1 trace (first probe fires, duplicate dedup'd).
# Warm: 1 trace (prior Terminal validated live once, duplicate dedup'd).

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

cat > "$TEST_ROOT/cb-body.nix" << 'NIX'
{ cb }: cb 42
NIX

expr='let
        f = builtins.cache { import = '"$TEST_ROOT"'/cb-body.nix; };
        arg = { cb = _: builtins.trace "cb-fired" 100; };
      in f arg + f arg'

clearCache
echo "=== cold ==="
result=$(nix eval --impure --expr "$expr" 2> "$TEST_ROOT/cold.err")
cold_traces=$(grep -c "cb-fired" "$TEST_ROOT/cold.err" || true)
echo "cold: result=$result, traces=$cold_traces"
[[ "$result" == 200 ]]
[[ "$cold_traces" == 1 ]]

echo "=== warm ==="
result=$(nix eval --impure --expr "$expr" 2> "$TEST_ROOT/warm.err")
warm_traces=$(grep -c "cb-fired" "$TEST_ROOT/warm.err" || true)
echo "warm: result=$result, traces=$warm_traces"
[[ "$result" == 200 ]]
[[ "$warm_traces" == 1 ]]
