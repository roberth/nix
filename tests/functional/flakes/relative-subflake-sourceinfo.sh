#!/usr/bin/env bash

# Within one nix eval, a relative subflake's `outPath` is the parent's
# `outPath` plus the relative subdir, and its `sourceInfo` matches the
# parent's (relative inputs share the parent's tree). Both assertions
# go first so they're not masked by any cross-eval / dirty-tree
# variability that other tests may exercise.
#
# The two-level case (A -> B -> C, both edges relative) pins that
# `parentNode.fetchResult` propagates transitively: B inherits A's
# fetchResult, then C inherits B's (= A's). C's sourceInfo.outPath
# must match A's, and C's outPath must be A's outPath + B's subdir +
# C's subdir.

source ./common.sh

requireGit

root="$TEST_ROOT/relroot"
sub="$root/sub"
subsub="$root/sub/subsub"

rm -rf "$root"
mkdir -p "$subsub"

cat > "$root/flake.nix" <<'EOF'
{
  inputs.sub.url = ./sub;
  outputs =
    { self, sub }:
    {
      probe = {
        rootOut = self.outPath;
        rootSrc = self.sourceInfo.outPath;
        subOut = sub.outPath;
        subSrc = sub.sourceInfo.outPath;
        subsubOut = sub.subsub.outPath;
        subsubSrc = sub.subsub.sourceInfo.outPath;
      };
    };
}
EOF

cat > "$sub/flake.nix" <<'EOF'
{
  inputs.subsub.url = ./subsub;
  outputs = { self, subsub }: { x = 1; inherit subsub; };
}
EOF

cat > "$subsub/flake.nix" <<'EOF'
{
  outputs = { self }: { y = 2; };
}
EOF

initGitRepo "$root"
git -C "$root" add flake.nix sub/flake.nix sub/subsub/flake.nix

result=$(nix eval --json "$root#probe")

rootOut=$(echo "$result" | jq -r .rootOut)
rootSrc=$(echo "$result" | jq -r .rootSrc)
subOut=$(echo "$result" | jq -r .subOut)
subSrc=$(echo "$result" | jq -r .subSrc)
subsubOut=$(echo "$result" | jq -r .subsubOut)
subsubSrc=$(echo "$result" | jq -r .subsubSrc)

# Relative subflake inherits the parent's tree (parentNode-derived).
[[ "$subOut" = "$rootOut/sub" ]] || {
    echo "subOut $subOut != $rootOut/sub" >&2
    exit 1
}

[[ "$subSrc" = "$rootSrc" ]] || {
    echo "subSrc $subSrc != rootSrc $rootSrc" >&2
    exit 1
}

# Root's own outPath and sourceInfo.outPath agree.
[[ "$rootOut" = "$rootSrc" ]] || {
    echo "rootOut $rootOut != rootSrc $rootSrc" >&2
    exit 1
}

# Two-level: subsub is relative to sub, sub is relative to root. The
# `parentNode.fetchResult` chain in call-flake.nix must propagate
# transitively, so subsub's tree is still root's tree (no fresh
# fetch). outPath composes by appending each level's relative subdir
# to root's outPath.
[[ "$subsubOut" = "$rootOut/sub/subsub" ]] || {
    echo "subsubOut $subsubOut != $rootOut/sub/subsub" >&2
    exit 1
}

[[ "$subsubSrc" = "$rootSrc" ]] || {
    echo "subsubSrc $subsubSrc != rootSrc $rootSrc" >&2
    exit 1
}
