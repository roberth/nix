#!/usr/bin/env bash

source common.sh

enableFeatures "tracing-eval-cache"

cacheDir="$TEST_HOME/.cache/nix/eval-tracing-index-v1"

clearCache() {
    rm -rf "$cacheDir"
}

# Start with a clean trie — no entries from previous test runs.
# No further clearCache calls: tests must handle accumulated entries.
clearCache

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

# First evaluation records into trie
echo '{ val = 1; }' > "$TEST_ROOT/cached.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/cached.nix; }).val') == 1 ]]

# Trie index should exist
[[ -f "$cacheDir/index.sqlite" ]]

# Second evaluation: must replay (parsing disallowed)
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/cached.nix; }).val') == 1 ]]

# --- Replay completeness: _NIX_DISALLOW_PARSE proves no re-evaluation ---
# With CLI-level tracing (--option tracing-eval-cache true), the outer
# evaluator replays the entire expression from the trie. If parsing is
# disallowed and the result is still correct, replay is working without
# falling back to re-evaluation.


cat > "$TEST_ROOT/replay-complete.nix" <<EOF
derivation { name = "replay-test"; system = builtins.currentSystem; builder = "/bin/sh"; args = [ "-c" "echo ok > \$out" ]; }
EOF

# Record with CLI-level tracing
nix build --option tracing-eval-cache true --impure --dry-run --expr 'builtins.cache { import = '"$TEST_ROOT"'/replay-complete.nix; }'

# Replay with parsing disallowed — proves result comes entirely from cache.
# Uses nix build --dry-run which navigates via the Object interface,
# unlike nix eval which calls defeatCache and bypasses replay.
_NIX_DISALLOW_PARSE=1 nix build --option tracing-eval-cache true --impure --dry-run --expr 'builtins.cache { import = '"$TEST_ROOT"'/replay-complete.nix; }'

# --- Cache invalidation ---

# Modify the file
sleep 1
echo '{ val = 999; }' > "$TEST_ROOT/cached.nix"

# Should return new value (cache invalidated by file content change)
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/cached.nix; }).val') == 999 ]]

# --- Transitive dependency invalidation ---


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

# Function that reads a transitive dependency
echo '{ x }: x + import ./addend.nix' > "$TEST_ROOT/fn-dep.nix"
echo '100' > "$TEST_ROOT/addend.nix"

# First call: 1 + 100 = 101
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fn-dep.nix; }) { x = 1; }') == 101 ]]

# Second call with same args: must replay (parsing disallowed)
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fn-dep.nix; }) { x = 1; }') == 101 ]]

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
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/call-fn.nix; }) { f = x: x + 1; x = 10; }') == 11 ]]

# Path values forwarded through the cache boundary
echo '{ p }: p' > "$TEST_ROOT/path-fn.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/path-fn.nix; }) { p = '"$TEST_ROOT"'; }') == /*/builtins-cache ]]

# Curried ambient function with self-referential attrset (callPackageWith pattern)
cat > "$TEST_ROOT/callpkg-fn.nix" << 'NIX'
{ callPackageWith }:
let
  self = { buildPackages = self; hello = "hi"; };
  mypkg = { buildPackages, hello }: hello;
