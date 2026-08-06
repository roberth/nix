#!/usr/bin/env bash

# System-kinded path Values crossing a callback boundary.
#
# Regression for the panic that fired when an inner-cached
# evaluation produced a path Value with `SourceRootKind::System`
# and the outer side received it through a callback arg. Each
# `EvalState` constructs its own `rootFSRoot` wrapper
# (`SourceRoot::make(rootFS, System, "path:")`), so pointer-equality
# ownership on the outer side treated the inner-owned wrapper as
# foreign and tried to delegate. Delegation only walks inner→outer,
# and outer has no parent — `stableRootIdentifier` returned nullopt,
# `WHNFPath` was stamped without a `sourceRootId`, and
# `reconstructPathFromWHNF` `std::terminate`d at the consuming end.
#
# The canonical real-world trip is `nixpkgs` NixOS modules: `_file`
# attributes hold path Values pointing at each module's source file,
# they're produced inside cached nixpkgs eval, and outer's callback
# code reads them. The scenario below reproduces the shape without
# needing `nixpkgs`.
#
# Fix: treat `SourceRootKind::System` as a singleton identity —
# any System-kinded root resolves to `"system"` on any state, with
# `getRootByIdentity("system")` returning the caller's own
# `rootFSRoot`. Semantically correct because System = the real
# filesystem, of which there is one.

source common.sh

enableFeatures "tracing-eval-cache"

cacheDir="$TEST_HOME/.cache/nix/eval-tracing-decision-graph"
clearCache() { rm -rf "$cacheDir"; }
clearCache

# `fn.nix` builds a value that contains a System-kinded path and
# hands it to the caller's callback. `mod.nix` is the file whose
# path the callback receives — actual contents don't matter, only
# the path Value's SourceRoot shape.
cat > "$TEST_ROOT/mod.nix" << 'NIX'
"contents of mod"
NIX

cat > "$TEST_ROOT/fn.nix" << 'NIX'
{ callback }: callback { modFile = ./mod.nix; }
NIX

expr='
  let
    cached = builtins.cache { import = '"$TEST_ROOT"'/fn.nix; };
  in cached {
    callback = pkg: toString pkg.modFile;
  }'

echo "=== cold ==="
cold=$(nix eval --impure --expr "$expr")
echo "cold: $cold"
[[ "$cold" == *mod.nix* ]] || { echo "expected cold result to contain mod.nix, got: $cold"; exit 1; }

echo "=== warm ==="
warm=$(nix eval --impure --expr "$expr")
echo "warm: $warm"
[[ "$warm" == *mod.nix* ]] || { echo "expected warm result to contain mod.nix, got: $warm"; exit 1; }
