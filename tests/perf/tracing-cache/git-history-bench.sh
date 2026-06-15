#!/usr/bin/env bash
# Cross-commit cache reuse bench.
# See README.md for usage.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

REPO="${1:?usage: $0 repo_path [n_commits] [expr]}"
N="${2:-5}"
EXPR="${3:-lib.version}"

WORK="${WORK:-/tmp/sets-validation-history}"
rm -rf "$WORK"
mkdir -p "$WORK"
export HOME="$WORK/home"
mkdir -p "$HOME"

DB="$HOME/.cache/nix/eval-tracing-index-v2/index.sqlite"

CLONE="$WORK/repo"
git clone --quiet --no-local --shared "$REPO" "$CLONE"
cd "$CLONE"

commits=$(git log --format=%H -n "$N")
prev_bindings=0

timing() {
    { time nix eval --impure --option tracing-eval-cache true \
        --extra-experimental-features 'nix-command' \
        --expr "(import $CLONE {}).$EXPR" >/dev/null 2>/dev/null; } \
        2>&1 | awk '/real/ {print $2}'
}

echo "commit,description,seconds_first,seconds_repeat,cache_kib,bindings_total,bindings_delta"
for sha in $commits; do
    short=$(git log --format='%h %s' -1 "$sha" | head -c 60 | tr ',' ';')
    git checkout --quiet --force "$sha"

    first_t=$(timing)
    repeat_t=$(timing)

    cache_kib=$(du -k "$DB" 2>/dev/null | cut -f1 || echo 0)
    bindings=$(sqlite3 "$DB" "SELECT COUNT(*) FROM Bindings;" 2>/dev/null || echo 0)
    delta=$(( bindings - prev_bindings ))
    prev_bindings=$bindings

    echo "$sha,\"$short\",$first_t,$repeat_t,$cache_kib,$bindings,$delta"
done