in callPackageWith self mypkg {}
NIX
[[ $(nix eval --impure --expr '
  let callPackageWith = autoArgs: fn: args:
    let
      fargs = builtins.functionArgs fn;
      allArgs = builtins.intersectAttrs fargs autoArgs // args;
    in fn allArgs;
  in (builtins.cache { import = '"$TEST_ROOT"'/callpkg-fn.nix; }) { inherit callPackageWith; }
') == '"hi"' ]]

# Self-referential args with ambient callback (overrideAttrs pattern):
# the same Value passed to the callback multiple times must reuse the
# same bridged thunk, otherwise cycle detection fails.
cat > "$TEST_ROOT/selfref-fn.nix" << 'NIX'
{ applyOverlay }:
let
  base = { name = "test"; version = "1.0"; };
  args = applyOverlay base (args // { extra = true; });
in args
NIX
[[ $(nix eval --impure --expr '
  let applyOverlay = base: final: base // { hasVersion = final ? version; };
  in ((builtins.cache { import = '"$TEST_ROOT"'/selfref-fn.nix; }) { inherit applyOverlay; }).hasVersion
') == true ]]

# mkOverridable pattern: rattrs produces attrset without forcing its argument
cat > "$TEST_ROOT/overridable-fn.nix" << 'NIX'
{ mkOverridable }:
mkOverridable (self: { name = "pkg"; version = "1.0"; override = newF: mkOverridable newF; })
NIX
[[ $(nix eval --impure --expr '
  let mkOverridable = rattrs:
    let args = rattrs (args // { extra = true; });
    in args;
  in ((builtins.cache { import = '"$TEST_ROOT"'/overridable-fn.nix; }) { inherit mkOverridable; }).name
') == '"pkg"' ]]

# functionArgs across the cache boundary
cat > "$TEST_ROOT/fargs-fn.nix" << 'NIX'
{ f }:
let innerFn = { p, q ? 0 }: p + q;
in {
  # inner's functionArgs on outer lambda
  innerSeesOuter = builtins.functionArgs f;
  # forward outer lambda back through inner
  outerFwd = f;
  # forward inner lambda out through inner
  innerFwd = innerFn;
}
NIX

# inner's builtins.functionArgs on a lambda that's in outer
[[ $(nix eval --impure --expr 'let r = (builtins.cache { import = '"$TEST_ROOT"'/fargs-fn.nix; }) { f = { a, b ? 1 }: a + b; }; in r.innerSeesOuter') == '{ a = false; b = true; }' ]]

# outer's builtins.functionArgs on a lambda that's in inner
[[ $(nix eval --impure --expr 'let r = (builtins.cache { import = '"$TEST_ROOT"'/fargs-fn.nix; }) { f = { a, b ? 1 }: a + b; }; in builtins.functionArgs r.innerFwd') == '{ p = false; q = true; }' ]]

# outer's builtins.functionArgs on a lambda declared in outer but forwarded through inner
[[ $(nix eval --impure --expr 'let r = (builtins.cache { import = '"$TEST_ROOT"'/fargs-fn.nix; }) { f = { a, b ? 1 }: a + b; }; in builtins.functionArgs r.outerFwd') == '{ a = false; b = true; }' ]]

# inner's builtins.functionArgs on a lambda declared in inner but forwarded through outer
echo '{ }: { g = { m, n ? 5 }: m + n; }' > "$TEST_ROOT/fargs-inner.nix"
[[ $(nix eval --impure --expr '
  let inner = builtins.cache { import = '"$TEST_ROOT"'/fargs-inner.nix; };
      pkg = inner {};
  in (builtins.cache { expr = "{ g }: builtins.functionArgs g"; baseDir = '"$TEST_ROOT"'; }) { g = pkg.g; }
') == '{ m = false; n = true; }' ]]

# Fixed-point combinator with callback: overlay accesses the self-reference.
# The local argument must be a virtual value (not eagerly forced) to avoid
# infinite recursion through the fixed-point.
echo '{ overlay }: let fix = f: let x = f x; in x; in fix (self: { a = 1; } // overlay self)' > "$TEST_ROOT/fix-fn.nix"
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/fix-fn.nix; }) { overlay = self: { b = self.a + 1; }; }') == '{ a = 1; b = 2; }' ]]

# --- Ambient replay: multiple attributes with nested traversal ---
# Tests that the ambient id mapping handles multiple child Objects
# and child-of-child queries, and that invalidation of a nested
# value produces correct results (not stale cached values).
# No clearCache — prior trie entries from other tests must not interfere.

cat > "$TEST_ROOT/multi-attr.nix" << 'NIX'
{ args }:
let
  # Access b.value before a — non-obvious order
  bVal = args.b.value;
  aVal = args.a;
in aVal + bVal
NIX

# First call: 10 + 20 = 30 (records)
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/multi-attr.nix; }) { args = { a = 10; b = { value = 20; }; }; }') == 30 ]]

# Second call with same args: must replay (parsing disallowed)
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/multi-attr.nix; }) { args = { a = 10; b = { value = 20; }; }; }') == 30 ]]

# Third call: change nested value — must NOT serve stale 30
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/multi-attr.nix; }) { args = { a = 10; b = { value = 99; }; }; }') == 109 ]]

# Fourth call with same changed args: must replay
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/multi-attr.nix; }) { args = { a = 10; b = { value = 99; }; }; }') == 109 ]]

# Fifth call: change top-level value
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/multi-attr.nix; }) { args = { a = 1; b = { value = 99; }; }; }') == 100 ]]


# --- Nested builtins.cache with function calls ---


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

# --- Ambient unification: shared virtual value across attributes ---
# The cached function accesses the SAME ambient field in two separate
# lazy result attributes. Each attribute's getInt triggers its own
# ambient event section. The second section's backward validation
# encounters the first section's ambient events. The FIFO unification
# in dispatchAmbientQuery must map ids correctly: child Objects from
# getAttr must be consumed by the immediately-following id reference,
# not by an unrelated id from a different section.

cat > "$TEST_ROOT/shared-val.nix" << 'NIX'
{ args }:
{ a = args.x; b = args.x + 1; }
NIX

# Record: a=10, b=11
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/shared-val.nix; }) { args = { x = 10; }; }') == '{ a = 10; b = 11; }' ]]

