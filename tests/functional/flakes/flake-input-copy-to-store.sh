#!/usr/bin/env bash

source ./common.sh

requireGit

# `inputs.*.copyToStore = true` opts an input back into pre-lazy-paths
# behaviour: the input's tree is materialised at fetch time, and the
# flake's `outputs` function sees `outPath` as a path Value whose
# CanonPath is the storepath. That makes `dirOf`/`baseNameOf` walk
# through the storepath structurally — which is what downstream code
# in nixpkgs (`lib.fileset`, `documentation.nix`, ...) anchors on.

sourceDir=$TEST_ROOT/source
createGitRepo "$sourceDir"
echo 'hello' > "$sourceDir"/file
git -C "$sourceDir" add file
git -C "$sourceDir" commit -m initial

hostDir=$TEST_ROOT/host
mkdir -p "$hostDir"
cat > "$hostDir"/flake.nix <<EOF
{
  inputs.src = {
    url = "git+file://$sourceDir";
    flake = false;
    copyToStore = true;
  };
  outputs = { src, self }: {
    outPathType  = builtins.typeOf src.outPath;
    outPathStr   = toString src.outPath;
    dirOfType    = builtins.typeOf (builtins.dirOf src.outPath);
    dirOfStr     = toString (builtins.dirOf src.outPath);
    twiceDirStr  = toString (builtins.dirOf (builtins.dirOf src.outPath));
    rootDirStr   = toString (builtins.dirOf (builtins.dirOf (builtins.dirOf src.outPath)));
  };
}
EOF

result=$(nix eval --no-eval-cache --json "$hostDir#outPathType")
[[ "$result" == '"path"' ]] || fail "outPathType: expected \"path\", got: $result"

# Tests run with a sandboxed nix store at $TEST_ROOT/store, so the
# storepath prefix isn't `/nix/store`. Match on the structure
# instead.
storeDir=$(nix eval --raw --impure --expr 'builtins.storeDir')
result=$(nix eval --no-eval-cache --raw "$hostDir#outPathStr")
[[ "$result" =~ ^${storeDir}/[a-z0-9]+-source$ ]] || fail "outPathStr: expected ${storeDir}/<hash>-source, got: $result"

# Without copyToStore (current lazy-paths default), dirOf would
# saturate immediately: same string as outPath. With copyToStore,
# dirOf must walk through.
result=$(nix eval --no-eval-cache --json "$hostDir#dirOfType")
[[ "$result" == '"path"' ]] || fail "dirOfType: expected \"path\", got: $result"

result=$(nix eval --no-eval-cache --raw "$hostDir#dirOfStr")
[[ "$result" == "$storeDir" ]] || fail "dirOfStr: expected $storeDir, got: $result"

result=$(nix eval --no-eval-cache --raw "$hostDir#twiceDirStr")
[[ "$result" == "$(dirname "$storeDir")" ]] || fail "twiceDirStr: expected $(dirname "$storeDir"), got: $result"

# Walk all the way to root: dirname repeatedly until we hit "/"
rootDir=$(dirname "$(dirname "$storeDir")")
[[ "$rootDir" == "/" ]] && expectedRoot=/ || expectedRoot=$rootDir
result=$(nix eval --no-eval-cache --raw "$hostDir#rootDirStr")
[[ "$result" == "$expectedRoot" ]] || fail "rootDirStr: expected $expectedRoot, got: $result"
