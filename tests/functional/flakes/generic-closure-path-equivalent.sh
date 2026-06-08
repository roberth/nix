#!/usr/bin/env bash

source ./common.sh

requireGit

# `builtins.genericClosure { pathEquivalent = true; ... }` lets the
# caller dedupe path keys against string keys without going through
# `toString path`. On a Copyable (fetched-tree) source, `toString`
# would walk the whole tree into the store — fatal under
# `lint-fetch-whole-source-to-store = fatal`. The point of this test
# is to pin that the new opt-in itself never falls back to that walk
# internally.

# Assert the marker for the feature is present. We're testing the
# Nix being built, so an absent marker is a real regression — fail
# loudly rather than skip (a downstream consumer probing
# `builtins ? isPathEquivalent` would see the same answer and
# silently disable the optimisation; we want the build to break
# first).
[[ $(nix eval --expr 'builtins ? isPathEquivalent') == "true" ]] \
    || fail 'builtins ? isPathEquivalent should be true'

# Flake with two outputs: one uses pathEquivalent (must stay lazy),
# one deliberately uses `toString` (must trigger the lint). The
# startSet mixes a path and a string so the cross-type arm of
# `PathEquivalentDedup` actually fires for the good output.
repo=$TEST_ROOT/repo
createGitRepo "$repo"
cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: {
    # Cheap-reject arm: the string isn't store-path-shaped, so
    # pathToStringEqual returns false without touching either
    # tree. The dedup keeps both elements in insertion order.
    # Smoke test that the simple branch never trips the lint.
    okCheapReject =
      let
        result = builtins.genericClosure {
          startSet = [
            { key = ./.; }
            { key = "/nonexistent/sentinel"; }
          ];
          operator = _: [ ];
          pathEquivalent = true;
        };
      in
      assert result == [
        { key = ./.; }
        { key = "/nonexistent/sentinel"; }
      ];
      "ok";

    # Substantive arm: pairs of (lazy path, matching store-path
    # string) that should each collapse to the *path* survivor
    # (insertion-order: path comes first in each pair).
    #
    # Pair A: both refer to the source root.
    # Pair B: both refer to the same *nonexistent* subpath inside
    #         the source. The subpath doesn't have to exist on
    #         disk — the cross-type check matches subpaths
    #         structurally (both sides are
    #         /nonexistent-subpath), and `contentsEqual`'s hint
    #         step skips when `maybeLstat` returns nullopt on
    #         either side. The full tree walk then runs and finds
    #         the two trees are byte-identical, so the pair
    #         collapses.
    #
    # The store-path-shaped strings are constructed via
    # `builtins.path { filter = _: _: true; }`. The explicit
    # filter routes through `addPath`/`fetchToStore` rather than
    # `copyPathToStore`, so it doesn't trip the lint itself —
    # an acknowledged loophole intended exactly for "I know I'm
    # asking to materialise this". Acceptable as test setup;
    # what we're verifying is that `pathEquivalent`'s *own*
    # implementation stays lint-quiet.
    #
    # Expected result: [{ key = ./.; } { key = ./nonexistent-subpath; }]
    # (the path keys survive — they appear first in each pair).
    # Cross-pair keys are not equivalent (different subpaths), so
    # both pairs survive as distinct closure entries.
    okSubstantive =
      let
        srcStr = builtins.path { path = ./.; name = "source"; filter = _: _: true; };
        result = builtins.genericClosure {
          startSet = [
            { key = ./.; }
            { key = srcStr; }
            { key = ./nonexistent-subpath; }
            { key = srcStr + "/nonexistent-subpath"; }
          ];
          operator = _: [ ];
          pathEquivalent = true;
        };
      in
      assert result == [
        { key = ./.; }
        { key = ./nonexistent-subpath; }
      ];
      "ok";

    # Negative control: the toString-based pattern that the
    # module system uses today. `toString ./.` walks the whole
    # Copyable tree into the store via `copyPathToStore` and the
    # lint catches it. No assert needed — evaluation aborts
    # before producing a value.
    bad = builtins.genericClosure {
      startSet = [
        { key = toString ./.; }
        { key = "/nonexistent/sentinel"; }
      ];
      operator = _: [ ];
    };
  };
}
EOF
git -C "$repo" add flake.nix
git -C "$repo" -c user.email=t@t -c user.name=t commit -q -m init

# Cheap-reject positive case: path + non-store-shaped string. The
# in-flake `assert` checks the exact result list (both elements
# present, in insertion order); the CLI just confirms the
# expression evaluated to "ok". A wrong-survivor regression would
# fire the in-flake assert before we get here.
[[ $(nix --no-eval-cache --lint-fetch-whole-source-to-store fatal eval --raw "$repo#okCheapReject") == "ok" ]] \
    || fail "okCheapReject did not evaluate to \"ok\" (in-flake assert failed)"

# Substantive positive case: two equivalent pairs, each collapsing
# to its path survivor. The in-flake `assert` pins both the
# survivors AND their order; the CLI just checks "ok".
#
# If a future refactor accidentally taught pathEquivalent to
# compute the lazy tree's store form via copyPathToStore, the
# fatal lint would fire here instead of producing "ok".
[[ $(nix --no-eval-cache --lint-fetch-whole-source-to-store fatal eval --raw "$repo#okSubstantive") == "ok" ]] \
    || fail "okSubstantive did not evaluate to \"ok\" (in-flake assert failed)"

# Negative control: the `toString` pattern materialises the source
# tree (sp.path.isRoot() at the copy site), the lint fires,
# evaluation aborts with the expected diagnostic. Demonstrates the
# lint is actually firing for the pattern we're moving callers
# away from.
expectStderr 1 nix --no-eval-cache --lint-fetch-whole-source-to-store fatal eval "$repo#bad" \
    | grepQuiet 'reading the entire contents of fetched source.*into the store.*lint-fetch-whole-source-to-store'

# The diagnostic must also surface the originating subpath as a
# trace ("while coercing path 'X' on a fetched source to a
# string"), so a real fatal hit in nixpkgs narrows to the
# offending file rather than just the accessor root. Pin the
# trace text so a refactor that dropped the catch-and-addTrace
# in coerceToString's Copyable arm would surface here.
expectStderr 1 nix --no-eval-cache --lint-fetch-whole-source-to-store fatal eval "$repo#bad" \
    | grepQuiet "while coercing path"
