#!/usr/bin/env bash
# Validate v13 against nixpkgs at scale.
#
# Walks across several recent nixpkgs commits and across edited variants
# of a leaf file, measuring cold/warm wallclock and v13 hit counts each
# time. The cache persists across all phases — we are looking for both
# raw speedup *and* graceful behavior as the cache grows and is
# perturbed.
set -euo pipefail

# Hermetic cache dir.
dir="$(mktemp -d -t nix-v13-nixpkgs-XXXXXX)"
trap 'rm -rf "$dir"' EXIT

NIXPKGS="${NIXPKGS:-$HOME/nixpkgs}"
ATTR="${ATTR:-hello}"
PHASES_OUT="${PHASES_OUT:-$dir/phases.log}"

if [[ ! -d "$NIXPKGS" ]]; then
    echo "FAIL: NIXPKGS=$NIXPKGS not found" >&2
    exit 1
fi

export NIX_TRACING_CACHE_DIR="$dir"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
NIX_BIN_DIR="${NIX_BIN_DIR:-$repo_root/build/src/nix}"

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

run_eval() {
    local label="$1"
    local stderr_file
    stderr_file="$(mktemp -t v13-stderr-XXXXXX)"
    local started_ms ended_ms wallclock_ms
    started_ms=$(date +%s%3N)
    _NIX_TRACING_CACHE_LOGGING=1 \
    "$NIX_BIN_DIR/nix" eval \
        --extra-experimental-features 'nix-command' \
        --option tracing-eval-cache true \
        --option allow-unfree true \
        -f "$NIXPKGS" \
        "${ATTR}.drvPath" \
        2> "$stderr_file" > /dev/null || {
            echo "FAIL: $label eval failed; stderr:" >&2
            cat "$stderr_file" >&2
            return 1
        }
    ended_ms=$(date +%s%3N)
    wallclock_ms=$((ended_ms - started_ms))
    local hits misses
    hits=$(grep -cE "replay hit" "$stderr_file" || true)
    misses=$(grep -cE "replay miss" "$stderr_file" || true)
    local db_size
    db_size=$(stat -c%s "$dir/decision-graph.sqlite" 2>/dev/null || echo 0)
    printf "%-50s wall=%5dms v13_hits=%5d misses=%5d db=%dKB\n" \
        "$label" "$wallclock_ms" "${hits:-0}" "${misses:-0}" "$((db_size / 1024))" \
        | tee -a "$PHASES_OUT"
    rm -f "$stderr_file"
}

echo "=== Phase 1: cold/warm across recent nixpkgs commits ==="
echo "Cache dir: $dir"
echo

(
    cd "$NIXPKGS"
    base_head=$(git rev-parse HEAD)
    trap 'git checkout -f -q "$base_head"' EXIT
    for sha in $(git log --first-parent -n 5 --format=%H HEAD); do
        git checkout -f -q "$sha"
        commit_label=$(git log -1 --format='%h %s' | cut -c1-50)
        run_eval "commit-cold $commit_label"
        run_eval "commit-warm $commit_label"
    done
)

echo
echo "=== Phase 1b: many-attribute sweep (large trace count) at HEAD ==="
echo
saved_attr="$ATTR"
for a in hello cowsay jq curl htop python3Minimal coreutils gnumake; do
    ATTR="$a"
    run_eval "many-cold  $a"
done
for a in hello cowsay jq curl htop python3Minimal coreutils gnumake; do
    ATTR="$a"
    run_eval "many-warm  $a"
done
ATTR="$saved_attr"

echo
echo "=== Phase 2: edit-replay churn at HEAD ==="
echo
(
    cd "$NIXPKGS"
    leaf=pkgs/by-name/he/hello/package.nix
    if [[ ! -f "$leaf" ]]; then
        echo "skip: leaf $leaf missing"
    else
        cp "$leaf" "$dir/leaf.bak"
        trap 'cp "$dir/leaf.bak" "$leaf"' EXIT

        for i in 1 2 3 4 5; do
            # Toggle a harmless meta description suffix.
            python3 -c "
import sys, re, pathlib
p = pathlib.Path('$leaf')
src = p.read_text()
src = re.sub(r'(description\s*=\s*\"[^\"]*?)(\s*\[edit\d+\])?\"', r'\1 [edit$i]\"', src, count=1)
p.write_text(src)
"
            run_eval "edit-$i"
            run_eval "edit-$i-warm"
        done
    fi
)

echo
echo "=== Summary ==="
echo "Cache dir size: $(du -sh "$dir" | awk '{print $1}')"
echo "Phases log: $PHASES_OUT"
echo
echo "=== Final DB stats ==="
sqlite3 "$dir/decision-graph.sqlite" \
    "SELECT 'asks=' || COUNT(*) FROM Asks
     UNION ALL SELECT 'terminals=' || COUNT(*) FROM Terminals
     UNION ALL SELECT 'requestSetNodes=' || COUNT(*) FROM RequestSetNodes
     UNION ALL SELECT 'queries=' || COUNT(*) FROM Queries
     UNION ALL SELECT 'requests=' || COUNT(*) FROM Requests
     UNION ALL SELECT 'results=' || COUNT(*) FROM Results"
