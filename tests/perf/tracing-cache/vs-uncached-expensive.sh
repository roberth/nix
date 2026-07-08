#!/usr/bin/env bash
# A/B compare with a more computation-heavy workload: a recursive
# fibonacci-like calculation on a list, plus deeper attribute set
# construction. Should let the cache earn its keep.
set -euo pipefail

dir="$(mktemp -d -t tracing-cache-expensive-XXXXXX)"
trap 'rm -rf "$dir"' EXIT

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

cat > "$dir/proj/expensive.nix" <<'EOF'
let
  range = n: let go = i: if i >= n then [] else [i] ++ go (i + 1); in go 0;

  # Cheap-per-element but n is big enough to add up.
  bigList = range 500;

  fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2);

  fibs = builtins.map fib (range 15);

  sumList = list: builtins.foldl' (acc: x: acc + x) 0 list;
in {
  rangeSum = sumList bigList;
  fibsSum = sumList fibs;
  product = sumList (builtins.map (x: x * x) bigList);
  listLen = builtins.length bigList;
}
EOF

run_n_times() {
    local label="$1"
    local cache_opt="$2"
    local cache_dir="$3"
    local n="${4:-5}"

    local times=()
    for ((i=0; i<n; i++)); do
        local t0 t1
        t0=$(date +%s%N)
        NIX_TRACING_CACHE_DIR="$cache_dir" "$NIX_BIN_DIR/nix" eval \
            --impure --option tracing-eval-cache "$cache_opt" \
            --extra-experimental-features 'nix-command' \
            --expr "import $dir/proj/expensive.nix" --json \
            >/dev/null 2>/dev/null
        t1=$(date +%s%N)
        times+=( $(( (t1 - t0) / 1000000 )) )
    done

    IFS=$'\n' sorted=($(printf '%s\n' "${times[@]}" | sort -n))
    unset IFS
    printf "%-40s  min=%4dms  median=%4dms  max=%4dms\n" "$label" "${sorted[0]}" "${sorted[$((n / 2))]}" "${sorted[$((n - 1))]}"
}

echo "Workload: recursive range(500) + map(fib, range(15)) + squared sum"
echo "Five runs each; min/median/max in ms"
echo "----------------------------------------------------------------------"

# Uncached A/B.
uncached_dir=$(mktemp -d)
run_n_times "cache OFF (uncached, n=5)" "false" "$uncached_dir" 5
rm -rf "$uncached_dir"

# Cached: do one cold to populate, then 5 warm runs.
warm_dir=$(mktemp -d)
NIX_TRACING_CACHE_DIR="$warm_dir" "$NIX_BIN_DIR/nix" eval \
    --impure --option tracing-eval-cache true \
    --extra-experimental-features 'nix-command' \
    --expr "import $dir/proj/expensive.nix" --json \
    >/dev/null 2>/dev/null
run_n_times "cache ON  (warm the tracing eval cache hits, n=5)" "true" "$warm_dir" 5
rm -rf "$warm_dir"
