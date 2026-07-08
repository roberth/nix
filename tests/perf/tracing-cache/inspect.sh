#!/usr/bin/env bash
# Run a real Nix eval with tracing-eval-cache enabled, then dump v13's
# decision-graph SQLite to confirm record() is actually receiving data.
set -euo pipefail

dir="$(mktemp -d -t nix-v13-inspect-XXXXXX)"
trap 'rm -rf "$dir"' EXIT

export NIX_TRACING_CACHE_DIR="$dir"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export NIX_BIN_DIR="${NIX_BIN_DIR:-$repo_root/build/src/nix}"
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

# A modestly non-trivial eval — exercises attribute lookup, arithmetic,
# string ops. Should generate at least a few Asks edges.
expr='let x = { a = 1; b = "hello"; }; in x.a + (builtins.stringLength x.b)'

echo "=== Eval ==="
"$NIX_BIN_DIR/nix" eval \
    --extra-experimental-features 'nix-command' \
    --expr "$expr" \
    --option tracing-eval-cache true

echo
echo "=== v13 decision-graph DB contents ==="
db="$dir/decision-graph.sqlite"
if [[ ! -f "$db" ]]; then
    echo "FAIL: v13 DB not created"
    exit 1
fi

sqlite3 "$db" <<'SQL'
.mode column
.headers on

SELECT 'Requests' AS table_name, COUNT(*) AS rows FROM Requests
UNION ALL SELECT 'Responses', COUNT(*) FROM Results
UNION ALL SELECT 'Queries', COUNT(*) FROM Queries
UNION ALL SELECT 'Results', COUNT(*) FROM Results
UNION ALL SELECT 'RequestSets', COUNT(*) FROM RequestSets
UNION ALL SELECT 'FactSets', COUNT(*) FROM Requests
UNION ALL SELECT 'Asks', COUNT(*) FROM Ask
UNION ALL SELECT 'Terminals', COUNT(*) FROM Terminal
ORDER BY table_name;
SQL

echo
echo "=== Sanity checks ==="
asks=$(sqlite3 "$db" "SELECT COUNT(*) FROM Ask")
terminals=$(sqlite3 "$db" "SELECT COUNT(*) FROM Terminal")
factsets=$(sqlite3 "$db" "SELECT COUNT(*) FROM Requests")

if [[ "$terminals" -lt 1 ]]; then
    echo "FAIL: expected at least 1 Terminal, got $terminals"
    exit 1
fi
if [[ "$factsets" -lt 1 ]]; then
    echo "FAIL: expected at least 1 FactSet, got $factsets"
    exit 1
fi
echo "OK: Terminals=$terminals, Asks=$asks, FactSets=$factsets"