# Replay: same args, must replay both attributes from cache
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/shared-val.nix; }) { args = { x = 10; }; }') == '{ a = 10; b = 11; }' ]]

# Change x: both must update — tests that the second section's
# backward walk correctly detects the change in a prior section
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/shared-val.nix; }) { args = { x = 99; }; }') == '{ a = 99; b = 100; }' ]]

# Replay changed
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/shared-val.nix; }) { args = { x = 99; }; }') == '{ a = 99; b = 100; }' ]]

# --- Ambient unification: independent fields, deep nesting ---
# Two attributes access DIFFERENT deeply-nested fields. Each path
# produces a chain of child ids (getAttr → child → getAttr → child → getInt).
# If the FIFO unification misorders these children, the wrong leaf
# value would be read: a would get b's value or vice versa.

cat > "$TEST_ROOT/deep-indep.nix" << 'NIX'
{ args }:
{ a = args.x.val; b = args.y.val; }
NIX

# Record: a=1, b=2
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 1; }; y = { val = 2; }; }; }') == '{ a = 1; b = 2; }' ]]

# Replay
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 1; }; y = { val = 2; }; }; }') == '{ a = 1; b = 2; }' ]]

# Change only y.val — a must stay 1, b must become 99.
# If the unification swapped x.val and y.val children, a=99 b=1.
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 1; }; y = { val = 99; }; }; }') == '{ a = 1; b = 99; }' ]]

[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 1; }; y = { val = 99; }; }; }') == '{ a = 1; b = 99; }' ]]

# Change only x.val — a must become 77, b stays 99
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/deep-indep.nix; }) { args = { x = { val = 77; }; y = { val = 99; }; }; }') == '{ a = 77; b = 99; }' ]]

# --- Ambient unification: interleaved deep access ---
# The function accesses fields from BOTH branches of the argument
# in an interleaved pattern: x first, then y, then back to x.
# The recording creates ambient ids in this interleaved order.
# Replay must map ids to Objects in the same order.

cat > "$TEST_ROOT/interleaved.nix" << 'NIX'
{ args }:
let
  xType = builtins.typeOf args.x;
  yType = builtins.typeOf args.y;
  xVal = args.x;
  yVal = args.y;
in xVal + yVal
NIX

[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/interleaved.nix; }) { args = { x = 10; y = 20; }; }') == 30 ]]
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/interleaved.nix; }) { args = { x = 10; y = 20; }; }') == 30 ]]

# Change y — if x and y children were swapped by unification, result
# would be 10+10=20 or 99+99=198 instead of correct 10+99=109
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/interleaved.nix; }) { args = { x = 10; y = 99; }; }') == 109 ]]
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/interleaved.nix; }) { args = { x = 10; y = 99; }; }') == 109 ]]

# --- Ambient unification: list elements ---
# Access list elements from a virtual value. getListElem also
# pushes to pendingChildren, same as getAttr. Tests that the FIFO
# order is maintained for list element children.

cat > "$TEST_ROOT/list-access.nix" << 'NIX'
{ args }:
let xs = args.items;
in builtins.elemAt xs 0 + builtins.elemAt xs 1 * 10
NIX

[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/list-access.nix; }) { args = { items = [ 3 7 ]; }; }') == 73 ]]
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/list-access.nix; }) { args = { items = [ 3 7 ]; }; }') == 73 ]]

# Swap elements — if unification mapped indices wrong, result stays 73
[[ $(nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/list-access.nix; }) { args = { items = [ 7 3 ]; }; }') == 37 ]]
[[ $(_NIX_DISALLOW_PARSE=1 nix eval --impure --expr '(builtins.cache { import = '"$TEST_ROOT"'/list-access.nix; }) { args = { items = [ 7 3 ]; }; }') == 37 ]]

# Clean up: remove tracing-eval-cache setting from nix.conf so it
# doesn't affect subsequent test runs. Match exactly to avoid
# removing the experimental-features line.
sed -i '/^tracing-eval-cache = true$/d' "$NIX_CONF_DIR/nix.conf"
