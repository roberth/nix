#!/usr/bin/env bash
# A/B compare: same eval workload with tracing-eval-cache OFF vs ON.
# OFF = no caching (re-evaluate every time).
# ON  = walk serves warm runs.
set -euo pipefail

dir="$(mktemp -d -t tracing-cache-vs-uncached-XXXXXX)"
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

cat > "$dir/proj/lib.nix" <<'EOF'
{
  add = a: b: a + b;
  greet = name: "Hello, ${name}!";
  sum = list: builtins.foldl' (acc: x: acc + x) 0 list;
  pickEven = list: builtins.filter (x: x - (x / 2) * 2 == 0) list;
}
EOF

cat > "$dir/proj/config.nix" <<'EOF'
{
  name = "test-project";
  numbers = [1 2 3 4 5 6 7 8 9 10];
  enabled = true;
}
EOF

cat > "$dir/proj/main.nix" <<'EOF'
let
  lib = import ./lib.nix;
  config = import ./config.nix;
in
  if config.enabled
  then {
    inherit (config) name;
    greeting = lib.greet config.name;
    evenSum = lib.sum (lib.pickEven config.numbers);
    totalSum = lib.sum config.numbers;
  }
  else { name = "disabled"; }
EOF

# Run the eval N times under the given cache option, return the
# median timing in ms.
run_n_times() {
    local label="$1"
    local cache_opt="$2"   # "true" or "false"
    local n="${3:-7}"
    local cache_dir
    cache_dir=$(mktemp -d -t bench-cache-XXXXXX)

    local times=()
    local i t0 t1
    for ((i=0; i<n; i++)); do
        t0=$(date +%s%N)
        if [[ "$cache_opt" == "true" ]]; then
            NIX_TRACING_CACHE_DIR="$cache_dir" "$NIX_BIN_DIR/nix" eval \
                --impure --option tracing-eval-cache "$cache_opt" \
                --extra-experimental-features 'nix-command' \
                --expr "import $dir/proj/main.nix" --json \
                >/dev/null 2>/dev/null
        else
            "$NIX_BIN_DIR/nix" eval \
                --impure --option tracing-eval-cache "$cache_opt" \
                --extra-experimental-features 'nix-command' \
                --expr "import $dir/proj/main.nix" --json \
                >/dev/null 2>/dev/null
        fi
        t1=$(date +%s%N)
        times+=( $(( (t1 - t0) / 1000000 )) )
    done

    # Median.
    IFS=$'\n' sorted=($(printf '%s\n' "${times[@]}" | sort -n))
    unset IFS
    local median=${sorted[$((n / 2))]}
    local min=${sorted[0]}
    local max=${sorted[$((n - 1))]}
    rm -rf "$cache_dir"
    printf "%-30s  min=%4dms  median=%4dms  max=%4dms  (n=%d)\n" "$label" "$min" "$median" "$max" "$n"
}

echo "Each row: same eval, $((7)) runs, reported as min/median/max in ms"
echo "----------------------------------------------------------------------"
run_n_times "cache OFF (uncached)"   "false" 7
echo
echo "cache ON (the tracing eval cache hits all warm runs):"
# Pre-warm one cache dir, then time the warm runs against it.
warm_dir=$(mktemp -d -t tracing-cache-warm-XXXXXX)
# Cold run to populate.
NIX_TRACING_CACHE_DIR="$warm_dir" "$NIX_BIN_DIR/nix" eval \
    --impure --option tracing-eval-cache true \
    --extra-experimental-features 'nix-command' \
    --expr "import $dir/proj/main.nix" --json \
    >/dev/null 2>/dev/null
# Now time the warm runs (cache already populated).
times=()
for i in 1 2 3 4 5 6 7; do
    t0=$(date +%s%N)
    NIX_TRACING_CACHE_DIR="$warm_dir" "$NIX_BIN_DIR/nix" eval \
        --impure --option tracing-eval-cache true \
        --extra-experimental-features 'nix-command' \
        --expr "import $dir/proj/main.nix" --json \
        >/dev/null 2>/dev/null
    t1=$(date +%s%N)
    times+=( $(( (t1 - t0) / 1000000 )) )
done
IFS=$'\n' sorted=($(printf '%s\n' "${times[@]}" | sort -n))
unset IFS
printf "%-30s  min=%4dms  median=%4dms  max=%4dms  (n=%d)\n" "warm the tracing eval cache hits" "${sorted[0]}" "${sorted[3]}" "${sorted[6]}" 7
rm -rf "$warm_dir"
