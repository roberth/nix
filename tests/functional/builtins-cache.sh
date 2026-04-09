#!/usr/bin/env bash

source common.sh

enableFeatures "tracing-eval-cache"

cacheDir="$TEST_HOME/.cache/nix/eval-tracing-index-v1"

clearCache() {
    rm -rf "$cacheDir"
}

# --- Basic functionality ---

# Scalar import
echo '42' > "$TEST_ROOT/scalar.nix"
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/scalar.nix; }') == 42 ]]

# String import
echo '"hello"' > "$TEST_ROOT/string.nix"
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/string.nix; }') == '"hello"' ]]

# Attrset import — full and attribute access
echo '{ x = 1; y = "world"; }' > "$TEST_ROOT/attrs.nix"
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/attrs.nix; }') == '{ x = 1; y = "world"; }' ]]
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/attrs.nix; }).x') == 1 ]]
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/attrs.nix; }).y') == '"world"' ]]

# Nested attrset
echo '{ a = { b = { c = 99; }; }; }' > "$TEST_ROOT/nested.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/nested.nix; }).a.b.c') == 99 ]]

# List import
echo '[ 1 2 3 ]' > "$TEST_ROOT/list.nix"
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/list.nix; }') == '[ 1 2 3 ]' ]]

# Expression form
[[ $(nix eval --impure --expr 'builtins.cache { expr = "1 + 1"; baseDir = '"$TEST_ROOT"'; }') == 2 ]]

# --- Error handling ---

# Missing import and expr
expectStderr 1 nix eval --impure --expr 'builtins.cache {}' \
    | grepQuiet "either 'import' or 'expr' is required"

# Both import and expr
expectStderr 1 nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/scalar.nix; expr = "1"; }' \
    | grepQuiet "'import' and 'expr' are mutually exclusive"

# expr without baseDir
expectStderr 1 nix eval --impure --expr 'builtins.cache { expr = "1"; }' \
    | grepQuiet "'baseDir' is required"

# Unknown attribute
expectStderr 1 nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/scalar.nix; foo = 1; }' \
    | grepQuiet "unsupported argument 'foo'"

# --- Caching behavior ---

clearCache

# First evaluation records into trie
echo '{ val = 1; }' > "$TEST_ROOT/cached.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/cached.nix; }).val') == 1 ]]

# Trie index should exist
[[ -f "$cacheDir/index.sqlite" ]]

# Second evaluation (same file) should succeed (cache hit)
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/cached.nix; }).val') == 1 ]]

# --- Cache invalidation ---

# Modify the file
sleep 1
echo '{ val = 999; }' > "$TEST_ROOT/cached.nix"

# Should return new value (cache invalidated by file content change)
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/cached.nix; }).val') == 999 ]]

# --- Transitive dependency invalidation ---

clearCache

echo '{ dep = import ./dep.nix; }' > "$TEST_ROOT/parent.nix"
echo '100' > "$TEST_ROOT/dep.nix"

[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/parent.nix; }).dep') == 100 ]]

# Modify transitive dependency
sleep 1
echo '200' > "$TEST_ROOT/dep.nix"

[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/parent.nix; }).dep') == 200 ]]

# --- Multiple cache calls ---

echo '10' > "$TEST_ROOT/a.nix"
echo '20' > "$TEST_ROOT/b.nix"

[[ $(nix eval --impure --expr 'builtins.add (builtins.cache { import = '"$TEST_ROOT"'/a.nix; }) (builtins.cache { import = '"$TEST_ROOT"'/b.nix; })') == 30 ]]

# --- Functions: functionArgs ---

echo '{ x, y ? 13 }: x + y' > "$TEST_ROOT/fn.nix"

# A cached function should report its formals via functionArgs
[[ $(nix eval --impure --expr 'builtins.functionArgs (builtins.cache { import = '"$TEST_ROOT"'/fn.nix; })') == '{ x = false; y = true; }' ]]

# Calling a cached function
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fn.nix; }) { x = 1; }') == 14 ]]

# Simple lambda
echo 'x: x + 1' > "$TEST_ROOT/simple-fn.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/simple-fn.nix; }) 5') == 6 ]]

# --- Function call: cache invalidation ---

clearCache

# Function that reads a transitive dependency
echo '{ x }: x + import ./addend.nix' > "$TEST_ROOT/fn-dep.nix"
echo '100' > "$TEST_ROOT/addend.nix"

# First call: 1 + 100 = 101
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fn-dep.nix; }) { x = 1; }') == 101 ]]

# Second call with same args: should produce same result (cache hit)
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fn-dep.nix; }) { x = 1; }') == 101 ]]

# Change the transitive dependency
sleep 1
echo '200' > "$TEST_ROOT/addend.nix"

# Function body dependency changed: 1 + 200 = 201
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fn-dep.nix; }) { x = 1; }') == 201 ]]

# Change the function itself
sleep 1
echo '{ x }: x * import ./addend.nix' > "$TEST_ROOT/fn-dep.nix"

