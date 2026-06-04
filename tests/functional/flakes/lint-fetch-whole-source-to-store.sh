#!/usr/bin/env bash

source ./common.sh

requireGit

# A flake whose evaluation copies its whole source to the store via
# `toString src`. With `lint-fetch-whole-source-to-store = ignore`
# (the default) this is allowed; the lint surfaces it as a warning or
# fatal error when the developer opts in.
repo=$TEST_ROOT/repo
createGitRepo "$repo"
cat > "$repo/flake.nix" <<EOF
{
  outputs = { self }: { x = toString self; };
}
EOF
git -C "$repo" add flake.nix
git -C "$repo" -c user.email=t@t -c user.name=t commit -q -m init

# Combining the eval cache (on by default for `nix eval` on a flake)
# with a non-`ignore` value of the lint is refused: the eval cache
# would silently bypass evaluation on a warm run, defeating the lint's
# purpose. The error tells the user to use `--no-eval-cache`.
expectStderr 1 nix --lint-fetch-whole-source-to-store fatal eval "$repo#x" \
    | grepQuiet 'no-eval-cache'

# Cold call with the eval cache off: the lint surfaces the callsite.
expectStderr 1 nix --no-eval-cache --lint-fetch-whole-source-to-store fatal eval "$repo#x" \
    | grepQuiet 'reading the entire contents of fetched source.*into the store.*lint-fetch-whole-source-to-store'

# Warm call: the sourcePathToHash sqlite cache is now populated, but
# the lint must still fire — the developer wants to find the offending
# callsite irrespective of whether a cache happens to short-circuit it.
expectStderr 1 nix --no-eval-cache --lint-fetch-whole-source-to-store fatal eval "$repo#x" \
    | grepQuiet 'reading the entire contents of fetched source.*into the store.*lint-fetch-whole-source-to-store'

# Per-file interpolation of a path literal (`${./inner.txt}`) reaches
# `copyPathToStore` with a non-root `sp.path` (the literal resolves
# to a subpath under the flake source's accessor). It copies the
# single file, not the whole tree, and the lint must *not* fire —
# flagging it would generate one warning per per-file interpolation
# in real-world evals (hundreds for an nix eval against nixpkgs) and
# drown the actual whole-tree offenders. Regression-guard for the
# `sp.path.isRoot()` gate.
#
# Note that `"${self}/inner.txt"` and `"${self + "/inner.txt"}"`
# both materialise the whole tree: `+` and `${...}` inductively peel
# `self.outPath`, and for flakes `outPath` remains a store-path
# *string* (lazy mode isn't exposed there). The peeling completes
# at the string layer; the whole tree is already materialised by
# then. The per-file gate only fires when the peeled value is a
# path *with a non-root subpath*; `./inner.txt` is path-typed in
# flake evaluation, so its interpolation hits the gate directly.
file_repo=$TEST_ROOT/file_repo
createGitRepo "$file_repo"
cat > "$file_repo/flake.nix" <<EOF
{
  outputs = { self }: { y = "\${./inner.txt}"; };
}
EOF
echo hi > "$file_repo/inner.txt"
git -C "$file_repo" add flake.nix inner.txt
git -C "$file_repo" -c user.email=t@t -c user.name=t commit -q -m init

# `fatal` would surface as exit 1 + a warning if the lint fired. The
# per-file copy must succeed silently (exit 0). Pre-emptively pass
# `--no-eval-cache` so the test still passes after the eval-cache+lint
# refuse rule lands in the next commit.
nix --no-eval-cache --lint-fetch-whole-source-to-store fatal eval "$file_repo#y" >/dev/null
