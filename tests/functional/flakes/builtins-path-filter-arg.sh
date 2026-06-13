#!/usr/bin/env bash

source ./common.sh

requireGit

# `builtins.path { path = <Copyable source>; filter = ...; }` calls
# the filter for each file in the source. Historically, the filter
# argument has been `toString <file>`-equivalent — the storepath of
# the source plus the file's path within it. NixOS modules like
# `documentation.nix` build regexes from `toString pkgs.path` and
# match them against the filter argument; if the argument is just
# the accessor-relative path under lazy-paths, the regex never
# matches and the filter rejects everything.

# Set up a git+file: tree that surfaces as a Copyable nPath when
# accessed via `builtins.fetchTree { ...; lazy = true; }`. Flake
# inputs would lose the nPath shape (they materialise outPath as a
# string-with-context), so call fetchTree from inside the flake's
# outputs instead.
sourceDir=$TEST_ROOT/source
createGitRepo "$sourceDir"
mkdir -p "$sourceDir"/lib
echo 'keep me' > "$sourceDir"/lib/keep
echo 'irrelevant' > "$sourceDir"/other
git -C "$sourceDir" add lib other
git -C "$sourceDir" commit -m initial

hostDir=$TEST_ROOT/host
mkdir -p "$hostDir"
cat > "$hostDir"/flake.nix <<EOF
{
  outputs = { self }: let
    src = (builtins.fetchTree {
      type = "git";
      url = "file://$sourceDir";
      lazy = true;
    }).outPath;
    prefixRegex = "^" + (toString src) + "(\$|/(lib)(\$|/.*))";
    filtered = builtins.path {
      name = "filtered";
      path = src;
      filter = n: _t: builtins.match prefixRegex n != null;
    };
  in {
    # Reading lib/keep from the filtered storepath confirms the
    # filter let \`lib\` through — which only happens when the
    # filter arg is the toString-equivalent form. Pre-fix, the
    # filter sees just "/lib/keep" and the storepath-anchored
    # regex never matches.
    libContents = builtins.readFile (filtered + "/lib/keep");
  };
}
EOF

result=$(nix eval --impure --json "$hostDir#libContents")
[[ "$result" == '"keep me\n"' ]] || fail "expected libContents = 'keep me\\n', got: $result"
