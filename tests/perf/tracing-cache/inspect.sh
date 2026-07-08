#!/usr/bin/env bash
# Run a real Nix eval with tracing-eval-cache enabled, then dump the tracing eval cache's
# decision-graph SQLite to confirm record() is actually receiving data.
set -euo pipefail

dir="$(mktemp -d -t nix-tracing-cache-inspect-XXXXXX)"
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
echo "=== the tracing eval cache decision-graph DB contents ==="
db="$dir/decision-graph.sqlite"
if [[ ! -f "$db" ]]; then
    echo "FAIL: the tracing eval cache DB not created"
    exit 1
fi

sqlite3 "$db" <<'SQL'
.mode column
.headers on

SELECT 'Requests' AS table_name, COUNT(*) AS rows FROM Requests
UNION ALL SELECT 'Queries', COUNT(*) FROM Queries
UNION ALL SELECT 'Results', COUNT(*) FROM Results
UNION ALL SELECT 'RequestSetNodes', COUNT(*) FROM RequestSetNodes
UNION ALL SELECT 'Ask', COUNT(*) FROM Ask
UNION ALL SELECT 'Terminal', COUNT(*) FROM Terminal
ORDER BY table_name;
SQL

echo
echo "=== Sanity checks ==="
asks=$(sqlite3 "$db" "SELECT COUNT(*) FROM Ask")
terminals=$(sqlite3 "$db" "SELECT COUNT(*) FROM Terminal")
results=$(sqlite3 "$db" "SELECT COUNT(*) FROM Results")

if [[ "$terminals" -lt 1 ]]; then
    echo "FAIL: expected at least 1 Terminal, got $terminals"
    exit 1
fi
if [[ "$results" -lt 1 ]]; then
    echo "FAIL: expected at least 1 Result payload, got $results"
    exit 1
fi
echo "OK: Terminal=$terminals, Ask=$asks, Results=$results"
