#!/usr/bin/env bash

# M4 T-comb-5: mutual recursion BETWEEN evaluators. `pong` is defined
# in the cached inner file; `pingOuter` is defined in outer. Each
# recursive step alternates:
#   - pong (inner) calls pingOuter (outer) via callback, passing a
#     contra-arg fn.
#   - pingOuter (outer) applies the contra-arg (an inner lambda) —
#     that's the higher-order-callback apply.
#   - contra-arg body calls pong (inner) again with n+1.
#
# So each recursion depth = one outer→inner cache call +
#                            one inner→outer callback +
#                            one outer's higher-order apply of contra-arg.
#
# Verifies:
#   - Cold/warm correctness for a mutually recursive shape.
#   - obsSet nesting depth accurately reflects the recorded structure
#     (checked via NIX_CACHE_STATS_FILE).

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/mut-cross.nix" << 'NIX'
{ pingOuter }: rec {
  pong = n: if n >= 3 then n else pingOuter (m: pong (n + 1));
}
NIX

echo "=== cold: pong 0 → recursion terminates at n=3 (expect 3) ==="
result=$(nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      pingOuter = cb: cb 0;
  in (cached { inherit pingOuter; }).pong 0')
echo "Got: $result"
[[ "$result" == 3 ]]

echo "=== warm replay (expect 3) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      pingOuter = cb: cb 0;
  in (cached { inherit pingOuter; }).pong 0')
echo "Got: $result"
[[ "$result" == 3 ]]

# Change starting n. Expected: pong 1 → recursion 2 layers → return 3.
echo "=== outer change: pong 1 (expect 3) ==="
result=$(nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      pingOuter = cb: cb 0;
  in (cached { inherit pingOuter; }).pong 1')
echo "Got: $result"
[[ "$result" == 3 ]]

# Change terminator threshold via making inner file take an arg.
cat > "$TEST_ROOT/mut-cross-5.nix" << 'NIX'
{ pingOuter }: rec {
  pong = n: if n >= 5 then n else pingOuter (m: pong (n + 1));
}
NIX

echo "=== fresh inner (threshold=5), cold pong 0 (expect 5) ==="
result=$(nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross-5.nix; };
      pingOuter = cb: cb 0;
  in (cached { inherit pingOuter; }).pong 0')
echo "Got: $result"
[[ "$result" == 5 ]]

echo "=== warm (expect 5) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross-5.nix; };
      pingOuter = cb: cb 0;
  in (cached { inherit pingOuter; }).pong 0')
echo "Got: $result"
[[ "$result" == 5 ]]

# Depth statistic: mutual recursion should reach some depth > 1 in the
# recorded obsSet structure. Higher inner-terminator = deeper nesting.
echo "=== depth-3 stats ==="
clearCache
statsFile=$(mktemp)
NIX_CACHE_STATS_FILE="$statsFile" nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross.nix; };
      pingOuter = cb: cb 0;
  in (cached { inherit pingOuter; }).pong 0' > /dev/null
depth=$(jq -r .maxCallbackObsSetNestingDepth "$statsFile" 2>/dev/null || cat "$statsFile" | sed 's/.*maxCallbackObsSetNestingDepth":\([0-9]*\).*/\1/')
echo "depth3 got maxCallbackObsSetNestingDepth=$depth"
[[ "$depth" -ge 1 ]]

echo "=== depth-5 stats ==="
clearCache
statsFile5=$(mktemp)
NIX_CACHE_STATS_FILE="$statsFile5" nix eval --impure --expr '
  let cached = builtins.cache { import = '"$TEST_ROOT"'/mut-cross-5.nix; };
      pingOuter = cb: cb 0;
  in (cached { inherit pingOuter; }).pong 0' > /dev/null
depth5=$(jq -r .maxCallbackObsSetNestingDepth "$statsFile5" 2>/dev/null || cat "$statsFile5" | sed 's/.*maxCallbackObsSetNestingDepth":\([0-9]*\).*/\1/')
echo "depth5 got maxCallbackObsSetNestingDepth=$depth5"
[[ "$depth5" -ge "$depth" ]]

rm -f "$statsFile" "$statsFile5"
