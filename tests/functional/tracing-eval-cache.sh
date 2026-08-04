#!/usr/bin/env bash

source common.sh

# Enable tracing in nix.conf. Use $test_nix_conf rather than
# $NIX_CONF_DIR/nix.conf so the test also works inside NixOS VM tests,
# where NIX_CONF_DIR is unset (the system nix daemon owns its own conf).
echo "tracing-eval-cache = true" >> "$test_nix_conf"

# getCacheDir() returns $HOME/.cache/nix; TracingDatabase appends eval-tracing-v1/traces
tracesDir="$TEST_HOME/.cache/nix/eval-tracing-v1/traces"
latestSymlink="$TEST_HOME/.cache/nix/eval-tracing-v1/latest.json"

# Evaluate a simple expression with tracing enabled.
# This should create a trace file.
nix eval --expr '1 + 2'

# Verify the traces directory was created
[[ -d "$tracesDir" ]]

# Verify at least one trace file was produced
traceFiles=("$tracesDir"/*.json)
(( ${#traceFiles[@]} >= 1 ))

# Verify the latest.json symlink exists and resolves to a file
[[ -L "$latestSymlink" ]]
[[ -f "$latestSymlink" ]]

# Verify the trace file has content (not just an empty array "[]")
latestTrace=$(readlink -f "$latestSymlink")
[[ $(wc -c < "$latestTrace") -gt 2 ]]

# Verify the trace contains query/result entries
grepQuiet '"type"' "$latestTrace"

# --------------------------------------------------------------------------
# Flake evaluation under tracing — regression net for the top-level apply
# boundary in `callFlakeViaEvaluator`.
#
# Flake commands route through `callFlakeViaEvaluator`, which builds a
# curried apply chain from `mkString` / `mkAttrs` / `getInternalPrimOp`
# args against an `evalFile`d call-flake.nix. Those args have no producer
# Selector, so `Evaluator::apply` under tracing has no way to derive a
# content-defined identity for them — the boundary panics
# ("fn/arg lacks a content-defined identity"). The fix must keep the
# apply boundary well-typed *and* avoid fabricating identity for
# indistinguishable args (which would allow wrong hits across flakes
# whose boundary args happen to hash the same but whose downstream
# values differ).
#
# Coverage below (in order of correctness surface):
#   1. Single flake, cold  → correct value  (panic regression).
#   2. Single flake, warm  → correct value  (warm-hit correctness).
#   3. Two flakes, cold+warm interleaved    → no cross-flake bleed.
#   4. Same flake, multi-attr, cold+warm    → attr routing preserved.
#   5. Repeat under one $NIX_TRACING_CACHE_DIR — verify the cache
#      dir accumulates trace files across invocations without the
#      apply-boundary panicking on any of them.
# --------------------------------------------------------------------------

requireGit

flakeA=$TEST_ROOT/flakeA
flakeB=$TEST_ROOT/flakeB
flakeM=$TEST_ROOT/flakeM
createGitRepo "$flakeA" ""
createGitRepo "$flakeB" ""
createGitRepo "$flakeM" ""

cat > "$flakeA/flake.nix" <<EOF
{ outputs = { self }: { answer = 42; }; }
EOF
cat > "$flakeB/flake.nix" <<EOF
{ outputs = { self }: { answer = 99; }; }
EOF
cat > "$flakeM/flake.nix" <<EOF
{ outputs = { self }: { a = 42; b = 99; c = 100; }; }
EOF

git -C "$flakeA" add flake.nix
git -C "$flakeB" add flake.nix
git -C "$flakeM" add flake.nix

# (1) Single flake cold — must not panic and must return the correct value.
[[ "$(nix eval "git+file://$flakeA#answer")" == 42 ]]

# (2) Single flake warm — cache is now populated for flakeA; second eval
# hits the cache and still yields 42.
[[ "$(nix eval "git+file://$flakeA#answer")" == 42 ]]

# (3) Two flakes, cold+warm interleaved — flakeB's cold recording must
# not be shadowed by flakeA's warmed cache (would signal a fabricated-
# identity hit collapsing distinct flakes onto one Terminal); flakeA's
# warm re-eval must not return flakeB's value either.
[[ "$(nix eval "git+file://$flakeB#answer")" == 99 ]]
[[ "$(nix eval "git+file://$flakeA#answer")" == 42 ]]
[[ "$(nix eval "git+file://$flakeB#answer")" == 99 ]]

# (4) Same-flake multi-attr — the apply chain into `callFlakeViaEvaluator`
# is identical across `#a` / `#b` / `#c`; the downstream getAttr walk must
# route to distinct values. Verifies that navigation past the top-level
# apply boundary is preserved.
[[ "$(nix eval "git+file://$flakeM#a")" == 42 ]]
[[ "$(nix eval "git+file://$flakeM#b")" == 99 ]]
[[ "$(nix eval "git+file://$flakeM#c")" == 100 ]]
# Warm repeats — same routing under the populated cache.
[[ "$(nix eval "git+file://$flakeM#c")" == 100 ]]
[[ "$(nix eval "git+file://$flakeM#a")" == 42 ]]
[[ "$(nix eval "git+file://$flakeM#b")" == 99 ]]

# (5) After the run, the trace directory should have accumulated one
# entry per invocation. If any invocation had panicked mid-flight the
# trace file for it either wouldn't exist or wouldn't be a complete
# JSON array.
traceFilesAfter=("$tracesDir"/*.json)
(( ${#traceFilesAfter[@]} > ${#traceFiles[@]} ))
