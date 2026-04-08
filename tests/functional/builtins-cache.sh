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
