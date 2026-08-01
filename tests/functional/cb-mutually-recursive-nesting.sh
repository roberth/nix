#!/usr/bin/env bash

# M4 T-comb-5: mutual recursion BETWEEN evaluators, with the recursion
# LIMIT threaded through the callback bounces.
#
# `pong` is defined in the cached inner file; `ping` in outer. Each
# recursion carries `{ n, limit }` as an attrset arg. `pong` checks
# the limit and either terminates or invokes `ping` with the current
# n, the limit, and a continuation callback that will be invoked with
# the next n value. `ping` in outer applies that continuation with
# `n + 1`, threading the same limit through so the next `pong` firing
# knows where to stop.
#
# The limit-as-a-parameter matters because it forces the callbacks to
# genuinely carry state across the boundary at each recursion step —
# a `>0` termination would collapse into a captured constant and hide
# the parameter-threading. Depth = arity of state flowing through
# each callback firing.
#
# Verifies:
#   - Cold/warm correctness with attrset-shaped state threading.
#   - Different limits produce different results; warm hits the same
#     recorded trace when args match.
#   - obsSet depth statistic reflects the recorded structure honestly.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/mut-cross.nix" << 'NIX'
{ ping }: rec {
  pong = args:
    if args.n >= args.limit then args.n
    else ping {
      n = args.n;
      limit = args.limit;
      cb = nextN: pong { n = nextN; limit = args.limit; };
    };
}
NIX

# ping's contract: read args.cb (a fn), apply it to args.n + 1.
# The limit passes through but ping doesn't inspect it.
OUTER_PING='ping = args: args.cb (args.n + 1)'

echo "=== cold: pong {n=0; limit=3} (expect 3) ==="
result=$(nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      '"$OUTER_PING"';
  in (cached { inherit ping; }).pong { n = 0; limit = 3; }')
echo "Got: $result"
[[ "$result" == 3 ]]

echo "=== warm replay same limit (expect 3) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      '"$OUTER_PING"';
  in (cached { inherit ping; }).pong { n = 0; limit = 3; }')
echo "Got: $result"
[[ "$result" == 3 ]]

echo "=== outer change: limit=5 (expect 5) ==="
result=$(nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      '"$OUTER_PING"';
  in (cached { inherit ping; }).pong { n = 0; limit = 5; }')
echo "Got: $result"
[[ "$result" == 5 ]]

echo "=== warm replay limit=5 (expect 5) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      '"$OUTER_PING"';
  in (cached { inherit ping; }).pong { n = 0; limit = 5; }')
echo "Got: $result"
[[ "$result" == 5 ]]

echo "=== warm replay limit=3 (still expect 3, both recordings coexist) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      '"$OUTER_PING"';
  in (cached { inherit ping; }).pong { n = 0; limit = 3; }')
echo "Got: $result"
[[ "$result" == 3 ]]

# Divergence: change ping's semantics (increment by 2 instead of 1).
echo "=== outer change: ping increments by 2, limit=5 (expect 6) ==="
result=$(nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      ping = args: args.cb (args.n + 2);
  in (cached { inherit ping; }).pong { n = 0; limit = 5; }')
echo "Got: $result"
[[ "$result" == 6 ]]

# Depth stat: verify the recorded structure has some nesting.
echo "=== depth stat for limit=3 ==="
clearCache
statsFile=$(mktemp)
NIX_CACHE_STATS_FILE="$statsFile" nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      '"$OUTER_PING"';
  in (cached { inherit ping; }).pong { n = 0; limit = 3; }' > /dev/null
depth=$(sed 's/.*maxCallbackObsSetNestingDepth":\([0-9]*\).*/\1/' "$statsFile")
echo "limit=3 maxCallbackObsSetNestingDepth=$depth"
[[ "$depth" -ge 1 ]]

echo "=== depth stat for limit=5 (expect >= limit=3 depth) ==="
clearCache
statsFile5=$(mktemp)
NIX_CACHE_STATS_FILE="$statsFile5" nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      '"$OUTER_PING"';
  in (cached { inherit ping; }).pong { n = 0; limit = 5; }' > /dev/null
depth5=$(sed 's/.*maxCallbackObsSetNestingDepth":\([0-9]*\).*/\1/' "$statsFile5")
echo "limit=5 maxCallbackObsSetNestingDepth=$depth5"
[[ "$depth5" -ge "$depth" ]]

rm -f "$statsFile" "$statsFile5"
