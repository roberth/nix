#!/usr/bin/env bash
# Stress the v13 cache across a growing number of recorded queries.
# At checkpoints, measure (a) warm time on a known-cached attribute,
# (b) DB row counts, and (c) on-disk size. Detects soft regressions:
# warm latency should be roughly constant in cache size for a fixed
# attribute, since walks should hit the same entries regardless of
# what else is recorded.
set -euo pipefail

dir="$(mktemp -d -t v13-scale-XXXXXX)"
trap 'rm -rf "$dir"' EXIT

NIXPKGS="${NIXPKGS:-$HOME/nixpkgs}"
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

# Wide list of nixpkgs attrs spanning languages, sizes, and dependency
# depths. The goal is breadth, not exhaustiveness: more variety means
# more distinct (Q, FactSet) terminals.
ATTRS=(
    hello cowsay jq curl htop python3Minimal coreutils gnumake
    bash gawk gnused gnugrep gnutar gzip xz zlib openssl libgcc
    libxml2 libxslt sqlite ncurses readline pkg-config m4 bison
    flex autoconf automake libtool patchelf binutils gnumake3
    git tig hub gh ripgrep fd bat eza zoxide fzf
    nodejs_20 ruby_3_3 perl python3 lua5_4 go rustc cargo
    cmake meson ninja scons go-tools golangci-lint shellcheck
    nano vim neovim emacs micro helix kakoune
    tmux screen byobu zellij
    file which less more tree dust ncdu
    procps psmisc util-linux coreutils-full
    findutils diffutils gnupatch gnumake42
    bc dc units calc
    wget aria2 curl axel
)

run_attr() {
    local attr="$1"
    local label="$2"
    local stderr_file
    stderr_file="$(mktemp -t v13-scale-stderr-XXXXXX)"
    local started_ms ended_ms wall_ms
    started_ms=$(date +%s%3N)
    "$NIX_BIN_DIR/nix" eval \
        --extra-experimental-features 'nix-command' \
        --option tracing-eval-cache true \
        --option allow-unfree true \
        -f "$NIXPKGS" "${attr}.drvPath" \
        2> "$stderr_file" > /dev/null \
        || { echo "FAIL $attr $label"; cat "$stderr_file"; rm -f "$stderr_file"; return 0; }
    ended_ms=$(date +%s%3N)
    wall_ms=$((ended_ms - started_ms))
    local hits misses
    hits=$(grep -cE "replay hit" "$stderr_file" 2>&1 || true)
    misses=$(grep -cE "replay miss|replay fallback" "$stderr_file" 2>&1 || true)
    printf "%-28s %-22s wall=%5dms hits=%-3d miss=%-3d\n" \
        "$attr" "$label" "$wall_ms" "${hits:-0}" "${misses:-0}"
    rm -f "$stderr_file"
}

db_stats() {
    local label="$1"
    local db="$dir/decision-graph.sqlite"
    local size_kb rows
    size_kb=$(stat -c%s "$db" 2>/dev/null || echo 0)
    size_kb=$((size_kb / 1024))
    rows=$(sqlite3 "$db" "
        SELECT 'asks=' || (SELECT COUNT(*) FROM Asks) ||
               ' terms=' || (SELECT COUNT(*) FROM Terminals) ||
               ' facts=' || (SELECT COUNT(*) FROM FactSets) ||
               ' reqs=' || (SELECT COUNT(*) FROM Requests) ||
               ' resps=' || (SELECT COUNT(*) FROM Responses);
    ")
    printf ">>> %-22s db=%6dKB %s\n" "$label" "$size_kb" "$rows"
}

# Run an attr we'll keep using as the anchor for the warm-latency test.
ANCHOR="hello"
"$NIX_BIN_DIR/nix" eval --extra-experimental-features 'nix-command' \
    --option tracing-eval-cache true --option allow-unfree true \
    -f "$NIXPKGS" "${ANCHOR}.drvPath" > /dev/null 2> /dev/null
db_stats "after anchor record"
run_attr "$ANCHOR" "anchor-warm-0"
echo

# Recording phase: walk through ATTRS, recording each. After each
# checkpoint, measure how anchor's warm wallclock changes.
N=0
CHECKPOINTS=(5 10 20 40 80)
last_cp_idx=0
for attr in "${ATTRS[@]}"; do
    "$NIX_BIN_DIR/nix" eval \
        --extra-experimental-features 'nix-command' \
        --option tracing-eval-cache true \
        --option allow-unfree true \
        -f "$NIXPKGS" "${attr}.drvPath" > /dev/null 2> /dev/null \
        || true
    N=$((N + 1))
    if [[ "$N" == "${CHECKPOINTS[$last_cp_idx]:-9999}" ]]; then
        db_stats "after $N recorded"
        run_attr "$ANCHOR"    "anchor-warm-after-$N"
        run_attr "$attr"      "current-warm-after-$N"
        echo
        last_cp_idx=$((last_cp_idx + 1))
    fi
done

# Final pass: run anchor 3× back-to-back to test repeatability at scale.
db_stats "final"
for i in 1 2 3; do
    run_attr "$ANCHOR" "anchor-final-$i"
done
