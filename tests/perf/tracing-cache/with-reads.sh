#!/usr/bin/env bash
# Cold/warm timings on an eval that actually reads files — exercises
# the v13 Request/Response insertion path and v13 walk()'s dispatch.
set -euo pipefail

dir="$(mktemp -d -t nix-v13-reads-XXXXXX)"
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

# Set up a small file fixture.
fixture="$dir/fixture"
mkdir -p "$fixture"
echo "world" > "$fixture/data.txt"
echo 'true'  > "$fixture/flag.txt"

expr="let
  data = builtins.readFile \"$fixture/data.txt\";
  flag = builtins.readFile \"$fixture/flag.txt\";
in data + flag"

report_db() {
    local db="$dir/decision-graph.sqlite"
    [[ -f "$db" ]] || { echo "  (v13 db not present)"; return; }
    local counts
    counts=$(sqlite3 "$db" "
        SELECT 'asks=' || COUNT(*) FROM Asks UNION ALL
        SELECT 'terminals=' || COUNT(*) FROM Terminals UNION ALL
        SELECT 'factsets=' || COUNT(*) FROM FactSets UNION ALL
        SELECT 'requests=' || COUNT(*) FROM Requests UNION ALL
        SELECT 'responses=' || COUNT(*) FROM Responses UNION ALL
        SELECT 'results=' || COUNT(*) FROM Results
    " | paste -sd ' ' -)
    echo "  v13 db: $counts"
}

timed_eval() {
    local label="$1"
    local t0 t1 elapsed value
    t0=$(date +%s%N)
    value=$("$NIX_BIN_DIR/nix" eval \
        --extra-experimental-features 'nix-command' \
        --impure \
        --expr "$expr" \
        --option tracing-eval-cache true 2>&1)
    t1=$(date +%s%N)
    elapsed=$(( (t1 - t0) / 1000000 ))
    echo "$label: ${elapsed}ms, value=${value}"
    report_db
}

echo "=== Cold ==="
timed_eval cold

echo
echo "=== Warm-1 ==="
timed_eval warm-1

echo
echo "=== Warm-2 ==="
timed_eval warm-2

echo
echo "=== Warm-3 ==="
timed_eval warm-3