# Function changed: 1 * 200 = 200
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fn-dep.nix; }) { x = 1; }') == 200 ]]

# Different argument value
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fn-dep.nix; }) { x = 3; }') == 600 ]]

# Nested function: cached function returns a function
echo 'x: y: x + y' > "$TEST_ROOT/curried.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/curried.nix; }) 10 20') == 30 ]]

# --- Laziness: cached function results must not be eagerly evaluated ---

echo '{ ... }: { a = throw "nope"; b = 42; }' > "$TEST_ROOT/lazy-fn.nix"

# typeOf should see "set" without forcing attributes
[[ $(nix eval --impure --expr 'builtins.typeOf ((builtins.cache { import = '"$TEST_ROOT"'/lazy-fn.nix; }) { })') == '"set"' ]]

# Accessing a non-throwing attribute should work
[[ $(nix eval --impure --expr '((builtins.cache { import = '"$TEST_ROOT"'/lazy-fn.nix; }) { }).b') == 42 ]]

# The throwing attribute should only fail when accessed
expectStderr 1 nix eval --impure --expr '((builtins.cache { import = '"$TEST_ROOT"'/lazy-fn.nix; }) { }).a' \
    | grepQuiet "nope"

# --- Covariant callbacks: outer functions called by inner ---
# When the cached function receives an argument containing a function
# (e.g. an overlay), calling that function is a covariant callback —
# the inner evaluator calls back into the outer evaluator.

echo '{ f, x }: f x' > "$TEST_ROOT/call-fn.nix"

# Outer function applied inside cached function
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/call-fn.nix; }) { f = x: x + 1; x = 10; }') == 11 ]]

# --- Nested builtins.cache with function calls ---

clearCache

# inner.nix: a cached module that exports a function
echo '{ f = x: x * 10; base = 1; }' > "$TEST_ROOT/inner-mod.nix"

# outer.nix: uses a nested cache call and calls inner's function
cat > "$TEST_ROOT/outer-mod.nix" <<OUTER
let inner = builtins.cache { import = $TEST_ROOT/inner-mod.nix; };
in inner.f inner.base + inner.f 2
OUTER

# Nested cache: inner.f 1 * 10 = 10, inner.f 2 * 10 = 20, total = 30
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }') == 30 ]]

# Second eval (both layers cached)
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }') == 30 ]]

# Change inner module's function: x * 10 → x * 100
sleep 1
echo '{ f = x: x * 100; base = 1; }' > "$TEST_ROOT/inner-mod.nix"

# Inner changed: 1 * 100 + 2 * 100 = 300
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }') == 300 ]]

# Change inner module's data without changing the function
sleep 1
echo '{ f = x: x * 100; base = 5; }' > "$TEST_ROOT/inner-mod.nix"

# base changed: 5 * 100 + 2 * 100 = 700
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }') == 700 ]]

# Change outer module
sleep 1
cat > "$TEST_ROOT/outer-mod.nix" <<OUTER
let inner = builtins.cache { import = $TEST_ROOT/inner-mod.nix; };
in inner.f inner.base
OUTER

# Outer changed: just 5 * 100 = 500
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }') == 500 ]]

# Outer cache returns a function that wraps the inner cache
sleep 1
cat > "$TEST_ROOT/outer-mod.nix" <<OUTER
let inner = builtins.cache { import = $TEST_ROOT/inner-mod.nix; };
in x: inner.f x + inner.base
OUTER

# Call the outer cached function
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }) 3') == 305 ]]

# Second call with same arg (cache hit)
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }) 3') == 305 ]]

# Different arg
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }) 7') == 705 ]]

# Change inner — outer function result should change
sleep 1
echo '{ f = x: x * 10; base = 0; }' > "$TEST_ROOT/inner-mod.nix"

# 3 * 10 + 0 = 30
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/outer-mod.nix; }) 3') == 30 ]]

# --- Replay validation propagates through Environment chain ---
# Scenario: outer trie records with inner state P. On re-eval with
# different outer files, inner replays (validates via file hashes).
# If validation reads bypass the outer Environment, a subsequent
# change to inner files won't invalidate the outer.

clearCache

echo "tracing-eval-cache = true" >> "$NIX_CONF_DIR/nix.conf"

echo '{ val = import ./rv-leaf.nix; }' > "$TEST_ROOT/rv-inner.nix"
echo '13' > "$TEST_ROOT/rv-leaf.nix"
echo 'builtins.cache { import = '"$TEST_ROOT"'/rv-inner.nix; }' > "$TEST_ROOT/rv-outer.nix"

# Run 1: evaluate outer — records both outer and inner
[[ $(nix eval --impure -f "$TEST_ROOT/rv-outer.nix" val) == 13 ]]

# Run 2: same — should replay
[[ $(nix eval --impure -f "$TEST_ROOT/rv-outer.nix" val) == 13 ]]

# Run 3: change the outer file (force outer re-record while inner replays)
sleep 1
echo '(builtins.cache { import = '"$TEST_ROOT"'/rv-inner.nix; })' > "$TEST_ROOT/rv-outer.nix"
[[ $(nix eval --impure -f "$TEST_ROOT/rv-outer.nix" val) == 13 ]]

