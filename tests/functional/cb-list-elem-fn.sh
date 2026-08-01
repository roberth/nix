#!/usr/bin/env bash

# Cached body returns an attrset containing a list of lambdas. Outer
# code accesses one element via getListElem and applies it. This must
# work — a nFunction list element is a routable value like any other.
#
# Pre-fix: TracingObject::getListElem wired the child's Selector only
# under cbApplyOrigin (and used self's producer, which was the wrong
# shape). Non-callback list-element children ended up with
# getSelector() = nullopt, which routed nFunction children through
# ExprFromObject::eval's makeOuterFnPrimOp fallback. That fallback
# wraps args as raw InterpreterObject with no identity; the resulting
# fnObj->queryApply lands in TracingObject::queryApply's identity
# check and panics.
#
# Symptom (pre-fix): the cold `nix eval` aborts with
# `terminating due to unexpected unrecoverable internal error:
# TracingObject::queryApply: fn/arg lacks a state hash`.
#
# Fix: TracingObject::getListElem now sets `child->withProducer(querySel)`
# unconditionally, mirroring maybeGetAttr. The nav child's identity IS
# SelectorGetListElem{index, parent=self}, always available whether or
# not the parent is a callback origin.

source common.sh

enableFeatures "tracing-eval-cache"

clearCache() {
    rm -rf "$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
}

clearCache

cat > "$TEST_ROOT/lst.nix" << 'NIX'
{ }: { fns = [ (x: x + 100) (x: x + 200) ]; }
NIX

# (1) cold: list-elem then apply
echo "=== cold ((cached {}).fns[0]) 42 ==="
result=$(nix eval --impure --expr \
    '(builtins.elemAt ((builtins.cache { import = '"$TEST_ROOT"'/lst.nix; }) {}).fns 0) 42')
[[ "$result" == 142 ]]

# (2) warm: same access hits the cache
echo "=== warm ((cached {}).fns[0]) 42 (DISALLOW_PARSE) ==="
result=$(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr \
    '(builtins.elemAt ((builtins.cache { import = '"$TEST_ROOT"'/lst.nix; }) {}).fns 0) 42')
[[ "$result" == 142 ]]

# (3) different list index (still a lambda) — applies too
echo "=== ((cached {}).fns[1]) 42 ==="
result=$(nix eval --impure --expr \
    '(builtins.elemAt ((builtins.cache { import = '"$TEST_ROOT"'/lst.nix; }) {}).fns 1) 42')
[[ "$result" == 242 ]]
