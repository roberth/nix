#!/usr/bin/env bash
# Find the input size at which the cache's quadratic behavior — likely
# in set canonicalisation / record() — starts dominating wallclock.
#
# Each invocation evaluates the same shape of work — force `drvPath`
# on the first K nixpkgs attrs whose name starts with "a" — varying K
# in a geometric sweep. Measures both:
#   - cold-record wall: time to populate cache from empty
#   - warm-replay wall: time to re-evaluate the same expression with
#     the cache primed (own process, fresh state each time)
#
# Look for the K at which cold or warm wall stops scaling linearly.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$repo_root/build/src/nix/nix}"
NIXPKGS="${NIXPKGS:-$HOME/nixpkgs}"

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

GT="${GT:-/nix/store/n0wrh3vjfwcqfyswwai0zcxvkpibq34v-time-1.10/bin/time}"

cd "$NIXPKGS"

# Pick the first K attrs from nixpkgs whose name starts with "a".
# Force drvPath on each. mapAttrs is too coarse — it'd iterate the
# whole attrset; using a known-bounded list keeps us in control of K.
expr_for_k() {
    local k="$1"
    # tryEval swallows aliases that throw (a4term → renamed, etc).
    # The (.drvPath or null) covers attrs that exist but aren't
    # derivations. Together: a stable bounded workload across K.
    cat <<EOF
with import ./. {};
let
  aAttrs = builtins.attrNames (lib.filterAttrs (n: _: lib.strings.hasPrefix "a" n) pkgs);
  picked = lib.take $k aAttrs;
  drvOf = n: let r = builtins.tryEval (pkgs.\${n}.drvPath or null);
             in if r.success then r.value else null;
in map drvOf picked
EOF
}

run_one() {
    local label="$1"
    local k="$2"
    local cache_dir="$3"
    local opt="$4"   # cache flag value
    local err
    err="$(mktemp -t v13-thresh-XXXXXX)"
    local started_ms ended_ms wall_ms
    started_ms=$(date +%s%3N)
    NIX_TRACING_CACHE_DIR="$cache_dir" \
        "$NIX_BIN" eval \
            --extra-experimental-features 'nix-command' \
            --option tracing-eval-cache "$opt" \
            --impure --json \
            --expr "$(expr_for_k "$k")" \
            > /dev/null 2> "$err" \
        || { echo "FAIL $label k=$k"; head -20 "$err"; rm -f "$err"; return 1; }
    ended_ms=$(date +%s%3N)
    wall_ms=$((ended_ms - started_ms))
    local db_kb=0
    if [[ -f "$cache_dir/decision-graph.sqlite" ]]; then
        db_kb=$(($(stat -c%s "$cache_dir/decision-graph.sqlite") / 1024))
    fi
    printf "k=%-5d %-10s wall=%6dms db=%6dKB\n" "$k" "$label" "$wall_ms" "$db_kb"
    rm -f "$err"
}

echo "K  off-wall  cold-wall  warm-wall  warm-speedup  db-final"
echo "----  --------  ---------  ---------  ------------  --------"

for k in 1 2 5 10 20 50 100 200 500 1000; do
    dir="$(mktemp -d -t v13-thresh-cache-XXXXXX)"
    trap 'rm -rf "$dir"' EXIT INT TERM
    # 1. Baseline: cache off (so we can compare warm-speedup later).
    s=$(date +%s%3N) ; \
        NIX_TRACING_CACHE_DIR="$dir" "$NIX_BIN" eval \
            --extra-experimental-features 'nix-command' \
            --option tracing-eval-cache false \
            --impure --json --expr "$(expr_for_k "$k")" \
            > /dev/null 2>/dev/null || { echo "k=$k: cache-off FAIL"; rm -rf "$dir"; continue; }
    off_ms=$(($(date +%s%3N) - s))

    # 2. Cold record.
    s=$(date +%s%3N) ; \
        NIX_TRACING_CACHE_DIR="$dir" "$NIX_BIN" eval \
            --extra-experimental-features 'nix-command' \
            --option tracing-eval-cache true \
            --impure --json --expr "$(expr_for_k "$k")" \
            > /dev/null 2>/dev/null || { echo "k=$k: cold FAIL"; rm -rf "$dir"; continue; }
    cold_ms=$(($(date +%s%3N) - s))

    # 3. Warm replay (best of 2 to drop kernel cache warm-up effects).
    warm1=99999 ; warm2=99999
    s=$(date +%s%3N) ; \
        NIX_TRACING_CACHE_DIR="$dir" "$NIX_BIN" eval \
            --extra-experimental-features 'nix-command' \
            --option tracing-eval-cache true \
            --impure --json --expr "$(expr_for_k "$k")" \
            > /dev/null 2>/dev/null && warm1=$(($(date +%s%3N) - s))
    s=$(date +%s%3N) ; \
        NIX_TRACING_CACHE_DIR="$dir" "$NIX_BIN" eval \
            --extra-experimental-features 'nix-command' \
            --option tracing-eval-cache true \
            --impure --json --expr "$(expr_for_k "$k")" \
            > /dev/null 2>/dev/null && warm2=$(($(date +%s%3N) - s))
    warm_ms=$(( warm1 < warm2 ? warm1 : warm2 ))

    db_kb=0
    [[ -f "$dir/decision-graph.sqlite" ]] && db_kb=$(($(stat -c%s "$dir/decision-graph.sqlite") / 1024))

    if [[ "$warm_ms" -gt 0 ]]; then
        speedup=$(( (off_ms * 100) / warm_ms ))
        speedup_disp="${speedup}/100x"
    else
        speedup_disp="n/a"
    fi
    printf "%-5d %7dms %8dms %8dms %12s %7dKB\n" \
        "$k" "$off_ms" "$cold_ms" "$warm_ms" "$speedup_disp" "$db_kb"

    rm -rf "$dir"
done
