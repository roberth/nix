#!/usr/bin/env bash
# Measure cold vs warm cache behavior with tracing-eval-cache's walk() integrated.
# Runs the same eval twice — first cold (writes recording), second
# warm (should hit tracing-eval-cache's walk()). Reports both timings and tracing-eval-cache DB
# row counts after each pass.
set -euo pipefail

dir="$(mktemp -d -t nix-tracing-eval-cache-cold-warm-XXXXXX)"
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

expr='let x = { a = 1; b = "hello"; c = [1 2 3]; }; in x.a + (builtins.stringLength x.b) + (builtins.length x.c)'

report_db() {
    local db="$dir/decision-graph.sqlite"
    [[ -f "$db" ]] || { echo "  (db not present)"; return; }
    local counts
    counts=$(sqlite3 "$db" "
        SELECT 'ask=' || COUNT(*) FROM Ask UNION ALL
        SELECT 'terminal=' || COUNT(*) FROM Terminal UNION ALL
        SELECT 'requests=' || COUNT(*) FROM Requests UNION ALL
        SELECT 'results=' || COUNT(*) FROM Results UNION ALL
        SELECT 'queries=' || COUNT(*) FROM Queries
    " | paste -sd ' ' -)
    echo "  db: $counts"
}

timed_eval() {
    local label="$1"
    local t0 t1 elapsed value
    t0=$(date +%s%N)
    value=$("$NIX_BIN_DIR/nix" eval \
        --extra-experimental-features 'nix-command' \
        --expr "$expr" \
        --option tracing-eval-cache true)
    t1=$(date +%s%N)
    elapsed=$(( (t1 - t0) / 1000000 ))
    echo "$label: value=$value, ${elapsed}ms"
    report_db
}

echo "=== Cold ==="
timed_eval "cold"

echo
echo "=== Warm 1 ==="
timed_eval "warm-1"

echo
echo "=== Warm 2 ==="
timed_eval "warm-2"

echo
echo "=== Warm 3 ==="
timed_eval "warm-3"
