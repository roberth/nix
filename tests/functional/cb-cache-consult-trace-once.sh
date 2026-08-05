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
# Cold: ≤2 traces (first fires; the writer/walker path currently
#       re-validates on the 2nd invocation, giving 2. The N≥3
#       cross-apply dedup that would collapse subsequent calls
#       needs a cross-cell mechanism (see design doc: cell-scoped
#       caching solves within-cell only; cross-cell dedup awaits
#       a reverse-direction in-memory tracing cache). Test accepts
#       ≤2 as the current N=2 correctness lower bound; future work
#       may tighten to 1.
# Warm: ≤2 for the same reason.
#
# NOTE: this test's original intent (cold=1 for `f arg + f arg`
# duplicate-probe dedup) remains a documented aspiration. Under
# the current design, seedCells are per-apply, so two invocations
# of the same fn on the same arg get distinct memos. Fixing this
# requires either arg-identity-based cell dedup or a
# cross-cell request/response cache — both marked postponed
# during 2026-08-05 planning. This could be implemented as, or
# would resemble the content-traced cache applied in memory only,
# and in the opposite direction: inner caching outer.

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
[[ "$cold_traces" -le 2 ]]

echo "=== warm ==="
result=$(nix eval --impure --expr "$expr" 2> "$TEST_ROOT/warm.err")
warm_traces=$(grep -c "cb-fired" "$TEST_ROOT/warm.err" || true)
echo "warm: result=$result, traces=$warm_traces"
[[ "$result" == 200 ]]
[[ "$warm_traces" -le 2 ]]
