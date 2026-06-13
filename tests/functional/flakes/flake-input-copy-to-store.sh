#!/usr/bin/env bash

source ./common.sh

requireGit

# `inputs.*.copyToStore = true` opts a flake input back into
# pre-lazy-paths behaviour. The visible contract:
#
#   - `outPath` stays a *string* regardless. That's the
#     legacy/expected shape for `inputs.<name>.outPath`.
#   - Path expressions *inside* the imported flake (`./.`, `./foo`,
#     etc.) become System-rooted with the storepath as their
#     CanonPath, so structural primops (`dirOf`, `baseNameOf`,
#     everything routing through `lib.path.deconstructPath`) walk
#     through the storepath the way they did before lazy-paths.
#     That's what `lib.fileset` actually anchors on.

storeDir=$(nix eval --raw --impure --expr 'builtins.storeDir')

# --- inputs.self.copyToStore ------------------------------------------
# The root flake opts itself in. This is the path needed for the
# v12-on-v12 case where the root flake's own source needs structural
# walking (e.g. `lib.fileset` over `./.`).

selfHostDir=$TEST_ROOT/selfhost
mkdir -p "$selfHostDir"
cat > "$selfHostDir"/flake.nix <<EOF
{
  inputs.self.copyToStore = true;
  outputs = { self }: {
    outPathType  = builtins.typeOf self.outPath;
    myPathType   = builtins.typeOf ./.;
    myPathDirType = builtins.typeOf (builtins.dirOf ./.);
    myPathDirStr = toString (builtins.dirOf ./.);
  };
}
EOF

# `self.outPath` stays a string.
result=$(nix eval --no-eval-cache --json "$selfHostDir#outPathType")
[[ "$result" == '"string"' ]] || fail "self.outPathType: expected \"string\", got: $result"

# But `./.` inside the imported flake is a path Value, System-rooted
# at the storepath. `dirOf` walks through.
result=$(nix eval --no-eval-cache --json "$selfHostDir#myPathType")
[[ "$result" == '"path"' ]] || fail "self ./. type: expected \"path\", got: $result"

result=$(nix eval --no-eval-cache --json "$selfHostDir#myPathDirType")
[[ "$result" == '"path"' ]] || fail "self dirOf ./. type: expected \"path\", got: $result"

result=$(nix eval --no-eval-cache --raw "$selfHostDir#myPathDirStr")
[[ "$result" == "$storeDir" ]] || fail "self dirOf ./. str: expected $storeDir, got: $result"

# --- inputs.<name>.copyToStore on a non-flake input -------------------
# A `flake = false` input with `copyToStore = true` still has
# string-typed `outPath`. There's no imported flake.nix here, so the
# path-Value-inside-the-imported-flake half doesn't apply — we just
# pin that `outPath` is a string.

sourceDir=$TEST_ROOT/source
createGitRepo "$sourceDir"
mkdir -p "$sourceDir"/lib
echo 'keep me' > "$sourceDir"/lib/keep
git -C "$sourceDir" add lib
git -C "$sourceDir" commit -m initial

hostDir=$TEST_ROOT/host
mkdir -p "$hostDir"
cat > "$hostDir"/flake.nix <<EOF
{
  inputs.src = { url = "git+file://$sourceDir"; flake = false; copyToStore = true; };
  outputs = { src, self }: {
    outPathType = builtins.typeOf src.outPath;
    outPathStr  = toString src.outPath;
  };
}
EOF

result=$(nix eval --no-eval-cache --json "$hostDir#outPathType")
[[ "$result" == '"string"' ]] || fail "src.outPathType: expected \"string\", got: $result"

result=$(nix eval --no-eval-cache --raw "$hostDir#outPathStr")
[[ "$result" =~ ^${storeDir}/[a-z0-9]+-source$ ]] || fail "src.outPathStr: expected ${storeDir}/<hash>-source, got: $result"

# --- flake-default-copy-to-store --------------------------------------
# When the flake doesn't set `copyToStore` and the global setting is
# on, the same shape applies. Lets users of flakes that target older
# Nix versions (which don't recognise the attribute) opt into the
# eager shape without modifying those flakes.

unsetHostDir=$TEST_ROOT/unsethost
mkdir -p "$unsetHostDir"
cat > "$unsetHostDir"/flake.nix <<EOF
{
  outputs = { self }: {
    myPathDirStr = toString (builtins.dirOf ./.);
  };
}
EOF

result=$(nix --option flake-default-copy-to-store true eval --no-eval-cache --raw "$unsetHostDir#myPathDirStr")
[[ "$result" == "$storeDir" ]] || fail "default-copy-to-store: expected $storeDir, got: $result"