# Run 4: change the inner leaf — outer must invalidate
sleep 1
echo '14' > "$TEST_ROOT/rv-leaf.nix"
[[ $(nix eval --impure -f "$TEST_ROOT/rv-outer.nix" val) == 14 ]]

# --- Inner file reads visible to outer tracing ---
# When the outer evaluator has tracing enabled, file reads inside
# builtins.cache must flow through the outer environment's accessor
# chain so the outer trace records them as dependencies.

clearCache

latestSymlink="$TEST_HOME/.cache/nix/eval-tracing-v0/latest.json"

# Enable outer tracing
echo "tracing-eval-cache = true" >> "$NIX_CONF_DIR/nix.conf"

echo '{ inner = 13; }' > "$TEST_ROOT/inner-traced.nix"

[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/inner-traced.nix; }).inner') == 13 ]]

# The outer trace must contain a FileReadRequest (with "absPath" key)
# for files read inside builtins.cache. If the inner TracingEnvironment
# wraps a fresh SystemEnvironment, inner file reads bypass the outer
# accessor chain and won't appear as FileReadRequests.
[[ -f "$latestSymlink" ]]
outerTrace=$(readlink -f "$latestSymlink")

# The path appears in the expression string, so a naive grep matches.
# We must specifically check for an absPath entry — that only comes
# from TracingSourceAccessor recording a file read.
grep -q '"absPath".*inner-traced\.nix' "$outerTrace"

# Modify the file and verify the new value comes through.
sleep 1
echo '{ inner = 14; }' > "$TEST_ROOT/inner-traced.nix"

[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/inner-traced.nix; }).inner') == 14 ]]

# The new outer trace must also contain the file read.
newOuterTrace=$(readlink -f "$latestSymlink")
grep -q '"absPath".*inner-traced\.nix' "$newOuterTrace"

# --- Nested builtins.cache: leaf dependency invalidation ---
# Two levels of builtins.cache: outer imports middle.nix, which itself
# calls builtins.cache to import leaf.nix. The outer call's trie entry
# must include leaf.nix as a dependency so that changes to the leaf
# invalidate the outer cached result.

clearCache

echo 'builtins.cache { import = '"$TEST_ROOT"'/leaf.nix; }' > "$TEST_ROOT/middle.nix"
echo '13' > "$TEST_ROOT/leaf.nix"

# First evaluation populates the trie for both the outer and inner calls.
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/middle.nix; }') == 13 ]]

# Change the leaf file.
sleep 1
echo '14' > "$TEST_ROOT/leaf.nix"

# The outer trie entry must be invalidated because leaf.nix changed.
# Without the fix (inner TracingEnvironment wrapping a fresh
# SystemEnvironment), the outer trie wouldn't include leaf.nix in its
# dependencies and would serve the stale value 13.
[[ $(nix eval --impure --expr 'builtins.cache { import = '"$TEST_ROOT"'/middle.nix; }') == 14 ]]

# The outer trace must also record the leaf file read.
nestedOuterTrace=$(readlink -f "$latestSymlink")
grep -q '"absPath".*leaf\.nix' "$nestedOuterTrace"

# --- Input-traced nesting: inner replay during outer re-record ---
# The input-traced nesting model requires that inner file reads flow
# through the outer environment's accessor chain, even when the inner
# builtins.cache replays from its own trie. Without this, the outer
# recording misses inner dependencies and serves stale results.
#
# Scenario:
#   Step 1: outer=A, inner state P — both record fresh
#   Step 2: outer=B, inner state P — outer re-records, inner replays
#   Step 3: outer=B, inner state Q — outer replays (B unchanged),
#           but must detect inner state change P→Q
#
# The bug: in step 2 the inner replay satisfies file reads from the
# trie cache, bypassing the outer tracing environment. The outer
# recording never sees leaf.nix, so step 3 serves the stale value.

clearCache

echo '{ val = import ./itn-leaf.nix; }' > "$TEST_ROOT/itn-inner.nix"
echo '13' > "$TEST_ROOT/itn-leaf.nix"
echo 'builtins.cache { import = '"$TEST_ROOT"'/itn-inner.nix; }' > "$TEST_ROOT/itn-outer.nix"

# Step 1: outer=A, inner=P — fresh recording of both layers
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/itn-outer.nix; }).val') == 13 ]]

# Step 2: outer=B, inner=P — outer re-records (file changed), inner replays
sleep 1
echo '(builtins.cache { import = '"$TEST_ROOT"'/itn-inner.nix; })' > "$TEST_ROOT/itn-outer.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/itn-outer.nix; }).val') == 13 ]]

# Step 3: outer=B, inner=Q — outer unchanged, inner leaf changed.
# The outer trie MUST invalidate because itn-leaf.nix is a transitive
# dependency that should have been recorded during step 2.
sleep 1
echo '14' > "$TEST_ROOT/itn-leaf.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/itn-outer.nix; }).val') == 14 ]]
