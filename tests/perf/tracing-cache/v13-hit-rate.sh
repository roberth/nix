#!/usr/bin/env bash
# Count v13 walk hits vs misses across the standard synthetic
# workload, with verbose logging properly captured from stderr.
set -euo pipefail

dir="$(mktemp -d -t v13-hitrate-XXXXXX)"
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
    local v13hits v12hits misses
    v13hits=$(grep -c "replay hit (v13 walk)" "$stderr_file" || true)
    v12hits=$(grep -c "replay hit (sets\|trie\|shortcut)" "$stderr_file" || true)
    misses=$(grep -c "replay miss" "$stderr_file" || true)
    echo "v13_walk_hits=$v13hits v12_hits=$v12hits misses=$misses"
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
echo "v13 db: $(sqlite3 "$db" "SELECT 'asks=' || COUNT(*) FROM Asks UNION ALL SELECT 'terminals=' || COUNT(*) FROM Terminals UNION ALL SELECT 'factsets=' || COUNT(*) FROM FactSets UNION ALL SELECT 'requests=' || COUNT(*) FROM Requests UNION ALL SELECT 'responses=' || COUNT(*) FROM Responses UNION ALL SELECT 'results=' || COUNT(*) FROM Results" | paste -sd ' ' -)"
echo "v12 db: $(sqlite3 "$dir/index.sqlite" "SELECT 'queries=' || COUNT(*) FROM Queries UNION ALL SELECT 'results=' || COUNT(*) FROM Results UNION ALL SELECT 'shortcuts=' || COUNT(*) FROM Shortcuts UNION ALL SELECT 'bindings=' || COUNT(*) FROM Bindings" | paste -sd ' ' -)"
