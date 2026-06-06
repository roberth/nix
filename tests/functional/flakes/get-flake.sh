#!/usr/bin/env bash

source ./common.sh

createFlake1

mkdir -p "$flake1Dir/subflake"
cat > "$flake1Dir/subflake/flake.nix" <<EOF
{
  outputs = { self }:
    let
      # String-form route via flakeRefToString (parseFlakeRef → lockByFlakeRef).
      parentFlake = builtins.getFlake (builtins.flakeRefToString { type = "path"; path = self.sourceInfo.outPath; narHash = self.narHash; });
      # Path-value route via \`./..\` (SourceRoot kind dispatch → Copyable arm,
      # because the subflake gets fetched into a storepath before evaluation).
      # The assertion below pins that both routes yield the same outPath.
      parentFlake2 = builtins.getFlake ./..;
      # Subdir-of-in-store-flake via flakeRefToString's dir attr: should
      # preserve the parent storepath and append /subflake.
      subflakeViaStorePath = (builtins.getFlake (builtins.flakeRefToString { type = "path"; path = self.sourceInfo.outPath; narHash = self.narHash; dir = "subflake"; })).outPath;
      # Explicit-copy idiom: both \`\${./..}\` and \`builtins.path { path = ./..; }\`
      # produce strings *with* store-path context, which \`forceStringNoCtx\` in
      # \`getFlake\` refuses. The canonical workaround is to strip the context
      # via \`unsafeDiscardStringContext\` and let the resulting plain path
      # string go through the normal \`path:\` flakeref route.
      #
      # \`name = "source"\` in the \`builtins.path\` form is load-bearing.
      # \`./..\` IS the parent storepath \`/nix/store/<gkwn>-source\`. The
      # flake-fetcher names its fetch "source"; reusing that name here
      # makes \`builtins.path { path = ./..; name = "source"; }\` collide
      # storepaths with the parent (content hash unchanged either way, but
      # the name is folded into the storepath-hash derivation alongside the
      # content hash — so same content + same name → same storepath
      # string, byte-for-byte). The assertion below treats that identity
      # collision as its probe.
      #
      # Drop the attr and \`builtins.path\` defaults to \`./..\`'s basename
      # \`flake1\`, producing \`/nix/store/<x>-flake1\` — different name →
      # different storepath hash → different storepath string. The
      # assertion then compares two unrelated storepaths and fails for a
      # non-obvious reason (the explicit-copy idiom itself still works).
      parentViaInterpolation = (builtins.getFlake (builtins.unsafeDiscardStringContext "\${./..}")).outPath;
      parentViaBuiltinsPath = (builtins.getFlake (builtins.unsafeDiscardStringContext (builtins.path { path = ./..; name = "source"; }))).outPath;
      # Escape via flakeref \`dir\` attr on a path: ref. Two variants —
      # without a pre-existing storeFS mount (hits the FlakeRef-keyed
      # lockFlake → readFlake path) and with one (forces a fetchTree
      # first so the in-store shortcut in lockByFlakeRef fires
      # instead). Both must be rejected by Part 3's kind-aware
      # wrapper + flake-input-specific rewrap.
      escapingDirAttr = (builtins.getFlake (builtins.flakeRefToString {
        type = "path"; path = self.sourceInfo.outPath; narHash = self.narHash; dir = "../oops";
      })).outPath;
      escapingDirAttrViaShortcut =
        let
          mountForced = builtins.fetchTree { type = "path"; path = self.sourceInfo.outPath; narHash = self.narHash; };
          bad = builtins.getFlake (builtins.flakeRefToString {
            type = "path"; path = self.sourceInfo.outPath; narHash = self.narHash; dir = "../oops";
          });
        in builtins.seq mountForced.outPath bad.outPath;
    in {
      x = parentFlake.number;
      y = parentFlake2.number;
      parentOutPath1 = parentFlake.outPath;
      parentOutPath2 = parentFlake2.outPath;
      inherit subflakeViaStorePath parentViaInterpolation parentViaBuiltinsPath escapingDirAttr escapingDirAttrViaShortcut;
    };
}
EOF
git -C "$flake1Dir" add subflake/flake.nix

[[ $(nix eval "$flake1Dir/subflake#x") = 123 ]]

[[ $(nix eval "$flake1Dir/subflake#y") = 123 ]]

# String-form and path-value form of \`getFlake\` on the same parent must yield
# identical outPaths. Catches a regression where the path-value branch fails to
# reuse the parent's storepath and instead constructs a fresh source object.
parentOut=$(nix eval --raw "$flake1Dir/subflake#parentOutPath1")
[[ $(nix eval --raw "$flake1Dir/subflake#parentOutPath2") = "$parentOut" ]]

# Subdir-of-in-store-flake: \`getFlake (parentStorePath + "/subflake")\` should
# preserve the parent storepath and append the subdir suffix verbatim, not
# re-NAR the subdir into a fresh standalone store object.
[[ $(nix eval --raw "$flake1Dir/subflake#subflakeViaStorePath") = "$parentOut/subflake" ]]

# Explicit-copy idiom: \`unsafeDiscardStringContext "\${./..}"\` and
# \`unsafeDiscardStringContext (builtins.path { path = ./..; name = "source"; })\`
# both yield the parent's storepath when used as \`getFlake\` arguments.
# These need \`--impure\` because the discarded-context path string is an
# unlocked flakeref by construction (no narHash attr survives the strip).
# Pinning this catches a regression where \`forceStringNoCtx\` changes its
# behaviour for discarded-context paths, or where the \`path:\` fetcher
# stops recognising the in-store result of these copies.
[[ $(nix eval --impure --raw "$flake1Dir/subflake#parentViaInterpolation") = "$parentOut" ]]
[[ $(nix eval --impure --raw "$flake1Dir/subflake#parentViaBuiltinsPath") = "$parentOut" ]]

# Literal-path-value \`getFlake\`: shell-expanded as a Nix path-syntax
# expression, this evaluates to a path-value rooted on rootFS (the
# parser's accessor for absolute literals). rootFS is admitted once
# under the System-kinded \`rootFSRoot\` (EvalState ctor), so the
# SourceRoot kind dispatch in \`prim_getFlake\` lands in the System
# arm. That arm builds a \`path:\` Input from attrs and attaches the
# store's narHash for in-store paths, making the resulting FlakeRef
# locked (\`isLocked()\` true → pure-eval lock check passes);
# downstream the \`path:\` fetcher's source-shortcut reuses the
# storepath verbatim. Catches: System arm not firing (different arm
# → different outPath or error), and missing narHash attachment
# (unlocked FlakeRef → pure-eval rejection fails the assertion
# loudly).
[[ $(nix eval --raw --expr "(builtins.getFlake $parentOut).outPath") = "$parentOut" ]]

# A flakeref \`dir\` attr that resolves past the storepath root (relative
# to whatever in-tree subPath the URL points at) should be rejected,
# not silently clamped. Two paths through `prim_getFlake` exercise the
# same surface:
#
# - escapingDirAttr: no pre-existing storeFS mount → FlakeRef-keyed
#   `lockByFlakeRef` → `readFlake` (Part 3B).
# - escapingDirAttrViaShortcut: forces a fetchTree first so the
#   in-store shortcut in lockByFlakeRef fires (Part 3C).
expect 1 nix eval --raw "$flake1Dir/subflake#escapingDirAttr" 2>&1 \
    | grep "flake input subdir '\.\./oops' escapes"
expect 1 nix eval --raw "$flake1Dir/subflake#escapingDirAttrViaShortcut" 2>&1 \
    | grep "flake input subdir '\.\./oops' escapes"
