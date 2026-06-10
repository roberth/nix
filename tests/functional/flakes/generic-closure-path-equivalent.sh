#!/usr/bin/env bash

source ./common.sh

requireGit

# `builtins.genericClosure { pathEquivalent = true; ... }` lets the
# caller dedupe path keys against string keys without ever paying
# the per-comparison NAR walk that the old contents-based design
# suffered. The cheap layers in `accessorsEquivalent` (pointer,
# fingerprint, srcToStore lookup, root-name SHA256, hint SHA256)
# decide whenever they can. When they can't, the fallback computes
# the source's storePath through `copyPathToStore`, which trips
# `lint-fetch-whole-source-to-store` exactly as `toString` would —
# by design: any storePath compute on a fetched root is a fact the
# user should know about, and the materialisation gets cached so
# subsequent comparisons of the same source against anything are
# O(1) lookups in `srcToStore`.

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
    # (insertion-order: path comes first in each pair). Cheap
    # probes can't decide here — root-name SHA256 matches and the
    # hint subpath either isn't a regular file (Pair A's "") or
    # is absent on both sides (Pair B). So pathEquivalent falls
    # through to the storePath compute, which trips the lint by
    # design. Run without the fatal lint so the comparison can
    # finish; the in-flake `assert` then pins the dedup outcome.
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
# survivors AND their order; the CLI just checks "ok". Run with
# the lint disabled (default `warn`) — the storePath compute that
# decides each pair would trip a fatal lint, and we want the
# assert to complete so we can pin the dedup outcome.
[[ $(nix --no-eval-cache eval --raw "$repo#okSubstantive") == "ok" ]] \
    || fail "okSubstantive did not evaluate to \"ok\" (in-flake assert failed)"

# Substantive lint trip: re-run the same expression under
# `--lint-fetch-whole-source-to-store fatal` and pin that
# pathEquivalent does trip the lint when cheap probes don't
# decide. This is the design's honest signal that a tree walk
# happened — the user can choose to silence it (warn / ignore)
# if their workload's probes typically don't decide.
expectStderr 1 nix --no-eval-cache --lint-fetch-whole-source-to-store fatal eval --raw "$repo#okSubstantive" \
    | grepQuiet 'reading the entire contents of fetched source.*into the store.*lint-fetch-whole-source-to-store'

# Negative control: the `toString` pattern materialises the source
# tree (sp.path.isRoot() at the copy site), the lint fires,
# evaluation aborts with the expected diagnostic. Same diagnostic
# as the substantive case above — `toString` is exactly the
# materialisation point pathEquivalent's compute fallback now
# matches.
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
