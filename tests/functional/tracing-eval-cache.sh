#!/usr/bin/env bash

source common.sh

# Enable tracing in nix.conf
echo "tracing-eval-cache = true" >> "$NIX_CONF_DIR/nix.conf"

# getCacheDir() returns $HOME/.cache/nix; TracingDatabase appends eval-tracing-v0/traces
tracesDir="$TEST_HOME/.cache/nix/eval-tracing-v0/traces"
latestSymlink="$TEST_HOME/.cache/nix/eval-tracing-v0/latest.json"

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
