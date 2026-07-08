#!/usr/bin/env bash
# Smoke-test the tracing eval cache's decision-graph SQLite creation in
# a hermetic cache directory.
set -euo pipefail

dir="$(mktemp -d -t nix-tracing-cache-smoke-XXXXXX)"
trap 'rm -rf "$dir"' EXIT

export NIX_TRACING_CACHE_DIR="$dir"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export NIX_BIN_DIR="${NIX_BIN_DIR:-$repo_root/build/src/nix}"

# Library path for the locally-built binaries.
LD_LIBRARY_PATH="$(
    printf '%s:' \
        "$repo_root"/build/src/libcmd \
        "$repo_root"/build/src/libexpr \
        "$repo_root"/build/src/libexpr-c \
        "$repo_root"/build/src/libfetchers \
        "$repo_root"/build/src/libflake \
        "$repo_root"/build/src/libmain \
        "$repo_root"/build/src/libstore \
        "$repo_root"/build/src/libutil
)"
export LD_LIBRARY_PATH

echo "Cache dir: $dir"
echo "Running: nix eval '1 + 2' with --option tracing-eval-cache true"
"$NIX_BIN_DIR/nix" eval \
    --extra-experimental-features 'nix-command' \
    --expr '1 + 2' \
    --option tracing-eval-cache true

echo
echo "Files in cache dir:"
ls -la "$dir"

if [[ ! -f "$dir/decision-graph.sqlite" ]]; then
    echo "FAIL: decision-graph.sqlite is missing"
    exit 1
fi
echo
echo "OK: decision-graph SQLite database created"
