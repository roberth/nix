#!/usr/bin/env bash

# Cached body passes a 2-arg curried lambda as contra-arg to an outer
# callback; the outer callback applies it in TWO stages. §6a's
# compositional-SCA case:
#
#   body:  { f1 }: f1 (a: b: a + b)
#   outer: cached { f1 = x1: x1 1 2; }
#
# `x1 1` records SCA{fn=arg[0], obs=∅} → whnf=lambda (contra-arg lambda
# applied once, no probes on the arg, returns partial). Cold's walker
# alongside a second `cached {...}` call matches that SCA and returns
# an RCA representing the applyResult. `(x1 1) 2` then applies that
# RCA further.
#
# Pre-H2: TCA::queryApply returned the applyResult bare; the primop
# routed the second apply as a plain SelectorApply through
# `<cached-fn>`. Warm's walker had no compositional SCA to look up,
# and RCA::queryApply on the returned childRca hit a null
# `obsSetResponses` branch (the childRca was constructed without it).
#
# Post-H2: TCA::queryApply wraps the applyResult in a TCA whose
# producer is the just-recorded compositional SCA. Subsequent applies
# re-enter TCA::queryApply and record another compositional SCA
# {fn=<parent SCA>, obs=<probes on new arg>}. Warm's childRca inherits
# the parent's `obsSetResponses` map; the existing filter (sca->parent
# == self->producer) selects the compositional SCA at each level.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/body.nix" << 'NIX'
{ f1 }: f1 (a: b: a + b)
NIX

# Two invocations so cold's walker-alongside can match the first's
# recorded SCA on the second (the "deferred case" path).
expr='let cached = builtins.cache { import = '"$TEST_ROOT"'/body.nix; };
          a = cached { f1 = x1: x1 1 2; };
          b = cached { f1 = x1: x1 1 2; };
      in a + b'

echo "=== cold: two identical curried callback invocations (expect 6) ==="
result=$(nix eval --impure --expr "$expr")
echo "Got: $result"
[[ "$result" == 6 ]]

echo "=== warm (DISALLOW_PARSE, expect 6) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr "$expr")
echo "Got: $result"
[[ "$result" == 6 ]]

# Different arg values also compose correctly.
expr2='let cached = builtins.cache { import = '"$TEST_ROOT"'/body.nix; };
           a = cached { f1 = x1: x1 10 20; };
           b = cached { f1 = x1: x1 100 200; };
       in a + b'

clearCache
echo "=== distinct args (expect 30 + 300 = 330) ==="
result=$(nix eval --impure --expr "$expr2")
echo "Got: $result"
[[ "$result" == 330 ]]
