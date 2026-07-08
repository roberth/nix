#!/usr/bin/env bash
# Count walk hits vs misses across the standard synthetic
# workload, with verbose logging properly captured from stderr.
set -euo pipefail

dir="$(mktemp -d -t tracing-cache-hitrate-XXXXXX)"
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

mkdir -p "$dir/proj"
cat > "$dir/proj/expr.nix" <<'EOF'
let
  contents = builtins.readFile ./data.txt;
  flag = builtins.readFile ./flag.txt;
in "${contents}|${flag}"
EOF
echo "data1" > "$dir/proj/data.txt"
echo "flagA" > "$dir/proj/flag.txt"

count_logs() {
    local stderr_file="$1"
    local hits misses fallbacks
    # Current log format from tracing-replay-object.cc:
    #   "replay hit: <QueryTag>"     — walker found a Terminal
    #   "replay fallback: <method>"  — walker missed, fell through to inner
    hits=$(grep -c "replay hit:" "$stderr_file" || true)
    fallbacks=$(grep -c "replay fallback:" "$stderr_file" || true)
    misses=$(grep -c "replay miss" "$stderr_file" || true)
    echo "hits=${hits:-0} misses=${misses:-0} fallbacks=${fallbacks:-0}"
}

run_eval() {
    local label="$1"
    local stderr_file
    stderr_file=$(mktemp)
    local t0 t1 elapsed
    t0=$(date +%s%N)
    _NIX_TRACING_CACHE_LOGGING=1 "$NIX_BIN_DIR/nix" eval \
        --impure --option tracing-eval-cache true \
        --extra-experimental-features 'nix-command' \
        --expr "import $dir/proj/expr.nix" \
        >/dev/null 2>"$stderr_file"
    t1=$(date +%s%N)
    elapsed=$(( (t1 - t0) / 1000000 ))
    printf "%-30s %4dms   %s\n" "$label" "$elapsed" "$(count_logs "$stderr_file")"
    rm -f "$stderr_file"
}

echo "label                            time   counts"
echo "-------------------------------- ------ ----------------------------------------"
run_eval "cold"
run_eval "warm-1 (same data1)"
run_eval "warm-2 (same data1)"

echo "data2" > "$dir/proj/data.txt"
run_eval "after data-edit"

echo "flagB" > "$dir/proj/flag.txt"
run_eval "after flag-edit"

echo "data1" > "$dir/proj/data.txt"
run_eval "after data revert"

echo "flagA" > "$dir/proj/flag.txt"
run_eval "after flag revert (back to data1/flagA)"

echo
db="$dir/decision-graph.sqlite"
echo "db: $(sqlite3 "$db" "SELECT 'ask=' || COUNT(*) FROM Ask UNION ALL SELECT 'terminal=' || COUNT(*) FROM Terminal UNION ALL SELECT 'requests=' || COUNT(*) FROM Requests UNION ALL SELECT 'results=' || COUNT(*) FROM Results UNION ALL SELECT 'queries=' || COUNT(*) FROM Queries" | paste -sd ' ' -)"
