#!/usr/bin/env bash
# Cross-branch cache reuse bench.
# See README.md for usage.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

REPO="${1:?usage: $0 repo_path [expr] [branches...]}"
EXPR="${2:-lib.version}"
shift 2 || true
BRANCHES=("$@")
if [[ ${#BRANCHES[@]} -eq 0 ]]; then
    BRANCHES=(upstream/main lazy-paths-v3)
fi

WORK="${WORK:-/tmp/sets-validation-multibranch}"
rm -rf "$WORK"
mkdir -p "$WORK"
export HOME="$WORK/home"
mkdir -p "$HOME"

DB="$HOME/.cache/nix/eval-tracing-index-v2/index.sqlite"

CLONE="$WORK/repo"
git clone --quiet --no-local --shared "$REPO" "$CLONE"
cd "$CLONE"

for b in "${BRANCHES[@]}"; do
    git fetch --quiet "$REPO" "${b#*/}":"refs/test/$(echo "$b" | tr '/' '_')" 2>/dev/null || true
done

timing() {
    { time nix eval --impure --option tracing-eval-cache true \
        --extra-experimental-features 'nix-command' \
        --expr "(import $CLONE {}).$EXPR" >/dev/null 2>/dev/null; } \
        2>&1 | awk '/real/ {print $2}'
}

echo "branch,result_hash,seconds_first,seconds_repeat,cache_kib,bindings_total,bindings_delta"
prev_bindings=0

for b in "${BRANCHES[@]}"; do
    refname="refs/test/$(echo "$b" | tr '/' '_')"
    git checkout --quiet --force "$refname" 2>/dev/null \
      || git checkout --quiet --force "$b" 2>/dev/null \
      || { echo "skip,$b,checkout-failed,,,,"; continue; }

    out=$(nix eval --impure --option tracing-eval-cache true \
        --extra-experimental-features 'nix-command' \
        --expr "(import $CLONE {}).$EXPR" 2>/dev/null || echo "EVAL_FAILED")
    result_hash=$(echo "$out" | sha256sum | head -c 12)

    first_t=$(timing)
    repeat_t=$(timing)

    cache_kib=$(du -k "$DB" 2>/dev/null | cut -f1 || echo 0)
    bindings=$(sqlite3 "$DB" "SELECT COUNT(*) FROM Bindings;" 2>/dev/null || echo 0)
    delta=$(( bindings - prev_bindings ))
    prev_bindings=$bindings

    echo "$b,$result_hash,$first_t,$repeat_t,$cache_kib,$bindings,$delta"
done
